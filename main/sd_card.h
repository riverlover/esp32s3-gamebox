/*
 * TF (microSD) 卡，走 SPI3
 *
 * 目前只提供上电自检：挂载 -> 打印卡信息 -> 列根目录 -> 写读校验 -> 卸载。
 * 还没接进 rom_store，先确认硬件这条路通。
 */

#pragma once

#include "esp_err.h"

/* 挂载并跑一遍完整自检，全程只往串口输出，不碰屏幕。
 * 结束时会卸载并释放 SPI3，所以模拟器跑起来后卡这边不占任何内存。
 * 返回 ESP_OK 表示读写全部通过。 */
esp_err_t sd_card_selftest(void);
