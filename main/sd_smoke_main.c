/*
 * TF 卡最小冒烟固件入口。
 *
 * 用根 CMakeLists 的 -DSD_SMOKE_ONLY=1 才会编进固件；平时正式固件走 main.c。
 * 故意不碰屏、音频、模拟器 —— 烧录体积小，串口日志不被别的子系统冲掉。
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_card.h"

static const char *TAG = "tf_smoke";

void app_main(void)
{
    printf("\n");
    printf("======== TF 冒烟（最小固件）========\n");
    printf("接线：SPI3  CLK=39 MOSI=41 MISO=40 CS=42  3V3/GND\n");
    printf("期望：挂载 → 卡信息 → 根目录 → 写读校验 → 扇区基准\n");
    printf("====================================\n\n");

    esp_err_t err = sd_card_selftest();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "结果 PASS");
        printf("\n*** TF SMOKE PASS ***\n\n");
    } else {
        ESP_LOGE(TAG, "结果 FAIL（%s）", esp_err_to_name(err));
        printf("\n*** TF SMOKE FAIL ***\n");
        printf("提示：%s\n\n", sd_card_mount_hint());
    }

    /* 停在这里反复提醒，方便只开着 monitor 看；按 RESET 重跑整次自检。 */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "冒烟结束，按 RESET 重跑（上次 %s）",
                 err == ESP_OK ? "PASS" : "FAIL");
    }
}
