/*
 * retro-go 框架垫片（pce-go 专用）
 *
 * 和 components/nofrendo/rg_system.h 是同一个套路：上游 pce-go 只在
 * `#ifdef RETRO_GO` 分支里用到框架的两样东西 ——
 *
 *   rg_system_log()  日志
 *   rg_crc32()       ROM CRC
 *
 * 提供这两个，pce-go 源码一行都不用改，上游更新可以直接覆盖。
 *
 * ⚠ 为什么不能省掉这个文件、直接走 pce-go.h 的 `#else` 分支：
 * 那个分支把 crc32_le() 定义成常量 0，而 pce-go.c 的 romFlags[] 是**按 CRC
 * 查表**的，查不中就没有以下修正：
 *   - Populous 的 ONBOARD_RAM（卡带自带 32 KB 额外 RAM，不给就跑不起来）
 *   - Blazing Lazers 的 TWO_PART_ROM
 *   - Legend of Hero Tonma 的 US_ENCODED 解密
 * 后者还有个 `ROM_DATA[0x1FFF] < 0xE0` 的兜底启发，前两个没有兜底。
 *
 * ⚠ 为什么放在 shim/ 子目录而不是组件根目录：nofrendo 的垫片也叫
 * rg_system.h，两个组件的 INCLUDE_DIRS 都会进 main 的头文件搜索路径，
 * 同名文件谁先谁后取决于 REQUIRES 顺序。放进 PRIV_INCLUDE_DIRS 之后
 * 只有 pce-go 自己的源文件看得见，main 那边不受影响。
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_rom_crc.h"    /* esp_rom_crc32_le */

#define RG_LOG_PRINTF 0

#define rg_system_log(level, ctx, ...)  printf(__VA_ARGS__)

/* retro-go 在 ESP 平台上就是直接调 ROM 里的 crc32_le，所以这里算出来的
 * 校验和与上游 romFlags[] 的条目一致，查表能真正命中。 */
#define rg_crc32(crc, buf, len) \
    esp_rom_crc32_le((uint32_t)(crc), (const uint8_t *)(buf), (uint32_t)(len))
