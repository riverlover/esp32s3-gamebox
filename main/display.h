/*
 * ST7789 SPI 显示层 —— 240x320 IPS，横屏使用为 320x240
 *
 * 对上层只暴露「一块 RGB565 帧缓冲 + 推屏」这一件事，
 * 后面接 NES 模拟器时上层代码不用动。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============ 接线（改这里就能换脚） ============
 *
 * 选脚原则：
 *   - SCK/MOSI/CS 用 SPI2(FSPI) 的 IOMUX 原生脚，才能跑到 80MHz；
 *     换成别的脚会自动走 GPIO 矩阵，上限降到 40MHz（能用，但慢一半）
 *   - 避开 GPIO33~37（Octal PSRAM 占用）
 *   - 避开 GPIO19/20（原生 USB D-/D+）
 *   - 避开 GPIO0/3/45/46（strapping，46 还是只读）
 *
 * 换屏时只改这几个宏 + 下面的方向/偏移即可，上层代码不用动。
 */
#define DISP_PIN_SCLK   12      /* 屏丝印 SCL / SCK   —— IOMUX FSPICLK */
#define DISP_PIN_MOSI   11      /* 屏丝印 SDA / MOSI  —— IOMUX FSPID   */
#define DISP_PIN_CS     10      /* 屏丝印 CS，没这个脚就填 -1          */
#define DISP_PIN_DC     14      /* 屏丝印 DC / RS                       */
#define DISP_PIN_RST    13      /* 屏丝印 RES / RST，没有就填 -1       */
#define DISP_PIN_BL      9      /* 屏丝印 BLK / LED，直接接 3V3 就填 -1 */

/* ============ 时序 ============
 *
 * 杜邦线飞线时先用 40MHz。稳定之后可以试着往上加：
 * 焊死/排线短的话 80MHz 一般没问题，满屏刷新时间直接减半。
 * 症状：太快会花屏、雪花、颜色错乱 —— 降回 40 即可。
 */
#define DISP_SPI_HZ     (80 * 1000 * 1000)

/* ============ 面板方向与偏移 ============
 *
 * 这块面板是满 240x320，正好等于 ST7789 的显存尺寸，所以**不需要 gap**。
 *
 * ⚠ 换成 170x320 那种窄屏时才要设 gap：那种面板只有 170 列，居中贴在
 * 240 列的显存上，列地址要整体偏移 (240-170)/2 = 35；横屏(swap_xy)之后
 * 这 35 落到 y 轴上，即 set_gap(0, 35)。满屏的这块设了反而会错位。
 *
 * 如果实际显示不对，按现象调这几个宏（每次只改一个）：
 *   画面整体偏移 / 边缘有花条  -> 调 GAP_X / GAP_Y（满屏应为 0）
 *   画面上下或左右颠倒        -> 翻转 MIRROR_X / MIRROR_Y
 *   颜色像底片（黑白反）      -> 翻转 INVERT_COLOR
 *   红蓝互换                  -> 翻转 BGR_ORDER
 */
#define DISP_SWAP_XY     true   /* true = 横屏 320x240 */
#define DISP_MIRROR_X    true
#define DISP_MIRROR_Y    false
#define DISP_GAP_X       0
#define DISP_GAP_Y       0
#define DISP_INVERT_COLOR true  /* IPS 屏基本都要 true，TN 屏多为 false */
#define DISP_BGR_ORDER   false

/* 面板横屏下的可见分辨率 */
#define DISP_W          320
#define DISP_H          240

/* ============ 画布 ============
 *
 * 画布不铺满整块屏。这里定义它的尺寸和落点，绘图坐标都是相对画布的。
 * 四周黑边由 display_init() 在开机时清一次，之后再不碰。
 *
 * ---- 288x224 是怎么来的 ----
 *
 * NES 裁掉上下各 8 行 overscan 之后是 256x224。竖向 1:1 直接用满 224 行。
 * 横向从 256 拉到 288，是为了修**像素宽高比**：
 *
 *   NES 在 NTSC 电视上的 PAR 是 8:7 ≈ 1.143，即像素不是方的，是偏宽的。
 *   256x224 原样显示（PAR 1.0）画面偏窄 12%，圆的东西看着是竖椭圆。
 *   拉到 288 之后 PAR = 288/256 = 1.125，误差只剩 1.6%。
 *
 * 288 = 256 * 9/8，即每 8 个源像素输出 9 个。选 9:8 而不是别的比例，是因为
 * NES 图块本来就是 8 像素宽且对齐的，复制点正好落在图块边界上，视觉最不
 * 突兀，而且内循环能 8x 展开、无分支无取模。
 *
 * ---- 现状：这块画布现在只有菜单和诊断画面在用 ----
 *
 * NES/SNES/GB/GBC/Genesis 都已经改走 display_stream_sized() 提交自己的
 * 原生尺寸，铺满整块 320x240 面板（GB/GBC 共用 gbc_emu.c 一套实现，各自
 * xxx_strip 里做缩放）。这里曾经记录过一条测算结论：铺满 320 宽会把 SPI
 * 传输占到 60fps 预算的 97%，只能靠掉到 30fps 换，而且 NES 跳帧会让精灵
 * 永久消失——上板实测已证伪，铺满后都能稳定 60fps，没有精灵丢失。DISP_FB_W/H 这块
 * 288x224 画布现在只被 rom_menu.c 和诊断画面（main.c/input_gamepad.c）
 * 使用，288 这个尺寸对它们没有特别意义，纯粹是历史遗留。
 */
#define DISP_FB_W       288
#define DISP_FB_H       224

#define DISP_FB_X       ((DISP_W - DISP_FB_W) / 2)
#define DISP_FB_Y       ((DISP_H - DISP_FB_H) / 2)

/* RGB565 颜色构造。
 *
 * ⚠ 帧缓冲里存的是**字节交换后**（大端）的 RGB565。
 *
 * ST7789 走 SPI 时按大端接收 16 位像素（高字节先出），而 DMA 是照内存顺序
 * 逐字节发的，小端机器上存 0xF800 会先发出 0x00 再发 0xF8 —— 到屏上就变成
 * 0x00F8（暗蓝）。esp_lcd 没有提供字节序开关（它的 RGB/BGR 选项管的是
 * MADCTL 的红蓝通道顺序，不是字节序），所以只能在这里换好。
 * 这也是 ESP-IDF 的 LVGL 移植都要开 LV_COLOR_16_SWAP 的原因。
 *
 * 参数是常量时 __builtin_bswap16 会在编译期折叠，运行时零开销；
 * 参数是变量时也只是一条指令。
 *
 * 直接往帧缓冲里写像素的代码（比如 NES 的调色板查表）同样要存大端值。 */
#define RGB565(r, g, b) \
    ((uint16_t)__builtin_bswap16(                                     \
        (uint16_t)(((((r) & 0xF8) << 8)) | ((((g) & 0xFC) << 3)) | ((b) >> 3))))

#define C_BLACK   RGB565(0,   0,   0)
#define C_WHITE   RGB565(255, 255, 255)
#define C_RED     RGB565(255, 0,   0)
#define C_GREEN   RGB565(0,   255, 0)
#define C_BLUE    RGB565(0,   0,   255)
#define C_YELLOW  RGB565(255, 255, 0)
#define C_CYAN    RGB565(0,   255, 255)
#define C_MAGENTA RGB565(255, 0,   255)
#define C_GRAY    RGB565(128, 128, 128)

/* ============ gruvbox light：游戏以外所有 UI 的唯一配色入口 ============
 *
 * 开机画面、模式选择、ROM 菜单、加载页、SETTINGS、单词学习、Controller
 * Test 全部只从这里取色。上面那组 C_RED/C_GREEN/... 原色只留给模拟器自己
 * 的叠加提示（gbc_emu.c 的存档提示）和 SHOW_DISPLAY_SELFTEST 诊断图——
 * 那两处要的就是「一眼认出通道对不对」，不该跟着 UI 主题走。
 *
 * 取自 morhetz/gruvbox 的 light 主题（github.com/morhetz/gruvbox）。换掉
 * 原来真机 GAMEBOY DMG 那套四阶黄绿，是因为 DMG 只有四阶、两个中间阶还挨
 * 得太近：浅阶 #8bac0f 压在深阶 #306230 上只有 2.75:1，卡片副标题那种
 * 16x16 点阵中文在小屏上发糊。gruvbox 同样是暖色底，但整套 30 个色都在，
 * 层次和色相都够分。
 *
 * ---- 分两层，画界面只用下面的语义层 ----
 *
 * 原色层（C_GVB_*）回答「gruvbox 是什么颜色」，名字和上游 palette 一一
 * 对应，方便对着上游 README 查；语义层（C_UI_* / C_SYS_*）回答「这个位置
 * 该用哪个颜色」。加新界面时选语义名，不要直接摸 C_GVB_*，更不要自己
 * RGB565() 一个新色——以前 main.c / rom_menu.c 各抄一份配色约定注释、
 * word_study.c 又自己 #define 了三个色，就是没有这层的结果。
 */

/* ---- 原色层 · 底色阶：LIGHT0 最浅，LIGHT4 最深 ---- */
#define C_GVB_LIGHT0_H RGB565(249, 245, 215)  /* #f9f5d7 light0_hard */
#define C_GVB_LIGHT0   RGB565(251, 241, 199)  /* #fbf1c7 light0      */
#define C_GVB_LIGHT0_S RGB565(242, 229, 188)  /* #f2e5bc light0_soft */
#define C_GVB_LIGHT1   RGB565(235, 219, 178)  /* #ebdbb2 light1      */
#define C_GVB_LIGHT2   RGB565(213, 196, 161)  /* #d5c4a1 light2      */
#define C_GVB_LIGHT3   RGB565(189, 174, 147)  /* #bdae93 light3      */
#define C_GVB_LIGHT4   RGB565(168, 153, 132)  /* #a89984 light4      */

/* ---- 原色层 · 字色阶：DARK0 最深，DARK4 最浅 ---- */
#define C_GVB_DARK0_H  RGB565(29,  32,  33)   /* #1d2021 dark0_hard  */
#define C_GVB_DARK0    RGB565(40,  40,  40)   /* #282828 dark0       */
#define C_GVB_DARK0_S  RGB565(50,  48,  47)   /* #32302f dark0_soft  */
#define C_GVB_DARK1    RGB565(60,  56,  54)   /* #3c3836 dark1       */
#define C_GVB_DARK2    RGB565(80,  73,  69)   /* #504945 dark2       */
#define C_GVB_DARK3    RGB565(102, 92,  84)   /* #665c54 dark3       */
#define C_GVB_DARK4    RGB565(124, 111, 100)  /* #7c6f64 dark4       */
#define C_GVB_GRAY     RGB565(146, 131, 116)  /* #928374 两个主题共用的中性灰 */

/* ---- 原色层 · 强调色 ----
 *
 * gruvbox 的三组彩色不是深浅备选，是按背景分工的：light 主题取 faded_*，
 * dark 主题取 bright_*。所以这里的默认选择永远是 faded_*——它够深，能
 * 直接当浅底上的文字色，也能当色块底铺 LIGHT0 反白字（4.3:1 ~ 8.6:1）。
 * neutral_* 亮一档，压在 LIGHT0 上当文字太飘，适合当纯装饰色块（进度条
 * 这种上面不写字的）和 faded_* 色块的高光边。bright_* 是给深色底用的，
 * **目前一处都没用**：整个 UI 没有深色面（首页字标试过一块深色牌匾，那块
 * 黑在暖底里太突兀，去掉了）。留着是为了调色板完整，将来真做深色面时直接
 * 取——但**别把 bright_* 铺在浅底上**，红的只有 1.7:1。 */
#define C_GVB_FADED_RED      RGB565(157, 0,   6)    /* #9d0006 */
#define C_GVB_FADED_GREEN    RGB565(121, 116, 14)   /* #79740e */
#define C_GVB_FADED_YELLOW   RGB565(181, 118, 20)   /* #b57614 */
#define C_GVB_FADED_BLUE     RGB565(7,   102, 120)  /* #076678 */
#define C_GVB_FADED_PURPLE   RGB565(143, 63,  113)  /* #8f3f71 */
#define C_GVB_FADED_AQUA     RGB565(66,  123, 88)   /* #427b58 */
#define C_GVB_FADED_ORANGE   RGB565(175, 58,  3)    /* #af3a03 */

#define C_GVB_NEUTRAL_RED    RGB565(204, 36,  29)   /* #cc241d */
#define C_GVB_NEUTRAL_GREEN  RGB565(152, 151, 26)   /* #98971a */
#define C_GVB_NEUTRAL_YELLOW RGB565(215, 153, 33)   /* #d79921 */
#define C_GVB_NEUTRAL_BLUE   RGB565(69,  133, 136)  /* #458588 */
#define C_GVB_NEUTRAL_PURPLE RGB565(177, 98,  134)  /* #b16286 */
#define C_GVB_NEUTRAL_AQUA   RGB565(104, 157, 106)  /* #689d6a */
#define C_GVB_NEUTRAL_ORANGE RGB565(214, 93,  14)   /* #d65d0e */

#define C_GVB_BRIGHT_RED     RGB565(251, 73,  52)   /* #fb4934 */
#define C_GVB_BRIGHT_GREEN   RGB565(184, 187, 38)   /* #b8bb26 */
#define C_GVB_BRIGHT_YELLOW  RGB565(250, 189, 47)   /* #fabd2f */
#define C_GVB_BRIGHT_BLUE    RGB565(131, 165, 152)  /* #83a598 */
#define C_GVB_BRIGHT_PURPLE  RGB565(211, 134, 155)  /* #d3869b */
#define C_GVB_BRIGHT_AQUA    RGB565(142, 192, 124)  /* #8ec07c */
#define C_GVB_BRIGHT_ORANGE  RGB565(254, 128, 25)   /* #fe8019 */

/* ---- 语义层 · 面 ----
 *
 * 三层底色拉开层次，别把面板和页面糊在同一个色上：页面 LIGHT0、面板/卡片
 * LIGHT1、面板里再深一层 LIGHT2。分割线和描边继续往下取 LIGHT3/LIGHT4，
 * 这样细线不会比正文还抢眼——四阶时期外框用的是 DARK3，边框比标题还重。
 *
 * ⚠ 字色三档都是对 C_UI_BG 挑过对比度的：FG 10:1、FG_DIM 7.5:1、FG_FAINT
 * 4.3:1。想再弱一档就别用字了，用 C_UI_LINE 画条线——GRAY 压在 BG 上只有
 * 3.2:1，屏小、点阵字又细，小孩看不清。 */
#define C_UI_BG         C_GVB_LIGHT0   /* 页面底色 */
#define C_UI_PANEL      C_GVB_LIGHT1   /* 面板、卡片、未选中的行块 */
#define C_UI_PANEL_ALT  C_GVB_LIGHT2   /* 面板里再深一层：进度槽、徽标底、字影 */
#define C_UI_LINE       C_GVB_LIGHT3   /* 行间细分割线 */
#define C_UI_EDGE       C_GVB_LIGHT4   /* 页面外框、未选中项的描边 */

/* ---- 语义层 · 字 ---- */
#define C_UI_FG         C_GVB_DARK1    /* 标题、正文，对页面底 10:1 */
#define C_UI_FG_DIM     C_GVB_DARK3    /* 次要信息 */
#define C_UI_FG_FAINT   C_GVB_DARK4    /* 操作提示这类最弱的字，仍有 4.3:1 */
#define C_UI_FG_INV     C_GVB_LIGHT0   /* 深色块上的反白字 */
#define C_UI_FG_INV_DIM C_GVB_LIGHT2   /* 深色块上的次要字 */

/* ---- 语义层 · 选中态 ---- */
#define C_UI_SEL        C_GVB_DARK2    /* 选中行/卡片的底 */
#define C_UI_SEL_EDGE   C_GVB_DARK0    /* 选中项的描边，比底再深一档 */

/* ---- 语义层 · 状态 ---- */
#define C_UI_OK         C_GVB_FADED_GREEN   /* 答对、成功 */
#define C_UI_BAD        C_GVB_FADED_RED     /* 答错、失败 */
#define C_UI_WARN       C_GVB_FADED_ORANGE  /* 需要注意，但不是错误 */
#define C_UI_INFO       C_GVB_FADED_BLUE    /* 附加信息 */
#define C_UI_GOLD       C_GVB_FADED_YELLOW  /* 已完成、未判定 */
#define C_UI_BAR        C_GVB_NEUTRAL_BLUE  /* 纯装饰色块（进度条），上面不写字 */

/* ---- 语义层 · 平台色 ----
 *
 * 这是换掉四阶单色最实在的收益：菜单里五个系统各占一个色相，不看字也能
 * 认出在哪个平台。四阶时期只能全用同一个深色，main.c 那段注释还专门解释
 * 过「没法再用色相区分五个系统」——现在可以了。
 *
 * 选色对着实机：NES 红（红白机）、SNES 紫（欧版手柄按键）、GB 绿（DMG
 * 屏）、GBC 青（初代 teal 配色）、MD 蓝（SEGA logo）、PCE 橙（TurboGrafx-16
 * 的橙色标识）。六个平台六个色相，faded 那组正好还剩一个黄没用上。
 *
 * 橙色和开机模式选择页的 GAME 卡片同色 —— 那是两个页面，不会同屏出现，
 * 和 MD 蓝跟 WORDS 卡片同色是一个情况。 */
#define C_SYS_NES       C_GVB_FADED_RED
#define C_SYS_GB        C_GVB_FADED_GREEN
#define C_SYS_GBC       C_GVB_FADED_AQUA
#define C_SYS_SNES      C_GVB_FADED_PURPLE
#define C_SYS_GENESIS   C_GVB_FADED_BLUE
#define C_SYS_PCE       C_GVB_FADED_ORANGE

/* 初始化 SPI + 面板 + 背光，分配两块可覆盖整屏宽度的条带缓冲，
 * 并在核 1 上起一个推屏任务。 */
esp_err_t display_init(void);

/* ============ 条带流式推屏 ============
 *
 * 这里**没有常驻帧缓冲**。默认 288x224 画布的 RGB565 要 126 KB，两块就是
 * 252 KB，内部 DMA 内存装不下（PSRAM 走不了 DMA，原因见 display.c）。
 *
 * 所以改成：只留两块最大 320x32 的小条带缓冲（各 20 KB），核 1 一边把第
 * N+1 条转换进一块、一边 DMA 推第 N 条，流水起来。调用方只要提供「把画布的
 * 第 y0..y0+h-1 行画进这块条带」的回调。
 *
 * 双缓冲并没有消失，只是下移了一层：NES 那边缓冲的是 8 位的 vidbuf（每块
 * 64 KB，比 RGB565 便宜一半），核 0 渲染下一帧的同时核 1 在读上一帧。
 * 帧时间仍然是 max(模拟, 推屏)，不是两者相加。
 *
 * 回调在**核 1** 上执行。传进去的 ctx 必须在整帧推完前保持有效。
 * strip 的每行跨度等于本次提交的画布宽度：display_stream() 是 DISP_FB_W，
 * display_stream_sized() 则是它的 width 参数。 */
typedef void (*disp_strip_fn)(uint16_t *strip, int y0, int h, void *ctx);

/* 提交一帧并立刻返回，实际转换+推送在核 1 上进行。
 * 只有上一帧还没推完时才阻塞 —— 天然把帧率限制在屏幕吃得下的上限。 */
void display_stream(disp_strip_fn fn, void *ctx);

/* 提交一帧自定义尺寸的居中画布。用于 Genesis 原生 320x224；width/height
 * 不能超过面板尺寸。其他模拟器继续调用 display_stream() 使用默认画布。 */
void display_stream_sized(disp_strip_fn fn, void *ctx, int width, int height);

/* 同上，但等这一帧推完才返回。ctx 指向栈上数据时用这个。 */
void display_stream_sync(disp_strip_fn fn, void *ctx);

/* 等待已提交的帧全部推完。 */
void display_wait_idle(void);

/* 背光亮度 0~100。BL 脚接 3V3 常亮时此函数无效果。 */
void display_backlight(int percent);

/* 当前背光百分比（display_backlight() 最后一次设的值，默认 100）。 */
int display_get_backlight(void);

/* ---- 基本绘图 ----
 *
 * ⚠ 只能在 disp_strip_fn 回调**内部**调用 —— 它们画的是「当前条带」。
 * 坐标是相对整块画布的（左上角 0,0），落在当前条带之外的部分自动丢弃。
 *
 * 也就是说同一段绘制代码每帧会被调用 BAND_COUNT 次，每次画出其中一横条。
 * 菜单/诊断画面这类低帧率场景重复执行绘制列表的开销可以忽略。 */
void display_clear(uint16_t color);
void display_fill_rect(int x, int y, int w, int h, uint16_t color);
void display_rect(int x, int y, int w, int h, uint16_t color);      /* 1px 描边 */
void display_hline(int x, int y, int w, uint16_t color);
void display_vline(int x, int y, int h, uint16_t color);
void display_pixel(int x, int y, uint16_t color);

/* UTF-8 文字：ASCII 用 5x7，菜单所需汉字用 16x16 子集；scale 为整数倍放大。 */
void display_text(int x, int y, const char *s, uint16_t color, int scale);

/* 菜单/加载页的同高字体：ASCII 用 Unifont 8x16 半角，中文用 16x16 全角。
 * 两种字形基线和高度一致；宽度函数与绘制使用完全相同的 UTF-8/fallback 规则。 */
void display_text_16(int x, int y, const char *s, uint16_t color);
int display_text_width_16(const char *s);

/* 英文学习卡的大字：沿用清晰的 Unifont 8x16 半角字形，再做整数倍放大。
 * 和 display_text() 的 5x7 放大字相比，升部、降部和曲线细节都更完整。 */
void display_text_ascii_16_scaled(int x, int y, const char *s,
                                  uint16_t color, int scale);
int display_text_width_ascii_16_scaled(const char *s, int scale);
