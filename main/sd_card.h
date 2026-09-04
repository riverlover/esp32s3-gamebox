/*
 * TF (microSD) 卡，走 SPI3
 *
 * 接线和硬件坑见 sd_card.c 文件头与 docs/hardware.md §10。
 * ROM 现在全部从这张卡读（见 rom_store.c），所以挂上之后不再卸载。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 卡的挂载点。rom_store 拼路径要用，所以公开出来。 */
#define SD_MOUNT_POINT "/sd"

/* 挂载 TF 卡。可以反复调用，只有第一次真的做事。
 * 挂载失败只返回错误、不 abort —— 没卡也该能开机（回退到内嵌 ROM）。 */
esp_err_t sd_card_mount(void);

/* 当前是否已挂载。 */
bool sd_card_mounted(void);

/* 最近一次挂载失败的短中文原因（给屏上提示）。已挂载或尚未尝试时返回空串。 */
const char *sd_card_mount_hint(void);

/* 卡容量与已用字节数。没挂载时两个都写 0。 */
void sd_card_usage(uint64_t *used_bytes, uint64_t *total_bytes);

/* 上电自检：挂载 -> 打印卡信息 -> 列根目录 -> 写读校验。全程只往串口输出。
 * 跑完**不卸载**（ROM 要接着用这张卡）。返回 ESP_OK 表示读写全部通过。 */
esp_err_t sd_card_selftest(void);
