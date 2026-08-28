/*
 * PC Engine / TurboGrafx-16 模拟器适配层（上游 pce-go，见 components/pce-go）
 *
 * pce-go 的宿主接口只有三个回调，比另外四个核心都干净：
 *
 *   osd_gfx_framebuffer(w, h)  每帧要画之前问宿主要一块 8 位索引缓冲
 *   osd_input_read(joypads[8]) 每帧问一次手柄
 *   osd_vsync()                一帧画完，宿主负责推屏、出声、和节流
 *
 * 输出格式和 NES 是同构的：**8 位调色板索引 + 256 项 RGB565 调色板**，
 * 所以整条路径直接复用 nes_emu.c 那套 —— 核 0 写索引缓冲，核 1 在条带回调
 * 里查表转成大端 RGB565。不在核 0 上转 RGB565 是关键：那等于把每帧
 * 320x240 的查表工作从空闲的核 1 挪到本来就吃紧的核 0 上。
 *
 * ---- 帧缓冲的形状（照抄不得想当然）----
 *
 * pce-go 要求缓冲每行左右各留 16 字节余量给出屏精灵写，行距固定
 * XBUF_WIDTH(368)，而**交给它的指针要指到 base + 16**（gfx.c 的
 * render_lines() 会自己算 `framebuffer_top = screen_buffer - 16`）。
 * 可见宽度因游戏而异（PCE 支持 256/336/512 三种），每帧从
 * osd_gfx_framebuffer() 的入参拿到，不是常量。
 *
 * 多分配 32 字节：gfx.c 的下界是 `screen_buffer + height * XBUF_WIDTH`，
 * 算上开头那 16 字节偏移，正好比 XBUF_WIDTH*XBUF_HEIGHT 多出 16 字节。
 * 上游自己也是这么擦边的（retro-go 那边分配的就是刚好的大小），这里花
 * 32 字节把这个边界买断，省得哪天某个游戏用满 242 行时踩到堆头。
 *
 * ---- 一块内部 + 一块 PSRAM 镜像，不是两块内部 ----
 *
 * gfx.c 逐扫描线读写的那块必须在内部 SRAM：放 PSRAM 会像当年 NES 那样
 * 把渲染从 2 ms 拖到 8.5 ms（见 AGENTS.md）。
 *
 * 但**两块 87 KB 内部缓冲在这块板子上凑不出来**，实测：分配掉第一块之后
 * 内部还剩 129 KB，最大连续块却只有 40 KB —— 长期存活的分配（推屏条带、
 * 任务栈、SD/I2S 驱动）把 230 KB 那个区切碎了。先分配再读 ROM 也没用，
 * 试过。
 *
 * 所以只留一块内部缓冲给 gfx.c 画，另在 PSRAM 开一块同样大的镜像：每帧
 * 画完 memcpy 过去再交给核 1，核 0 立刻接着画下一帧。代价是一次 87 KB
 * 的 memcpy，换回来的是双核重新并行（否则核 0 得干等核 1 推完那 17 ms）。
 * 镜像只被核 1 顺序读一遍，PSRAM 对顺序访问是够的。
 * 只复制真正用到的行（16 + h*XBUF_WIDTH），h 通常是 239 不是 242。
 *
 * 连 PSRAM 镜像都开不出来时退回单缓冲 + 同步推屏：能跑，帧率大约减半。
 * 走没走这条退路看串口 `pce` tag 的启动行。
 */

#include <string.h>
#include "pce_emu.h"
#include "audio_output.h"
#include "display.h"
#include "game_menu.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "input_usb.h"
#include "nes_emu.h"
#include "rgb_led.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pce-go.h"
#include "psg.h"

/* pce-go.h 在没有 RETRO_GO 时会把 crc32_le() 定义成常量 0（那是给不带
 * 框架的宿主用的兜底）。我们这边用的是 esp_rom_crc32_le，名字不冲突，
 * 但 IDF 的某些 rom 头文件里确实有个真的 crc32_le —— 拆掉这个宏，避免
 * 以后谁在这个文件里 include 到那些头时出现无声的错误结果。 */
#undef crc32_le

static const char *TAG = "pce";

#define PCE_FB_BYTES        (XBUF_WIDTH * XBUF_HEIGHT + 32)
#define PCE_FRAME_PERIOD_US 16667      /* PCE 是 60 Hz */

/* 24000 / 60 = 400 个立体声帧。psg.c 里那句注释说得很明白：DDA（采样
 * 回放）通道并不跟踪每个采样该播多久，靠的是「一帧里被调用足够多次」
 * 来近似，上游按每帧约 10 次调。所以这里不能图省事一帧只调一次大的，
 * 拆成 8 段填进同一个缓冲，最后一次性提交给 I2S。
 *
 * 另一个必须拆的理由：psg_update() 内部是 `sample_t mix_buffer[length+1]`
 * 这样的**栈上变长数组**，length 越大越吃栈。 */
#define PCE_AUDIO_FRAMES     (AUDIO_OUTPUT_SAMPLE_RATE / 60)
#define PCE_AUDIO_CHUNKS     8
#define PCE_AUDIO_CHUNK      (PCE_AUDIO_FRAMES / PCE_AUDIO_CHUNKS)

/* SELECT+X 打开游戏内菜单，和 GB/GBC 一致。X 是宿主公共高位，
 * 不占 PCE 原机按键（原机只有 I/II/SELECT/RUN）。 */
#define MENU_COMBO_BITS      (GAMEPAD_BIT_SELECT | GAMEPAD_BIT_X)

static uint16_t  s_palette[256];   /* 8 位索引 -> RGB565（大端，见 display.h） */

/* 可见尺寸随每块缓冲一起走，不做成全局变量：PCE 会在运行中切分辨率
 * （256/336/512），而核 1 读这一帧的像素时，核 0 已经在给下一帧要缓冲了。
 * 尺寸放全局的话，切分辨率那一帧核 1 会拿着新尺寸去解释旧像素，画面会撕
 * 成斜的。绑进 ctx 之后，核 1 看到的永远是这块像素自己的尺寸。 */
typedef struct {
    uint8_t *pixels;   /* 已经偏移过左侧 16 字节余量 */
    int      w, h;
} pce_frame_t;

static uint8_t     *s_fb;        /* 内部 SRAM，gfx.c 画这块 */
static uint8_t     *s_mirror;    /* PSRAM 镜像，核 1 读这块 */
static pce_frame_t  s_draw;      /* 指向 s_fb 的可见区 */
static pce_frame_t  s_shown;     /* 指向 s_mirror 的可见区 */
static bool         s_mirrored;

/* 跳帧：本帧只跑 CPU/PSG，不画图、不推屏。
 *
 * ⚠ 这条不是为了画面流畅，是**音频的硬约束**：osd_vsync() 每次固定产
 * PCE_AUDIO_FRAMES 个采样，而 I2S 固定按 AUDIO_OUTPUT_SAMPLE_RATE 消费，
 * 两者对得上的唯一条件是每秒正好调 60 次 vsync。渲染满帧时实测只有
 * 38~41 次，等于每秒少喂 1/3 采样，队列反复见底，听感就是持续破音。
 * 所以宁可少画几帧，也要让模拟保持 60 Hz。上游 retro-go 跑 PCE 同样是
 * 默认 frameskip=1。
 *
 * 做成自适应而不是固定隔帧跳：画面轻的场景仍然满帧。
 *
 * 裸 CPU 模拟约 13.9 ms（出厂主频），已经吃掉 16.67 ms 预算的 83%，而渲染
 * 一帧还要约 9.6 ms —— 最多只能画约 29% 的帧，实画 12~18 fps。开了
 * OVERCLOCK_LEVEL=4 之后是 20~28 fps（重场景）/ 28~36（轻场景）。
 *
 * ⚠ 试过把 h6280 解释器用 IDF linker fragment 放进 IRAM（不改上游源码，
 * 按目标文件重定位）：裸模拟降到约 12.0 ms，实画帧翻到 23~31 fps。但它要
 * 吃 30 KB 片内 SRAM，而 IRAM 和 DRAM 共用片内那块，**实测把 Genesis 打崩
 * 了**（Golden Axe 启动即 StoreProhibited；撤掉就好，单变量对照过）。
 * 所以没有启用。将来真要捡这 10 帧，得先给 gwenesis 腾出这 30 KB。 */
static bool         s_skip_frame;

static int16_t   s_audio[PCE_AUDIO_FRAMES * 2];
static int64_t   s_next_frame_us;
static uint32_t  s_rom_crc;

/* 条带回调 —— 跑在**核 1** 上。把 8 位索引缓冲的可见区域最近邻放大到
 * 整块 320x240 面板，顺便查表转成大端 RGB565。和 nes_strip/gbc_strip
 * 同一套算法，没有插值。 */
static void pce_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const pce_frame_t *frame = ctx;
    const uint8_t  *base = frame->pixels;
    const uint16_t *pal  = s_palette;
    const int src_w = frame->w;
    const int src_h = frame->h;

    for (int r = 0; r < h; r++) {
        int src_y = ((y0 + r) * src_h) / DISP_H;
        const uint8_t *src = base + (size_t)src_y * XBUF_WIDTH;
        uint16_t *dst = strip + (size_t)r * DISP_W;

        /* 横向比例逐帧可变（256/336/512），只能按像素算源列。用定点步进
         * 而不是每列一次除法：除法在这条内循环上是实打实的开销。 */
        uint32_t step = ((uint32_t)src_w << 16) / DISP_W;
        uint32_t pos  = 0;
        for (int x = 0; x < DISP_W; x++) {
            dst[x] = pal[src[pos >> 16]];
            pos += step;
        }
    }
}

static void black_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);
}

static bool pce_state_save_file(const char *path, void *ctx)
{
    (void)ctx;
    return SaveState(path) == 0;
}

static bool pce_state_load_file(const char *path, void *ctx)
{
    (void)ctx;
    if (LoadState(path) != 0) {
        ResetPCE(false);
        return false;
    }
    return true;
}

static void pce_state_reset(bool hard, void *ctx)
{
    (void)ctx;
    ResetPCE(hard);
}

/* ============ pce-go 要求宿主提供的三个回调 ============ */

uint8_t *osd_gfx_framebuffer(int width, int height)
{
    /* gfx.c 的 render_lines() 一帧里会按光栅推进调用多次，每次都问一遍
     * 尺寸；始终是同一块内部缓冲，所以这里直接覆盖即可。尺寸要照常记，
     * 跳帧只是不画，模式该变还是变。 */
    if (width > 0 && height > 0) {
        /* PCE 的可见宽度是 VDC 的 HDR 寄存器现编的，游戏运行中会改（标题
         * 画面和关卡内常常不是一个模式）。只在变化时打一行，别每帧刷屏。 */
        if (width != s_draw.w || height != s_draw.h) {
            ESP_LOGI(TAG, "画面模式 %dx%d -> 面板 %dx%d（%s）",
                     width, height, DISP_W, DISP_H,
                     width == DISP_W ? "横向 1:1" : "横向缩放");
        }
        s_draw.w = width;
        s_draw.h = height;
    }
    /* 返回 NULL，gfx.c 的 render_lines() 立刻 return，整帧的图块 + 精灵
     * 绘制全部省掉；h6280 和 PSG 不受影响，照常推进。 */
    return s_skip_frame ? NULL : s_draw.pixels;
}

void osd_input_read(uint8_t joypads[8])
{
    uint16_t pad = input_serial_poll() | input_gamepad_poll() | input_usb_poll();

    if ((pad & MENU_COMBO_BITS) == MENU_COMBO_BITS) {
        const game_menu_config_t config = {
            .system     = "pce",
            .rom_crc    = s_rom_crc,
            .save_state = pce_state_save_file,
            .load_state = pce_state_load_file,
            .reset      = pce_state_reset,
        };
        audio_output_flush();
        if (game_menu_open(&config) == GAME_MENU_RESTART) {
            rgb_led_off();
            esp_restart();
        }
        /* 菜单期间时间在走，不补这一下的话回到游戏会连着快进好几帧。 */
        s_next_frame_us = esp_timer_get_time();
    }

    /* ⚠ 不能把 pad 的低 8 位直接当 joypads[0]：PCE 的方向位序是
     * UP/RIGHT/DOWN/LEFT（0x10/0x20/0x40/0x80），宿主这边是
     * UP/DOWN/LEFT/RIGHT，上下之外的两位正好换了位置。 */
    uint8_t buttons = 0;
    if (pad & GAMEPAD_BIT_LEFT)   buttons |= JOY_LEFT;
    if (pad & GAMEPAD_BIT_RIGHT)  buttons |= JOY_RIGHT;
    if (pad & GAMEPAD_BIT_UP)     buttons |= JOY_UP;
    if (pad & GAMEPAD_BIT_DOWN)   buttons |= JOY_DOWN;
    /* PCE 的 I / II 在实机上是右手两颗，位置和 NES 的 A/B 一致。 */
    if (pad & GAMEPAD_BIT_A)      buttons |= JOY_A;
    if (pad & GAMEPAD_BIT_B)      buttons |= JOY_B;
    if (pad & GAMEPAD_BIT_START)  buttons |= JOY_RUN;
    if (pad & GAMEPAD_BIT_SELECT) buttons |= JOY_SELECT;

    joypads[0] = buttons;
}

void osd_vsync(void)
{
    if (s_skip_frame) {
        /* 这一帧没画，屏幕继续显示上一次推上去的内容。 */
    } else if (s_mirrored) {
        /* ⚠ 必须先等核 1 把上一帧的镜像读完再往里 memcpy，否则会改到正在
         * 推的那一帧。这个等待通常是 0：核 1 那 17 ms 早就跑在核 0 这一帧
         * 的模拟时间里了。 */
        display_wait_idle();
        memcpy(s_mirror, s_fb, (size_t)16 + (size_t)s_draw.h * XBUF_WIDTH);
        s_shown.w = s_draw.w;
        s_shown.h = s_draw.h;
        display_stream_sized(pce_strip, &s_shown, DISP_W, DISP_H);
    } else {
        /* 只有一块缓冲，必须等核 1 读完才能继续画，否则会画在正在推的帧上。
         * display_stream_sync() 只认默认画布，所以这里手工拼出 sized 版本
         * ——它自己也就是 stream + wait_idle 两句。 */
        display_stream_sized(pce_strip, &s_draw, DISP_W, DISP_H);
        display_wait_idle();
    }

    /* 音频：分段调用保证 DDA 通道的近似精度，见 PCE_AUDIO_CHUNKS 注释。 */
    for (int i = 0; i < PCE_AUDIO_CHUNKS; i++) {
        psg_update(s_audio + (size_t)i * PCE_AUDIO_CHUNK * 2,
                   PCE_AUDIO_CHUNK, 0xFF);
    }
    audio_output_submit_stereo(s_audio, PCE_AUDIO_FRAMES);

    /* 每秒自报一次，格式对齐 nes_emu.c 那行。emu_us 量的是「上一次 vsync
     * 返回到这一次进来」的时间，也就是核 0 真正花在模拟上的部分；单缓冲时
     * 它还包含等核 1 推完的阻塞，两者要分开看。 */
    static int      frames, skipped;
    static int64_t  window_us, emu_us, last_exit_us;
    if (s_skip_frame) skipped++;
    if (last_exit_us) emu_us += esp_timer_get_time() - last_exit_us;
    if (++frames >= 60) {
        int64_t now_us = esp_timer_get_time();
        if (window_us) {
            int64_t span = now_us - window_us;
            ESP_LOGI(TAG, "PCE %d Hz（画 %d 帧，模拟 %.1f ms/帧，%s）",
                     (int)((int64_t)frames * 1000000 / span),
                     frames - skipped,
                     (double)emu_us / frames / 1000.0,
                     s_mirrored ? "PSRAM 镜像" : "单缓冲");
        }
        window_us = now_us;
        frames = 0;
        skipped = 0;
        emu_us = 0;
    }

    /* 节流到 60 Hz。display_stream() 只保证不超过屏幕吃得下的速度，
     * 那大约是 66 fps，不等于 PCE 的 60。落后太多就直接对表，
     * 免得欠下的时间越滚越多、之后连着快进。 */
    s_next_frame_us += PCE_FRAME_PERIOD_US;
    int64_t now = esp_timer_get_time();
    int64_t sleep_us = s_next_frame_us - now;
    /* 落后就跳下一帧的渲染。只看「这一帧结束时是否已经欠时间」，不做更复杂
     * 的预测：连续跳两帧的观感比偶尔慢一帧差得多，而这里每跳一帧就会把时间
     * 追回来一截，自然形成「满帧 / 跳帧」交替。 */
    s_skip_frame = (sleep_us < 0);

    if (sleep_us < -PCE_FRAME_PERIOD_US * 4) {
        s_next_frame_us = now;
    }
    if (sleep_us > 1000) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(sleep_us / 1000)));
    } else {
        /* 跑不满时也要喂核 0 的 idle task/watchdog —— 和 nes/gbc/genesis
         * 三个核心同一条。漏了这句的症状实测过：PSRAM 镜像上线后帧时间
         * 26 ms、永远追不上 60 Hz，于是一次都不进上面那个分支，跑十几秒
         * 就刷 `task_wdt: IDLE0 (CPU 0)`。 */
        vTaskDelay(1);
    }
    last_exit_us = esp_timer_get_time();
}

/* ============ 宿主侧启动 ============ */

static esp_err_t build_palette(void)
{
    uint16_t *pal = PalettePCE(16);
    if (!pal) return ESP_ERR_NO_MEM;

    /* PalettePCE(16) 给的是主机字节序 RGB565，而帧缓冲里要存大端
     * （见 display.h 的 RGB565 说明），所以逐项换字节。 */
    for (int i = 0; i < 256; i++) {
        s_palette[i] = (uint16_t)((pal[i] << 8) | (pal[i] >> 8));
    }
    free(pal);
    return ESP_OK;
}

static void free_framebuffers(void)
{
    heap_caps_free(s_fb);     s_fb = NULL;
    heap_caps_free(s_mirror); s_mirror = NULL;
}

static esp_err_t alloc_framebuffers(void)
{
    /* 画的那块必须内部 SRAM，没得商量。 */
    s_fb = heap_caps_calloc(1, PCE_FB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) return ESP_ERR_NO_MEM;

    /* 可见区从 base+16 开始（见文件头）。尺寸先按最常见的 256x239 兜底，
     * gfx.c 第一次调 osd_gfx_framebuffer() 就会改成真值。 */
    s_draw.pixels = s_fb + 16;
    s_draw.w = 256;
    s_draw.h = 239;

    s_mirror = heap_caps_calloc(1, PCE_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mirror) {
        s_shown.pixels = s_mirror + 16;
        s_shown.w = s_draw.w;
        s_shown.h = s_draw.h;
        s_mirrored = true;
        return ESP_OK;
    }

    s_mirrored = false;
    ESP_LOGW(TAG, "PSRAM 镜像开不出来，退回单缓冲 + 同步推屏，帧率大约减半");
    return ESP_OK;
}

esp_err_t pce_emu_run(const rom_store_entry_t *entry)
{
    if (!entry) {
        ESP_LOGE(TAG, "PC Engine 没有编译期内置 ROM，必须从 TF 卡选游戏");
        return ESP_ERR_INVALID_ARG;
    }

    /* NES 预留的两块 64 KB 内部缓冲这时候还占着，先交还，
     * 否则下面两块 87 KB 帧缓冲凑不出来。 */
    nes_emu_release_prealloc();

    /* ⚠ 帧缓冲必须在读 ROM **之前**分配。第一版反过来，结果只凑得出一块：
     * rom_store_load() 会先要一块最大 64 KB 的内部中转缓冲，用完虽然还回来
     * 了，但堆已经被搅出缝，再要两块连续 87 KB 就失败，白白退回单缓冲。
     * 先分配则是在最干净的堆上要，两块都拿得到。 */
    esp_err_t err = alloc_framebuffers();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "帧缓冲分配失败，内部 RAM 只剩 %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        return err;
    }

    /* 顺手把其中一块借给读盘当 DMA 中转 —— 这时候 gfx.c 还没开始画，
     * 87 KB 内部 RAM 白放着。和 nes_emu.c 借 vidbuf 是同一个手法：
     * 中转块越大，高延迟 TF 卡上的读命令次数越少。 */
    uint8_t *scratch = s_fb;

    rom_store_image_t image = {0};
    err = rom_store_load_with_scratch(entry, 0, &image, scratch, PCE_FB_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 %s 失败：%s", entry->name, esp_err_to_name(err));
        free_framebuffers();
        return err;
    }
    /* LoadCard() 之后 image.data 的所有权就归 pce-go 了，CRC 得在那之前算。 */
    s_rom_crc = esp_rom_crc32_le(0, image.data, image.size);

    /* 借完还回来：上面那块马上就要给 gfx.c 画了，必须是干净的。 */
    memset(scratch, 0, PCE_FB_BYTES);
    display_stream_sync(black_strip, NULL);

    if (InitPCE(AUDIO_OUTPUT_SAMPLE_RATE, true) != 0) {
        ESP_LOGE(TAG, "InitPCE 失败");
        rom_store_image_release(&image);
        free_framebuffers();
        return ESP_FAIL;
    }

    /* ⚠ LoadCard() 的注释写明「takes ownership of data」：成功之后这块
     * PSRAM 归 pce-go 管（它自己 free），宿主再调 rom_store_image_release()
     * 就是二次释放。所以只有失败路径才释放 —— 查过上游实现，它唯一的失败
     * 返回在 `PCE.ROM = data` **之前**（只判 NULL 和大小范围），拿到所有权
     * 之后不再有失败分支，所以失败路径上释放是安全的。 */
    if (LoadCard(image.data, image.size) != 0) {
        ESP_LOGE(TAG, "%s 不是可用的 PC Engine ROM", entry->name);
        rom_store_image_release(&image);
        ShutdownPCE();
        free_framebuffers();
        return ESP_ERR_INVALID_ARG;
    }
    image.data = NULL;
    image.size = 0;

    if (build_palette() != ESP_OK) {
        ESP_LOGE(TAG, "调色板构建失败");
        ShutdownPCE();
        free_framebuffers();
        return ESP_ERR_NO_MEM;
    }

    if (audio_output_init(AUDIO_OUTPUT_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGW(TAG, "音频没起来，静音继续");
    }

    ESP_LOGI(TAG, "PC Engine 启动：%s，ROM CRC %08lx，%s，内部 RAM 余 %u KB",
             entry->name, (unsigned long)s_rom_crc,
             s_mirrored ? "PSRAM 镜像" : "单缓冲",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    s_next_frame_us = esp_timer_get_time();
    RunPCE();   /* 正常情况下不返回 */

    ESP_LOGE(TAG, "RunPCE 意外返回");
    return ESP_FAIL;
}
