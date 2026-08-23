/*
 * 从 TF 卡上读游戏列表
 *
 * 开机递归扫描卡上认识的扩展名，按 ROM 头判定平台（**不看目录名**），
 * 建一份目录放在 PSRAM；用户选中之后才把那个文件读进 PSRAM。
 *
 * 早期版本是从 flash 的 roms 分区 mmap 一整块打包镜像，条目直接是指向 flash
 * 的指针。改成 SD 之后那套指针模型不成立了（文件系统没法 mmap），所以 entry
 * 里存的是路径，真正的字节要 rom_store_load() 去读。对调用方来说接口没变：
 * 仍然是 entry -> rom_store_load() -> rom_store_image_release()。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* 目录里最多认这么多个游戏。卡上超出的部分会被忽略并打一行提示 ——
 * 这道上限同时限制了目录本身占的 PSRAM（见 rom_store.c 的 s_pool）。 */
#define ROM_STORE_MAX      256

/* 显示名的缓冲长度（含结尾 NUL）。菜单一行最多也就显示三十几个字符，
 * 名字太长会被截断。 */
#define ROM_STORE_NAME_LEN 48

/* 卡上路径的最大长度（含结尾 NUL）。"/sd/" + 最多三层目录 + 文件名。 */
#define ROM_STORE_PATH_LEN 160

typedef enum {
    ROM_SYSTEM_NES  = 1,
    ROM_SYSTEM_GB   = 2,
    ROM_SYSTEM_GBC  = 3,
    ROM_SYSTEM_SNES = 4,
    ROM_SYSTEM_GENESIS = 5,
} rom_system_t;

typedef struct {
    const char  *name;        /* 显示名（文件名去掉扩展名），NUL 结尾 */
    const char  *path;        /* 卡上的完整路径，NUL 结尾 */
    size_t       size;        /* ROM 字节数，已扣掉 SNES 拷贝机头 */
    /* 老式拷贝机会在 .smc 头部多加 512 字节。扫描时判出来记在这里，
     * 读取时跳过；size 已经是扣掉之后的数。 */
    size_t       file_offset;
    rom_system_t system;
} rom_store_entry_t;

typedef struct {
    uint8_t  *data;
    size_t    size;
    uint32_t  crc32;
    bool      owned; /* 恒为 true；保留字段是为了不动调用方的 release 逻辑 */
} rom_store_image_t;

/* 挂卡并扫描。返回认到的游戏数，0 表示没有可玩的（没插卡、卡挂不上、
 * 或者卡上一个合法 ROM 都没有）—— 调用方应当回退到编译期嵌入的那个 ROM。
 * 可以反复调用，只有第一次真的做事。 */
int rom_store_init(void);

/* 第 i 个游戏（0 <= i < rom_store_init() 的返回值）。越界返回 NULL。
 * 返回的指针在整个运行期间有效。 */
const rom_store_entry_t *rom_store_entry(int i);

/* 把整个 ROM 读进 PSRAM。SNES 传入映射所需余量（extra_bytes）后会直接得到
 * 最终可写缓冲，避免 4 MiB ROM 同时保留两份而耗尽 8 MiB PSRAM。 */
esp_err_t rom_store_load(const rom_store_entry_t *entry, size_t extra_bytes,
                         rom_store_image_t *out);

/* 释放 rom_store_load() 分配的缓冲。 */
void rom_store_image_release(rom_store_image_t *image);

/* TF 卡的已用字节数和总容量，给诊断画面用。没挂上卡时两个都是 0。
 * 用 uint64_t 而不是 size_t：size_t 在 ESP32 上是 32 位，32 GB 的卡会溢出。 */
void rom_store_usage(uint64_t *used_bytes, uint64_t *capacity_bytes);
