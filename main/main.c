/*
 * 在 ESP32-S3 + ST7789（240x320）上运行 NES / GB / GBC / SNES / Genesis
 *
 * 流程：打印板级信息 -> 初始化屏 -> 选择 GAME/WORDS/SETTINGS -> 学习或启动模拟器
 *
 * 接线见 display.h 顶部。换屏或显示不正常时改那里的宏，不用动这个文件。
 * 把 SHOW_DISPLAY_SELFTEST 改成 1 可以在启动模拟器前先跑一遍点屏诊断图。
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display.h"
#include "audio_output.h"
#include "input_serial.h"
#include "input_usb.h"
#include "loading_screen.h"
#include "input_gamepad.h"
#include "nofrendo.h"
#include "nes_emu.h"
#include "gbc_emu.h"
#include "snes_emu.h"
#include "genesis_emu.h"
#include "rom_menu.h"
#include "overclock.h"
#include "sd_card.h"
#include "word_study.h"
#include "word_audio.h"

static const char *TAG = "main";

#define SHOW_DISPLAY_SELFTEST  0

/* TF 卡自检：在挂载之后多跑一遍「列根目录 + 写读校验」，只往串口输出。
 * ROM 现在全部从卡上读，挂载本身是必做的（rom_store_init 会调）；这个开关
 * 只控制那些额外的诊断输出，在慢卡上要多花两秒。**换卡或改接线时打开**，
 * 平时关着。见 sd_card.c 文件头。 */
#define SD_SELFTEST 0

/* 超频实验开关，见 overclock.h。0 = 不动寄存器，维持 Kconfig 里配置的 240MHz；
 * 非零走 overclock_apply()，档位范围 [-8, 8]，实测主频打印在串口日志的
 * "overclock" tag 下。先从这个值开始测，稳定的话再往上加、不稳就往下退——
 * 一次只改这一个数，配合 nes_emu.c 每秒自报的 "CPU 余量" 那行看效果。 */
#define OVERCLOCK_LEVEL 0

static void print_board_info(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);

    printf("\n========= ESP32-S3 GAMEBOX =========\n");
    printf("芯片      : ESP32-S3, %d core(s), rev %d.%d\n",
           info.cores, info.revision / 100, info.revision % 100);
    printf("Flash     : %" PRIu32 " MB\n", flash / (1024 * 1024));
    printf("PSRAM     : %u KB\n",
           (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("内部空闲  : %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("====================================\n\n");
}

#if SHOW_DISPLAY_SELFTEST
/* 点屏诊断图：换屏之后用它确认旋转 / 颜色顺序 / 反色是否设对。
 *   左上红右上绿左下蓝右下黄 -> 旋转和镜像对
 *   色条 红绿蓝黄青品白灰    -> RGB 顺序对
 *   灰阶左黑右白      -> invert 对
 *   白框四边贴满**画布**（不是面板）-> 画布落点对
 *
 * ⚠ 画的是 288x224 的画布，不是 320x240 的面板 —— 白框贴的是画布边界，
 * 外面还有 16/8 像素的黑边。要验 gap 得先把 DISP_FB_W/H 临时改成 DISP_W/H。
 * （以前这里用的是 DISP_W/DISP_H，画布比它小，右边和下边其实被裁掉了。）
 */
static void diag_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    display_clear(C_BLACK);

    static const uint16_t bars[8] = {
        C_RED, C_GREEN, C_BLUE, C_YELLOW, C_CYAN, C_MAGENTA, C_WHITE, C_GRAY,
    };
    int bar_w = DISP_FB_W / 8;
    for (int i = 0; i < 8; i++) {
        display_fill_rect(i * bar_w, 24, bar_w, 46, bars[i]);
    }

    for (int x = 0; x < DISP_FB_W; x++) {
        int v = (x * 255) / (DISP_FB_W - 1);
        display_fill_rect(x, 78, 1, 30, RGB565(v, v, v));
    }

    display_fill_rect(4, 4, 22, 14, C_RED);
    display_fill_rect(DISP_FB_W - 26, 4, 22, 14, C_GREEN);
    display_fill_rect(4, DISP_FB_H - 18, 22, 14, C_BLUE);
    display_fill_rect(DISP_FB_W - 26, DISP_FB_H - 18, 22, 14, C_YELLOW);

    display_text(8, 118, "border must touch all 4 edges", C_GRAY, 1);

    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_WHITE);
    display_rect(1, 1, DISP_FB_W - 2, DISP_FB_H - 2, C_WHITE);
    display_rect(2, 2, DISP_FB_W - 4, DISP_FB_H - 4, C_RED);
}

static void screen_diagnostic(void)
{
    display_stream_sync(diag_strip, NULL);
    vTaskDelay(pdMS_TO_TICKS(6000));
}
#endif

/* 经典 GAMEBOY DMG 绿色 4 阶（C_GB0..C_GB3，见 display.h），不是中性
 * 灰阶——参照真实一代机屏幕的浅黄绿底色。背景 C_GB0，标题/正文用最深
 * 的 C_GB3 压对比度，次要信息（副标题、分割线、平台色块）用 C_GB2，
 * 比背景深但不抢标题。平台色块以前是每个系统一个专属色、和 rom_menu.c
 * 的 system_color() 对应，改这套配色后没法再用色相区分五个系统，就
 * 统一用 C_GB2——标签文字本身已经写明系统名，颜色只是点缀不是必需信息。
 * splash_strip 和 boot_menu_strip 共用这部分（标题/副标题/平台色块/
 * 上下分割线），只有分割线下方的内容不一样，所以拆出来避免抄两份。 */
static void splash_frame_common(void)
{
    display_clear(C_GB0);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_GB2);

    display_text(81, 40, "GAMEBOX", C_GB3, 3);
    display_text(96, 70, "ESP32-S3  5-IN-1", C_GB2, 1);

    display_hline(24, 94, DISP_FB_W - 48, C_GB2);

    static const char *systems[] = { "[NES]", "[SNES]", "[GB]", "[GBC]", "[MD]" };
    int x = 64;
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++) {
        display_text(x, 112, systems[i], C_GB2, 1);
        x += (int)strlen(systems[i]) * 6 + 4;
    }

    display_hline(24, 136, DISP_FB_W - 48, C_GB2);
}

static void splash_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    splash_frame_common();
    display_text(114, 176, "loading...", C_GB2, 1);
}

static void splash(void)
{
    display_stream_sync(splash_strip, NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

/* loading 那 1.5 秒过完之后，开机画面停下来问 GAME/WORDS/SETTINGS，不再自动往下走——
 * 之前是只要 PAD_DIAG_SCREEN=1（编译期开关）就每次开机都强制看一遍摇杆
 * 诊断画面，想跳过看不了。现在交给玩家自己选：GAME 直接进 ROM 菜单，
 * SETTINGS 统一放音量、亮度和 input_gamepad_show() 那套摇杆/按键测试。 */
#define BOOT_MENU_POLL_MS 16   /* 和 rom_menu.c 的 POLL_MS 同一个量级 */

typedef enum {
    BOOT_MODE_GAME,
    BOOT_MODE_WORDS,
    BOOT_MODE_SETTINGS,
    BOOT_MODE_COUNT,
} boot_mode_t;

static void boot_text_center_ascii(int y, const char *text, uint16_t color, int scale)
{
    int width = (int)strlen(text) * 6 * scale;
    display_text((DISP_FB_W - width) / 2, y, text, color, scale);
}

static void boot_draw_icon(boot_mode_t mode, int cx, int y, uint16_t color)
{
    if (mode == BOOT_MODE_GAME) {
        /* 小手柄：十字键和两颗面键比文字更快让孩子认出“游戏”。 */
        display_rect(cx - 21, y + 3, 42, 23, color);
        display_fill_rect(cx - 14, y + 12, 13, 3, color);
        display_fill_rect(cx - 9, y + 7, 3, 13, color);
        display_fill_rect(cx + 7, y + 9, 4, 4, color);
        display_fill_rect(cx + 13, y + 15, 4, 4, color);
    } else if (mode == BOOT_MODE_WORDS) {
        /* 打开的书，中缝留一列底色，缩到 86px 卡片里仍然看得清。 */
        display_rect(cx - 21, y + 2, 20, 25, color);
        display_rect(cx + 1, y + 2, 20, 25, color);
        display_fill_rect(cx - 16, y + 8, 11, 2, color);
        display_fill_rect(cx + 5, y + 8, 11, 2, color);
        display_fill_rect(cx - 16, y + 14, 11, 2, color);
        display_fill_rect(cx + 5, y + 14, 11, 2, color);
    } else {
        /* 三条滑杆比齿轮在 42x29 的小区域里更清楚，也直接对应设置页内容。 */
        display_hline(cx - 18, y + 7, 36, color);
        display_hline(cx - 18, y + 15, 36, color);
        display_hline(cx - 18, y + 23, 36, color);
        display_fill_rect(cx - 8, y + 4, 4, 7, color);
        display_fill_rect(cx + 7, y + 12, 4, 7, color);
        display_fill_rect(cx - 2, y + 20, 4, 7, color);
    }
}

static void boot_menu_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    const int *selected = ctx;
    static const char *labels[BOOT_MODE_COUNT] = { "GAME", "WORDS", "SETTINGS" };
    static const char *descriptions[BOOT_MODE_COUNT] = {
        "选择并启动游戏", "按教材学习单词", "声音 亮度 手柄测试"
    };

    display_clear(C_GB0);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_GB2);
    boot_text_center_ascii(9, "GAMEBOX", C_GB3, 3);
    boot_text_center_ascii(38, "PLAY  LEARN  EXPLORE", C_GB2, 1);
    display_hline(24, 55, DISP_FB_W - 48, C_GB2);
    int choose_w = display_text_width_16("选择模式");
    display_text_16((DISP_FB_W - choose_w) / 2, 64, "选择模式", C_GB3);

    for (int i = 0; i < BOOT_MODE_COUNT; i++) {
        const int card_w = 86;
        const int card_h = 68;
        int x = 8 + i * 93;
        int cx = x + card_w / 2;
        bool active = i == *selected;
        uint16_t color = active ? C_GB0 : C_GB2;

        if (active) display_fill_rect(x, 87, card_w, card_h, C_GB2);
        display_rect(x, 87, card_w, card_h, active ? C_GB3 : C_GB2);
        boot_draw_icon((boot_mode_t)i, cx, 94, color);

        if (i == BOOT_MODE_SETTINGS) {
            int label_w = display_text_width_16(labels[i]);
            display_text_16(cx - label_w / 2, 130, labels[i], color);
        } else {
            int label_w = (int)strlen(labels[i]) * 6 * 2;
            display_text(cx - label_w / 2, 132, labels[i], color, 2);
        }
    }

    int desc_w = display_text_width_16(descriptions[*selected]);
    display_text_16((DISP_FB_W - desc_w) / 2, 166,
                    descriptions[*selected], C_GB3);
    for (int i = 0; i < BOOT_MODE_COUNT; i++) {
        display_fill_rect(124 + i * 16, 188, 8, 4,
                          i == *selected ? C_GB3 : C_GB1);
    }
    int footer_w = display_text_width_16("左右选择  A确认");
    display_text_16((DISP_FB_W - footer_w) / 2, 204,
                    "左右选择  A确认", C_GB2);
}

/* 独立设置页沿用 retro-go 的 Options 结构：纯色面板、居中标题、反选行；
 * 颜色统一复用主页和 ROM 菜单的四阶 Game Boy 绿色。设置仍只对本次开机
 * 有效，避免孩子不小心静音后每次上电都以为机器坏了。 */
#define SETTINGS_COUNT       3
#define SETTINGS_BRIGHTNESS  0
#define SETTINGS_VOLUME      1
#define SETTINGS_TEST        2

static void settings_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    int selected = *(const int *)ctx;

    display_clear(C_GB0);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_GB2);

    const int box_x = 27;
    const int box_y = 34;
    const int box_w = 234;
    const int box_h = 130;
    const int row_x = box_x + 9;
    const int row_w = box_w - 18;
    const int row_y = box_y + 39;
    const int row_h = 24;

    display_fill_rect(box_x, box_y, box_w, box_h, C_GB1);
    display_rect(box_x, box_y, box_w, box_h, C_GB3);

    const char *title = "Options";
    display_text_16(box_x + (box_w - display_text_width_16(title)) / 2,
                    box_y + 9, title, C_GB3);

    char rows[SETTINGS_COUNT][28];
    snprintf(rows[SETTINGS_BRIGHTNESS], sizeof(rows[0]),
             "Brightness: %3d%%", display_get_backlight());
    snprintf(rows[SETTINGS_VOLUME], sizeof(rows[0]),
             "Volume    : %3d%%", audio_output_get_volume());
    snprintf(rows[SETTINGS_TEST], sizeof(rows[0]), "Controller Test");

    for (int i = 0; i < SETTINGS_COUNT; i++) {
        int y = row_y + i * row_h;
        uint16_t fg = C_GB2;
        if (i == selected) {
            display_fill_rect(row_x, y - 2, row_w, row_h - 2, C_GB2);
            fg = C_GB0;
        }
        display_text_16(row_x + 6, y, rows[i], fg);
    }

    int hint1_w = display_text_width_16("上下选择  左右调整");
    display_text_16((DISP_FB_W - hint1_w) / 2, 178,
                    "上下选择  左右调整", C_GB3);
    int hint2_w = display_text_width_16("A进入测试  B返回");
    display_text_16((DISP_FB_W - hint2_w) / 2, 202,
                    "A进入测试  B返回", C_GB2);
}

static void settings_menu(bool *refresh_rom_index)
{
    int selected = SETTINGS_VOLUME;
    bool volume_preview_active = false;
    display_stream_sync(settings_strip, &selected);

    uint16_t prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_MENU_POLL_MS));
        uint16_t now = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        uint16_t edge = now & ~prev;
        prev = now;
        bool dirty = false;

        if (edge & NES_PAD_B) {
            if (volume_preview_active) word_audio_shutdown();
            return;
        }

        if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
            int delta = (edge & NES_PAD_UP) ? -1 : 1;
            selected = (selected + delta + SETTINGS_COUNT) % SETTINGS_COUNT;
            dirty = true;
        } else if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
            int delta = (edge & NES_PAD_LEFT) ? -1 : 1;
            if (selected == SETTINGS_VOLUME) {
                /* 每档 10%，并立即用真实教材语音试听。以前 5% 线性振幅既没有
                 * 试听，档间又只有约 0.8 dB，听起来就像设置没传给 WORDS。 */
                int volume = audio_output_get_volume() + delta * 10;
                if (volume < 0) volume = 0;
                if (volume > 100) volume = 100;
                audio_output_set_volume(volume);
                if (volume_preview_active || (volume > 0 && word_audio_init())) {
                    volume_preview_active = true;
                    word_audio_play("hello");
                }
                dirty = true;
            } else if (selected == SETTINGS_BRIGHTNESS) {
                int backlight = display_get_backlight() + delta * 10;
                if (backlight < 5) backlight = 5;
                if (backlight > 100) backlight = 100;
                display_backlight(backlight);
                dirty = true;
            }
        } else if (selected == SETTINGS_TEST &&
                   (edge & (NES_PAD_A | NES_PAD_START))) {
            /* TEST 原来在主页；只在真正进入诊断时才碰 ROM 目录，WORDS 和普通
             * 设置路径仍不会承担 SD 全盘扫描。 */
            if (volume_preview_active) {
                word_audio_shutdown();
                volume_preview_active = false;
            }
            rom_store_init(*refresh_rom_index);
            *refresh_rom_index = false;
            input_gamepad_show();
            display_stream_sync(settings_strip, &selected);
            prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        }

        if (dirty) display_stream_sync(settings_strip, &selected);
    }
}

static boot_mode_t boot_menu(void)
{
    int selected = 0;
    display_stream_sync(boot_menu_strip, &selected);

    uint16_t prev = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_MENU_POLL_MS));
        uint16_t now = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
        uint16_t edge = now & ~prev;
        prev = now;

        if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
            int delta = (edge & NES_PAD_LEFT) ? -1 : 1;
            selected = (selected + delta + BOOT_MODE_COUNT) % BOOT_MODE_COUNT;
            display_stream_sync(boot_menu_strip, &selected);
        }
        if (edge & (NES_PAD_A | NES_PAD_START)) {
            return (boot_mode_t)selected;
        }
    }
}

void app_main(void)
{
#if OVERCLOCK_LEVEL != 0
    overclock_apply(OVERCLOCK_LEVEL);
#endif

    print_board_info();

    /* 必须先于 display_init()：NES 视频缓冲要 65 KB 连续内部内存，
     * 等两块帧缓冲分配完就凑不出来了。 */
    if (nes_emu_prealloc() != ESP_OK) {
        ESP_LOGE(TAG, "NES 视频缓冲分配失败，内部 RAM 不够");
        return;
    }

    /* 挂卡放在 prealloc 之后：那两块 64 KB 连续内部内存先占住，再让 FATFS
     * 去要它的工作缓冲，免得把内存碎片化连累到 NES 视频缓冲。
     * 挂不上不是致命错误——没卡时下面回退到编译期嵌入的 ROM。 */
#if SD_SELFTEST
    sd_card_selftest();
#else
    sd_card_mount();
#endif

    /* 声音开关只在当前运行中有效；每次启动都先恢复默认开启。 */
    audio_output_settings_init();

    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "屏幕初始化失败，检查接线和 display.h 里的引脚定义");
        return;
    }

#if SHOW_DISPLAY_SELFTEST
    screen_diagnostic();
#endif
    splash();

    /* boot_menu() 要读输入，所以三路输入源在这里先装好；rom_menu_pick()
     * 里还会再调一遍，都是幂等的，不会重复初始化出问题。
     *
     * WORDS 完全离线，不应该为了学单词先等一次 ROM 全盘扫描。因此先选模式，
     * 只有 GAME 或 SETTINGS 里真正进入 Controller Test 时才初始化 ROM 目录。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();
    uint16_t boot_keys = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    bool refresh_rom_index = (boot_keys & NES_PAD_SELECT) != 0;
    if (refresh_rom_index) {
        ESP_LOGI(TAG, "检测到 SELECT，忽略 ROM 目录缓存并完整重扫");
    }
    /* 开机选单只返回目录项；各模拟器在自己的大块内存准备妥当后再从卡上读，
     * SNES 尤其不能先读出 4 MiB 再复制一份，否则 8 MiB PSRAM 会在峰值时耗尽。
     * 卡不可用时 entry 留 NULL，NES 继续走编译期嵌入 ROM 的回退路径。 */
    const rom_store_entry_t *entry = NULL;
    uint16_t launch_keys = 0;
    while (1) {
        boot_mode_t boot_mode = boot_menu();
        if (boot_mode == BOOT_MODE_WORDS) {
            word_study_run();
            continue;
        }
        if (boot_mode == BOOT_MODE_SETTINGS) {
            settings_menu(&refresh_rom_index);
            continue;
        }

        rom_store_init(refresh_rom_index);
        refresh_rom_index = false;  /* 同一次开机只强制重扫一次，返回菜单不再重扫 */

        rom_menu_result_t menu_result = rom_menu_pick(&entry, &launch_keys);
        if (menu_result == ROM_MENU_BACK) continue;
        break;  /* 已选游戏，或目录不可用而回退到内置 NES */
    }

    /* ZIP 为了开机快只登记外层文件名，到玩家真正选择时才读一次内部目录。 */
    rom_store_entry_t resolved;
    const rom_store_entry_t *run_entry = entry;
    if (entry) {
        loading_screen_begin(entry->name);
        rom_store_set_progress_callback(loading_screen_progress);
    }
    if (entry && rom_store_resolve(entry, &resolved) != ESP_OK) {
        ESP_LOGE(TAG, "%s 不是可用的 ROM ZIP，1.5 秒后返回菜单", entry->name);
        loading_screen_error("ZIP 不可用");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    if (entry) run_entry = &resolved;

    rom_system_t system = run_entry ? run_entry->system : ROM_SYSTEM_NES;
    esp_err_t run_err = system == ROM_SYSTEM_NES     ? nes_emu_run(run_entry)
                      : system == ROM_SYSTEM_SNES    ? snes_emu_run(run_entry, launch_keys)
                      : system == ROM_SYSTEM_GENESIS ? genesis_emu_run(run_entry)
                                                     : gbc_emu_run(run_entry);
    if (run_err != ESP_OK) {
        ESP_LOGE(TAG, "模拟器启动失败");
        loading_screen_error("游戏启动失败");
    }
}
