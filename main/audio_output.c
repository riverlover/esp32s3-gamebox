/*
 * MAX98357 I2S 音频输出
 *
 * 所有模拟器共用的非阻塞宿主后端。nofrendo 没有音频回调，所以仍用
 * --wrap=apu_emulate 接出 PCM；gnuboy 则直接调用 audio_output_submit_stereo()。
 *
 * 模拟线程只生成采样并向队列复制，绝不等 I2S。消费任务留在核 0 的低优先级；
 * 核 1 同时承担 LCD 和 Genesis 声音，实机把 I2S 也迁过去会让推屏从 15 ms
 * 恶化到 21 ms，并开始丢音频包。
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "audio_output.h"
#include "nes/nes.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

#define I2S_PIN_BCLK       4
#define I2S_PIN_LRC        5
#define I2S_PIN_DOUT       6
#define AUDIO_QUEUE_FRAMES 4
#define AUDIO_FADE_MS      20      /* 音量变化时缓变，避免 MAX98357 突然跳变发出爆音 */
#define AUDIO_GAIN_MAX_Q15 ((32768 * 9) / 10)  /* 满量程留 10% 余量防削波，档位 100% 封顶在这 */
#define AUDIO_VOLUME_DEFAULT 50    /* 无 NVS 记录时的开机默认档位（0~100，10 的整数倍） */
/* 与 display.c 背光共用命名空间；键名短是因为 NVS 键最长 15 字符。 */
#define UI_NVS_NS          "ui_prefs"
#define UI_NVS_KEY_VOLUME  "volume"

typedef struct {
    uint16_t sample_count;
    int16_t stereo[AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET * 2];
} audio_packet_t;

static i2s_chan_handle_t s_tx;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static int16_t s_mono[NES_AUDIO_MAX_SAMPLES_PER_FRAME];
static audio_packet_t s_producer_packet;
static uint32_t s_dropped;
static uint32_t s_write_errors;
static uint32_t s_sample_rate;
static atomic_int s_volume = ATOMIC_VAR_INIT(AUDIO_VOLUME_DEFAULT);

/* 供 rgb_led.c 轮询的音量联动峰值：audio_task() 按已经过档位缩放的采样
 * （不是模拟器产出的原始 PCM）记录峰值，静音或调低音量时灯自然跟着暗，
 * 不会出现"听不见但灯还在闪"的错位。take 语义（读后清零）让每次轮询
 * 拿到的是"上次轮询以来"的峰值，不会被同一窗口内的旧值覆盖漏掉瞬态。 */
static atomic_int s_recent_peak = ATOMIC_VAR_INIT(0);

/* 百分比是给人看的档位，不该直接当线性振幅。线性 50->55 只增加约 0.8 dB，
 * 小喇叭上几乎听不出变化；这条以 50% 为锚点的 S 曲线保持默认响度不变，
 * 同时把中间常用区的档位差拉开、把两端压细。 */
static int volume_curve_percent(int percent)
{
    if (percent <= 0) return 0;
    if (percent >= 100) return 100;
    if (percent <= 50) return (percent * percent + 25) / 50;
    int remaining = 100 - percent;
    return 100 - (remaining * remaining + 25) / 50;
}

static int32_t volume_gain_q15(int percent)
{
    return AUDIO_GAIN_MAX_Q15 * volume_curve_percent(percent) / 100;
}

static int volume_clamp(int percent)
{
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

/* 打开 ui_prefs。失败时返回 false，调用方继续用内存默认值，不整区 erase NVS。 */
static bool ui_nvs_open(nvs_handle_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 未就绪，音量只在本次开机有效：%s", esp_err_to_name(err));
        return false;
    }
    err = nvs_open(UI_NVS_NS, NVS_READWRITE, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ui_prefs 打不开：%s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int volume_load_or_default(void)
{
    nvs_handle_t nvs;
    if (!ui_nvs_open(&nvs)) return AUDIO_VOLUME_DEFAULT;

    uint8_t saved = AUDIO_VOLUME_DEFAULT;
    esp_err_t err = nvs_get_u8(nvs, UI_NVS_KEY_VOLUME, &saved);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return AUDIO_VOLUME_DEFAULT;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读音量失败：%s", esp_err_to_name(err));
        return AUDIO_VOLUME_DEFAULT;
    }
    if (saved > 100) {
        ESP_LOGW(TAG, "音量 NVS 值异常 %u，回退默认", (unsigned)saved);
        return AUDIO_VOLUME_DEFAULT;
    }
    return (int)saved;
}

static void volume_save(int percent)
{
    nvs_handle_t nvs;
    if (!ui_nvs_open(&nvs)) return;

    esp_err_t err = nvs_set_u8(nvs, UI_NVS_KEY_VOLUME, (uint8_t)percent);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "音量保存失败：%s", esp_err_to_name(err));
    }
}

esp_err_t audio_output_settings_init(void)
{
    int percent = volume_load_or_default();
    atomic_store_explicit(&s_volume, percent, memory_order_relaxed);
    ESP_LOGI(TAG, "音量恢复：%d%%", percent);
    return ESP_OK;
}

int audio_output_get_volume(void)
{
    return atomic_load_explicit(&s_volume, memory_order_relaxed);
}

esp_err_t audio_output_set_volume(int percent)
{
    percent = volume_clamp(percent);
    atomic_store_explicit(&s_volume, percent, memory_order_relaxed);
    volume_save(percent);
    ESP_LOGI(TAG, "音量：%d%%（已写入 NVS）", percent);
    return ESP_OK;
}

int audio_output_take_peak(void)
{
    return atomic_exchange_explicit(&s_recent_peak, 0, memory_order_relaxed);
}

/* 原样透传满量程 PCM——不再像早期那样固定 >>2 留 25% 天花板。缩放全部
 * 交给 audio_task() 里的 gain_q15，档位 100% 封顶在 AUDIO_GAIN_MAX_Q15
 * （满量程的 90%，留 10% 余量防削波）。这段还没在新喇叭上测过高档位
 * 会不会削波/失真，听到破音就把默认档位往下调，或者把这个余量再调大。 */
static bool submit_stereo(const int16_t *samples, size_t frame_count, TickType_t wait)
{
    if (!s_queue || !samples || frame_count == 0) return false;
    bool complete = true;

    /* 超过一个包的量拆开排队，不再截断。SNES 跑不满 60 fps 时按墙钟补的
     * 采样数会超过 AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET（见 snes_emu.c），
     * 旧的截断写法会把多出来的那截静默丢掉 —— 正好是最难查的那类
     * “声音有点不对”。
     * ⚠ samples 不能是 s_producer_packet.stereo 本身：拆包后第二轮的
     *   memcpy 就会自我重叠。__wrap_apu_emulate() 那条路径靠 NES 每帧
     *   不超过一个包来保证只循环一次。 */
    while (frame_count > 0) {
        size_t chunk = frame_count > AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET
                     ? AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET : frame_count;

        s_producer_packet.sample_count = chunk;
        memcpy(s_producer_packet.stereo, samples, chunk * 2 * sizeof(int16_t));

        if (xQueueSend(s_queue, &s_producer_packet, wait) != pdTRUE) {
            s_dropped++;
            complete = false;
        }
        samples     += chunk * 2;
        frame_count -= chunk;
    }
    return complete;
}

void audio_output_submit_stereo(const int16_t *samples, size_t frame_count)
{
    (void)submit_stereo(samples, frame_count, 0);
}

bool audio_output_submit_stereo_wait(const int16_t *samples, size_t frame_count,
                                     uint32_t timeout_ms)
{
    TickType_t wait = timeout_ms == UINT32_MAX
                    ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return submit_stereo(samples, frame_count, wait);
}

void audio_output_flush(void)
{
    if (s_queue) xQueueReset(s_queue);
}

/* 由链接器替换 nofrendo 的 apu_emulate() 调用。函数仍在模拟线程里运行，
 * 所以这里不能阻塞等待 I2S；队列满时宁可丢当前帧并计数。 */
void __wrap_apu_emulate(void)
{
    int refresh_rate = nes_getptr()->refresh_rate;
    int sample_count = NES_AUDIO_SAMPLE_RATE / refresh_rate;
    if (sample_count > NES_AUDIO_MAX_SAMPLES_PER_FRAME) {
        /* 目前 nofrendo 只会给 50/60 Hz；遇到意外制式时宁可截断，也不能写出缓冲。 */
        sample_count = NES_AUDIO_MAX_SAMPLES_PER_FRAME;
    }
    apu_process(s_mono, sample_count, false);

    for (int i = 0; i < sample_count; i++) {
        int16_t sample = s_mono[i];
        s_producer_packet.stereo[i * 2] = sample;
        s_producer_packet.stereo[i * 2 + 1] = sample;
    }
    audio_output_submit_stereo(s_producer_packet.stereo, sample_count);
}

static void audio_task(void *arg)
{
    audio_packet_t packet;
    uint32_t frames_written = 0;
    int32_t gain_q15 = volume_gain_q15(audio_output_get_volume());
    uint32_t fade_frames = (s_sample_rate * AUDIO_FADE_MS + 999) / 1000;
    int32_t fade_step = (AUDIO_GAIN_MAX_Q15 + (int32_t)fade_frames - 1) / (int32_t)fade_frames;

    while (1) {
        if (xQueueReceive(s_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        if (packet.sample_count == 0) {
            xSemaphoreGive(s_stopped);
            vTaskDelete(NULL);
        }

        /* 不停 I2S、不停队列：静音仍持续送零采样，因此恢复时不用重建 DMA，
         * 也不会让模拟线程因为队列状态变化而丢帧。20ms 线性淡变只在消费侧
         * 修改栈上的包，NES/GB/GBC 的生产路径完全不用分叉。 */
        int32_t target_gain = volume_gain_q15(audio_output_get_volume());
        size_t packet_bytes = packet.sample_count * 2 * sizeof(int16_t);
        if (gain_q15 == 0 && target_gain == 0) {
            /* 稳定静音是常态路径，整包清零比每个采样做乘法快得多。 */
            memset(packet.stereo, 0, packet_bytes);
        } else {
            /* AUDIO_GAIN_MAX_Q15 不是真正的酉增益（90% 满量程），所以
             * "稳定不淡变就跳过缩放"这条快捷路径已经不安全——之前直接
             * 判等 32768 的写法在满量程封顶改成 90% 后会漏乘，把没缩放
             * 过的原始采样直接送 I2S。改成每个采样都过一遍，顺便把缩放
             * 后（也就是耳朵真正听到）的峰值记下来供 LED 联动读取。 */
            int32_t peak = 0;
            for (uint16_t i = 0; i < packet.sample_count; i++) {
                if (gain_q15 < target_gain) {
                    gain_q15 += fade_step;
                    if (gain_q15 > target_gain) gain_q15 = target_gain;
                } else if (gain_q15 > target_gain) {
                    gain_q15 -= fade_step;
                    if (gain_q15 < target_gain) gain_q15 = target_gain;
                }
                int16_t l = (int16_t)((int32_t)packet.stereo[i * 2] * gain_q15 / 32768);
                int16_t r = (int16_t)((int32_t)packet.stereo[i * 2 + 1] * gain_q15 / 32768);
                packet.stereo[i * 2] = l;
                packet.stereo[i * 2 + 1] = r;

                int32_t mag = l < 0 ? -(int32_t)l : l;
                if (mag > peak) peak = mag;
                mag = r < 0 ? -(int32_t)r : r;
                if (mag > peak) peak = mag;
            }
            if (peak > atomic_load_explicit(&s_recent_peak, memory_order_relaxed)) {
                atomic_store_explicit(&s_recent_peak, peak, memory_order_relaxed);
            }
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx, packet.stereo, packet_bytes,
                                          &bytes_written,
                                          pdMS_TO_TICKS(1000));
        if (err != ESP_OK || bytes_written != packet_bytes) {
            s_write_errors++;
            ESP_LOGW(TAG, "I2S 写入异常：%s，%u/%u 字节",
                     esp_err_to_name(err), (unsigned)bytes_written,
                     (unsigned)packet_bytes);
        }

        frames_written++;
        if (frames_written % 300 == 0) {
            ESP_LOGI(TAG,
                     "I2S %uHz：%u 帧，排队 %u，丢帧 %u，写错 %u，音量 %d%%，栈余 %uB",
                     (unsigned)s_sample_rate, (unsigned)frames_written,
                     (unsigned)uxQueueMessagesWaiting(s_queue),
                     (unsigned)s_dropped, (unsigned)s_write_errors,
                     audio_output_get_volume(),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

esp_err_t audio_output_init(uint32_t sample_rate)
{
    if (sample_rate == 0) return ESP_ERR_INVALID_ARG;
    if (s_tx && s_sample_rate == sample_rate) return ESP_OK;
    if (s_tx) {
        ESP_LOGI(TAG, "I2S 采样率从 %uHz 切换到 %uHz",
                 (unsigned)s_sample_rate, (unsigned)sample_rate);
        audio_output_shutdown();
    }
    if (audio_output_get_volume() == 0) {
        /* 档位只在开机选单里改，进入游戏后本局状态固定。0% 时连 I2S、DMA、
         * 队列和消费任务都不创建；各模拟器仍可推进自己的混音器状态，只把
         * PCM 丢掉。下次在菜单调到非零档位后重启游戏即可正常初始化输出。 */
        ESP_LOGI(TAG, "音量 0%%：不启动 MAX98357/I2S");
        return ESP_OK;
    }
    s_sample_rate = sample_rate;

    s_queue = xQueueCreate(AUDIO_QUEUE_FRAMES, sizeof(audio_packet_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = NES_AUDIO_MAX_SAMPLES_PER_FRAME;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) goto fail_queue;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_PIN_BCLK,
            .ws = I2S_PIN_LRC,
            .dout = I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) goto fail_channel;
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) goto fail_channel;

    /* 包缓冲本身接近 2 KB，I2S 写入和带中文的诊断日志还会叠加调用栈。
     * 4096 字节在连续运行到约 900 包时实测触发栈溢出；6144 留出余量，
     * 同时用上面的 high-water mark 持续观察，而不是靠短时启动判断。 */
    BaseType_t created = xTaskCreatePinnedToCore(
        audio_task, "game_audio", 6144, NULL, 3, &s_task, 0);
    if (created != pdPASS) {
        err = ESP_ERR_NO_MEM;
        i2s_channel_disable(s_tx);
        goto fail_channel;
    }

    ESP_LOGI(TAG,
             "MAX98357 就绪：%uHz/16-bit，BCLK=%d LRC=%d DIN=%d，音量 %d%%",
             (unsigned)sample_rate, I2S_PIN_BCLK, I2S_PIN_LRC, I2S_PIN_DOUT,
             audio_output_get_volume());
    return ESP_OK;

fail_channel:
    i2s_del_channel(s_tx);
    s_tx = NULL;
fail_queue:
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    vQueueDelete(s_queue);
    s_queue = NULL;
    return err;
}

void audio_output_shutdown(void)
{
    if (!s_tx) return;

    /* 队列可能正塞着 80 ms 的语音。先清空再送零长度哨兵，才能保证消费任务
     * 很快退出；直接删任务有机会把它截在 i2s_channel_write() 内部并留下锁。 */
    /* audio_packet_t 含 528 帧立体声，整个对象约 2.1 KB。这里若在调用栈上
     * 临时创建，app_main -> word_study_run -> word_audio_shutdown 的嵌套栈
     * 会超过主任务仅有的 3584 字节，按 B 退出 WORDS 时直接触发栈溢出重启。
     * 单词播放任务已经先退出，此时没有生产者会再碰这个全局包，复用它发送
     * 零长度哨兵既不增加常驻内存，也不再消耗主任务栈。 */
    s_producer_packet.sample_count = 0;
    xQueueReset(s_queue);
    if (xQueueSend(s_queue, &s_producer_packet, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(1500)) != pdTRUE) {
            ESP_LOGE(TAG, "音频任务停止超时，保留 I2S 避免在写入中强拆");
            return;
        }
    } else {
        ESP_LOGE(TAG, "音频停止哨兵发送失败，保留 I2S");
        return;
    }

    s_task = NULL;
    esp_err_t err = i2s_channel_disable(s_tx);
    if (err != ESP_OK) ESP_LOGW(TAG, "I2S 停止异常：%s", esp_err_to_name(err));
    err = i2s_del_channel(s_tx);
    if (err != ESP_OK) ESP_LOGW(TAG, "I2S 释放异常：%s", esp_err_to_name(err));
    s_tx = NULL;
    vQueueDelete(s_queue);
    s_queue = NULL;
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    s_sample_rate = 0;
    ESP_LOGI(TAG, "MAX98357/I2S 已释放");
}

bool audio_output_ready(void)
{
    return s_queue != NULL;
}

esp_err_t audio_output_beep(uint16_t freq_hz, uint16_t duration_ms)
{
    if (!s_queue || s_sample_rate == 0) return ESP_ERR_INVALID_STATE;
    if (freq_hz < 100) freq_hz = 100;
    if (freq_hz > 4000) freq_hz = 4000;
    if (duration_ms < 20) duration_ms = 20;
    if (duration_ms > 500) duration_ms = 500;

    /* 半周期采样数；方波足够听清接线是否通，不必上 sinf。 */
    uint32_t half = s_sample_rate / (2u * (uint32_t)freq_hz);
    if (half == 0) half = 1;
    uint32_t period = half * 2;

    size_t total = (size_t)s_sample_rate * duration_ms / 1000u;
    int16_t buf[AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET * 2];
    uint32_t phase = 0;
    size_t done = 0;

    while (done < total) {
        size_t n = total - done;
        if (n > AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET) {
            n = AUDIO_OUTPUT_MAX_FRAMES_PER_PACKET;
        }
        for (size_t i = 0; i < n; i++) {
            int16_t s = (phase < half) ? 9000 : -9000;
            buf[i * 2]     = s;
            buf[i * 2 + 1] = s;
            if (++phase >= period) phase = 0;
        }
        audio_output_submit_stereo(buf, n);
        done += n;
        /* 按墙钟让消费任务跟上：一包约 n/rate 秒，再留一点余量。 */
        uint32_t wait_ms = (uint32_t)(n * 1000u / s_sample_rate);
        if (wait_ms < 1) wait_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    return ESP_OK;
}
