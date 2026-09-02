#include "ui_sound.h"

#include <stdbool.h>

#include "audio_output.h"

/* 菜单音效固定用 24 kHz。和开机上电音同一个速率，所以在菜单里反复出声不会
 * 触发 audio_output_init() 的采样率切换分支（那会拆掉再重建 I2S 通道）。
 *
 * ⚠ 但**必须每次都调一遍** audio_output_init()：WORDS 返回主菜单时会释放
 * 24 kHz I2S（见 AGENTS.md），不重新初始化的话从 WORDS 回来之后菜单就全哑了。
 * 采样率相同时它在开头就 return ESP_OK，代价可以忽略。 */
#define UI_RATE   24000

/* 比开机上电音轻一档。上电音是「仪式」，一次就过；菜单音是每按一下都响，
 * 同样响度会很吵。 */
#define UI_PEAK   9000

#define UI_EDGE_MS  3
#define UI_TAIL_MS  8

/* 上电音：方波上行分解和弦，收尾音衰减到静音。用 50% 占空比 —— 上电音要的是
 * 厚实，和菜单音那种薄脆的刻意不同。 */
const ui_note_t UI_SOUND_BOOT[] = {
    { 523,  0, 70,  0 },   /* C5 */
    { 659,  0, 70,  0 },   /* E5 */
    { 784,  0, 70,  0 },   /* G5 */
    { 1047, 0, 340, 0 },   /* C6 */
};
const size_t UI_SOUND_BOOT_COUNT = sizeof(UI_SOUND_BOOT) / sizeof(UI_SOUND_BOOT[0]);

/* 菜单三音。都压在 50 ms 以内：菜单轮询是 16 ms 一次，音效比按键间隔还长的话
 * 会在队列里堆起来，快速划光标时听着像糊成一片。
 *
 * **光标移动没有提示音**，是刻意的：游戏列表有两千多项，划光标时每一下都响
 * 会很烦，而移动本身屏幕上已经有反馈（反白行跟着走），声音是多余的一层。
 * 试过三版都不合适：恒定 50% 方波像电话蜂鸣器；窄占空扫频音色对了但仍是个
 * 「音调」；噪声通道的「咔哒」质感最像掌机，可放到快速连划的场景下照样吵。
 * 问题不在音色，在于这个动作根本不需要声音。
 *
 * 确认/返回保留：这两个动作低频、且真的改变了所在的层级，值得一个反馈。
 * 它们用脉冲通道 —— 要表达「上行/下行」的语义，需要音高。 */
static const ui_note_t NOTE_ENTER[] = { { 988, 0, 12, 0 }, { 1568, 1976, 34, 0 } };
static const ui_note_t NOTE_BACK[]  = { { 988, 0, 10, 0 }, { 659,  440, 40, 0 } };

#define MENU_DUTY   UI_DUTY_25

#define UI_MAX_MS     50
#define UI_MAX_FRAMES (UI_RATE * UI_MAX_MS / 1000)

/* GB 噪声通道的线性反馈移位寄存器。反馈取低两位异或，结果移进最高位；
 * 7 位模式下同时也移进 bit6，于是周期从 32767 步缩到 127 步 —— 短周期正是
 * 「咔哒」那种金属脆响的来源。输出取反相的最低位，和硬件一致。 */
static uint16_t lfsr_step(uint16_t reg, int bits)
{
    uint16_t feedback = (reg ^ (reg >> 1)) & 1u;
    reg >>= 1;
    reg |= (uint16_t)(feedback << 14);
    if (bits == 7) {
        reg &= (uint16_t)~(1u << 6);
        reg |= (uint16_t)(feedback << 6);
    }
    return reg;
}

uint32_t ui_sound_ms(const ui_note_t *notes, size_t note_count)
{
    uint32_t ms = 0;
    for (size_t i = 0; i < note_count; i++) ms += (uint32_t)notes[i].ms;
    return ms;
}

void ui_sound_render(int16_t *out, size_t frames, uint32_t index,
                     const ui_note_t *notes, size_t note_count,
                     int rate, int peak, int duty8)
{
    uint16_t reg = 0x7FFFu;     /* 噪声通道状态，见下面的追赶注释 */
    uint32_t reg_steps = 0;
    int      reg_note = -1;

    for (size_t f = 0; f < frames; f++) {
        uint32_t n = index + (uint32_t)f;
        uint32_t t_ms = n * 1000u / (uint32_t)rate;

        int note = -1;
        uint32_t acc_ms = 0;
        for (size_t i = 0; i < note_count; i++) {
            if (t_ms < acc_ms + (uint32_t)notes[i].ms) { note = (int)i; break; }
            acc_ms += (uint32_t)notes[i].ms;
        }

        int16_t v = 0;
        if (note >= 0) {
            uint32_t into = t_ms - acc_ms;
            uint32_t len  = (uint32_t)notes[note].ms;

            /* 本音内已经走了多少个采样。相位必须能从绝对下标算出来，不能靠
             * 累加器 —— 开机上电音是分块渲染的，中间不保存状态。 */
            uint32_t start_n = acc_ms * (uint32_t)rate / 1000u;
            uint32_t note_n  = len * (uint32_t)rate / 1000u;
            if (note_n == 0) note_n = 1;
            uint32_t k = n - start_n;

            /* 线性扫频的相位是频率对时间的积分，闭式算出来即可：
             * pos = f0*k + (f1-f0)*k^2/(2N)，单位是 Hz·采样，每积够 rate
             * 就是一个完整周期。 */
            int f0 = notes[note].freq_hz;
            int f1 = notes[note].end_hz ? notes[note].end_hz : f0;
            int64_t pos = (int64_t)f0 * k
                        + (int64_t)(f1 - f0) * k * k / (2 * (int64_t)note_n);
            uint32_t cyc = (uint32_t)(pos % rate);

            int amp = peak;
            if (note == (int)note_count - 1) {
                amp = (int)((uint32_t)amp * (len - into) / len);
            }
            if (into < UI_EDGE_MS) amp = amp * (int)into / UI_EDGE_MS;
            if (len - into < UI_TAIL_MS) amp = amp * (int)(len - into) / UI_TAIL_MS;

            if (notes[note].lfsr_bits) {
                /* 噪声通道。LFSR 每积够一个周期移位一次，移位次数由上面同一个
                 * 相位累加量决定，所以扫频对噪声同样有效（移位越快音色越亮）。
                 *
                 * ⚠ LFSR 本质是有状态的，而这个函数要求能按绝对下标分块渲染。
                 * 解法是每次调用从固定种子重新推进到 index 处：噪声只用在
                 * 一次性渲染完的短音里（index 恒为 0），追赶循环实际不会跑；
                 * 真按分块调用也只是多花几步移位，结果仍然一致。 */
                uint32_t want = (uint32_t)(pos / rate);
                if (reg_note != note || reg_steps > want) {
                    reg = 0x7FFFu;
                    reg_steps = 0;
                    reg_note = note;
                }
                while (reg_steps < want) {
                    reg = lfsr_step(reg, notes[note].lfsr_bits);
                    reg_steps++;
                }
                /* 噪声本身就是约 50% 占空，直接 ±amp，不用按 duty 配平。 */
                v = (int16_t)((~reg & 1u) ? amp : -amp);
            } else {
                /* 按占空比配平，零直流；50% 时退化成 ±amp。 */
                bool on = (uint64_t)cyc * 8u < (uint64_t)rate * (uint32_t)duty8;
                v = (int16_t)(on ? amp * (8 - duty8) / 4 : -amp * duty8 / 4);
            }
        }
        out[f * 2]     = v;
        out[f * 2 + 1] = v;
    }
}

static void play(const ui_note_t *notes, size_t note_count, int duty8)
{
    static int16_t buf[UI_MAX_FRAMES * 2];

    if (audio_output_init(UI_RATE) != ESP_OK) return;

    uint32_t ms = ui_sound_ms(notes, note_count);
    size_t frames = (size_t)ms * UI_RATE / 1000;
    if (frames > UI_MAX_FRAMES) frames = UI_MAX_FRAMES;

    ui_sound_render(buf, frames, 0, notes, note_count, UI_RATE, UI_PEAK, duty8);
    /* 非阻塞提交：菜单线程不能为了出声卡住，队列满了这一下不响也无所谓。 */
    audio_output_submit_stereo(buf, frames);
}

void ui_sound_enter(void)
{
    play(NOTE_ENTER, sizeof(NOTE_ENTER) / sizeof(NOTE_ENTER[0]), MENU_DUTY);
}

void ui_sound_back(void)
{
    play(NOTE_BACK, sizeof(NOTE_BACK) / sizeof(NOTE_BACK[0]), MENU_DUTY);
}
