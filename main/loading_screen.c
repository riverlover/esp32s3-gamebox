/* 固定游戏装载页
 *
 * 屏没有常驻帧缓冲，所以每次更新都重新提交一整帧。64 KB 一块的 ROM 读取若
 * 每块都刷屏，4 MiB 游戏会额外推 64 帧；这里按 5% 节流，最多约 20 帧，既能
 * 看出进度，又不会为了进度条明显拖慢装载。 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "loading_screen.h"
#include "display.h"
#include "rom_store.h"
#include "esp_log.h"

#define TEXT_X       16
#define TITLE_Y      26
#define NAME_Y       68
#define STAGE_Y      101
#define BAR_X        16
#define BAR_Y        126
#define BAR_W        (DISP_FB_W - 2 * BAR_X)
#define BAR_H        18
#define PERCENT_Y    155
#define FOOTER_Y     197
#define REFRESH_STEP 5

static const char *TAG = "loading";

typedef struct {
    char name[ROM_STORE_NAME_LEN];
    char stage[32];
    unsigned percent;
    bool error;
} loading_draw_t;

static char s_name[ROM_STORE_NAME_LEN];
static char s_stage[32];
static unsigned s_last_percent;
static bool s_started;

static int text_width(const char *s)
{
    int width = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            width += 17;
            p += 3;
        } else {
            width += 6;
            p++;
        }
    }
    return width;
}

/* 按屏幕像素宽度截断，不在 UTF-8 汉字中间下刀。菜单名字已经去掉区域标记，
 * 这里再做最后一道边界保护，避免长文件名顶出画布。 */
static void copy_fitted(char *dst, size_t dst_size, const char *src, int max_width)
{
    size_t out = 0;
    int width = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && out + 1 < dst_size) {
        size_t bytes = ((*p & 0xF0) == 0xE0 && p[1] && p[2]) ? 3 : 1;
        int glyph_width = bytes == 3 ? 17 : 6;
        if (width + glyph_width > max_width || out + bytes >= dst_size) break;
        memcpy(dst + out, p, bytes);
        out += bytes;
        width += glyph_width;
        p += bytes;
    }
    dst[out] = '\0';
}

static void loading_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const loading_draw_t *a = ctx;
    (void)strip;
    (void)y0;
    (void)h;

    display_clear(C_GB0);
    display_text(TEXT_X, TITLE_Y, a->error ? "加载失败" : "正在加载", C_GB3, 1);

    int name_x = (DISP_FB_W - text_width(a->name)) / 2;
    if (name_x < TEXT_X) name_x = TEXT_X;
    display_text(name_x, NAME_Y, a->name, C_GB2, 1);

    display_text(TEXT_X, STAGE_Y, a->stage, a->error ? C_RED : C_GB2, 1);
    display_rect(BAR_X, BAR_Y, BAR_W, BAR_H, C_GB2);
    int fill = (BAR_W - 4) * (int)a->percent / 100;
    if (fill > 0) {
        display_fill_rect(BAR_X + 2, BAR_Y + 2, fill, BAR_H - 4,
                          a->error ? C_RED : C_GB2);
    }

    char percent[8];
    snprintf(percent, sizeof(percent), "%u%%", a->percent);
    display_text((DISP_FB_W - text_width(percent)) / 2, PERCENT_Y,
                 percent, a->error ? C_RED : C_GB3, 1);
    display_text((DISP_FB_W - text_width("请勿拔出 TF 卡")) / 2, FOOTER_Y,
                 "请勿拔出 TF 卡", C_GB2, 1);
}

static void render(bool error, unsigned percent)
{
    loading_draw_t draw = {
        .percent = percent > 100 ? 100 : percent,
        .error = error,
    };
    snprintf(draw.name, sizeof(draw.name), "%s", s_name);
    snprintf(draw.stage, sizeof(draw.stage), "%s", s_stage);
    if (error) {
        ESP_LOGE(TAG, "%s：%s（%u%%）", s_name, s_stage, draw.percent);
    } else {
        ESP_LOGI(TAG, "%s：%s（%u%%）", s_name, s_stage, draw.percent);
    }
    display_stream_sync(loading_strip, &draw);
}

void loading_screen_begin(const char *game_name)
{
    copy_fitted(s_name, sizeof(s_name), game_name, DISP_FB_W - 2 * TEXT_X);
    snprintf(s_stage, sizeof(s_stage), "准备加载");
    s_last_percent = 0;
    s_started = true;
    render(false, 0);
}

void loading_screen_progress(const char *stage, unsigned percent)
{
    if (!s_started) return;
    if (percent > 100) percent = 100;

    bool stage_changed = stage && strcmp(stage, s_stage) != 0;
    if (!stage_changed && percent < 100 &&
        percent < s_last_percent + REFRESH_STEP) return;

    if (stage) snprintf(s_stage, sizeof(s_stage), "%s", stage);
    s_last_percent = percent;
    render(false, percent);
}

void loading_screen_error(const char *message)
{
    if (!s_started) return;
    snprintf(s_stage, sizeof(s_stage), "%s", message ? message : "未知错误");
    render(true, s_last_percent);
}
