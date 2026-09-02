/*
 * 面向小学三至六年级的离线单词学习模式。
 *
 * 一轮学习所选单元的完整词表：先逐张认识，再用同一组词做三选一。这个顺序
 * 故意不让孩子一上来就猜陌生词；选项也只来自刚看过的同单元词，测的是短时提取，
 * 不是靠排除完全陌生的干扰项碰运气。
 *
 * 先选教材，再选 Unit；每单元词数来自教材数据，不把别的单元混进来。
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
#include "word_audio.h"

static const char *TAG = "words";

#define POLL_MS          16
#define PROGRESS_MAGIC    0x574F5244u  /* "WORD" */
#define PROGRESS_VERSION  3

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t word_count;
    uint16_t new_cursor;
    uint16_t review_cursor;
    uint32_t rounds;
    uint8_t level[STUDY_DECK_WORDS_MAX];
} study_progress_t;

/* v2 固定 64 词。完整三上启用 127 词时只更换三上的 NVS 键；其余七册可从
 * 旧结构按原索引迁移，避免一次数据结构升级清空所有已学进度。 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t word_count;
    uint16_t new_cursor;
    uint16_t review_cursor;
    uint32_t rounds;
    uint8_t level[STUDY_STANDARD_DECK_WORDS];
} study_progress_v2_t;

typedef struct {
    int word[STUDY_UNIT_WORDS_MAX];
    int quiz_order[STUDY_UNIT_WORDS_MAX];
    int8_t quiz_result[STUDY_UNIT_WORDS_MAX];
    int count;
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
    bool audio_ok;
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

    /* 浅色阴影把深色主字从底色里托起来，但仍保持像素掌机风格。 */
    display_text_ascii_16_scaled(x + shadow, y + shadow, s, C_UI_PANEL_ALT, scale);
    display_text_ascii_16_scaled(x, y, s, C_UI_FG, scale);
}

static void draw_frame(void)
{
    display_clear(C_UI_BG);
    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);
}

static void unit_word_range(const study_deck_t *deck, int unit,
                            int *first, int *count)
{
    int begin = -1;
    int total = 0;
    for (int i = 0; i < deck->word_count; i++) {
        if (deck->words[i].unit != unit) continue;
        if (begin < 0) begin = i;
        total++;
    }
    *first = begin < 0 ? 0 : begin;
    *count = total;
}

static int unit_word_count(const study_deck_t *deck, int unit)
{
    int first;
    int count;
    unit_word_range(deck, unit, &first, &count);
    return count;
}

static int count_level(const study_progress_t *progress, int minimum)
{
    int count = 0;
    for (int i = 0; i < progress->word_count; i++) {
        if (progress->level[i] >= minimum) count++;
    }
    return count;
}

static int count_unit_level(const study_progress_t *progress,
                            const study_deck_t *deck, int unit, int minimum)
{
    int count = 0;
    int first;
    int unit_count;
    unit_word_range(deck, unit, &first, &unit_count);
    for (int i = first; i < first + unit_count; i++) {
        if (progress->level[i] >= minimum) count++;
    }
    return count;
}

static void draw_segment_bar(const study_session_t *session, int current,
                             bool show_quiz_results, int y, int height)
{
    int count = session->count;
    int gap = count <= 8 ? 6 : 2;
    int available = count <= 8 ? 146 : DISP_FB_W - 36;
    int width = (available - gap * (count - 1)) / count;
    int used = width * count + gap * (count - 1);
    int x0 = (DISP_FB_W - used) / 2;

    for (int i = 0; i < count; i++) {
        uint16_t color;
        if (show_quiz_results && session->quiz_result[i] >= 0) {
            color = session->quiz_result[i] ? C_UI_OK : C_UI_BAD;
        } else {
            color = i < current ? C_UI_GOLD
                                : (i == current ? C_UI_FG : C_UI_LINE);
        }
        display_fill_rect(x0 + i * (width + gap), y, width, height, color);
    }
}

static void draw_card_header(const view_t *view, const char *title)
{
    text_center_latin_16(7, title, C_UI_FG, 1);
    /* 最多 31 格（完整三上 Unit 7），按单元词数自动压缩，仍保留逐题红绿反馈。 */
    draw_segment_bar(view->session, view->pos, view->kind == VIEW_QUIZ, 31, 4);
    display_hline(18, 42, DISP_FB_W - 36, C_UI_LINE);
}

static void draw_select(const view_t *view)
{
    char line[40];
    draw_frame();
    text_center_latin_16(10, "YILIN ENGLISH", C_UI_FG, 1);
    text_center_16(37, "苏州小学 选择教材", C_UI_FG_DIM);

    for (int i = 0; i < STUDY_DECK_COUNT; i++) {
        int col = i & 1;
        int row = i / 2;
        int x = col ? 147 : 13;
        int y = 63 + row * 31;
        bool active = i == view->selected;
        display_fill_rect(x, y, 128, 25, active ? C_UI_SEL : C_UI_PANEL);
        display_rect(x, y, 128, 25, active ? C_UI_SEL_EDGE : C_UI_EDGE);
        uint16_t text = active ? C_UI_FG_INV : C_UI_FG;
        snprintf(line, sizeof(line), "%d年级 %s册", STUDY_DECKS[i].grade,
                 STUDY_DECKS[i].upper ? "上" : "下");
        text_center_16_box(x, 128, y + 5, line, text);
    }

    snprintf(line, sizeof(line), "当前: %s", STUDY_DECKS[view->selected].revision);
    text_center_16(190, line, C_UI_FG_DIM);
    text_center_16(207, "方向选择  A确认  B返回", C_UI_FG_FAINT);
}

static void draw_unit_select(const view_t *view)
{
    char line[48];
    draw_frame();
    snprintf(line, sizeof(line), "%d年级%s册  选择单元", view->deck->grade,
             view->deck->upper ? "上" : "下");
    text_center_16(12, line, C_UI_FG);
    text_center_16(36, view->deck->revision, C_UI_FG_DIM);

    for (int i = 0; i < STUDY_UNIT_COUNT; i++) {
        int col = i & 1;
        int row = i / 2;
        int x = col ? 147 : 13;
        int y = 58 + row * 31;
        bool active = i == view->selected;
        display_fill_rect(x, y, 128, 25, active ? C_UI_SEL : C_UI_PANEL);
        display_rect(x, y, 128, 25, active ? C_UI_SEL_EDGE : C_UI_EDGE);
        uint16_t text = active ? C_UI_FG_INV : C_UI_FG;
        snprintf(line, sizeof(line), "U%d %s", i + 1, view->deck->unit_titles[i]);
        text_center_16_box(x, 128, y + 5, line, text);
    }

    int unit_count = unit_word_count(view->deck, view->selected);
    int learned = count_unit_level(view->progress, view->deck, view->selected, 1);
    int mastered = count_unit_level(view->progress, view->deck, view->selected, 3);
    snprintf(line, sizeof(line), "本单元 已学%d/%d  掌握%d/%d",
             learned, unit_count, mastered, unit_count);
    text_center_16(186, line, C_UI_FG_DIM);
    text_center_16(207, "方向选择  A确认  B返回", C_UI_FG_FAINT);
}

static void draw_home(const view_t *view)
{
    char line[48];
    int unit_count = unit_word_count(view->deck, view->unit);
    int learned = count_unit_level(view->progress, view->deck, view->unit, 1);
    int mastered = count_unit_level(view->progress, view->deck, view->unit, 3);

    draw_frame();
    draw_study_word("WORD QUEST", 26, 2);
    snprintf(line, sizeof(line), "%d年级 %s册  %s", view->deck->grade,
             view->deck->upper ? "上" : "下", view->deck->revision);
    text_center_16(50, line, C_UI_FG_DIM);
    display_hline(38, 82, DISP_FB_W - 76, C_UI_LINE);
    snprintf(line, sizeof(line), "Unit %d  %s", view->unit + 1,
             view->deck->unit_titles[view->unit]);
    text_center_16(92, line, C_UI_FG);

    snprintf(line, sizeof(line), "已学习 %d/%d   已掌握 %d/%d",
             learned, unit_count, mastered, unit_count);
    text_center_16(124, line, C_UI_INFO);
    snprintf(line, sizeof(line), "本轮学习本单元全部%d项", unit_count);
    text_center_16(149, line, C_UI_FG_DIM);
    if (!view->audio_ok) {
        text_center_16(176, "英式发音包未安装 学习仍可继续", C_UI_WARN);
    } else if (!view->storage_ok) {
        text_center_16(176, "进度仅在本次开机有效", C_UI_WARN);
    }
    text_center_16(199, "A 开始   B 换单元", C_UI_FG_FAINT);
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
    text_center_16(50, line, C_UI_FG_DIM);
    draw_study_word(word->word, 94, 3);

    if (view->kind == VIEW_CARD_FRONT) {
        text_center_16(132, "先猜一猜它的意思", C_UI_FG_DIM);
        text_center_16(160, "想好后按 A 翻开", C_UI_FG);
        text_center_16(199, view->audio_ok ? "A 翻开  X 发音  SELECT 返回"
                                           : "A 翻开   SELECT 返回", C_UI_FG_FAINT);
    } else {
        snprintf(line, sizeof(line), "意思: %s", word->meaning);
        text_center_16(124, line, C_UI_FG);
        text_center_16(153, "看英文说中文 再大声读3遍", C_UI_FG_DIM);
        text_center_16(199, view->audio_ok ? "A 下一张  B 再想  X 发音"
                                           : "A 下一张   B 再想想", C_UI_FG_FAINT);
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
    text_center_16(80, "选出正确的中文意思", C_UI_FG_DIM);

    for (int i = 0; i < 3; i++) {
        int y = 103 + i * 29;
        uint16_t fill = C_UI_PANEL;
        uint16_t edge = C_UI_EDGE;
        uint16_t text = C_UI_FG;
        if (view->answered >= 0 && i == view->correct_slot) {
            fill = C_UI_OK;
            edge = C_UI_SEL_EDGE;
            text = C_UI_FG_INV;
        } else if (view->answered == 0 && i == view->selected) {
            fill = C_UI_BAD;
            edge = C_UI_SEL_EDGE;
            text = C_UI_FG_INV;
        } else if (view->answered < 0 && i == view->selected) {
            fill = C_UI_SEL;
            edge = C_UI_SEL_EDGE;
            text = C_UI_FG_INV;
        }
        display_fill_rect(33, y - 4, DISP_FB_W - 66, 24, fill);
        display_rect(33, y - 4, DISP_FB_W - 66, 24, edge);
        /* 用 1/2/3 而不是 A/B/C 标选项：实体手柄也有 A/B/C 丝印，写字母
         * 会误导孩子去按对应面键；这里始终是摇杆上下移动、A 确认。 */
        snprintf(line, sizeof(line), "%d  %s", i + 1,
                 view->deck->words[view->options[i]].meaning);
        text_center_16(y, line, text);
    }

    if (view->answered < 0) {
        text_center_16(199, view->audio_ok ? "上下选择  A 确认  X 发音"
                                           : "上下选择   A 确认", C_UI_FG_FAINT);
    } else if (view->answered) {
        text_center_16(199, "答对了!   A 继续", C_UI_OK);
    } else {
        text_center_16(199, "记住绿色答案   A 继续", C_UI_BAD);
    }
}

static void draw_result(const view_t *view)
{
    char line[48];
    int mastered = count_unit_level(view->progress, view->deck, view->unit, 3);
    int count = view->session->count;
    bool strong_score = view->score * 4 >= count * 3;

    draw_frame();
    text_center_latin_16(15, "ROUND COMPLETE", C_UI_FG, 1);
    text_center_16(52, "本轮得分", C_UI_FG_DIM);
    snprintf(line, sizeof(line), "%d / %d", view->score, count);
    text_center_ascii(76, line, strong_score ? C_UI_OK : C_UI_FG, 4);

    draw_segment_bar(view->session, count, true, 118, 8);
    if (view->score == count) {
        text_center_16(139, "太棒了 全部答对!", C_UI_OK);
    } else if (strong_score) {
        text_center_16(139, "很好 错词下轮会再来", C_UI_FG);
    } else {
        text_center_16(139, "慢慢来 记牢比求快重要", C_UI_FG);
    }
    snprintf(line, sizeof(line), "本单元已掌握 %d/%d", mastered, count);
    text_center_16(169, line, C_UI_INFO);
    text_center_16(199, "A 再来一轮   B 返回", C_UI_FG_FAINT);
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

static void progress_default(study_progress_t *progress, const study_deck_t *deck)
{
    memset(progress, 0, sizeof(*progress));
    progress->magic = PROGRESS_MAGIC;
    progress->version = PROGRESS_VERSION;
    progress->word_count = deck->word_count;
}

static void progress_load(study_progress_t *progress, const study_deck_t *deck)
{
    progress_default(progress, deck);
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

    size_t size = 0;
    err = nvs_get_blob(s_nvs, deck->progress_key, NULL, &size);
    if (err == ESP_OK && size == sizeof(study_progress_t)) {
        study_progress_t saved;
        err = nvs_get_blob(s_nvs, deck->progress_key, &saved, &size);
        bool levels_ok = true;
        if (err == ESP_OK) {
            for (int i = 0; i < deck->word_count; i++) {
                if (saved.level[i] > 3) levels_ok = false;
            }
        } else {
            levels_ok = false;
        }
        if (err == ESP_OK && saved.magic == PROGRESS_MAGIC &&
            saved.version == PROGRESS_VERSION &&
            saved.word_count == deck->word_count &&
            saved.new_cursor < deck->word_count &&
            saved.review_cursor < deck->word_count && levels_ok) {
            *progress = saved;
        } else {
            ESP_LOGW(TAG, "学习进度内容不兼容，从第一课开始");
        }
    } else if (err == ESP_OK && size == sizeof(study_progress_v2_t) &&
               deck->word_count == STUDY_STANDARD_DECK_WORDS) {
        study_progress_v2_t saved;
        err = nvs_get_blob(s_nvs, deck->progress_key, &saved, &size);
        if (err == ESP_OK && saved.magic == PROGRESS_MAGIC && saved.version == 2 &&
            saved.word_count == STUDY_STANDARD_DECK_WORDS) {
            bool levels_ok = true;
            for (int i = 0; i < STUDY_STANDARD_DECK_WORDS; i++) {
                if (saved.level[i] > 3) levels_ok = false;
            }
            if (levels_ok) {
                memcpy(progress->level, saved.level, sizeof(saved.level));
                progress->new_cursor = saved.new_cursor;
                progress->review_cursor = saved.review_cursor;
                progress->rounds = saved.rounds;
                ESP_LOGI(TAG, "学习进度已从 v2 无损迁移到可变词表格式");
            }
        }
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

static void session_build(study_session_t *session,
                          const study_deck_t *deck, int unit)
{
    int first;
    unit_word_range(deck, unit, &first, &session->count);
    for (int i = 0; i < session->count; i++) {
        session->word[i] = first + i;
        session->quiz_order[i] = i;
        session->quiz_result[i] = -1;
    }
    for (int i = session->count - 1; i > 0; i--) {
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
    do other1 = random_next() % session->count; while (other1 == correct_pos);
    do other2 = random_next() % session->count;
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
    session_build(&session, view->deck, view->unit);
    view->session = &session;
    bool dirty = false;

    for (int pos = 0; pos < session.count; pos++) {
        view->pos = pos;
        view->kind = VIEW_CARD_FRONT;
        render(view);
        int deck_index = session.word[pos];
        word_audio_play(view->deck->words[deck_index].word);
        uint16_t prev = poll_input();
        bool revealed = false;
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & GAMEPAD_BIT_X) {
                word_audio_play(view->deck->words[deck_index].word);
            }
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
    for (int pos = 0; pos < session.count; pos++) {
        view->kind = VIEW_QUIZ;
        view->pos = pos;
        view->selected = 0;
        view->answered = -1;
        quiz_options(&session, pos, view);
        render(view);
        int quiz_deck_index = session.word[session.quiz_order[pos]];

        uint16_t prev = poll_input();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            uint16_t now = poll_input();
            uint16_t edge = now & ~prev;
            prev = now;
            if (edge & GAMEPAD_BIT_X) {
                word_audio_play(view->deck->words[quiz_deck_index].word);
            }
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
                session.quiz_result[pos] = correct ? 1 : 0;
                if (correct) {
                    view->score++;
                    if (progress->level[deck_index] < 3) progress->level[deck_index]++;
                } else {
                    progress->level[deck_index] = progress->level[deck_index] > 1
                                                ? progress->level[deck_index] - 1 : 1;
                }
                word_audio_play_quiz_result(correct);
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
    if (view->score == session.count) word_audio_play_quiz_perfect();
    return true;
}

static void run_unit(const study_deck_t *deck, int unit, study_progress_t *progress)
{
    int unit_count = unit_word_count(deck, unit);
    if (unit_count < 3 || unit_count > STUDY_UNIT_WORDS_MAX) {
        ESP_LOGE(TAG, "Unit %d 词数 %d 超出可测验范围 3..%d",
                 unit + 1, unit_count, STUDY_UNIT_WORDS_MAX);
        return;
    }

    view_t view = {
        .kind = VIEW_HOME,
        .deck = deck,
        .progress = progress,
        .unit = unit,
        .storage_ok = s_storage_ok,
        .audio_ok = word_audio_is_available(),
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

    ESP_LOGI(TAG, "%d年级%s册（%s）：%u 个词条，已掌握 %d 个",
             deck->grade, deck->upper ? "上" : "下", deck->revision,
             (unsigned)deck->word_count, count_level(&progress, 3));

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
                int unit_count = unit_word_count(deck, view.selected);
                ESP_LOGI(TAG, "选择 Unit %d（%s），已学习 %d/%d，已掌握 %d/%d",
                         view.selected + 1, deck->unit_titles[view.selected],
                         count_unit_level(&progress, deck, view.selected, 1), unit_count,
                         count_unit_level(&progress, deck, view.selected, 3), unit_count);
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
    (void)word_audio_init();

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
            if (edge & (NES_PAD_B | NES_PAD_SELECT)) {
                word_audio_shutdown();
                return;
            }
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
