/*
 * 面向小学三至六年级的离线单词学习模式。
 *
 * 一轮只放 8 个词：先逐张认识，再用同一组词做三选一。这个顺序故意不让
 * 孩子一上来就猜陌生词；选项也只来自刚看过的 8 个词，测的是短时提取，
 * 不是靠排除完全陌生的干扰项碰运气。
 *
 * 先选教材，再选 Unit；一轮严格学习所选单元的 8 个词，不把别的单元混进来。
 * 掌握度分 0..3 四档，答对升一档，答错至少回到“待复习”。没有可靠时钟，
 * 不能假装做按天的间隔重复；同一单元经过三轮成功提取才进入“已掌握”。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nofrendo.h"
#include "display.h"
#include "input_gamepad.h"
#include "input_serial.h"
#include "input_usb.h"
#include "word_study.h"
#include "word_study_data.h"

static const char *TAG = "words";

#define WORD_COUNT       STUDY_DECK_WORDS
#define SESSION_WORDS     8
#define POLL_MS          16
#define PROGRESS_MAGIC    0x574F5244u  /* "WORD" */
#define PROGRESS_VERSION  2

#define C_OK   RGB565(42, 116, 54)
#define C_BAD  RGB565(174, 62, 48)
#define C_GOLD RGB565(238, 174, 36)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t word_count;
    uint16_t new_cursor;
    uint16_t review_cursor;
    uint32_t rounds;
    uint8_t level[WORD_COUNT];
} study_progress_t;

typedef struct {
    int word[SESSION_WORDS];
    int quiz_order[SESSION_WORDS];
} study_session_t;

typedef enum {
    VIEW_SELECT,
    VIEW_UNIT_SELECT,
    VIEW_HOME,
    VIEW_CARD_FRONT,
    VIEW_CARD_BACK,
    VIEW_QUIZ,
    VIEW_RESULT,
} view_kind_t;

typedef struct {
    view_kind_t kind;
    const study_deck_t *deck;
    const study_progress_t *progress;
    const study_session_t *session;
    int pos;
    int score;
    int selected;
    int unit;
    int options[3];
    int correct_slot;
    int answered;            /* -1=未作答，0=错，1=对 */
    bool storage_ok;
} view_t;

static nvs_handle_t s_nvs;
static bool s_storage_ok;
static uint32_t s_rng;

static uint16_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

static uint32_t random_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x ? x : 0x13579BDFu;
    return s_rng;
}

static int ascii_width(const char *s, int scale)
{
    return (int)strlen(s) * 6 * scale;
}

static void text_center_ascii(int y, const char *s, uint16_t color, int scale)
{
    display_text((DISP_FB_W - ascii_width(s, scale)) / 2, y, s, color, scale);
}

static void text_center_16(int y, const char *s, uint16_t color)
{
    display_text_16((DISP_FB_W - display_text_width_16(s)) / 2, y, s, color);
}

static void text_center_16_box(int x, int w, int y, const char *s, uint16_t color)
{
    display_text_16(x + (w - display_text_width_16(s)) / 2, y, s, color);
}

static void text_center_latin_16(int y, const char *s, uint16_t color, int scale)
{
    int width = display_text_width_ascii_16_scaled(s, scale);
    display_text_ascii_16_scaled((DISP_FB_W - width) / 2, y, s, color, scale);
}

static void draw_study_word(const char *s, int center_y, int maximum_scale)
{
    int scale = maximum_scale;
    while (scale > 1 &&
           display_text_width_ascii_16_scaled(s, scale) > DISP_FB_W - 28) {
        scale--;
    }

    int width = display_text_width_ascii_16_scaled(s, scale);
    int x = (DISP_FB_W - width) / 2;
    int y = center_y - 8 * scale;
    int shadow = scale > 1 ? 2 : 1;

    /* 浅色阴影把深色主字从 DMG 绿背景里托起来，但仍保持像素掌机风格。 */
    display_text_ascii_16_scaled(x + shadow, y + shadow, s, C_GB1, scale);
    display_text_ascii_16_scaled(x, y, s, C_GB3, scale);
}

static void draw_frame(void)
{
    display_clear(C_GB0);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_GB2);
}

static int count_level(const study_progress_t *progress, int minimum)
{
    int count = 0;
    for (int i = 0; i < WORD_COUNT; i++) {
        if (progress->level[i] >= minimum) count++;
    }
    return count;
}

static int count_unit_level(const study_progress_t *progress, int unit, int minimum)
{
    int count = 0;
    int first = unit * SESSION_WORDS;
    for (int i = first; i < first + SESSION_WORDS; i++) {
        if (progress->level[i] >= minimum) count++;
    }
    return count;
}

static void draw_card_header(const view_t *view, const char *title)
{
    text_center_latin_16(7, title, C_GB3, 1);
    for (int i = 0; i < SESSION_WORDS; i++) {
        uint16_t color = i < view->pos ? C_GOLD : (i == view->pos ? C_GB3 : C_GB1);
        display_fill_rect(71 + i * 19, 31, 13, 4, color);
    }
    display_hline(18, 42, DISP_FB_W - 36, C_GB2);
}

static void draw_select(const view_t *view)
{
    char line[40];
    draw_frame();
    text_center_latin_16(10, "YILIN ENGLISH", C_GB3, 1);
    text_center_16(37, "苏州小学 选择教材", C_GB2);

    for (int i = 0; i < STUDY_DECK_COUNT; i++) {
        int col = i & 1;
        int row = i / 2;
        int x = col ? 147 : 13;
        int y = 63 + row * 31;
        uint16_t fill = i == view->selected ? C_GB2 : C_GB0;
        uint16_t text = i == view->selected ? C_GB0 : C_GB3;
        display_fill_rect(x, y, 128, 25, fill);
        display_rect(x, y, 128, 25, C_GB2);
        snprintf(line, sizeof(line), "%d年级 %s册", STUDY_DECKS[i].grade,
                 STUDY_DECKS[i].upper ? "上" : "下");
        text_center_16_box(x, 128, y + 5, line, text);
    }

    snprintf(line, sizeof(line), "当前: %s", STUDY_DECKS[view->selected].revision);
    text_center_16(190, line, C_GB2);
    text_center_16(207, "方向选择  A确认  B返回", C_GB3);
}

static void draw_unit_select(const view_t *view)
{
    char line[48];
    draw_frame();
    snprintf(line, sizeof(line), "%d年级%s册  选择单元", view->deck->grade,
             view->deck->upper ? "上" : "下");
    text_center_16(12, line, C_GB3);
    text_center_16(36, view->deck->revision, C_GB2);

    for (int i = 0; i < STUDY_UNIT_COUNT; i++) {
        int col = i & 1;
        int row = i / 2;
        int x = col ? 147 : 13;
        int y = 58 + row * 31;
        uint16_t fill = i == view->selected ? C_GB2 : C_GB0;
        uint16_t text = i == view->selected ? C_GB0 : C_GB3;
        display_fill_rect(x, y, 128, 25, fill);
        display_rect(x, y, 128, 25, C_GB2);
        snprintf(line, sizeof(line), "U%d %s", i + 1, view->deck->unit_titles[i]);
        text_center_16_box(x, 128, y + 5, line, text);
    }

    int learned = count_unit_level(view->progress, view->selected, 1);
    int mastered = count_unit_level(view->progress, view->selected, 3);
    snprintf(line, sizeof(line), "本单元 已学%d/8  掌握%d/8", learned, mastered);
    text_center_16(186, line, C_GB2);
    text_center_16(207, "方向选择  A确认  B返回", C_GB3);
}

static void draw_home(const view_t *view)
{
    char line[48];
    int learned = count_unit_level(view->progress, view->unit, 1);
    int mastered = count_unit_level(view->progress, view->unit, 3);

    draw_frame();
    draw_study_word("WORD QUEST", 26, 2);
    snprintf(line, sizeof(line), "%d年级 %s册  %s", view->deck->grade,
             view->deck->upper ? "上" : "下", view->deck->revision);
    text_center_16(50, line, C_GB2);
    display_hline(38, 82, DISP_FB_W - 76, C_GB2);
    snprintf(line, sizeof(line), "Unit %d  %s", view->unit + 1,
             view->deck->unit_titles[view->unit]);
    text_center_16(92, line, C_GB3);

    snprintf(line, sizeof(line), "已学习 %d/8   已掌握 %d/8", learned, mastered);
    text_center_16(124, line, C_GB2);
    text_center_16(149, "本轮学习教材顺序中的8个词", C_GB2);
    if (!view->storage_ok) {
        text_center_16(176, "进度仅在本次开机有效", C_BAD);
    }
    text_center_16(199, "A 开始   B 换单元", C_GB3);
}

static void draw_card(const view_t *view)
{
    int deck_index = view->session->word[view->pos];
    const study_word_t *word = &view->deck->words[deck_index];
    char line[40];

    draw_frame();
    draw_card_header(view, "LEARN");
    snprintf(line, sizeof(line), "Unit %d  %s", word->unit + 1,
             view->deck->unit_titles[word->unit]);
    text_center_16(50, line, C_GB2);
    draw_study_word(word->word, 94, 3);

    if (view->kind == VIEW_CARD_FRONT) {
        text_center_16(132, "先猜一猜它的意思", C_GB2);
        text_center_16(160, "想好后按 A 翻开", C_GB3);
        text_center_16(199, "A 翻开   SELECT 返回", C_GB3);
    } else {
        snprintf(line, sizeof(line), "意思: %s", word->meaning);
        text_center_16(124, line, C_GB3);
        text_center_16(153, "看英文说中文 再大声读3遍", C_GB2);
        text_center_16(199, "A 下一张   B 再想想", C_GB3);
    }
}

static void draw_quiz(const view_t *view)
{
    int deck_index = view->session->word[view->session->quiz_order[view->pos]];
    const study_word_t *word = &view->deck->words[deck_index];
    char line[40];

    draw_frame();
    draw_card_header(view, "QUIZ");
    draw_study_word(word->word, 61, 2);
    text_center_16(80, "选出正确的中文意思", C_GB2);

    for (int i = 0; i < 3; i++) {
        int y = 103 + i * 29;
        uint16_t fill = C_GB0;
        uint16_t text = C_GB3;
        if (view->answered >= 0 && i == view->correct_slot) {
            fill = C_OK;
            text = C_WHITE;
        } else if (view->answered == 0 && i == view->selected) {
            fill = C_BAD;
            text = C_WHITE;
        } else if (view->answered < 0 && i == view->selected) {
            fill = C_GB2;
            text = C_GB0;
        }
        display_fill_rect(33, y - 4, DISP_FB_W - 66, 24, fill);
        display_rect(33, y - 4, DISP_FB_W - 66, 24, C_GB2);
        /* 用 1/2/3 而不是 A/B/C 标选项：实体手柄也有 A/B/C 丝印，写字母
         * 会误导孩子去按对应面键；这里始终是摇杆上下移动、A 确认。 */
        snprintf(line, sizeof(line), "%d  %s", i + 1,
                 view->deck->words[view->options[i]].meaning);
        text_center_16(y, line, text);
    }

    if (view->answered < 0) {
        text_center_16(199, "上下选择   A 确认", C_GB3);
    } else if (view->answered) {
        text_center_16(199, "答对了!   A 继续", C_OK);
    } else {
        text_center_16(199, "记住绿色答案   A 继续", C_BAD);
    }
}

static void draw_result(const view_t *view)
{
    char line[48];
    int mastered = count_unit_level(view->progress, view->unit, 3);

    draw_frame();
    text_center_latin_16(15, "ROUND COMPLETE", C_GB3, 1);
    text_center_16(52, "本轮得分", C_GB2);
    snprintf(line, sizeof(line), "%d / %d", view->score, SESSION_WORDS);
    text_center_ascii(76, line, view->score >= 6 ? C_OK : C_GB3, 4);

    for (int i = 0; i < SESSION_WORDS; i++) {
        uint16_t color = i < view->score ? C_GOLD : C_GB1;
        display_fill_rect(48 + i * 24, 118, 16, 8, color);
    }
    if (view->score == SESSION_WORDS) {
        text_center_16(139, "太棒了 全部答对!", C_OK);
    } else if (view->score >= 6) {
        text_center_16(139, "很好 错词下轮会再来", C_GB3);
    } else {
        text_center_16(139, "慢慢来 记牢比求快重要", C_GB3);
    }
    snprintf(line, sizeof(line), "本单元已掌握 %d/%d", mastered, SESSION_WORDS);
    text_center_16(169, line, C_GB2);
    text_center_16(199, "A 再来一轮   B 返回", C_GB3);
}

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    (void)strip;
    (void)y0;
    (void)h;
    const view_t *view = ctx;
    switch (view->kind) {
    case VIEW_SELECT:     draw_select(view); break;
    case VIEW_UNIT_SELECT: draw_unit_select(view); break;
    case VIEW_HOME:       draw_home(view);   break;
    case VIEW_CARD_FRONT:
    case VIEW_CARD_BACK:  draw_card(view);   break;
    case VIEW_QUIZ:       draw_quiz(view);   break;
    case VIEW_RESULT:     draw_result(view); break;
    }
}

static void render(view_t *view)
{
    display_stream_sync(draw_strip, view);
}

static void progress_default(study_progress_t *progress)
{
    memset(progress, 0, sizeof(*progress));
    progress->magic = PROGRESS_MAGIC;
    progress->version = PROGRESS_VERSION;
    progress->word_count = WORD_COUNT;
}

static void progress_load(study_progress_t *progress, const study_deck_t *deck)
{
    progress_default(progress);
    s_storage_ok = false;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* 不照常见示例那样在 NO_FREE_PAGES 时整区 erase：NVS 是公共分区，
         * 为学习进度清空未来可能加入的设置，比这次不保存更糟。 */
        ESP_LOGW(TAG, "NVS 未就绪，学习进度只在本次开机有效：%s", esp_err_to_name(err));
        return;
    }
    err = nvs_open("word_study", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "学习进度空间打不开：%s", esp_err_to_name(err));
        return;
    }
    s_storage_ok = true;

    study_progress_t saved;
    size_t size = sizeof(saved);
    err = nvs_get_blob(s_nvs, deck->progress_key, &saved, &size);
    if (err == ESP_OK && size == sizeof(saved) && saved.magic == PROGRESS_MAGIC &&
        saved.version == PROGRESS_VERSION && saved.word_count == WORD_COUNT &&
        saved.new_cursor < WORD_COUNT && saved.review_cursor < WORD_COUNT) {
        bool levels_ok = true;
        for (int i = 0; i < WORD_COUNT; i++) {
            if (saved.level[i] > 3) levels_ok = false;
        }
        if (levels_ok) *progress = saved;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "旧学习进度格式不兼容，从第一课开始");
    }
}

static void progress_save(const study_progress_t *progress, const study_deck_t *deck)
{
    if (!s_storage_ok) return;
    esp_err_t err = nvs_set_blob(s_nvs, deck->progress_key, progress, sizeof(*progress));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "学习进度保存失败：%s", esp_err_to_name(err));
    }
}

static void session_build(study_session_t *session, int unit)
{
    int first = unit * SESSION_WORDS;
    for (int i = 0; i < SESSION_WORDS; i++) {
        session->word[i] = first + i;
        session->quiz_order[i] = i;
    }
    for (int i = SESSION_WORDS - 1; i > 0; i--) {
        int j = random_next() % (i + 1);
        int t = session->quiz_order[i];
        session->quiz_order[i] = session->quiz_order[j];
        session->quiz_order[j] = t;
    }
}

static void quiz_options(const study_session_t *session, int quiz_pos, view_t *view)
{
    int correct_pos = session->quiz_order[quiz_pos];
    int other1;
    int other2;
    do other1 = random_next() % SESSION_WORDS; while (other1 == correct_pos);
    do other2 = random_next() % SESSION_WORDS;
    while (other2 == correct_pos || other2 == other1);

    view->correct_slot = random_next() % 3;
    int distractor = 0;
    for (int i = 0; i < 3; i++) {
        int session_pos;
        if (i == view->correct_slot) {
            session_pos = correct_pos;
        } else {
            session_pos = distractor++ == 0 ? other1 : other2;
        }
        view->options[i] = session->word[session_pos];
    }
}

/* false 表示 SELECT 中途退出。已答过的题仍会保存，避免孩子完成一半后白做。 */
static bool run_session(study_progress_t *progress, view_t *view)
{
    study_session_t session;
    session_build(&session, view->unit);
    view->session = &session;
    bool dirty = false;

    for (int pos = 0; pos < SESSION_WORDS; pos++) {
        view->pos = pos;
        view->kind = VIEW_CARD_FRONT;
        render(view);
        uint16_t prev = poll_input();
        bool revealed = false;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & NES_PAD_SELECT) {
                return false;
            }
            if (!revealed && (edge & (NES_PAD_A | NES_PAD_START))) {
                revealed = true;
                view->kind = VIEW_CARD_BACK;
                render(view);
            } else if (revealed && (edge & NES_PAD_B)) {
                revealed = false;
                view->kind = VIEW_CARD_FRONT;
                render(view);
            } else if (revealed && (edge & (NES_PAD_A | NES_PAD_START))) {
                break;
            }
        }
    }

    view->score = 0;
    for (int pos = 0; pos < SESSION_WORDS; pos++) {
        view->kind = VIEW_QUIZ;
        view->pos = pos;
        view->selected = 0;
        view->answered = -1;
        quiz_options(&session, pos, view);
        render(view);

        uint16_t prev = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & NES_PAD_SELECT) {
                if (dirty) progress_save(progress, view->deck);
                return false;
            }
            if (view->answered < 0 && (edge & (NES_PAD_UP | NES_PAD_DOWN))) {
                int delta = (edge & NES_PAD_UP) ? -1 : 1;
                view->selected = (view->selected + delta + 3) % 3;
                render(view);
            } else if (view->answered < 0 && (edge & (NES_PAD_A | NES_PAD_START))) {
                int session_pos = session.quiz_order[pos];
                int deck_index = session.word[session_pos];
                bool correct = view->selected == view->correct_slot;
                view->answered = correct ? 1 : 0;
                if (correct) {
                    view->score++;
                    if (progress->level[deck_index] < 3) progress->level[deck_index]++;
                } else {
                    progress->level[deck_index] = progress->level[deck_index] > 1
                                                ? progress->level[deck_index] - 1 : 1;
                }
                dirty = true;
                render(view);
            } else if (view->answered >= 0 && (edge & (NES_PAD_A | NES_PAD_START))) {
                break;
            }
        }
    }

    progress->rounds++;
    progress_save(progress, view->deck);
    view->kind = VIEW_RESULT;
    render(view);
    return true;
}

static void run_unit(const study_deck_t *deck, int unit, study_progress_t *progress)
{
    view_t view = {
        .kind = VIEW_HOME,
        .deck = deck,
        .progress = progress,
        .unit = unit,
        .storage_ok = s_storage_ok,
    };

    while (1) {
        view.kind = VIEW_HOME;
        render(&view);
        uint16_t prev = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & (NES_PAD_B | NES_PAD_SELECT)) {
                return;
            }
            if (edge & (NES_PAD_A | NES_PAD_START)) break;
        }

        if (!run_session(progress, &view)) continue;

        uint16_t prev_result = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev_result;
            prev_result = now;
            if (edge & (NES_PAD_B | NES_PAD_SELECT)) break;
            if (edge & (NES_PAD_A | NES_PAD_START)) {
                if (run_session(progress, &view)) {
                    prev_result = poll_input();
                    continue;
                }
                break;
            }
        }
    }
}

static void run_deck(const study_deck_t *deck)
{
    study_progress_t progress;
    progress_load(&progress, deck);

    ESP_LOGI(TAG, "%d年级%s册（%s）：%d 个核心词，已掌握 %d 个",
             deck->grade, deck->upper ? "上" : "下", deck->revision,
             WORD_COUNT, count_level(&progress, 3));

    view_t view = {
        .kind = VIEW_UNIT_SELECT,
        .deck = deck,
        .progress = &progress,
        .selected = 0,
        .storage_ok = s_storage_ok,
    };

    while (1) {
        view.kind = VIEW_UNIT_SELECT;
        render(&view);
        uint16_t prev = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & (NES_PAD_B | NES_PAD_SELECT)) {
                if (s_storage_ok) {
                    nvs_close(s_nvs);
                    s_storage_ok = false;
                }
                return;
            }
            if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
                view.selected ^= 1;
                render(&view);
            } else if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
                int delta = (edge & NES_PAD_UP) ? -2 : 2;
                view.selected = (view.selected + delta + STUDY_UNIT_COUNT) % STUDY_UNIT_COUNT;
                render(&view);
            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                ESP_LOGI(TAG, "选择 Unit %d（%s），已学习 %d/8，已掌握 %d/8",
                         view.selected + 1, deck->unit_titles[view.selected],
                         count_unit_level(&progress, view.selected, 1),
                         count_unit_level(&progress, view.selected, 3));
                run_unit(deck, view.selected, &progress);
                break;
            }
        }
    }
}

void word_study_run(void)
{
    s_rng = (uint32_t)esp_timer_get_time() ^ 0xA53C9E17u;
    if (!s_rng) s_rng = 1;

    view_t view = {
        .kind = VIEW_SELECT,
        .selected = 0,
    };

    while (1) {
        view.kind = VIEW_SELECT;
        render(&view);
        uint16_t prev = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & (NES_PAD_B | NES_PAD_SELECT)) return;
            if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
                view.selected ^= 1;
                render(&view);
            } else if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
                int delta = (edge & NES_PAD_UP) ? -2 : 2;
                view.selected = (view.selected + delta + STUDY_DECK_COUNT) % STUDY_DECK_COUNT;
                render(&view);
            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                run_deck(&STUDY_DECKS[view.selected]);
                break;
            }
        }
    }
}
