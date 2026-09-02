/*
 * retro-go 风格的统一游戏内菜单与 TF 即时存档
 *
 * 本机用 SELECT+X 打开 retro-go 风格菜单，主流程是
 * Save & Continue / Save & Quit / Load game / Reset / Quit，存取档各有 0~3
 * 四个槽。本项目的四个核心继续保留自己的快照格式，这里只统一交互和落盘。
 *
 * 槽文件写入先落到 .tmp；成功关闭以后，旧档暂存为 .bak，再把 .tmp 改名为
 * 正式文件。这样写卡中途复位不会直接覆盖唯一好档，读档也会在正式文件失败
 * 时尝试 .bak。不能只 fopen("wb") 原地覆盖——FAT 上掉电后常见结果是零字节档。
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_output.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "game_menu.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "input_usb.h"
#include "sd_card.h"

static const char *TAG = "game_menu";

#define SAVE_ROOT       SD_MOUNT_POINT "/gamebox/saves"
#define SAVE_SLOT_COUNT 4
#define MENU_ROWS       5
#define POLL_MS         10

static const char *const MAIN_ITEMS[MENU_ROWS] = {
    "SAVE & CONTINUE",
    "SAVE & QUIT",
    "LOAD GAME",
    "RESET",
    "QUIT",
};

typedef struct {
    const char *title;
    const char *const *items;
    int count;
    int selected;
    bool slot_view;
    bool used[SAVE_SLOT_COUNT];
    const char *footer;
} menu_draw_t;

/* 提示框把状态色画成**底色**、文字固定 C_UI_FG_INV 反白。别改回「状态色
 * 当文字色」：那样红字压在深色框上只有 1.6:1，SAVE FAILED 基本看不见。 */
typedef struct {
    const char *text;
    uint16_t fill;
} notice_t;

static void notice_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const notice_t *n = ctx;
    (void)strip;
    (void)y0;
    (void)h;
    display_clear(C_UI_BG);
    int w = display_text_width_16(n->text);
    display_fill_rect(24, 96, DISP_W - 48, 48, n->fill);
    display_rect(24, 96, DISP_W - 48, 48, C_UI_SEL_EDGE);
    display_text_16((DISP_W - w) / 2, 112, n->text, C_UI_FG_INV);
}

static uint16_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

static void draw_menu_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const menu_draw_t *m = ctx;
    (void)strip;
    (void)y0;
    (void)h;

    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_W, DISP_H, C_UI_EDGE);
    display_text_16(10, 9, m->title, C_UI_FG);
    display_fill_rect(10, 29, DISP_W - 20, 1, C_UI_LINE);

    for (int i = 0; i < m->count; i++) {
        int y = 39 + i * 30;
        bool active = i == m->selected;
        if (active) {
            display_fill_rect(8, y - 4, DISP_W - 16, 25, C_UI_SEL);
        }

        char line[32];
        if (m->slot_view) {
            snprintf(line, sizeof(line), "SLOT %d     %s", i,
                     m->used[i] ? "USED" : "EMPTY");
        } else {
            snprintf(line, sizeof(line), "%s", m->items[i]);
        }
        display_text_16(18, y, line, active ? C_UI_FG_INV : C_UI_FG);
        if (active) display_text_16(DISP_W - 26, y, ">", C_UI_FG_INV);
    }

    display_fill_rect(10, 214, DISP_W - 20, 1, C_UI_LINE);
    int footer_w = display_text_width_16(m->footer);
    display_text_16((DISP_W - footer_w) / 2, 219, m->footer, C_UI_FG_FAINT);
}

static void stream_menu_sync(disp_strip_fn fn, void *ctx)
{
    /* 游戏画面都铺满 320x240。若这里退回默认 288x224，四周会残留上一帧；
     * 游戏内菜单必须完整覆盖面板，再等本帧 DMA 结束后读取栈上参数。 */
    display_stream_sized(fn, ctx, DISP_W, DISP_H);
    display_wait_idle();
}

static void show_message(const char *message, uint16_t fill)
{
    notice_t notice = {message, fill};
    stream_menu_sync(notice_strip, &notice);
}

static bool mkdir_one(const char *path)
{
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static bool valid_system_name(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) {
            return false;
        }
    }
    return true;
}

static bool game_menu_prepare(const game_menu_config_t *config)
{
    if (!config || !valid_system_name(config->system) ||
        !sd_card_mounted()) return false;

    char system_dir[96];
    snprintf(system_dir, sizeof(system_dir), SAVE_ROOT "/%s", config->system);
    bool ok = mkdir_one(SD_MOUNT_POINT "/gamebox") &&
              mkdir_one(SAVE_ROOT) && mkdir_one(system_dir);
    if (!ok) {
        ESP_LOGE(TAG, "创建存档目录失败：%s (%d)", system_dir, errno);
    }
    return ok;
}

static bool game_menu_slot_path(const game_menu_config_t *config, int slot,
                                char *path, size_t path_size)
{
    if (!config || !path || path_size == 0 || slot < 0 ||
        slot >= SAVE_SLOT_COUNT || !valid_system_name(config->system)) {
        return false;
    }
    int n = snprintf(path, path_size, SAVE_ROOT "/%s/%08lx.sav%d",
                     config->system, (unsigned long)config->rom_crc, slot);
    return n > 0 && (size_t)n < path_size;
}

static bool game_menu_slot_exists(const game_menu_config_t *config, int slot)
{
    char path[128], bak[136];
    struct stat st;
    if (!game_menu_slot_path(config, slot, path, sizeof(path))) return false;
    if (stat(path, &st) == 0 && st.st_size > 0) return true;
    snprintf(bak, sizeof(bak), "%s.bak", path);
    return stat(bak, &st) == 0 && st.st_size > 0;
}

static bool save_slot(const game_menu_config_t *config, int slot)
{
    if (!game_menu_prepare(config) || !config->save_state) return false;

    char path[128], tmp[136], bak[136];
    if (!game_menu_slot_path(config, slot, path, sizeof(path))) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);
    unlink(tmp);

    if (!config->save_state(tmp, config->ctx)) {
        unlink(tmp);
        ESP_LOGE(TAG, "%s 槽 %d 快照写入失败", config->system, slot);
        return false;
    }

    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size <= 0) {
        unlink(tmp);
        ESP_LOGE(TAG, "%s 槽 %d 快照为空", config->system, slot);
        return false;
    }
    off_t new_size = st.st_size;

    bool had_old = stat(path, &st) == 0;
    if (had_old) {
        /* 正式档存在时才替换旧 .bak；若上次恰好停在 path->bak 之后，本次
         * path 不存在，必须让那份仅存的 .bak 一直活到新档提交成功。 */
        unlink(bak);
        if (rename(path, bak) != 0) {
            unlink(tmp);
            ESP_LOGE(TAG, "%s 槽 %d 旧档备份失败 (%d)", config->system, slot, errno);
            return false;
        }
    }
    if (rename(tmp, path) != 0) {
        if (had_old) rename(bak, path);
        unlink(tmp);
        ESP_LOGE(TAG, "%s 槽 %d 原子提交失败 (%d)", config->system, slot, errno);
        return false;
    }
    unlink(bak);
    ESP_LOGI(TAG, "%s 槽 %d 保存完成：%ld 字节，ROM CRC %08lx",
             config->system, slot, (long)new_size,
             (unsigned long)config->rom_crc);
    return true;
}

static bool load_slot(const game_menu_config_t *config, int slot)
{
    if (!config->load_state) return false;
    char path[128], bak[136];
    if (!game_menu_slot_path(config, slot, path, sizeof(path))) return false;
    snprintf(bak, sizeof(bak), "%s.bak", path);

    if (config->load_state(path, config->ctx)) {
        ESP_LOGI(TAG, "%s 槽 %d 读取完成，ROM CRC %08lx", config->system,
                 slot, (unsigned long)config->rom_crc);
        return true;
    }
    /* 上一次保存若恰好在两次 rename 之间掉电，正式文件可能暂时不存在；
     * .bak 仍是完整旧档。核心若拒绝了损坏正式档，也给旧档一次恢复机会。 */
    if (access(bak, F_OK) == 0 && config->load_state(bak, config->ctx)) {
        ESP_LOGW(TAG, "%s 槽 %d 正式档不可用，已恢复备份", config->system, slot);
        return true;
    }
    return false;
}

static void wait_release(void)
{
    /* 串口按键靠 250ms 超时模拟松开；这里同样等待它清掉，返回游戏后不会把
     * 确认用的 A 或打开菜单用的 SELECT+X 注入下一帧。 */
    while (poll_input() != 0) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
}

static int run_list(menu_draw_t *draw)
{
    stream_menu_sync(draw_menu_strip, draw);
    wait_release();
    uint16_t previous = 0;

    while (1) {
        uint16_t keys = poll_input();
        uint16_t edge = keys & ~previous;
        previous = keys;

        int old = draw->selected;
        if (edge & GAMEPAD_BIT_UP) {
            draw->selected = (draw->selected + draw->count - 1) % draw->count;
        } else if (edge & GAMEPAD_BIT_DOWN) {
            draw->selected = (draw->selected + 1) % draw->count;
        } else if (edge & GAMEPAD_BIT_A) {
            int selected = draw->selected;
            wait_release();
            return selected;
        } else if (edge & GAMEPAD_BIT_B) {
            wait_release();
            return -1;
        }
        if (old != draw->selected) stream_menu_sync(draw_menu_strip, draw);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static int select_slot(const game_menu_config_t *config, bool saving)
{
    menu_draw_t draw = {
        .title = saving ? "SAVE STATE" : "LOAD STATE",
        .count = SAVE_SLOT_COUNT,
        .slot_view = true,
        .footer = "A:OK  B:BACK",
    };
    for (int i = 0; i < SAVE_SLOT_COUNT; i++) {
        draw.used[i] = game_menu_slot_exists(config, i);
    }

    while (1) {
        int slot = run_list(&draw);
        if (slot < 0) return -1;
        if (saving || draw.used[slot]) return slot;
        show_message("NO SAVE", C_UI_WARN);
        vTaskDelay(pdMS_TO_TICKS(600));
        stream_menu_sync(draw_menu_strip, &draw);
    }
}

static void reset_menu(const game_menu_config_t *config)
{
    static const char *const items[] = {"SOFT RESET", "HARD RESET"};
    menu_draw_t draw = {
        .title = "RESET EMULATION?",
        .items = items,
        .count = 2,
        .footer = "A:OK  B:BACK",
    };
    int selected = run_list(&draw);
    if (selected >= 0 && config->reset) config->reset(selected == 1, config->ctx);
}

game_menu_result_t game_menu_open(const game_menu_config_t *config)
{
    if (!config || !config->save_state || !config->load_state) {
        return GAME_MENU_CONTINUE;
    }

    audio_output_flush();
    ESP_LOGI(TAG, "打开游戏内菜单：%s，ROM CRC %08lx", config->system,
             (unsigned long)config->rom_crc);
    menu_draw_t draw = {
        .title = "GAME MENU",
        .items = MAIN_ITEMS,
        .count = MENU_ROWS,
        .footer = "A:OK  B:RESUME",
    };

    while (1) {
        int action = run_list(&draw);
        if (action < 0) {
            ESP_LOGI(TAG, "返回游戏");
            return GAME_MENU_CONTINUE;
        }
        ESP_LOGI(TAG, "菜单选择：%s", MAIN_ITEMS[action]);

        if (action == 0 || action == 1) {
            int slot = select_slot(config, true);
            if (slot < 0) continue;
            show_message("SAVING...", C_UI_SEL);
            bool ok = save_slot(config, slot);
            show_message(ok ? "SAVE OK" : "SAVE FAILED", ok ? C_UI_OK : C_UI_BAD);
            vTaskDelay(pdMS_TO_TICKS(700));
            if (ok) return action == 1 ? GAME_MENU_RESTART : GAME_MENU_CONTINUE;
        } else if (action == 2) {
            int slot = select_slot(config, false);
            if (slot < 0) continue;
            show_message("LOADING...", C_UI_SEL);
            bool ok = load_slot(config, slot);
            show_message(ok ? "LOAD OK" : "LOAD FAILED", ok ? C_UI_OK : C_UI_BAD);
            vTaskDelay(pdMS_TO_TICKS(700));
            if (ok) return GAME_MENU_CONTINUE;
        } else if (action == 3) {
            reset_menu(config);
            return GAME_MENU_CONTINUE;
        } else if (action == 4) {
            ESP_LOGI(TAG, "不保存并退出游戏");
            return GAME_MENU_RESTART;
        }
        stream_menu_sync(draw_menu_strip, &draw);
    }
}
