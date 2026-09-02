#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 各核心只负责把自己的状态写进/读出一个普通文件；槽位、TF 目录、原子替换
 * 和游戏内菜单都集中在这一层，避免四个模拟器各自长出一套按键和文件命名。 */
typedef bool (*game_state_io_fn)(const char *path, void *ctx);
typedef void (*game_reset_fn)(bool hard, void *ctx);

typedef struct {
    const char       *system;      /* 只传固定的小写目录名：nes/gb/gbc/snes/md */
    uint32_t          rom_crc;
    game_state_io_fn  save_state;
    game_state_io_fn  load_state;
    game_reset_fn     reset;
    void             *ctx;
} game_menu_config_t;

typedef enum {
    GAME_MENU_CONTINUE = 0,
    GAME_MENU_RESTART,
} game_menu_result_t;

/* SELECT+X 由各核心截获后调用。返回 RESTART 表示用户选了 Quit，或
 * Save & Quit 已经成功；调用方负责关状态灯并 esp_restart()。 */
game_menu_result_t game_menu_open(const game_menu_config_t *config);
