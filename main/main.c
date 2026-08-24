/*
 * 在 ESP32-S3 + ST7789（240x320）上运行 NES / GB / GBC / SNES / Genesis
 *
 * 流程：打印板级信息 -> 初始化屏 -> 选择 GAME/WORDS/TEST -> 学习或启动模拟器
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

/* loading 那 1.5 秒过完之后，开机画面停下来问 GAME/WORDS/TEST，不再自动往下走——
 * 之前是只要 PAD_DIAG_SCREEN=1（编译期开关）就每次开机都强制看一遍摇杆
 * 诊断画面，想跳过看不了。现在交给玩家自己选：GAME 直接进 ROM 菜单，
 * TEST 先看一遍 input_gamepad_show() 那套摇杆/按键可视化。 */
#define BOOT_MENU_POLL_MS 16   /* 和 rom_menu.c 的 POLL_MS 同一个量级 */

typedef enum {
    BOOT_MODE_GAME,
    BOOT_MODE_WORDS,
    BOOT_MODE_TEST,
    BOOT_MODE_COUNT,
} boot_mode_t;

static void boot_menu_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const int *selected = ctx;
    splash_frame_common();

    static const char *labels[BOOT_MODE_COUNT] = { "GAME", "WORDS", "TEST" };
    const int char_w = 6 * 2;               /* scale 2 */
    static const int chars[BOOT_MODE_COUNT] = { 4, 5, 4 };
    const int gap = 12;
    int total_w = (chars[0] + chars[1] + chars[2]) * char_w + gap * 2;
    int x = (DISP_FB_W - total_w) / 2;
    const int y = 168;

    for (int i = 0; i < BOOT_MODE_COUNT; i++) {
        int word_w = chars[i] * char_w;
        if (i == *selected) {
            display_fill_rect(x - 6, y - 3, word_w + 12, 7 * 2 + 6, C_GB2);
            display_text(x, y, labels[i], C_GB0, 2);
        } else {
            display_text(x, y, labels[i], C_GB2, 2);
        }
        x += word_w + gap;
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
     * 只有 GAME/TEST 路径才初始化 ROM 目录；TEST 仍能在诊断画面看到存储占用。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();
    uint16_t boot_keys = input_serial_poll() | input_gamepad_poll() | input_usb_poll();
    bool refresh_rom_index = (boot_keys & NES_PAD_SELECT) != 0;
    if (refresh_rom_index) {
        ESP_LOGI(TAG, "检测到 SELECT，忽略 ROM 目录缓存并完整重扫");
    }
    boot_mode_t boot_mode;
    do {
        boot_mode = boot_menu();
        if (boot_mode == BOOT_MODE_WORDS) word_study_run();
    } while (boot_mode == BOOT_MODE_WORDS);

    rom_store_init(refresh_rom_index);
    if (boot_mode == BOOT_MODE_TEST) {
        input_gamepad_show();
    }

    /* 开机选单只返回目录项；各模拟器在自己的大块内存准备妥当后再从卡上读，
     * SNES 尤其不能先读出 4 MiB 再复制一份，否则 8 MiB PSRAM 会在峰值时耗尽。
     * 卡不可用时 entry 留 NULL，NES 继续走编译期嵌入 ROM 的回退路径。 */
    const rom_store_entry_t *entry = NULL;
    uint16_t launch_keys = 0;
    rom_menu_pick(&entry, &launch_keys);

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
