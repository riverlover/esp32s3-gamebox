/*
 * 开机选单的实现
 *
 * ---- 两级结构 ----
 *
 * 开机页 <--B-- 分类页（选平台）--A--> 游戏列表（选游戏）--A--> 启动
 *                  ^                      |
 *                  +----------B-----------+
 *
 * 两页共用同一个轮询循环、同一份边沿检测和同一个条带回调。平台页用 2x3
 * 卡片网格，游戏页用带编号徽标的列表；draw_*() 各自填好 draw_args_t 快照，
 * 回调只负责按当前页面类型绘制。
 *
 * B 在两页都只有“返回上一级”一个含义，避免孩子记两套规则。音量、亮度和
 * 手柄测试统一收进开机主页的 SETTINGS，不再把 X/Y 变成选单专用键。
 *
 * ---- 布局 ----
 *
 * 画布是 288x224（NES 画布尺寸，居中在 320x240 的屏上，见 display.h）。
 * 游戏列表每页 8 项 x 21px 行距 = 168px，给选中框和分隔线留出呼吸感；
 * 平台页最多 6 类，正好排成 2 列 x 3 行。
 *
 * 选中项用反白（填充块 + 浅色字），比单独一个箭头更醒目。中文是 16x16
 * 点阵，有限的 288x224 画布仍能同时容纳标题、8 行列表和操作提示。
 *
 * ---- 为什么要边沿检测 ----
 *
 * 摇杆报的是**状态**不是事件：推着不动，每次 poll 都返回 UP。直接拿它移动
 * 光标的话，一次推杆会在几毫秒里把光标扫到底。所以只在「上一帧没按、这一帧
 * 按了」的瞬间移动一格。
 *
 * 串口那路同理，而且它本来就没有「松手」事件（靠终端的按键重复维持按下），
 * 边沿检测对两路都是必须的。
 */

#include <string.h>
#include "rom_menu.h"
#include "rom_store.h"
#include "display.h"
#include "input_serial.h"
#include "input_gamepad.h"
#include "input_usb.h"
#include "nofrendo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "menu";

#define TITLE_Y        5
#define HEADER_LINE_Y  25
#define LIST_Y         32      /* 第一行文字基线 */
#define ROW_H          21      /* 16px 点阵 + 选中框和分隔线 */
#define PAGE_ROWS      8
#define FOOTER_LINE_Y  204
#define FOOTER_Y       207
#define TEXT_X         8       /* 文字左缩进 */

#define CARD_MARGIN_X  8
#define CARD_GAP_X     8
#define CARD_GAP_Y     6
#define CARD_W         132
#define CARD_H         48
#define CARD_Y         35

/* 六个平台，正好填满平台页的 2 列 x 3 行卡片网格。ZIP 不再单列一类
 * （见 rom_store.h 的 ROM_SYSTEM_LAST 注释）。 */
#define SYSTEM_COUNT   6

#define POLL_MS     16      /* 约 60 Hz，和游戏帧率一个量级 */

static uint16_t poll_input(void)
{
    return input_serial_poll() | input_gamepad_poll() | input_usb_poll();
}

typedef struct {
    rom_system_t system;
    int          count;
} category_t;

/* 条带回调的输入。整份绘制列表每帧会被逐条带调用 BAND_COUNT 次，每次只画到
 * 落在当前条带里的那几行（display.c 的绘图原语自己裁）。菜单只有按键时才
 * 重画，重复执行这段的开销可以忽略。
 *
 * ctx 指向调用方栈上的这份快照，所以画的时候必须用 display_stream_sync()
 * 等推完再返回。 */
typedef struct {
    const char *title;
    const char *footer;
    char  meta[32];          /* 标题右侧：总数或总数 + 页码 */
    bool  category_grid;
    int   page;             /* 0 基；page_count <= 1 时整个页码都不画 */
    int   page_count;
    int   sel_row;          /* 反白哪一行（页内行号） */
    int   row_count;
    int   first_number;     /* 游戏列表第一项的 1 基编号 */
    rom_system_t systems[SYSTEM_COUNT];
    int   counts[SYSTEM_COUNT];
    char  lines[PAGE_ROWS][64];
} draw_args_t;

static const char *system_name(rom_system_t system)
{
    if (system == ROM_SYSTEM_SNES) return "SNES";
    if (system == ROM_SYSTEM_GENESIS) return "MD";
    if (system == ROM_SYSTEM_GBC) return "GBC";
    if (system == ROM_SYSTEM_GB) return "GB";
    if (system == ROM_SYSTEM_PCE) return "PCE";
    return "NES";
}

/* 平台色见 display.h 的 C_SYS_*。和 system_name() 分成两个函数而不是一张
 * 结构体表：这两处的 switch 顺序不一样（名字按长度、颜色按平台世代），
 * 合表反而要多记一层对应关系。 */
static uint16_t system_color(rom_system_t system)
{
    switch (system) {
    case ROM_SYSTEM_GB:      return C_SYS_GB;
    case ROM_SYSTEM_GBC:     return C_SYS_GBC;
    case ROM_SYSTEM_SNES:    return C_SYS_SNES;
    case ROM_SYSTEM_GENESIS: return C_SYS_GENESIS;
    case ROM_SYSTEM_PCE:     return C_SYS_PCE;
    default:                 return C_SYS_NES;
    }
}

static void draw_strip(uint16_t *strip, int y0, int h, void *ctx)
{
    const draw_args_t *a = ctx;

    /* 配色全部走 display.h 的语义层，别在这里直接写 C_GVB_* 或新色。 */
    display_clear(C_UI_BG);

    display_rect(0, 0, DISP_FB_W, DISP_FB_H, C_UI_EDGE);
    display_text_16(TEXT_X, TITLE_Y, a->title, C_UI_FG);
    int meta_x = DISP_FB_W - TEXT_X - display_text_width_16(a->meta);
    display_text_16(meta_x, TITLE_Y, a->meta, C_UI_FG_DIM);
    display_fill_rect(TEXT_X, HEADER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_UI_LINE);

    if (a->category_grid) {
        for (int i = 0; i < a->row_count; i++) {
            int col = i % 2;
            int row = i / 2;
            int x = CARD_MARGIN_X + col * (CARD_W + CARD_GAP_X);
            int y = CARD_Y + row * (CARD_H + CARD_GAP_Y);
            bool active = i == a->sel_row;
            uint16_t accent = system_color(a->systems[i]);

            /* 选中的卡片整块铺平台色，没选中的只在左边留一条 5px 色条：
               六张卡同时铺满色会花，色条已经够认平台。 */
            display_fill_rect(x, y, CARD_W, CARD_H, active ? accent : C_UI_PANEL);
            display_rect(x, y, CARD_W, CARD_H,
                         active ? C_UI_SEL_EDGE : C_UI_EDGE);
            if (!active) display_fill_rect(x + 1, y + 1, 5, CARD_H - 2, accent);

            display_text_16(x + 13, y + 6, system_name(a->systems[i]),
                            active ? C_UI_FG_INV : C_UI_FG);

            char count_text[24];
            snprintf(count_text, sizeof(count_text), "%d GAMES", a->counts[i]);
            display_text_16(x + 13, y + 27, count_text,
                            active ? C_UI_FG_INV_DIM : C_UI_FG_DIM);
        }
    } else {
        for (int row = 0; row < a->row_count; row++) {
            int y = LIST_Y + row * ROW_H;
            bool active = row == a->sel_row;

            if (active) {
                display_fill_rect(6, y - 3, DISP_FB_W - 12, ROW_H - 1, C_UI_SEL);
            } else if (row + 1 < a->row_count) {
                display_fill_rect(TEXT_X, y + 17, DISP_FB_W - 2 * TEXT_X, 1,
                                  C_UI_LINE);
            }

            /* 编号徽标在选中行里要比行底更深才分得出来，所以用 SEL_EDGE。 */
            display_fill_rect(TEXT_X, y - 1, 31, 17,
                              active ? C_UI_SEL_EDGE : C_UI_PANEL_ALT);
            char number[8];
            snprintf(number, sizeof(number), "%02d", a->first_number + row);
            display_text_16(TEXT_X + 6, y, number,
                            active ? C_UI_FG_INV : C_UI_FG_DIM);
            display_text_16(46, y, a->lines[row],
                            active ? C_UI_FG_INV : C_UI_FG);
            if (active) display_text_16(DISP_FB_W - 17, y, ">", C_UI_FG_INV);
        }
    }

    display_fill_rect(TEXT_X, FOOTER_LINE_Y, DISP_FB_W - 2 * TEXT_X, 1,
                      C_UI_LINE);
    display_text_16((DISP_FB_W - display_text_width_16(a->footer)) / 2, FOOTER_Y,
                    a->footer, C_UI_FG_FAINT);
}

/* 统计每个平台各有多少游戏，返回分类数。
 *
 * 故意不记录「起始下标 + 长度」：那等于把「同平台条目在 rom_store 里连续」
 * 变成硬约束。目前 pack_roms.py 确实按 system 排过序，但没必要让菜单依赖它
 * ——下面 nth_of_system() 每次线性扫描，条目只有几十个，省下的复杂度更值。 */
static int build_categories(int count, category_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;

        int k = 0;
        while (k < n && out[k].system != e->system) k++;
        if (k == n) {
            if (n >= max) continue;     /* 不该发生：枚举就五个取值 */
            out[n].system = e->system;
            out[n].count  = 0;
            n++;
        }
        out[k].count++;
    }
    return n;
}

/* 平台 system 的第 j 个游戏在 rom_store 里的下标；没有就返回 -1。 */
static int nth_of_system(int count, rom_system_t system, int j)
{
    for (int i = 0; i < count; i++) {
        const rom_store_entry_t *e = rom_store_entry(i);
        if (!e) break;
        if (e->system == system && j-- == 0) return i;
    }
    return -1;
}

static void draw_categories(const category_t *cats, int cat_count, int sel)
{
    draw_args_t a = {
        .title       = "选择游戏平台",
        .footer      = "方向选择  A进入  B返回",
        .category_grid = true,
        .page        = 0,
        .page_count  = 1,
        .sel_row     = sel,
        .row_count   = cat_count,
    };

    int total = 0;
    for (int i = 0; i < a.row_count; i++) {
        a.systems[i] = cats[i].system;
        a.counts[i] = cats[i].count;
        total += cats[i].count;
    }
    snprintf(a.meta, sizeof(a.meta), "%d GAMES", total);
    display_stream_sync(draw_strip, &a);
}

static void draw_games(int count, const category_t *cat, int sel)
{
    int page = sel / PAGE_ROWS;
    int first = page * PAGE_ROWS;
    int last = first + PAGE_ROWS;
    if (last > cat->count) last = cat->count;

    draw_args_t a = {
        /* 标题就是平台名，所以行里不再重复画平台徽标，省下的宽度给名字。 */
        .title       = system_name(cat->system),
        .footer      = "A开始 B返回 左右翻页",
        .category_grid = false,
        .page        = page,
        .page_count  = (cat->count + PAGE_ROWS - 1) / PAGE_ROWS,
        .sel_row     = sel - first,
        .row_count   = last - first,
        .first_number = first + 1,
    };
    snprintf(a.meta, sizeof(a.meta), "%d GAMES  %d/%d", cat->count,
             page + 1, a.page_count);

    for (int j = first; j < last; j++) {
        int i = nth_of_system(count, cat->system, j);
        const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
        if (!e) break;

        /* ROM 目录位于 PSRAM，格式化也会使用较深的 libc 调用栈。都在
         * 菜单任务里先完成，核 1 的推屏回调只读取这份栈上快照，避免长列表
         * 页面令 3 KB 推屏任务栈承受目录访问和 snprintf。
         *
         * 编号是平台内序号，每个平台都从 01 起；翻页后继续递增，不能接着
         * 全局目录编号一路数，否则看不出这是该平台里的第几个。 */
        char *line = a.lines[j - first];
        snprintf(line, sizeof(a.lines[0]), "%s", e->name);
    }
    display_stream_sync(draw_strip, &a);
}

rom_menu_result_t rom_menu_pick(const rom_store_entry_t **entry)
{
    int count = rom_store_init(false);
    if (count <= 0) {
        ESP_LOGW(TAG, "TF 卡上没有游戏，用编译期嵌入的那个");
        return ROM_MENU_FALLBACK;
    }

    category_t cats[SYSTEM_COUNT];
    int cat_count = build_categories(count, cats, SYSTEM_COUNT);
    if (cat_count <= 0) {       /* count > 0 就不该发生，稳妥起见 */
        ESP_LOGW(TAG, "目录里一个平台都认不出来，用编译期嵌入的那个");
        return ROM_MENU_FALLBACK;
    }

    /* 三路输入并存：飞线手柄、USB HID、串口调试键盘。init 都是幂等的，
     * 模拟器启动后再调一次没有副作用。 */
    input_serial_init();
    input_usb_init();
    input_gamepad_init();

    printf("\n开机选单：%d 个游戏，%d 个平台。\n", count, cat_count);
    printf("方向键选择，A 进入/确认，B 返回上一级。\n");
    printf("（想换游戏按板子上的 RST 重启）\n\n");

    int cat = 0;
    int sel[SYSTEM_COUNT] = { 0 };  /* 每个平台各记各的，退出去再进来回原位 */
    bool in_games = false;

    uint16_t prev = poll_input();   /* 先读一次当基线：上电时可能有键按着 */
    draw_categories(cats, cat_count, cat);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint16_t now = poll_input();
        uint16_t edge = now & ~prev;    /* 这一帧新按下的位 */
        prev = now;

        bool dirty = false;

        if (!in_games) {
            /* ---- 分类页 ---- */

            /* 分类页的上一级就是 GAME/WORDS/SETTINGS 开机页。用独立返回值告诉
             * app_main，不能冒充“没有 ROM”，否则会误启动编译期内置游戏。 */
            if (edge & NES_PAD_B) {
                return ROM_MENU_BACK;

            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                in_games = true;
                dirty = true;

            } else if (edge & (NES_PAD_LEFT | NES_PAD_RIGHT)) {
                /* 卡片按行排列：左右切同一行。最后一行只有左卡时，向右不动，
                 * 不突然跳去别的行，孩子看到的移动方向和光标完全一致。 */
                int target = cat + ((edge & NES_PAD_LEFT) ? -1 : 1);
                if (target >= 0 && target < cat_count && target / 2 == cat / 2) {
                    cat = target;
                    dirty = true;
                }
            } else if (edge & (NES_PAD_UP | NES_PAD_DOWN)) {
                /* 上下切同一列，到边缘后在该列内环绕。 */
                int col = cat % 2;
                int target = cat + ((edge & NES_PAD_UP) ? -2 : 2);
                if (target < 0) {
                    target = cat_count - 1;
                    while (target >= 0 && target % 2 != col) target--;
                } else if (target >= cat_count) {
                    target = col;
                }
                if (target >= 0 && target < cat_count) {
                    cat = target;
                    dirty = true;
                }
            }

        } else {
            /* ---- 游戏列表 ---- */
            const category_t *c = &cats[cat];
            int page_count = (c->count + PAGE_ROWS - 1) / PAGE_ROWS;

            if (edge & NES_PAD_B) {
                in_games = false;
                dirty = true;

            } else if (edge & (NES_PAD_A | NES_PAD_START)) {
                int i = nth_of_system(count, c->system, sel[cat]);
                const rom_store_entry_t *e = i >= 0 ? rom_store_entry(i) : NULL;
                if (e) {
                    *entry = e;
                    if (e->storage == ROM_STORAGE_ZIP_PENDING) {
                        printf("选中：[%s] %s（ZIP，按需识别）\n  %s\n\n",
                               system_name(e->system), e->name, e->path);
                    } else {
                        printf("选中：[%s] %s（%u KB）\n  %s\n\n",
                               system_name(e->system), e->name,
                               (unsigned)(e->size / 1024), e->path);
                    }
                    return ROM_MENU_SELECTED;
                }

            } else if (page_count > 1 && (edge & (NES_PAD_LEFT | NES_PAD_RIGHT))) {
                /* 左右直接翻一整页，并尽量保留当前行。最后一页不足 8 项时，
                 * 同一行不存在就落到最后一项；页首和页尾之间同样可以环绕。
                 * 这一支排在上下之前单独走，是为了让斜推摇杆时以翻页为准，
                 * 避免同时又上下移动一格。
                 *
                 * ⚠ `page_count > 1` 这个前置条件不能去掉。摇杆是模拟量，往上
                 * 推很容易同时越过左/右阈值，这一帧的 edge 就是 UP|RIGHT。只有
                 * 一页时翻页是空操作（(0±1+1)%1 == 0），但它排在前面会把上下
                 * 吃掉——整个列表看着就像按键失灵。分平台之前列表是 31 个游戏
                 * 铺 4 页，翻页总有事可做所以没暴露；拆成平台之后 GB/GBC/SNES
                 * 都只有一页，全都中招。 */
                int page = sel[cat] / PAGE_ROWS;
                int row = sel[cat] % PAGE_ROWS;
                int page_delta = (edge & NES_PAD_LEFT) ? -1 : +1;

                page = (page + page_delta + page_count) % page_count;
                sel[cat] = page * PAGE_ROWS + row;
                if (sel[cat] >= c->count) sel[cat] = c->count - 1;
                dirty = true;

            } else {
                /* 上下环绕。跨过页边界时 draw_games() 会按新的 sel 自动切页。 */
                int moved = 0;
                if (edge & NES_PAD_UP)   moved = -1;
                if (edge & NES_PAD_DOWN) moved = +1;
                if (moved) {
                    sel[cat] = (sel[cat] + moved + c->count) % c->count;
                    dirty = true;
                }
            }
        }

        /* 统一在这里重画：省得每个分支各写一遍，还要各自挑对是哪一页。 */
        if (dirty) {
            if (in_games) draw_games(count, &cats[cat], sel[cat]);
            else          draw_categories(cats, cat_count, cat);
        }
    }
}
