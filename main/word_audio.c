/*
 * WORDS 离线英式发音包。
 *
 * 524 个不重复词条按 FNV-1a 哈希排序，板上只留约 8.2 KB 索引；PCM 用
 * 4-bit IMA ADPCM 保存到独立 13.94 MiB data 分区，播放时每次从 flash 读 256
 * 字节、解成 20 ms 立体声包送公共 I2S。既不占 app 分区，也不把整段语音
 * 搬进 RAM。生成格式的另一半在 tools/build_word_audio.py。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "audio_output.h"
#include "word_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "word_audio";

#define WORD_AUDIO_MAGIC       "GBWUK01"
#define WORD_AUDIO_VERSION     1u
#define WORD_AUDIO_SUBTYPE     ((esp_partition_subtype_t)0x40)
#define DECODE_FRAMES          480
#define FLASH_CHUNK            256
#define REQUEST_STOP           UINT32_MAX
#define REQUEST_QUIZ_CORRECT   (UINT32_MAX - 1u)
#define REQUEST_QUIZ_WRONG     (UINT32_MAX - 2u)
#define REQUEST_QUIZ_PERFECT   (UINT32_MAX - 3u)
#define EFFECT_AMPLITUDE       24000
#define SPEECH_GAIN_NUM        6
#define SPEECH_GAIN_DEN        5

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t sample_rate;
    uint32_t entry_count;
    uint32_t index_offset;
    uint32_t data_offset;
    uint32_t image_size;
    uint32_t index_hash;
    uint32_t reserved;
} word_audio_header_t;

typedef struct __attribute__((packed)) {
    uint32_t word_hash;
    uint32_t offset;
    uint32_t byte_count;
    uint32_t sample_count;
} word_audio_entry_t;

static const int16_t IMA_INDEX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t IMA_STEP[89] = {
       7,     8,     9,    10,    11,    12,    13,    14,
      16,    17,    19,    21,    23,    25,    28,    31,
      34,    37,    41,    45,    50,    55,    60,    66,
      73,    80,    88,    97,   107,   118,   130,   143,
     157,   173,   190,   209,   230,   253,   279,   307,
     337,   371,   408,   449,   494,   544,   598,   658,
     724,   796,   876,   963,  1060,  1166,  1282,  1411,
    1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
    3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
    7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
   15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
   32767,
};

static const esp_partition_t *s_partition;
static word_audio_header_t s_header;
static word_audio_entry_t *s_index;
static QueueHandle_t s_requests;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static int16_t s_pcm[DECODE_FRAMES * 2];
static uint8_t s_compressed[FLASH_CHUNK];
static bool s_available;

static uint32_t fnv1a_bytes(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t word_hash(const char *word)
{
    return fnv1a_bytes(word, strlen(word));
}

static const word_audio_entry_t *find_entry(uint32_t hash)
{
    int lo = 0;
    int hi = (int)s_header.entry_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t candidate = s_index[mid].word_hash;
        if (candidate == hash) return &s_index[mid];
        if (candidate < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static int16_t decode_nibble(uint8_t code, int *predictor, int *step_index)
{
    int step = IMA_STEP[*step_index];
    int diff = step >> 3;
    if (code & 1) diff += step >> 2;
    if (code & 2) diff += step >> 1;
    if (code & 4) diff += step;
    if (code & 8) *predictor -= diff;
    else *predictor += diff;
    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;

    *step_index += IMA_INDEX[code & 0x0f];
    if (*step_index < 0) *step_index = 0;
    if (*step_index > 88) *step_index = 88;
    return (int16_t)*predictor;
}

static bool submit_sample(int16_t sample, size_t *frames)
{
    s_pcm[*frames * 2] = sample;
    s_pcm[*frames * 2 + 1] = sample;
    (*frames)++;
    if (*frames < DECODE_FRAMES) return true;
    bool ok = audio_output_submit_stereo_wait(s_pcm, *frames, 250);
    *frames = 0;
    return ok;
}

static int16_t amplify_speech(int16_t sample)
{
    /* QUIZ 音效已经足够响，只给 ADPCM 单词发音加约 1.6 dB。生成器把峰值
     * 控在 -2 dB，6/5 通常仍有余量；这里保留饱和限幅兜住少数瞬态。 */
    int32_t amplified = (int32_t)sample * SPEECH_GAIN_NUM / SPEECH_GAIN_DEN;
    if (amplified > INT16_MAX) amplified = INT16_MAX;
    if (amplified < INT16_MIN) amplified = INT16_MIN;
    return (int16_t)amplified;
}

/* 不用浮点 sin()，避免为两个 400 ms 内的提示音把 libm 和浮点运算拖进实时
 * 路径。50% 方波配短音阶是典型的 8-bit 掌机听感；2 ms 起音和 8 ms 收尾
 * 保留清脆起音，同时消掉直接切换电平造成的咔哒声。 */
static bool play_tone(uint32_t frequency, uint32_t duration_ms,
                      uint32_t *replacement)
{
    uint32_t total = AUDIO_OUTPUT_SAMPLE_RATE * duration_ms / 1000;
    uint32_t attack = AUDIO_OUTPUT_SAMPLE_RATE * 2 / 1000;
    uint32_t release = AUDIO_OUTPUT_SAMPLE_RATE * 8 / 1000;
    uint32_t phase = 0;
    uint32_t phase_step = (uint32_t)(((uint64_t)frequency << 32) /
                                     AUDIO_OUTPUT_SAMPLE_RATE);
    size_t frames = 0;

    for (uint32_t i = 0; i < total; i++) {
        if (frames == 0 && xQueueReceive(s_requests, replacement, 0) == pdTRUE) {
            return false;
        }
        int32_t square = phase & 0x80000000u ? 32767 : -32768;
        int32_t envelope = 32767;
        if (i < attack) envelope = (int32_t)i * 32767 / (int32_t)attack;
        uint32_t remaining = total - 1 - i;
        if (remaining < release) {
            int32_t tail = (int32_t)remaining * 32767 / (int32_t)release;
            if (tail < envelope) envelope = tail;
        }
        int16_t sample = (int16_t)(square * EFFECT_AMPLITUDE / 32768 *
                                   envelope / 32768);
        if (!submit_sample(sample, &frames)) return true;
        phase += phase_step;
    }
    if (frames) (void)audio_output_submit_stereo_wait(s_pcm, frames, 250);
    return true;
}

static bool play_quiz_effect(bool correct, uint32_t *replacement)
{
    if (correct) {
        /* E5-G5-B5-E6 快速琶音：原创的过关提示，不照搬商业游戏旋律。 */
        if (!play_tone(659, 55, replacement)) return false;
        if (!play_tone(784, 55, replacement)) return false;
        if (!play_tone(988, 65, replacement)) return false;
        return play_tone(1319, 145, replacement);
    }
    /* G4-E4-C4 下降音：像掌机选错提示，但尾音不做刺耳长蜂鸣。 */
    if (!play_tone(392, 90, replacement)) return false;
    if (!play_tone(330, 90, replacement)) return false;
    return play_tone(262, 180, replacement);
}

static bool play_quiz_perfect(uint32_t *replacement)
{
    /* C5-E5-G5-C6 先报喜，再用 G5-C6-E6-G6 收成一个短过关尾奏。
     * 旋律原创且只有 0.9 秒，不拖慢孩子按 A 开下一轮。 */
    if (!play_tone(523, 70, replacement)) return false;
    if (!play_tone(659, 70, replacement)) return false;
    if (!play_tone(784, 70, replacement)) return false;
    if (!play_tone(1047, 130, replacement)) return false;
    if (!play_tone(784, 70, replacement)) return false;
    if (!play_tone(1047, 70, replacement)) return false;
    if (!play_tone(1319, 90, replacement)) return false;
    return play_tone(1568, 260, replacement);
}

/* 返回 false 表示队列里来了更新的播放请求，调用者应立刻切换。 */
static bool play_entry(const word_audio_entry_t *entry, uint32_t *replacement)
{
    if (entry->byte_count < 4 || entry->sample_count == 0 ||
        entry->offset < s_header.data_offset ||
        entry->offset + entry->byte_count > s_header.image_size) {
        ESP_LOGW(TAG, "发音条目越界：offset=%u bytes=%u samples=%u",
                 (unsigned)entry->offset, (unsigned)entry->byte_count,
                 (unsigned)entry->sample_count);
        return true;
    }

    uint8_t state[4];
    if (esp_partition_read(s_partition, entry->offset, state, sizeof(state)) != ESP_OK) {
        ESP_LOGW(TAG, "读取发音状态失败");
        return true;
    }
    int predictor = (int16_t)((uint16_t)state[0] | ((uint16_t)state[1] << 8));
    int step_index = state[2];
    if (step_index > 88) return true;

    size_t frames = 0;
    uint32_t samples_left = entry->sample_count;
    if (!submit_sample(amplify_speech((int16_t)predictor), &frames)) return true;
    samples_left--;

    uint32_t flash_offset = entry->offset + 4;
    uint32_t bytes_left = entry->byte_count - 4;
    while (samples_left && bytes_left) {
        if (xQueueReceive(s_requests, replacement, 0) == pdTRUE) return false;

        size_t chunk = bytes_left > FLASH_CHUNK ? FLASH_CHUNK : bytes_left;
        if (esp_partition_read(s_partition, flash_offset, s_compressed, chunk) != ESP_OK) {
            ESP_LOGW(TAG, "读取 ADPCM 数据失败");
            return true;
        }
        flash_offset += chunk;
        bytes_left -= chunk;

        for (size_t i = 0; i < chunk && samples_left; i++) {
            int16_t sample = decode_nibble(s_compressed[i] & 0x0f,
                                           &predictor, &step_index);
            if (!submit_sample(amplify_speech(sample), &frames)) return true;
            samples_left--;
            if (!samples_left) break;
            sample = decode_nibble(s_compressed[i] >> 4, &predictor, &step_index);
            if (!submit_sample(amplify_speech(sample), &frames)) return true;
            samples_left--;
        }
    }

    if (frames) (void)audio_output_submit_stereo_wait(s_pcm, frames, 250);
    if (samples_left) ESP_LOGW(TAG, "发音数据提前结束，还差 %u 个采样", (unsigned)samples_left);
    return true;
}

static void word_audio_task(void *arg)
{
    (void)arg;
    uint32_t request;
    while (xQueueReceive(s_requests, &request, portMAX_DELAY) == pdTRUE) {
        while (request != REQUEST_STOP) {
            audio_output_flush();
            uint32_t replacement = 0;
            bool finished;
            if (request == REQUEST_QUIZ_CORRECT) {
                finished = play_quiz_effect(true, &replacement);
            } else if (request == REQUEST_QUIZ_WRONG) {
                finished = play_quiz_effect(false, &replacement);
            } else if (request == REQUEST_QUIZ_PERFECT) {
                finished = play_quiz_perfect(&replacement);
            } else {
                const word_audio_entry_t *entry = find_entry(request);
                if (!entry) {
                    ESP_LOGW(TAG, "语音包中没有词条 hash=%08x", (unsigned)request);
                    break;
                }
                finished = play_entry(entry, &replacement);
            }
            if (finished) break;
            request = replacement;
        }
        if (request == REQUEST_STOP) break;
    }
    xSemaphoreGive(s_stopped);
    vTaskDelete(NULL);
}

bool word_audio_init(void)
{
    if (s_available) return true;
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                           WORD_AUDIO_SUBTYPE, "word_audio");
    if (!s_partition) {
        ESP_LOGW(TAG, "未找到 word_audio 分区，学习模式保持静音");
        return false;
    }
    esp_err_t err = esp_partition_read(s_partition, 0, &s_header, sizeof(s_header));
    if (err != ESP_OK || memcmp(s_header.magic, WORD_AUDIO_MAGIC, 8) != 0 ||
        s_header.version != WORD_AUDIO_VERSION ||
        s_header.sample_rate != AUDIO_OUTPUT_SAMPLE_RATE ||
        s_header.entry_count == 0 || s_header.entry_count > 2048 ||
        s_header.index_offset != sizeof(s_header) ||
        s_header.data_offset != sizeof(s_header) +
                                s_header.entry_count * sizeof(word_audio_entry_t) ||
        s_header.image_size > s_partition->size ||
        s_header.data_offset > s_header.image_size) {
        ESP_LOGW(TAG, "英式发音包未安装或格式不兼容");
        return false;
    }

    size_t index_bytes = s_header.entry_count * sizeof(word_audio_entry_t);
    s_index = heap_caps_malloc(index_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_index) return false;
    err = esp_partition_read(s_partition, s_header.index_offset, s_index, index_bytes);
    if (err != ESP_OK || fnv1a_bytes(s_index, index_bytes) != s_header.index_hash) {
        ESP_LOGW(TAG, "英式发音包索引损坏");
        heap_caps_free(s_index);
        s_index = NULL;
        return false;
    }

    for (uint32_t i = 1; i < s_header.entry_count; i++) {
        if (s_index[i - 1].word_hash >= s_index[i].word_hash) {
            ESP_LOGW(TAG, "英式发音包索引未排序或有哈希冲突");
            heap_caps_free(s_index);
            s_index = NULL;
            return false;
        }
    }

    s_requests = xQueueCreate(1, sizeof(uint32_t));
    s_stopped = xSemaphoreCreateBinary();
    if (!s_requests || !s_stopped) goto fail_runtime;
    err = audio_output_init(s_header.sample_rate);
    if (err != ESP_OK) goto fail_runtime;
    if (xTaskCreatePinnedToCore(word_audio_task, "word_pronounce", 4096,
                                NULL, 2, &s_task, 0) != pdPASS) {
        goto fail_audio;
    }

    s_available = true;
    ESP_LOGI(TAG, "英式发音包就绪：%u 个词条，%uHz，镜像 %.2f MiB",
             (unsigned)s_header.entry_count, (unsigned)s_header.sample_rate,
             (double)s_header.image_size / (1024.0 * 1024.0));
    return true;

fail_audio:
    audio_output_shutdown();
fail_runtime:
    if (s_requests) vQueueDelete(s_requests);
    if (s_stopped) vSemaphoreDelete(s_stopped);
    s_requests = NULL;
    s_stopped = NULL;
    heap_caps_free(s_index);
    s_index = NULL;
    return false;
}

void word_audio_play(const char *word)
{
    if (!s_available || !word || !word[0]) return;
    uint32_t hash = word_hash(word);
    if (hash >= REQUEST_QUIZ_PERFECT) {
        ESP_LOGW(TAG, "词条哈希与停止标记冲突：%s", word);
        return;
    }
    xQueueOverwrite(s_requests, &hash);
}

void word_audio_play_quiz_result(bool correct)
{
    if (!s_available) return;
    uint32_t request = correct ? REQUEST_QUIZ_CORRECT : REQUEST_QUIZ_WRONG;
    ESP_LOGI(TAG, "QUIZ 音效：%s", correct ? "答对" : "答错");
    xQueueOverwrite(s_requests, &request);
}

void word_audio_play_quiz_perfect(void)
{
    if (!s_available) return;
    uint32_t request = REQUEST_QUIZ_PERFECT;
    ESP_LOGI(TAG, "QUIZ 音效：本单元全对");
    xQueueOverwrite(s_requests, &request);
}

bool word_audio_is_available(void)
{
    return s_available;
}

void word_audio_shutdown(void)
{
    if (!s_available) return;
    uint32_t stop = REQUEST_STOP;
    xQueueOverwrite(s_requests, &stop);
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGE(TAG, "单词发音任务停止超时，暂不释放共享音频");
        return;
    }

    s_available = false;
    s_task = NULL;
    vQueueDelete(s_requests);
    vSemaphoreDelete(s_stopped);
    s_requests = NULL;
    s_stopped = NULL;
    heap_caps_free(s_index);
    s_index = NULL;
    s_partition = NULL;
    audio_output_shutdown();
}
