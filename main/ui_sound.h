/*
 * 界面音效：方波合成 + 菜单提示音
 *
 * 游戏以外的页面（开机模式选择、平台页、游戏列表、SETTINGS）共用这一套。
 * 游戏内的声音各模拟器自己出，单词学习的答题音效在 word_audio.c，都不走这里。
 *
 * 音色和开机上电音是同一套方波合成 —— 那段原来写在 main.c 里，菜单音效要用
 * 同样的东西，所以合成部分挪到这里，main.c 只留它自己那段按动画帧喂数据的
 * 节奏控制。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int freq_hz;    /* 起始频率；噪声音里是 LFSR 的移位速率 */
    int end_hz;     /* 结束频率；0 表示整个音保持 freq_hz 不扫频 */
    int ms;
    int lfsr_bits;  /* 0 = 脉冲通道（按 duty8 出方波）；7 或 15 = 噪声通道 */
} ui_note_t;

/* 噪声通道的 LFSR 位宽。GB 的噪声通道只有这两档：
 *   7 位  —— 周期只有 127 步，音色金属、发脆，短促的「咔哒」用它
 *   15 位 —— 周期 32767 步，接近白噪声，更像「沙沙」
 * 很多掌机游戏的光标音走的是噪声通道而不是方波：那种打击感方波做不出来。
 *
 * ⚠ 当前**没有任何音符用到噪声**（光标音去掉之后就没调用者了）。实现和硬件
 * 对过：7 位周期 127 步、15 位 32767 步、输出 50% 密度、同电平游程不等长。
 * 留着是因为它是合成器的一项通用能力，将来要做打击感的音效直接把 lfsr_bits
 * 填上就行；真确定用不上就连同 ui_sound_render() 里那段追赶逻辑一起删。 */
#define UI_LFSR_CLICK  7
#define UI_LFSR_HISS   15

/* 占空比，单位是 1/8。GB 的脉冲通道就只有这四档硬件占空比。
 *
 * 这一项是音色的关键：50% 方波谐波最"圆"，听感就是电话蜂鸣器；12.5% 和 25%
 * 薄而脆，才是掌机菜单音的那个味道。第一版菜单音用 50% 恒定音高，所以听着
 * 不像掌机。 */
#define UI_DUTY_12   1
#define UI_DUTY_25   2
#define UI_DUTY_50   4

/* 音符序列的总时长。 */
uint32_t ui_sound_ms(const ui_note_t *notes, size_t note_count);

/* 按**绝对采样下标**把音符序列渲染成交错立体声 PCM。调用方只要往前推 index
 * 就能分块取，不用保存相位 —— 开机上电音靠这条按动画帧分批喂。
 *
 * 最后一个音整段线性衰减，其余等响；每个音都有 3 ms 起振和 8 ms 收尾，
 * 不加的话方波从零直接跳到峰值会「啪」一声，比音符本身还响。
 *
 * 输出是零直流的：高低电平按占空比配平（占空 d 时高 2A(1-d)、低 -2A·d），
 * 峰峰值恒为 2A。不配平的话窄占空比会带着一大截直流，出声瞬间「噗」一下。
 * 50% 时这个公式正好退化成 ±A，和原来一致。 */
void ui_sound_render(int16_t *out, size_t frames, uint32_t index,
                     const ui_note_t *notes, size_t note_count,
                     int rate, int peak, int duty8);

/* 开机上电音。main.c 按动画帧的节奏自己喂，所以只导出音符表。 */
extern const ui_note_t UI_SOUND_BOOT[];
extern const size_t    UI_SOUND_BOOT_COUNT;
#define UI_SOUND_BOOT_PEAK  13000

/* 菜单提示音：确认进入 / 返回上一级。光标移动**故意不出声**，原因见 .c。
 *
 * 都短到能一次塞进 I2S 队列（队列只有 4 个包、约 88 ms），所以直接渲染完
 * 提交、不阻塞也不分批。音量档位由 audio_output 那层统一施加。 */
void ui_sound_enter(void);
void ui_sound_back(void);
