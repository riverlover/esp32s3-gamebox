/*
 * 开机选游戏
 *
 * 从 TF 卡（见 rom_store.h）读列表，画在屏上，摇杆上下选，A/START 确认。
 * 选单只在开机时出现 —— 想换游戏按板子上的 RST 重启。
 *
 * 为什么不做游戏里热切换：nofrendo 没有「卸载卡再装另一张」的干净路径，
 * 硬来容易留下 PPU 状态残留或者内存泄漏。重启一次才两秒，不值得为它冒险。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rom_store.h"

typedef enum {
    ROM_MENU_FALLBACK,  /* 无游戏提示页上按 A，启动编译期内置游戏 */
    ROM_MENU_SELECTED,  /* 已通过出参返回玩家选中的游戏 */
    ROM_MENU_BACK,      /* 平台页按 B，调用方返回开机模式选择 */
} rom_menu_result_t;

/* 显示选单并阻塞，直到用户选定一个游戏或从平台页返回。
 *
 * 选中的目录项通过 `*entry` 返回；模拟器再按条目决定是直接 mmap 还是解压。
 *
 * 返回 ROM_MENU_FALLBACK 表示玩家在「无游戏」提示页主动选了玩内置 ROM
 * （没插卡、卡挂不上、卡上没有合法 ROM 时会出现该页）。这时出参不变。
 * 返回 ROM_MENU_BACK 表示玩家按 B 回到开机模式选择——无游戏时默认也走这条，
 * 不能静默 FALLBACK，否则看起来像菜单坏了直接进超级玛丽。
 *
 * 前置条件：display_init() 已经成功。输入初始化在函数内部做（幂等，
 * nes_emu_run() 之后再调一次没关系）。 */
rom_menu_result_t rom_menu_pick(const rom_store_entry_t **entry);
