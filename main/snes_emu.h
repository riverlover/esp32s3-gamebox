#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rom_store.h"

/* 运行一个 SNES ROM（.sfc/.smc），正常情况下不返回。
 * entry 的 ROM 会直接解压/复制进最终 PSRAM 缓冲，因为 snes9x 的内存映射
 * 会就地改写 ROM 头部区域。
 *
 * ⚠ L/R 暂无实体键；即时状态由统一游戏内菜单写入 TF 四槽。 */
esp_err_t snes_emu_run(const rom_store_entry_t *entry);
