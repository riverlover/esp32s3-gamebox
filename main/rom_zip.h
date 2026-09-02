/*
 * ZIP 目录解析和单文件流式解压
 *
 * 这里只实现 ROM 卡包需要的 ZIP 子集：普通单卷 ZIP、store(0) 和 deflate(8)。
 * 目录从文件末尾的中央目录读取，因此兼容 bit 3 data descriptor；不照搬一些
 * 模拟器项目只信第一个 local header 里尺寸的做法。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ROM_ZIP_METHOD_STORE   0
#define ROM_ZIP_METHOD_DEFLATE 8

typedef struct {
    char     name[160];          /* ZIP 内路径，NUL 结尾 */
    uint32_t local_header_offset;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint16_t method;
    uint16_t flags;
} rom_zip_member_t;

/* visitor 返回 false 时提前结束扫描（例如菜单目录已经满了）。 */
typedef bool (*rom_zip_visitor_t)(const rom_zip_member_t *member, void *ctx);
typedef void (*rom_zip_progress_fn)(void *ctx, size_t done, size_t total);

/* 只读 ZIP 的中央目录，不解压文件。archive_size 来自外层已经做过的 stat。 */
esp_err_t rom_zip_scan(const char *path, size_t archive_size,
                       rom_zip_visitor_t visitor, void *ctx);

/* 把一个中央目录条目流式解压/复制进调用方给的 PSRAM 缓冲，并校验 CRC。 */
esp_err_t rom_zip_extract(const char *path, const rom_zip_member_t *member,
                          uint8_t *output, size_t output_size,
                          uint8_t *scratch, size_t scratch_size,
                          size_t *read_chunk_size,
                          rom_zip_progress_fn progress, void *progress_ctx);
