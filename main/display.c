/*
 * ST7789 显示层实现 —— 基于 ESP-IDF 自带的 esp_lcd 组件
 *
 * 条带流式推屏 + 核 1 推屏任务。
 *
 * ---- 为什么不用常驻帧缓冲 ----
 *
 * 画布 288x224 的 RGB565 是 126 KB，双缓冲要 252 KB，内部 DMA 内存装不下
 * （PSRAM 不能直接 DMA，原因见下面 display_init 里的注释）。
 *
 * 所以这里只留两块最大 320x32 的条带缓冲（各 20 KB）：核 1 把第 N+1 条
 * 转换进一块的同时，DMA 正在推第 N 条，两者流水。整块画布由调用方提供的
 * disp_strip_fn 回调按条带现算，不需要落地。
 *
 * 双缓冲没有消失，只是下移到了 8 位的 NES vidbuf 那一层（每块 64 KB，比
 * RGB565 便宜一半）—— 核 0 渲染下一帧、核 1 读上一帧，帧时间仍然是
 * max(模拟, 推屏) 而不是两者相加。这一点很关键：如果把转换+推送放回核 0，
 * 每帧就变成「模拟 9ms 首尾相接推屏 14ms」= 23ms，直接超出 60fps 预算。
 *
 * ---- 条带切多大 ----
 *
 * esp_lcd_panel_draw_bitmap 在发下一条带的 CASET/RASET/RAMWR 之前必须等上
 * 一条带传完（命令和数据共用一条 SPI 总线），所以每条带的驱动开销是**串行
 * 叠加**在总线时间上的，不是并行掉的。
 *
 * 实测（256x192 = 98304 字节/帧，80MHz，理论 9.83 ms）：
 *
 *     条带数   4      8      12     24
 *     实测   10.34  10.83  11.31  12.78 ms
 *
 * 完美线性，斜率 122 us/条带，每帧固定开销约等于 0。122us 远不是命令本身的
 * 总线时间（11 字节只值 1us），是每次 draw_bitmap 的驱动/ISR/信号量往返。
 *
 * 所以条带**越少越快**，只受条带缓冲内存和流水粒度的约束。取 32 行/条：
 * 224/32 = 7 条整带，开销 0.86 ms，缓冲 2x18 KB。
 * 若取 16 行会多花 0.86ms，取 8 行多花 2.6ms —— 都不划算。
 */

#include <string.h>
#include "display.h"
#include "menu_font.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

/* 一条带多少行。选值理由见文件头的实测表：32 行 -> 224/32 = 7 条整带。 */
#define BAND_LINES      32
#define MAX_BAND_COUNT  ((DISP_H + BAND_LINES - 1) / BAND_LINES)
#define BAND_BYTES      (DISP_W * BAND_LINES * 2)
#define BACKLIGHT_DEFAULT 100
/* 与 audio_output.c 音量共用；SETTINGS 背光下限是 5%，读回时也按这个夹。 */
#define UI_NVS_NS            "ui_prefs"
#define UI_NVS_KEY_BACKLIGHT "backlight"
#define BACKLIGHT_MIN_PCT    5

_Static_assert(MAX_BAND_COUNT >= 2, "条带数至少 2，否则流水不起来");

/* 每秒往串口打一行推屏耗时。调条带尺寸/画布尺寸时很有用，平时可以关掉。 */
#define DISP_PROFILE    1

static esp_lcd_panel_handle_t   s_panel;
static esp_lcd_panel_io_handle_t s_io;

static uint16_t          *s_strip[2];    /* 两块条带缓冲，转换/DMA 乒乓 */
static disp_strip_fn      s_fn;          /* 本帧的绘制回调 */
static void              *s_ctx;
static int                s_frame_w = DISP_FB_W;
static int                s_frame_h = DISP_FB_H;
static int                s_frame_x = DISP_FB_X;
static int                s_frame_y = DISP_FB_Y;

/* 绘图原语的作用目标：当前正在填的那条带。只在核 1 的回调里有效。 */
static uint16_t          *s_cur;         /* 当前条带缓冲 */
static int                s_cur_y0;      /* 它对应画布的起始行 */
static int                s_cur_h;       /* 它有多少行 */
static int                s_cur_w;       /* 本帧画布宽度，也是条带行跨度 */
static int                s_cur_frame_h; /* 本帧画布总高度 */

static SemaphoreHandle_t  s_band_done;

/* 等一条带的 DMA 回执。**必须带超时**，不能 portMAX_DELAY。
 *
 * 实测过一个偶发死锁：菜单里翻页时推屏任务永远等不到某一次回执，于是不再
 * 归还 s_idle，菜单跟着阻塞在 display_stream_sync，屏幕停在推了一半的画面
 * （底部几条带保留旧内容），只能按 RST。整机其余部分是活的 —— 探针任务照
 * 常输出，也没有任何看门狗告警，因为推屏任务是阻塞不是空转。
 *
 * 根因还没定死（往推屏任务里加几个 volatile 写就不复现了，说明窗口很窄）。
 * 但无论根因是什么，「少收一次回执」都不该让整机瘫痪：超时就放弃本帧、把
 * 残留回执排空、照常归还 s_idle，代价只是丢一帧。真发生时这里会大声报出来，
 * 带上条带序号和回执计数 —— 那正是定位根因缺的最后一块数据。 */
/* ⚠ UI 画面必须串行推屏，不能走流水线 —— 这条是实测逼出来的，别"优化"回去。
 *
 * 默认路径是流水线：一边把第 N+1 条转换进另一块条带缓冲、一边 DMA 推第 N
 * 条，靠 s_strip 乒乓 + 滞后两条收回执实现重叠，任何时刻最多两个传输在飞。
 * 游戏路径继续走这条，快得多。
 *
 * 但在菜单里用摇杆快速翻页时会**偶发死锁**：推屏任务卡死在
 * esp_lcd_panel_draw_bitmap() 内部再也不返回，于是不归还 s_idle，菜单跟着
 * 阻塞在 display_stream_sync，屏幕停在推了一半的画面，只能按 RST。
 *
 * 死锁签名极其稳定（多次复现完全一致）：永远卡在**最后一条带**的
 * draw_bitmap 里，回执计数恒为 1 —— 也就是此刻已发出的 6 次传输全部完成、
 * SPI 队列是空的，它却在里面等。队列深度配的是 MAX_BAND_COUNT+2=10，
 * 远没填满；推屏任务栈余 1104 B，也不是栈溢出。改成「发一条、等一条」
 * （任何时刻最多一个传输在飞）后不再复现。
 *
 * **根因没有查清**，只锁定了触发条件。所以这里不是全局关掉流水线，而是
 * 只对 UI 画面关：
 *   - 代价只落在 UI 上：推屏 15.5 -> 26 ms/帧。菜单只在按键时重绘一次，
 *     看不出来；游戏保持原速（PCE 那边核 1 推屏本来就卡在边缘，掉到 26 ms
 *     会直接拖垮帧率）。
 *   - 判据用「是不是默认画布」：UI 全部走 288x224（rom_menu、开机菜单、
 *     加载页、单词学习、手柄测试），五个模拟器都提交自己的原生尺寸。
 *   - ⚠ 游戏路径上这个隐患理论上还在，只是从未触发过。怀疑触发条件和
 *     条带回调的耗时有关：UI 每条带要重画 8 行文字、逐字查字模，比游戏那种
 *     查表转换慢得多，DMA 早就完成、下一次 draw_bitmap 隔很久才来。
 *
 * 排查中已排除：菜单翻页算术、s_idle/s_submit/s_band_done 的收发配平、
 * menu_font 字库排序与二分查找、utf8_next() 对截断多字节序列的处理、
 * DMA 回执丢失（band_wait 的 200 ms 超时从未触发）、SPI 队列满、栈溢出。 */
#define BAND_WAIT_MS  200       /* 一条带正常约 2 ms，200 ms 是纯粹的兜底 */

static bool band_wait(int band, int band_count)
{
    if (xSemaphoreTake(s_band_done, pdMS_TO_TICKS(BAND_WAIT_MS)) == pdTRUE) {
        return true;
    }
    static uint32_t timeouts;
    timeouts++;
    ESP_LOGE(TAG, "等 DMA 回执超时：条带 %d/%d，回执计数 %d，累计 %lu 次，丢弃本帧",
             band, band_count, (int)uxSemaphoreGetCount(s_band_done),
             (unsigned long)timeouts);
    /* 排空可能迟到的回执，让下一帧从干净状态开始。 */
    while (xSemaphoreTake(s_band_done, 0) == pdTRUE) { }
    return false;
}
   /* 计数：每条带 DMA 传完 +1 */
static SemaphoreHandle_t  s_submit;      /* 二值：有新帧待推 */
static SemaphoreHandle_t  s_idle;        /* 二值：推屏任务空闲，可以收新帧 */

static bool IRAM_ATTR on_trans_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev,
                                    void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_band_done, &hp);
    return hp == pdTRUE;
}

/* 推屏任务：钉在核 1。逐条带调用绘制回调、排进 SPI 队列，全部传完再报空闲。
 * 这期间核 0 的调用方可以自由地算下一帧。 */
static void blit_task(void *arg)
{
#if DISP_PROFILE
    int64_t acc = 0, t0 = esp_timer_get_time();
    int     n   = 0;
#endif

    for (;;) {
        xSemaphoreTake(s_submit, portMAX_DELAY);
        const int frame_w = s_frame_w;
        const int frame_h = s_frame_h;
        const int frame_x = s_frame_x;
        const int frame_y = s_frame_y;
        const int band_count = (frame_h + BAND_LINES - 1) / BAND_LINES;
        /* 默认画布 = UI 画面，见上面那段。 */
        const bool serial_push = (frame_w == DISP_FB_W && frame_h == DISP_FB_H);
        bool aborted = false;
#if DISP_PROFILE
        int64_t push_t0 = esp_timer_get_time();
#endif

        for (int i = 0; i < band_count; i++) {
            int y0 = i * BAND_LINES;
            int h  = frame_h - y0;
            if (h > BAND_LINES) h = BAND_LINES;

            /* 乒乓：第 i 条要写的缓冲正是第 i-2 条用过的那块，
             * 得先确认它的 DMA 已经回执。
             *
             * 实际上 draw_bitmap 内部会等上一条传完才发命令，所以这一步几乎
             * 总是立刻返回。但那是驱动的实现细节，不是契约 —— 显式等一次让
             * 「不会覆写正在 DMA 的缓冲」这件事在本地就能看明白，代价为零。 */
            if (!serial_push && i >= 2 &&
                !band_wait(i, band_count)) { aborted = true; break; }

            s_cur    = s_strip[i & 1];
            s_cur_y0 = y0;
            s_cur_h  = h;
            s_cur_w  = frame_w;
            s_cur_frame_h = frame_h;
            s_fn(s_cur, y0, h, s_ctx);

            esp_lcd_panel_draw_bitmap(s_panel,
                                      frame_x,           frame_y + y0,
                                      frame_x + frame_w, frame_y + y0 + h,
                                      s_cur);
            /* 串行模式：立刻收掉这一条的回执，任何时刻最多一个传输在飞。 */
            if (serial_push && !band_wait(i, band_count)) { aborted = true; break; }
        }
        /* 循环里至多漏收两条（i=0、1 时没等），这里补齐最后的 DMA 回执。 */
        /* 串行模式每条带已经就地收过回执，不用补。 */
        int pending = serial_push ? 0 : (band_count < 2 ? band_count : 2);
        for (int i = 0; !aborted && i < pending; i++) {
            if (!band_wait(band_count + i, band_count)) break;
        }

#if DISP_PROFILE
        acc += esp_timer_get_time() - push_t0;
        n++;
#endif
        xSemaphoreGive(s_idle);     /* 先放行核 0，printf 别卡在关键路径上 */

#if DISP_PROFILE
        int64_t now = esp_timer_get_time();
        if (now - t0 >= 1000000) {
            printf("推屏 %.2f ms/帧（%d 行/条 x %d 条 = %d 字节，%d 帧）\n",
                   (float)acc / 1000.0f / n, BAND_LINES, band_count,
                   frame_w * frame_h * 2, n);
            acc = 0; n = 0; t0 = now;
        }
#endif
    }
}

/* 用一行像素反复推，把整块面板刷成同一个颜色。
 * 只在开机时调一次，用来把画布之外的黑边清干净。
 * 复用同一个行缓冲，所以每推一行都要等它传完。 */
static void fill_whole_panel(uint16_t color)
{
    uint16_t *line = heap_caps_malloc(DISP_W * 2,
                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!line) return;

    for (int i = 0; i < DISP_W; i++) line[i] = color;

    for (int y = 0; y < DISP_H; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, DISP_W, y + 1, line);
        xSemaphoreTake(s_band_done, portMAX_DELAY);
    }
    free(line);
}

static void backlight_init(void)
{
    if (DISP_PIN_BL < 0) return;

    ledc_timer_config_t t = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = DISP_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static int s_backlight_pct = BACKLIGHT_DEFAULT;

static int backlight_clamp(int percent)
{
    if (percent < BACKLIGHT_MIN_PCT) return BACKLIGHT_MIN_PCT;
    if (percent > 100) return 100;
    return percent;
}

static bool ui_nvs_open(nvs_handle_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 未就绪，背光只在本次开机有效：%s", esp_err_to_name(err));
        return false;
    }
    err = nvs_open(UI_NVS_NS, NVS_READWRITE, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ui_prefs 打不开：%s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int backlight_load_or_default(void)
{
    nvs_handle_t nvs;
    if (!ui_nvs_open(&nvs)) return BACKLIGHT_DEFAULT;

    uint8_t saved = BACKLIGHT_DEFAULT;
    esp_err_t err = nvs_get_u8(nvs, UI_NVS_KEY_BACKLIGHT, &saved);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return BACKLIGHT_DEFAULT;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读背光失败：%s", esp_err_to_name(err));
        return BACKLIGHT_DEFAULT;
    }
    if (saved < BACKLIGHT_MIN_PCT || saved > 100) {
        ESP_LOGW(TAG, "背光 NVS 值异常 %u，回退默认", (unsigned)saved);
        return BACKLIGHT_DEFAULT;
    }
    return (int)saved;
}

static void backlight_save(int percent)
{
    nvs_handle_t nvs;
    if (!ui_nvs_open(&nvs)) return;

    esp_err_t err = nvs_set_u8(nvs, UI_NVS_KEY_BACKLIGHT, (uint8_t)percent);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "背光保存失败：%s", esp_err_to_name(err));
    }
}

/* 只改 PWM，不写 NVS。开机恢复用这条，避免每次上电白写一次。 */
static void backlight_apply(int percent)
{
    if (DISP_PIN_BL < 0) return;
    percent = backlight_clamp(percent);
    s_backlight_pct = percent;

    /* 10 位分辨率下满亮是 1024 而不是 1023 —— 用 1023 会让每个 PWM 周期
     * 留一个 1/1024 的熄灭窗口，虽然 5kHz 下肉眼基本看不出，但那不是真正的常亮。 */
    uint32_t duty = (1024 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_backlight(int percent)
{
    percent = backlight_clamp(percent);
    backlight_apply(percent);
    backlight_save(percent);
    ESP_LOGI(TAG, "背光：%d%%（已写入 NVS）", percent);
}

int display_get_backlight(void)
{
    return s_backlight_pct;
}

esp_err_t display_init(void)
{
    s_band_done = xSemaphoreCreateCounting(MAX_BAND_COUNT * 2, 0);
    s_submit    = xSemaphoreCreateBinary();
    s_idle      = xSemaphoreCreateBinary();
    if (!s_band_done || !s_submit || !s_idle) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_idle);     /* 开机时推屏任务是空闲的 */

    /* 两块条带缓冲必须都在内部 SRAM，这是**硬约束**，不能退到 PSRAM。
     *
     * 试过退 PSRAM，不行：spi_master 的 setup_priv_desc() 用 esp_ptr_dma_capable()
     * 判断缓冲能不能直接 DMA，而那个函数只认内部 DRAM 地址段（SOC_DMA_LOW/HIGH），
     * PSRAM 指针一律返回 false。于是驱动会**每条带**临时 malloc 一块内部 DMA
     * 缓冲再 memcpy 过去 —— 数据最后照样落在内部 RAM，等于白绕一圈还多一次拷贝。
     *
     * 好在条带才 18 KB，和以前 96 KB 的整帧缓冲不是一个量级，分配压力小得多。 */
    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_strip[i]) {
            ESP_LOGE(TAG, "条带缓冲 %d 分配失败（每块需要 %d 字节内部 DMA 内存，"
                          "当前最大空闲块 %u 字节）—— 调小 display.c 的 BAND_LINES",
                     i, BAND_BYTES,
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            return ESP_ERR_NO_MEM;
        }
        memset(s_strip[i], 0, BAND_BYTES);
    }

    backlight_init();

    spi_bus_config_t bus = {
        .sclk_io_num     = DISP_PIN_SCLK,
        .mosi_io_num     = DISP_PIN_MOSI,
        .miso_io_num     = -1,          /* 只写屏，不读回 */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = DISP_PIN_CS,
        .dc_gpio_num       = DISP_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = DISP_SPI_HZ,
        .trans_queue_depth = MAX_BAND_COUNT + 2,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        .rgb_ele_order  = DISP_BGR_ORDER ? LCD_RGB_ELEMENT_ORDER_BGR
                                         : LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, DISP_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, DISP_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, DISP_MIRROR_X, DISP_MIRROR_Y));
    /* 关键：170 列的面板居中贴在 240 列的显存上，偏移 35 */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, DISP_GAP_X, DISP_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 把整块面板（含画布之外的黑边）清一次。之后每帧只推画布那块，
     * 黑边再也不会被碰到。必须在推屏任务起来之前做，好独占 s_band_done。 */
    fill_whole_panel(C_BLACK);

    /* 从 NVS 恢复上次亮度；没有记录才满亮。不走 display_backlight()，
     * 免得开机白写一次 NVS。 */
    int bl = backlight_load_or_default();
    backlight_apply(bl);
    ESP_LOGI(TAG, "背光恢复：%d%%", bl);

    /* 推屏任务钉在核 1：核 0 画图，核 1 推屏，互不抢占 */
    BaseType_t ok = xTaskCreatePinnedToCore(blit_task, "lcd_blit", 3072,
                                            NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "推屏任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ST7789 就绪 面板%dx%d @ %d MHz, gap(%d,%d), "
                  "默认画布%dx%d@(%d,%d), 最大条带宽 %d, 条带流式 %d 行, 缓冲 2x%d KB",
             DISP_W, DISP_H, DISP_SPI_HZ / 1000000, DISP_GAP_X, DISP_GAP_Y,
             DISP_FB_W, DISP_FB_H, DISP_FB_X, DISP_FB_Y,
             DISP_W, BAND_LINES, BAND_BYTES / 1024);
    return ESP_OK;
}

void display_wait_idle(void)
{
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

void display_stream(disp_strip_fn fn, void *ctx)
{
    display_stream_sized(fn, ctx, DISP_FB_W, DISP_FB_H);
}

void display_stream_sized(disp_strip_fn fn, void *ctx, int width, int height)
{
    if (!fn || width <= 0 || height <= 0 || width > DISP_W || height > DISP_H) {
        ESP_LOGE(TAG, "拒绝非法画布 %dx%d（面板 %dx%d）", width, height, DISP_W, DISP_H);
        return;
    }

    /* 等推屏任务把上一帧收完。这里是唯一的阻塞点，也正好起到帧率节流的作用。 */
    xSemaphoreTake(s_idle, portMAX_DELAY);

    s_fn  = fn;
    s_ctx = ctx;
    s_frame_w = width;
    s_frame_h = height;
    s_frame_x = (DISP_W - width) / 2;
    s_frame_y = (DISP_H - height) / 2;

    xSemaphoreGive(s_submit);
}

void display_stream_sync(disp_strip_fn fn, void *ctx)
{
    display_stream(fn, ctx);
    display_wait_idle();
}

/* ================= 绘图：目标是「当前条带」 =================
 *
 * 坐标都是相对整块画布的，落在当前条带之外的行直接丢掉。
 * 调用方（菜单、诊断画面）因此完全不用知道条带的存在，同一段绘制代码
 * 被逐条带重复调用就是了。 */

void display_pixel(int x, int y, uint16_t color)
{
    if ((unsigned)x >= (unsigned)s_cur_w) return;
    int row = y - s_cur_y0;
    if ((unsigned)row >= (unsigned)s_cur_h) return;
    s_cur[(size_t)row * s_cur_w + x] = color;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    /* 先裁到画布 */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_cur_w) w = s_cur_w - x;
    if (y + h > s_cur_frame_h) h = s_cur_frame_h - y;
    if (w <= 0 || h <= 0) return;

    /* 再裁到当前条带 */
    int y_lo = y > s_cur_y0 ? y : s_cur_y0;
    int y_hi = (y + h) < (s_cur_y0 + s_cur_h) ? (y + h) : (s_cur_y0 + s_cur_h);
    if (y_lo >= y_hi) return;

    for (int yy = y_lo; yy < y_hi; yy++) {
        uint16_t *p = s_cur + (size_t)(yy - s_cur_y0) * s_cur_w + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

void display_clear(uint16_t color)
{
    display_fill_rect(0, 0, s_cur_w, s_cur_frame_h, color);
}

void display_hline(int x, int y, int w, uint16_t color)
{
    display_fill_rect(x, y, w, 1, color);
}

void display_vline(int x, int y, int h, uint16_t color)
{
    display_fill_rect(x, y, 1, h, color);
}

void display_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    display_hline(x, y,         w, color);
    display_hline(x, y + h - 1, w, color);
    display_vline(x,         y, h, color);
    display_vline(x + w - 1, y, h, color);
}

/* ---- 5x7 点阵字库，ASCII 0x20~0x7E，每字符 5 列，每列低 7 位为一行 ---- */
static const uint8_t FONT5X7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, /*   ! */
    {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14}, /* " # */
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, /* $ % */
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, /* & ' */
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, /* ( ) */
    {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08}, /* * + */
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, /* , - */
    {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}, /* . / */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, /* 0 1 */
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, /* 2 3 */
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, /* 4 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, /* 6 7 */
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, /* 8 9 */
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00}, /* : ; */
    {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14}, /* < = */
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, /* > ? */
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, /* @ A */
    {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, /* B C */
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, /* D E */
    {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, /* F G */
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, /* H I */
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, /* J K */
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x04,0x02,0x7F}, /* L M */
    {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, /* N O */
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, /* P Q */
    {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, /* R S */
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, /* T U */
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, /* V W */
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, /* X Y */
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00}, /* Z [ */
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, /* \ ] */
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40}, /* ^ _ */
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, /* ` a */
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, /* b c */
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, /* d e */
    {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E}, /* f g */
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, /* h i */
    {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00}, /* j k */
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, /* l m */
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, /* n o */
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, /* p q */
    {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20}, /* r s */
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, /* t u */
    {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C}, /* v w */
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, /* x y */
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, /* z { */
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, /* | } */
    {0x08,0x08,0x2A,0x1C,0x08},                             /* ~   */
};

/* 菜单标题来自 flash 里的 UTF-8 文件名。这里只解码标准 UTF-8；坏序列每次
 * 消耗一个字节并画成问号，避免卡在同一个指针上死循环。 */
static uint32_t utf8_next(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;

    if (s[0] < 0x80) {
        *text = (const char *)(s + 1);
        return s[0];
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *text = (const char *)(s + 2);
        return cp;
    }
    if (s[0] >= 0xE0 && s[0] <= 0xEF &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[0] != 0xE0 || s[1] >= 0xA0) &&
        (s[0] != 0xED || s[1] < 0xA0)) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *text = (const char *)(s + 3);
        return cp;
    }

    *text = (const char *)(s + 1);
    return '?';
}

void display_text(int x, int y, const char *s, uint16_t color, int scale)
{
    if (scale < 1) scale = 1;

    int cx = x;
    while (*s) {
        uint32_t codepoint = utf8_next(&s);
        const uint8_t *han = codepoint > 0x7E ? menu_font_glyph(codepoint) : NULL;

        if (han) {
            for (int row = 0; row < 16; row++) {
                uint16_t bits = ((uint16_t)han[row * 2] << 8) | han[row * 2 + 1];
                for (int col = 0; col < 16; col++) {
                    if (bits & (0x8000u >> col)) {
                        display_fill_rect(cx + col * scale, y + row * scale,
                                          scale, scale, color);
                    }
                }
            }
            cx += 17 * scale;   /* 16 列字形 + 1 列间距 */
            continue;
        }

        unsigned char ch = codepoint >= 0x20 && codepoint <= 0x7E
                         ? (unsigned char)codepoint : '?';
        const uint8_t *ascii = FONT5X7[ch - 0x20];

        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (ascii[col] & (1 << row)) {
                    display_fill_rect(cx + col * scale, y + row * scale,
                                      scale, scale, color);
                }
            }
        }
        cx += 6 * scale;    /* 5 列字形 + 1 列间距 */
    }
}

void display_text_16(int x, int y, const char *s, uint16_t color)
{
    int cx = x;
    while (*s) {
        uint32_t codepoint = utf8_next(&s);
        const uint8_t *han = codepoint > 0x7E ? menu_font_glyph(codepoint) : NULL;

        if (han) {
            for (int row = 0; row < 16; row++) {
                uint16_t bits = ((uint16_t)han[row * 2] << 8) | han[row * 2 + 1];
                for (int col = 0; col < 16; col++) {
                    if (bits & (0x8000u >> col)) {
                        display_pixel(cx + col, y + row, color);
                    }
                }
            }
            cx += 17;       /* 16px 全角 + 1px 字距 */
            continue;
        }

        const uint8_t *ascii = menu_font_ascii_glyph(codepoint);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = ascii[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80u >> col)) {
                    display_pixel(cx + col, y + row, color);
                }
            }
        }
        cx += 9;            /* 8px 半角 + 1px 字距 */
    }
}

void display_text_ascii_16_scaled(int x, int y, const char *s,
                                  uint16_t color, int scale)
{
    if (scale < 1) scale = 1;

    int cx = x;
    while (*s) {
        uint32_t codepoint = utf8_next(&s);
        const uint8_t *ascii = menu_font_ascii_glyph(codepoint);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = ascii[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80u >> col)) {
                    display_fill_rect(cx + col * scale, y + row * scale,
                                      scale, scale, color);
                }
            }
        }
        cx += 9 * scale;
    }
}

int display_text_width_ascii_16_scaled(const char *s, int scale)
{
    if (scale < 1) scale = 1;

    int width = 0;
    while (*s) {
        (void)utf8_next(&s);
        width += 9 * scale;
    }
    return width;
}

int display_text_width_16(const char *s)
{
    int width = 0;
    while (*s) {
        uint32_t codepoint = utf8_next(&s);
        width += codepoint > 0x7E && menu_font_glyph(codepoint) ? 17 : 9;
    }
    return width;
}
