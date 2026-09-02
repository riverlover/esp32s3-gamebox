#pragma once

#include <stdbool.h>

/* 挂载离线英式发音包并启动异步播放任务。语音包缺失或损坏只返回 false，
 * WORDS 的认读、测验和进度保存仍照常可用。 */
bool word_audio_init(void);

/* 播放教材里显示的原始英文。重复调用会取消尚未播完的上一条，始终以孩子
 * 当前看到的卡片为准。 */
void word_audio_play(const char *word);

/* 仅供 QUIZ 判题：答对为四级上升琶音，答错为三级下降提示音。实时合成，不占
 * 发音包；新音效会取消还在排队的单词发音，避免两路声音叠加。 */
void word_audio_play_quiz_result(bool correct);

/* 一轮把本单元全部词条答对后，在结果页播放一次专属过关短曲。 */
void word_audio_play_quiz_perfect(void);

bool word_audio_is_available(void);

/* 返回 GAME/WORDS/SETTINGS 首页前停止播放、释放 I2S 和索引内存。 */
void word_audio_shutdown(void);
