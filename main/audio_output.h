#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* NES/GB/SNES 使用 24 kHz；Genesis 按芯片时钟输出约 26.4 kHz。PAL Genesis
 * 每帧最多 528 个采样，所以通用队列不能继续按 24k/50 的 480 个来定长。 */
#define AUDIO_OUTPUT_SAMPLE_RATE          24000
#define AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET 528

/* nofrendo 适配层仍沿用这些名字，避免把 NES 的制式知识泄漏进通用 I2S 后端。 */
#define NES_AUDIO_SAMPLE_RATE AUDIO_OUTPUT_SAMPLE_RATE
#define NES_AUDIO_SAMPLES_PER_FRAME (NES_AUDIO_SAMPLE_RATE / 60)
#define NES_AUDIO_MAX_SAMPLES_PER_FRAME AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET

/* 游戏装载后初始化 MAX98357 和音频消费任务。失败时模拟器仍可静音运行。 */
esp_err_t audio_output_init(uint32_t sample_rate);

/* WORDS 会暂时借用同一套 I2S。返回主菜单前必须停掉消费任务并释放通道，
 * 否则随后进入 Genesis 时会沿用 24 kHz，实际需要的约 26.4 kHz 就配不上。 */
void audio_output_shutdown(void);

/* 在开机选单前把音量重置为默认档位。只在本次运行中有效，不读写 NVS。 */
esp_err_t audio_output_settings_init(void);

/* 菜单里的音量档位：0~100，menu 只用 10 的整数倍。0 视为静音——
 * audio_output_init() 会因此完全不启动 I2S/DMA/消费任务。set 立即生效
 * （消费任务下一包就按新档位淡变），但重启后一定恢复默认档位。 */
int audio_output_get_volume(void);
esp_err_t audio_output_set_volume(int percent);

/* 供 rgb_led.c 轮询的音量联动：返回自上次调用以来、已经过档位缩放的
 * 采样峰值幅度（0~32767），读完立即清零。周期性轮询（比如每 40ms）
 * 就能拿到"这一小段时间里有多响"，静音或调低音量时也会跟着变小。 */
int audio_output_take_peak(void);

/* 向 I2S 队列提交交错排列的立体声 S16 帧。只复制、不等待 DMA；超过
 * AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET 的量拆成多个包依次排队（不截断），
 * 队列满时丢当前包并记入诊断计数。所有模拟器共用这一条宿主接口。 */
void audio_output_submit_stereo(const int16_t *samples, size_t frame_count);

/* 单词发音由独立任务生产，允许等队列腾出位置，避免一段语音只留下最前面
 * 四个包。游戏模拟线程仍必须使用上面的非阻塞接口，不能改用这一条。 */
bool audio_output_submit_stereo_wait(const int16_t *samples, size_t frame_count,
                                     uint32_t timeout_ms);

/* 取消上一条单词发音时丢掉尚未播放的包；已经交给 I2S DMA 的最后约 20 ms
 * 无法收回，但不会和下一条完整语音叠在一起。 */
void audio_output_flush(void);
