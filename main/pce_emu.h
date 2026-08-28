#pragma once

#include "esp_err.h"
#include "rom_store.h"

/* 启动 PC Engine / TurboGrafx-16。和其它四个核心一样：成功就不再返回，
 * 失败才带着错误码回到调用方。 */
esp_err_t pce_emu_run(const rom_store_entry_t *entry);
