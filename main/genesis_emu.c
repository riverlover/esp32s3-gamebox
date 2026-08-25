/*
 * retro-go Gwenesis 单核宿主层
 *
 * 这个版本故意沿用 retro-go 的执行顺序：68000、Z80、VDP 和声音都在核 0
 * 逐扫描线推进，固定每四帧绘制一帧。它不是最终性能方案，而是用更简单、
 * 经 retro-go 使用的路径做 A/B 对照，判断此前静止画面是否来自双核同步。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_bus.h"
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"
#include "gwenesis_sn76489.h"
/* ESP-IDF 的 BIT(n) 与 retro-go VDP 的 BIT(value, index) 同名。宿主层后面
 * 不再使用 IDF 版本，先取消定义，避免无意义的重定义告警。 */
#ifdef BIT
#undef BIT
#endif
#include "gwenesis_vdp.h"

#include "audio_output.h"
#include "display.h"
#include "game_menu.h"
#include "genesis_emu.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "input_usb.h"
#include "nes_emu.h"
#include "rgb_led.h"
#include "esp_crc.h"

static const char *TAG = "genesis";

#define GEN_SOURCE_W       320
#define GEN_VISIBLE_H      224
#define GEN_FRAME_STORAGE  (GEN_SOURCE_W * 241 + 64)
#define GEN_AUDIO_BUF_LEN  GWENESIS_AUDIO_BUFFER_LENGTH_PAL
#define GEN_FRAME_SKIP     3u

/* 真实 Genesis 手柄没有 SELECT/X，这两位只由宿主产生；组合键不会占用
 * Genesis 原机按键。 */
#define MENU_COMBO_BITS    (GAMEPAD_BIT_SELECT | GAMEPAD_BIT_X)

_Static_assert(GEN_SOURCE_W <= DISP_W && GEN_VISIBLE_H <= DISP_H,
               "Genesis 原生画布必须能装进面板");

/* retro-go 核心从宿主取得这些逐帧变量和声音缓冲。声音先按约 53 kHz 生成，
 * 提交给 MAX98357 前抽取为约 26 kHz，数组大小同时覆盖 PAL 的最长一帧。 */
int system_clock;
int scan_line;
int16_t gwenesis_sn76489_buffer[GEN_AUDIO_BUF_LEN];
int sn76489_index;
int sn76489_clock;
int16_t gwenesis_ym2612_buffer[GEN_AUDIO_BUF_LEN];
int ym2612_index;
int ym2612_clock;

extern uint8_t *VRAM;
extern uint8_t *M68K_RAM;
extern uint16_t CRAM565[];
extern uint8_t gwenesis_vdp_regs[];
extern uint16_t gwenesis_vdp_status;
extern int hint_pending;
extern int screen_width;
extern int screen_height;
extern int zclk;

typedef struct {
    uint8_t *storage;
    uint8_t *pixels;
    uint16_t palette[256];
    int width;
    int height;
    uint32_t hash;
} genesis_frame_t;

static genesis_frame_t s_frames[2];
static int s_frame_index;
static rom_store_image_t s_rom;
static FILE *s_savestate_fp;
static int s_savestate_errors;

typedef struct {
    char key[28];
    uint32_t length;
} genesis_state_var_t;

static inline int16_t clamp16(int value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static inline uint16_t swap16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static void free_runtime(void)
{
    free(M68K_RAM);
    M68K_RAM = NULL;
    free(VRAM);
    VRAM = NULL;
    for (int i = 0; i < 2; i++) {
        free(s_frames[i].storage);
        memset(&s_frames[i], 0, sizeof(s_frames[i]));
    }
    rom_store_image_release(&s_rom);
}

static bool allocate_runtime(void)
{
    M68K_RAM = heap_caps_malloc(MAX_RAM_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (M68K_RAM) ESP_LOGI(TAG, "内存 M68K RAM %u B -> 内部 RAM", MAX_RAM_SIZE);

    VRAM = heap_caps_malloc(VRAM_MAX_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *vram_where = "内部 RAM";
    if (!VRAM) {
        VRAM = heap_caps_malloc(VRAM_MAX_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        vram_where = "PSRAM";
    }
    if (VRAM) ESP_LOGI(TAG, "内存 VRAM %u B -> %s", VRAM_MAX_SIZE, vram_where);

    for (int i = 0; i < 2; i++) {
        s_frames[i].storage = heap_caps_calloc(
            1, GEN_FRAME_STORAGE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* retro-go 的 H32 路径会在每行起点前清 32 字节，保留前置空间避免
         * 第一行越界；其后仍严格按 320 字节跨行。 */
        if (s_frames[i].storage) s_frames[i].pixels = s_frames[i].storage + 32;
    }

    if (!M68K_RAM || !VRAM || !s_frames[0].storage || !s_frames[1].storage) {
        ESP_LOGE(TAG, "retro-go Genesis 运行内存不足");
        free_runtime();
        return false;
    }
    ESP_LOGI(TAG, "两块 320x241 索引帧缓冲 -> PSRAM");
    return true;
}

static void genesis_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const genesis_frame_t *frame = ctx;
    int crop_y = (frame->height - GEN_VISIBLE_H) / 2;
    if (crop_y < 0) crop_y = 0;

    for (int y = 0; y < h; y++) {
        const uint8_t *src = frame->pixels + (y0 + y + crop_y) * GEN_SOURCE_W;
        uint16_t *dst = strip + y * GEN_SOURCE_W;
        int source_w = frame->width;
        if (source_w > GEN_SOURCE_W) source_w = GEN_SOURCE_W;
        int left = (GEN_SOURCE_W - source_w) / 2;

        for (int x = 0; x < left; x++) dst[x] = C_BLACK;
        for (int x = 0; x < source_w; x++) {
            dst[left + x] = frame->palette[src[x]];
        }
        for (int x = left + source_w; x < GEN_SOURCE_W; x++) dst[x] = C_BLACK;
    }
}

static uint32_t frame_hash(const genesis_frame_t *frame)
{
    /* 只抽样约 2500 个像素，不让诊断本身改变帧率。 */
    uint32_t hash = 2166136261u;
    const size_t bytes = GEN_SOURCE_W * GEN_VISIBLE_H;
    for (size_t i = 0; i < bytes; i += 29) {
        hash = (hash ^ frame->pixels[i]) * 16777619u;
    }
    return hash;
}

static void update_input(uint16_t state)
{
    const bool pressed[8] = {
        state & GAMEPAD_BIT_UP, state & GAMEPAD_BIT_DOWN,
        state & GAMEPAD_BIT_LEFT, state & GAMEPAD_BIT_RIGHT,
        state & GAMEPAD_BIT_A,       /* Genesis B：菱形右键 */
        state & GAMEPAD_BIT_Y,       /* Genesis C：菱形左键 */
        state & GAMEPAD_BIT_B,       /* Genesis A：菱形下键 */
        state & GAMEPAD_BIT_START,
    };
    for (int i = 0; i < 8; i++) {
        if (pressed[i]) gwenesis_io_pad_press_button(0, i);
        else gwenesis_io_pad_release_button(0, i);
    }
}

static int mix_audio_downsample(void)
{
    int source_len = sn76489_index > ym2612_index ? sn76489_index : ym2612_index;
    if (source_len > GEN_AUDIO_BUF_LEN) source_len = GEN_AUDIO_BUF_LEN;
    int output_len = source_len / 2;

    for (int i = 0; i < output_len; i++) {
        int src = i * 2;
        int sample = 0;
        if (src < ym2612_index) sample += gwenesis_ym2612_buffer[src];
        if (src < sn76489_index) sample += gwenesis_sn76489_buffer[src];
        int16_t mixed = clamp16(sample);
        gwenesis_ym2612_buffer[i * 2] = mixed;
        gwenesis_ym2612_buffer[i * 2 + 1] = mixed;
    }
    return output_len;
}

static void run_frame(bool render)
{
    int lines = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    screen_width = REG12_MODE_H40 ? 320 : 256;
    screen_height = REG1_PAL ? 240 : 224;
    int hint_counter = gwenesis_vdp_regs[10];

    gwenesis_vdp_render_config();
    /* retro-go 头文件沿用旧的 unsigned short * 声明，实际实现按 8 位调色板
     * 索引写入；这里显式转换，保持上游源码不动。 */
    if (render)
        gwenesis_vdp_set_buffer((unsigned short *)s_frames[s_frame_index].pixels);

    system_clock = 0;
    zclk = 0;
    ym2612_clock = ym2612_index = 0;
    sn76489_clock = sn76489_index = 0;
    scan_line = 0;

    while (scan_line < lines) {
        m68k_run(system_clock + VDP_CYCLES_PER_LINE);
        z80_run(system_clock + VDP_CYCLES_PER_LINE);

        if (render && scan_line < screen_height)
            gwenesis_vdp_render_line(scan_line);

        if (scan_line == 0 || scan_line > screen_height)
            hint_counter = REG10_LINE_COUNTER;

        if (--hint_counter < 0) {
            if (REG0_LINE_INTERRUPT && scan_line <= screen_height) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0)
                    m68k_update_irq(4);
            }
            hint_counter = REG10_LINE_COUNTER;
        }

        scan_line++;
        if (scan_line == screen_height) {
            if (REG1_VBLANK_INTERRUPT) {
                gwenesis_vdp_status |= STATUS_VIRQPENDING;
                m68k_set_irq(6);
            }
            z80_irq_line(1);
        }
        if (scan_line == screen_height + 1) z80_irq_line(0);
        system_clock += VDP_CYCLES_PER_LINE;
    }

    /* retro-go 组件默认使用 cycle-accurate 声音，寄存器访问时已经推进过；
     * 这里补齐这一帧尾部尚未生成的采样点。 */
    gwenesis_SN76489_run(system_clock);
    ym2612_run(system_clock);
    m68k.cycles -= system_clock;
}

/* Gwenesis 核心把状态拆成带名字的小字段，文件封装照 retro-go 的宿主格式。
 * OpenForRead/Write 的 name 是逻辑分组名，实际所有分组顺序写进同一个槽文件。 */
SaveState *saveGwenesisStateOpenForRead(const char *name)
{
    (void)name;
    return s_savestate_fp ? (SaveState *)1 : NULL;
}

SaveState *saveGwenesisStateOpenForWrite(const char *name)
{
    (void)name;
    return s_savestate_fp ? (SaveState *)1 : NULL;
}

int saveGwenesisStateGet(SaveState *state, const char *tag)
{
    int value = 0;
    saveGwenesisStateGetBuffer(state, tag, &value, sizeof(value));
    return value;
}
void saveGwenesisStateSet(SaveState *state, const char *tag, int value)
{
    saveGwenesisStateSetBuffer(state, tag, &value, sizeof(value));
}
void saveGwenesisStateGetBuffer(SaveState *state, const char *tag, void *buffer, int length)
{
    (void)state;
    if (!s_savestate_fp || !buffer || length < 0) {
        s_savestate_errors++;
        return;
    }

    long initial = ftell(s_savestate_fp);
    bool from_start = false;
    genesis_state_var_t var;
    while (!from_start || ftell(s_savestate_fp) < initial) {
        if (fread(&var, sizeof(var), 1, s_savestate_fp) != 1) {
            if (!from_start) {
                clearerr(s_savestate_fp);
                fseek(s_savestate_fp, 0, SEEK_SET);
                from_start = true;
                continue;
            }
            break;
        }
        if (strncmp(var.key, tag, sizeof(var.key)) == 0) {
            size_t take = var.length < (uint32_t)length ? var.length : (uint32_t)length;
            if (fread(buffer, 1, take, s_savestate_fp) != take) {
                s_savestate_errors++;
            }
            if (var.length > take) fseek(s_savestate_fp, var.length - take, SEEK_CUR);
            return;
        }
        if (fseek(s_savestate_fp, var.length, SEEK_CUR) != 0) break;
    }
    ESP_LOGW(TAG, "Genesis 存档字段缺失：%s", tag);
    s_savestate_errors++;
}
void saveGwenesisStateSetBuffer(SaveState *state, const char *tag, void *buffer, int length)
{
    (void)state;
    if (!s_savestate_fp || !buffer || length < 0) {
        s_savestate_errors++;
        return;
    }
    genesis_state_var_t var = {{0}, (uint32_t)length};
    strncpy(var.key, tag, sizeof(var.key) - 1);
    if (fwrite(&var, sizeof(var), 1, s_savestate_fp) != 1 ||
        fwrite(buffer, 1, length, s_savestate_fp) != (size_t)length) {
        s_savestate_errors++;
    }
}
void gwenesis_io_get_buttons(void) {}

static bool genesis_state_save_file(const char *path, void *ctx)
{
    (void)ctx;
    s_savestate_fp = fopen(path, "wb");
    if (!s_savestate_fp) return false;
    s_savestate_errors = 0;
    gwenesis_save_state();
    bool ok = s_savestate_errors == 0 && fflush(s_savestate_fp) == 0 &&
              fsync(fileno(s_savestate_fp)) == 0;
    fclose(s_savestate_fp);
    s_savestate_fp = NULL;
    return ok;
}

static bool genesis_state_load_file(const char *path, void *ctx)
{
    (void)ctx;
    s_savestate_fp = fopen(path, "rb");
    if (!s_savestate_fp) return false;
    s_savestate_errors = 0;
    gwenesis_load_state();
    fclose(s_savestate_fp);
    s_savestate_fp = NULL;
    bool ok = s_savestate_errors == 0;
    if (!ok) reset_emulation();
    return ok;
}

static void genesis_state_reset(bool hard, void *ctx)
{
    (void)hard;
    (void)ctx;
    reset_emulation();
}

esp_err_t genesis_emu_run(const rom_store_entry_t *entry)
{
    if (!entry || entry->system != ROM_SYSTEM_GENESIS) return ESP_ERR_INVALID_ARG;

    nes_emu_release_prealloc();
    esp_err_t err = rom_store_load(entry, 1, &s_rom);
    if (err != ESP_OK) return err;
    if (!allocate_runtime()) return ESP_ERR_NO_MEM;
    uint32_t rom_crc = esp_crc32_le(0, s_rom.data, s_rom.size);

    load_cartridge(s_rom.data, s_rom.size);
    power_on();
    reset_emulation();

    bool pal = REG1_PAL;
    int refresh = pal ? GWENESIS_REFRESH_RATE_PAL : GWENESIS_REFRESH_RATE_NTSC;
    int source_rate = pal ? GWENESIS_AUDIO_BUFFER_LENGTH_PAL * refresh
                          : GWENESIS_AUDIO_BUFFER_LENGTH_NTSC * refresh;
    gwenesis_SN76489_Init(pal ? 3546895 : 3579545,
                          source_rate, AUDIO_FREQ_DIVISOR);

    uint32_t sample_rate = source_rate / 2;
    audio_output_init(sample_rate);
    input_serial_init();
    input_usb_init();
    input_gamepad_init();
    rgb_led_init();

    const game_menu_config_t menu = {
        .system = "md",
        .rom_crc = rom_crc,
        .save_state = genesis_state_save_file,
        .load_state = genesis_state_load_file,
        .reset = genesis_state_reset,
    };

    ESP_LOGI(TAG,
             "retro-go Gwenesis 启动：%s，%s，单核，显示每 %u 帧取 1 帧，音频 %u Hz",
             entry->name, pal ? "PAL 50Hz" : "NTSC 60Hz",
             GEN_FRAME_SKIP + 1, (unsigned)sample_rate);

    int64_t frame_us = 1000000 / refresh;
    int64_t deadline = esp_timer_get_time();
    int64_t stat_at = deadline;
    unsigned frames = 0;
    unsigned presented = 0;
    unsigned changed = 0;
    unsigned frame_seq = 0;
    int64_t emu_total = 0;
    uint16_t previous_keys = UINT16_MAX;
    uint32_t previous_hash = 0;

    while (1) {
        int64_t begin = esp_timer_get_time();
        uint16_t keys = input_serial_poll() | input_gamepad_poll() | input_usb_poll();

        if ((keys & MENU_COMBO_BITS) == MENU_COMBO_BITS) {
            update_input(0);
            if (game_menu_open(&menu) == GAME_MENU_RESTART) {
                rgb_led_off();
                esp_restart();
            }
            previous_keys = UINT16_MAX;
            frames = presented = changed = 0;
            emu_total = 0;
            deadline = stat_at = esp_timer_get_time();
            continue;
        }

        if (keys != previous_keys) {
            update_input(keys);
            previous_keys = keys;
        }

        bool render = (frame_seq++ % (GEN_FRAME_SKIP + 1u)) == 0;
        run_frame(render);

        if (render) {
            genesis_frame_t *frame = &s_frames[s_frame_index];
            frame->width = screen_width;
            frame->height = screen_height;
            for (int i = 0; i < 256; i++) frame->palette[i] = swap16(CRAM565[i]);
            frame->hash = frame_hash(frame);
            if (previous_hash && frame->hash != previous_hash) changed++;
            previous_hash = frame->hash;
            display_stream_sized(genesis_strip, frame, GEN_SOURCE_W, GEN_VISIBLE_H);
            s_frame_index ^= 1;
            presented++;
        }

        int audio_len = mix_audio_downsample();
        audio_output_submit_stereo(gwenesis_ym2612_buffer, audio_len);
        emu_total += esp_timer_get_time() - begin;
        frames++;

        deadline += frame_us;
        int64_t wait_us = deadline - esp_timer_get_time();
        if (wait_us > 1000) vTaskDelay(pdMS_TO_TICKS(wait_us / 1000));
        else {
            /* 单核 retro-go 在重场景会超过 16.7 ms。即使已经落后也必须
             * 主动让出一次 CPU，让 IDLE0 喂任务看门狗。 */
            vTaskDelay(1);
            if (wait_us < -frame_us * 3) deadline = esp_timer_get_time();
        }

        int64_t now = esp_timer_get_time();
        if (now - stat_at >= 1000000) {
            int headroom = 100 - (int)(emu_total * 100 / (now - stat_at));
            ESP_LOGI(TAG,
                     "模拟 %u fps / 显示 %u fps，平均 %.1f ms/帧，CPU 余量 %d%%，画面变化 %u 次，校验 %08lx，内部空闲 %u KB",
                     frames, presented,
                     frames ? (double)emu_total / frames / 1000.0 : 0.0,
                     headroom, changed, (unsigned long)previous_hash,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            int fps_pct = presented * 100 / refresh;
            rgb_led_report_perf(fps_pct > 100 ? 100 : fps_pct);
            frames = presented = changed = 0;
            emu_total = 0;
            stat_at = now;
        }
    }
}
