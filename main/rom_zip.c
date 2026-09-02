/*
 * ROM ZIP 支持
 *
 * 解析目录自己做，DEFLATE 用 ESP32-S3 ROM 里的 miniz tinfl，固件不带一整份
 * ZIP 库。压缩数据按 64→32→16→8→4 KB 读进内部 DMA RAM，再直接解到 PSRAM，
 * 不会为了一个 4 MiB ROM 同时保留整份压缩数据。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rom_zip.h"

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "rom_zip";

#define ZIP_LOCAL_MAGIC   0x04034b50u
#define ZIP_CDIR_MAGIC    0x02014b50u
#define ZIP_EOCD_MAGIC    0x06054b50u
#define ZIP_LOCAL_SIZE    30u
#define ZIP_CDIR_SIZE     46u
#define ZIP_EOCD_SIZE     22u
#define ZIP_TAIL_MAX      (ZIP_EOCD_SIZE + 65535u)
#define ZIP_CDIR_MAX      (256u * 1024u)
#define ZIP_READ_CHUNK    (64u * 1024u)
#define ZIP_FAST_HEADER   512u

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool rom_extension(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".nes") == 0 || strcasecmp(ext, ".gb") == 0 ||
           strcasecmp(ext, ".gbc") == 0 || strcasecmp(ext, ".sfc") == 0 ||
           strcasecmp(ext, ".smc") == 0 || strcasecmp(ext, ".md") == 0 ||
           strcasecmp(ext, ".bin") == 0;
}

static bool seek_to(int fd, size_t offset)
{
    return lseek(fd, (off_t)offset, SEEK_SET) >= 0;
}

static bool read_exact(int fd, void *dst, size_t len)
{
    uint8_t *p = dst;
    while (len) {
        ssize_t got = read(fd, p, len);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += (size_t)got;
        len -= (size_t)got;
    }
    return true;
}

static uint8_t *alloc_dma_chunk(size_t *size)
{
    *size = ZIP_READ_CHUNK;
    while (*size >= 4096) {
        uint8_t *p = heap_caps_malloc(*size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (p) return p;
        *size /= 2;
    }
    *size = 0;
    return NULL;
}

static uint8_t *get_dma_chunk(uint8_t *scratch, size_t scratch_size,
                              size_t *size, bool *owned)
{
    if (scratch && scratch_size >= 4096) {
        *size = scratch_size > ZIP_READ_CHUNK ? ZIP_READ_CHUNK : scratch_size;
        *owned = false;
        return scratch;
    }
    *owned = true;
    return alloc_dma_chunk(size);
}

/* dst 通常在 PSRAM，不能直接交给 SDSPI。这里单独反弹，扫描 ZIP 目录也不会
 * 意外退化成一次一个 512 字节扇区。 */
static bool read_region(int fd, size_t offset, uint8_t *dst, size_t len)
{
    if (!seek_to(fd, offset)) return false;

    size_t chunk_size;
    uint8_t *chunk = alloc_dma_chunk(&chunk_size);
    if (!chunk) return false;

    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        if (want > chunk_size) want = chunk_size;
        if (!read_exact(fd, chunk, want)) break;
        memcpy(dst + done, chunk, want);
        done += want;
    }
    free(chunk);
    return done == len;
}

/* EOCD 最多离文件尾 65557 字节。绝大多数卡包没有 comment，先只读末尾 4 KB；
 * 找不到时才付出读满标准上限的代价。 */
static bool find_eocd(int fd, size_t archive_size, uint8_t eocd[ZIP_EOCD_SIZE])
{
    size_t sizes[2] = {
        archive_size < 4096 ? archive_size : 4096,
        archive_size < ZIP_TAIL_MAX ? archive_size : ZIP_TAIL_MAX,
    };

    for (int pass = 0; pass < 2; pass++) {
        size_t tail_size = sizes[pass];
        if (tail_size < ZIP_EOCD_SIZE || (pass == 1 && sizes[1] == sizes[0])) continue;

        uint8_t *tail = heap_caps_malloc(tail_size,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tail) return false;
        bool ok = read_region(fd, archive_size - tail_size, tail, tail_size);
        if (ok) {
            for (size_t i = tail_size - ZIP_EOCD_SIZE + 1; i-- > 0; ) {
                if (le32(tail + i) != ZIP_EOCD_MAGIC) continue;
                uint16_t comment = le16(tail + i + 20);
                if (i + ZIP_EOCD_SIZE + comment != tail_size) continue;
                memcpy(eocd, tail + i, ZIP_EOCD_SIZE);
                free(tail);
                return true;
            }
        }
        free(tail);
    }
    return false;
}

/* ROM 卡包几乎都是“一个 ZIP 里一个 ROM”，且 seekable ZIP 会把尺寸直接写在
 * 第一个 local header。一次顺序读就够识别，能省掉末尾 EOCD + 中央目录两轮
 * 随机访问；本机这张高延迟卡上几十个 ZIP 的差距是几十秒。
 *
 * bit 3 data descriptor、前置目录/说明文件、超长文件名等复杂情况返回 false，
 * 调用方继续走下面完整的中央目录解析，不牺牲兼容性。 */
static bool scan_fast_local_header(int fd, size_t archive_size,
                                   rom_zip_visitor_t visitor, void *ctx)
{
    size_t read_size = archive_size < ZIP_FAST_HEADER ? archive_size : ZIP_FAST_HEADER;
    if (read_size < ZIP_LOCAL_SIZE) return false;

    uint8_t *buf = heap_caps_malloc(read_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) return false;
    bool ok = seek_to(fd, 0) && read_exact(fd, buf, read_size);
    if (!ok || le32(buf) != ZIP_LOCAL_MAGIC) {
        free(buf);
        return false;
    }

    uint16_t flags = le16(buf + 6);
    uint16_t method = le16(buf + 8);
    uint16_t name_len = le16(buf + 26);
    uint16_t extra_len = le16(buf + 28);
    size_t data_offset = ZIP_LOCAL_SIZE + (size_t)name_len + extra_len;
    if ((flags & (1u << 3)) || name_len == 0 || name_len >= 160 ||
        ZIP_LOCAL_SIZE + (size_t)name_len > read_size || data_offset > archive_size) {
        free(buf);
        return false;
    }

    const uint8_t *raw_name = buf + ZIP_LOCAL_SIZE;
    const uint8_t *base = raw_name;
    size_t base_len = name_len;
    for (size_t k = 0; k < name_len; k++) {
        if (raw_name[k] == '/' || raw_name[k] == '\\') {
            base = raw_name + k + 1;
            base_len = name_len - k - 1;
        }
    }
    if (base_len == 0 || base_len >= 160) {
        free(buf);
        return false;
    }

    rom_zip_member_t member = {
        .local_header_offset = 0,
        .compressed_size = le32(buf + 18),
        .uncompressed_size = le32(buf + 22),
        .crc32 = le32(buf + 14),
        .method = method,
        .flags = flags,
    };
    memcpy(member.name, base, base_len);
    member.name[base_len] = '\0';

    bool candidate = rom_extension(member.name) && member.uncompressed_size > 0 &&
                     member.compressed_size <= archive_size - data_offset;
    /* visitor 返回 true 表示还想继续找：例如第一个 ROM 加密、压缩方式不支持，
     * 或尺寸不合理。此时必须回退中央目录，不能把后面的可用 ROM 漏掉。 */
    bool handled = candidate && !visitor(&member, ctx);
    free(buf);
    return handled;
}

esp_err_t rom_zip_scan(const char *path, size_t archive_size,
                       rom_zip_visitor_t visitor, void *ctx)
{
    if (!path || !visitor || archive_size < ZIP_EOCD_SIZE) return ESP_ERR_INVALID_ARG;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGW(TAG, "打不开 %s（errno %d: %s）", path, errno, strerror(errno));
        return ESP_ERR_NOT_FOUND;
    }

    if (scan_fast_local_header(fd, archive_size, visitor, ctx)) {
        close(fd);
        return ESP_OK;
    }

    uint8_t eocd[ZIP_EOCD_SIZE];
    if (!find_eocd(fd, archive_size, eocd)) {
        ESP_LOGW(TAG, "%s 找不到 ZIP 中央目录", path);
        close(fd);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint16_t disk = le16(eocd + 4);
    uint16_t cdir_disk = le16(eocd + 6);
    uint16_t disk_entries = le16(eocd + 8);
    uint16_t entries = le16(eocd + 10);
    uint32_t cdir_size = le32(eocd + 12);
    uint32_t cdir_offset = le32(eocd + 16);
    if (disk != 0 || cdir_disk != 0 || disk_entries != entries || entries == UINT16_MAX ||
        cdir_size == UINT32_MAX || cdir_offset == UINT32_MAX) {
        ESP_LOGW(TAG, "%s 是多卷或 ZIP64，当前不支持", path);
        close(fd);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (cdir_size > ZIP_CDIR_MAX || cdir_size < (size_t)entries * ZIP_CDIR_SIZE ||
        (size_t)cdir_offset + cdir_size > archive_size) {
        ESP_LOGW(TAG, "%s 的中央目录尺寸异常（%u 字节，%u 项）", path,
                 (unsigned)cdir_size, (unsigned)entries);
        close(fd);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *cdir = heap_caps_malloc(cdir_size ? cdir_size : 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cdir) {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    bool read_ok = read_region(fd, cdir_offset, cdir, cdir_size);
    close(fd);
    if (!read_ok) {
        ESP_LOGW(TAG, "%s 的中央目录读取失败", path);
        free(cdir);
        return ESP_FAIL;
    }

    size_t pos = 0;
    for (uint16_t i = 0; i < entries; i++) {
        if (pos + ZIP_CDIR_SIZE > cdir_size || le32(cdir + pos) != ZIP_CDIR_MAGIC) {
            ESP_LOGW(TAG, "%s 的第 %u 个中央目录项损坏", path, (unsigned)i);
            free(cdir);
            return ESP_ERR_INVALID_RESPONSE;
        }

        const uint8_t *h = cdir + pos;
        uint16_t name_len = le16(h + 28);
        uint16_t extra_len = le16(h + 30);
        uint16_t comment_len = le16(h + 32);
        size_t record_size = ZIP_CDIR_SIZE + (size_t)name_len + extra_len + comment_len;
        if (record_size > cdir_size - pos) {
            ESP_LOGW(TAG, "%s 的第 %u 个中央目录项越界", path, (unsigned)i);
            free(cdir);
            return ESP_ERR_INVALID_SIZE;
        }

        const uint8_t *raw_name = h + ZIP_CDIR_SIZE;
        bool is_dir = name_len > 0 && raw_name[name_len - 1] == '/';
        if (!is_dir) {
            const uint8_t *base = raw_name;
            size_t base_len = name_len;
            for (size_t k = 0; k < name_len; k++) {
                if (raw_name[k] == '/' || raw_name[k] == '\\') {
                    base = raw_name + k + 1;
                    base_len = name_len - k - 1;
                }
            }

            if (base_len > 0 && base_len < sizeof(((rom_zip_member_t *)0)->name)) {
                rom_zip_member_t member = {
                    .local_header_offset = le32(h + 42),
                    .compressed_size = le32(h + 20),
                    .uncompressed_size = le32(h + 24),
                    .crc32 = le32(h + 16),
                    .method = le16(h + 10),
                    .flags = le16(h + 8),
                };
                memcpy(member.name, base, base_len);
                member.name[base_len] = '\0';
                if (!visitor(&member, ctx)) break;
            }
        }
        pos += record_size;
    }

    free(cdir);
    return ESP_OK;
}

static esp_err_t copy_stored(int fd, size_t compressed_size, uint8_t *output,
                             size_t output_size, uint8_t *scratch,
                             size_t scratch_size, size_t *chunk_size_out,
                             rom_zip_progress_fn progress, void *progress_ctx)
{
    if (compressed_size != output_size) return ESP_ERR_INVALID_SIZE;

    size_t chunk_size;
    bool owned;
    uint8_t *chunk = get_dma_chunk(scratch, scratch_size, &chunk_size, &owned);
    if (!chunk) return ESP_ERR_NO_MEM;

    size_t done = 0;
    while (done < output_size) {
        size_t want = output_size - done;
        if (want > chunk_size) want = chunk_size;
        if (!read_exact(fd, chunk, want)) break;
        memcpy(output + done, chunk, want);
        done += want;
        if (progress) progress(progress_ctx, done, output_size);
    }
    if (owned) free(chunk);
    if (chunk_size_out) *chunk_size_out = chunk_size;
    return done == output_size ? ESP_OK : ESP_FAIL;
}

static esp_err_t inflate_deflate(int fd, size_t compressed_size, uint8_t *output,
                                 size_t output_size, uint8_t *scratch,
                                 size_t scratch_size, size_t *chunk_size_out,
                                 rom_zip_progress_fn progress, void *progress_ctx)
{
    size_t chunk_size;
    bool input_owned;
    uint8_t *input = get_dma_chunk(scratch, scratch_size, &chunk_size, &input_owned);
    /* 解压状态约十几 KB，放 PSRAM 是为了不把输入反弹缓冲从 32/64 KB 挤成 4 KB。
     * 这张卡的瓶颈是每条 SD 命令约 40 ms，保住大块读取比状态访问更重要。 */
    tinfl_decompressor *decomp = heap_caps_malloc(sizeof(*decomp),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!input || !decomp) {
        if (input_owned) free(input);
        free(decomp);
        return ESP_ERR_NO_MEM;
    }
    if (chunk_size_out) *chunk_size_out = chunk_size;

    tinfl_init(decomp);
    size_t input_pos = 0, input_have = 0, compressed_left = compressed_size;
    size_t output_pos = 0;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;

    while (1) {
        if (input_pos == input_have && compressed_left) {
            size_t want = compressed_left < chunk_size ? compressed_left : chunk_size;
            if (!read_exact(fd, input, want)) {
                status = TINFL_STATUS_FAILED;
                break;
            }
            input_pos = 0;
            input_have = want;
            compressed_left -= want;
        }

        size_t in_bytes = input_have - input_pos;
        size_t out_bytes = output_size - output_pos;
        uint32_t flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        if (compressed_left) flags |= TINFL_FLAG_HAS_MORE_INPUT;
        status = tinfl_decompress(decomp, input + input_pos, &in_bytes,
                                  output, output + output_pos, &out_bytes, flags);
        input_pos += in_bytes;
        output_pos += out_bytes;
        if (progress && out_bytes) progress(progress_ctx, output_pos, output_size);

        if (status == TINFL_STATUS_DONE) break;
        if (status < TINFL_STATUS_DONE) break;
        if (output_pos >= output_size ||
            (in_bytes == 0 && out_bytes == 0) ||
            (input_pos == input_have && compressed_left == 0)) {
            status = TINFL_STATUS_FAILED;
            break;
        }
    }

    if (input_owned) free(input);
    free(decomp);
    if (status != TINFL_STATUS_DONE || output_pos != output_size) {
        ESP_LOGE(TAG, "DEFLATE 失败：状态 %d，输出 %u/%u 字节", (int)status,
                 (unsigned)output_pos, (unsigned)output_size);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t rom_zip_extract(const char *path, const rom_zip_member_t *member,
                          uint8_t *output, size_t output_size,
                          uint8_t *scratch, size_t scratch_size,
                          size_t *read_chunk_size,
                          rom_zip_progress_fn progress, void *progress_ctx)
{
    if (!path || !member || !output || output_size != member->uncompressed_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (member->flags & 1u) {
        ESP_LOGE(TAG, "%s 已加密，无法解压", member->name);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (member->method != ROM_ZIP_METHOD_STORE &&
        member->method != ROM_ZIP_METHOD_DEFLATE) {
        ESP_LOGE(TAG, "%s 使用 ZIP 压缩方式 %u，只支持 store/deflate",
                 member->name, (unsigned)member->method);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return ESP_ERR_NOT_FOUND;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return ESP_FAIL;
    }

    uint8_t local[ZIP_LOCAL_SIZE];
    if (!seek_to(fd, member->local_header_offset) || !read_exact(fd, local, sizeof(local)) ||
        le32(local) != ZIP_LOCAL_MAGIC) {
        ESP_LOGE(TAG, "%s 的 ZIP local header 损坏", member->name);
        close(fd);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t local_flags = le16(local + 6);
    uint16_t local_method = le16(local + 8);
    size_t data_offset = (size_t)member->local_header_offset + ZIP_LOCAL_SIZE +
                         le16(local + 26) + le16(local + 28);
    if ((local_flags & 1u) || local_method != member->method ||
        data_offset > (size_t)st.st_size ||
        member->compressed_size > (size_t)st.st_size - data_offset ||
        !seek_to(fd, data_offset)) {
        ESP_LOGE(TAG, "%s 的 ZIP 条目偏移或压缩方式不一致", member->name);
        close(fd);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = member->method == ROM_ZIP_METHOD_STORE
                  ? copy_stored(fd, member->compressed_size, output, output_size,
                                scratch, scratch_size, read_chunk_size,
                                progress, progress_ctx)
                  : inflate_deflate(fd, member->compressed_size, output, output_size,
                                    scratch, scratch_size, read_chunk_size,
                                    progress, progress_ctx);
    close(fd);
    if (err != ESP_OK) return err;

    uint32_t actual_crc = esp_crc32_le(0, output, output_size);
    if (actual_crc != member->crc32) {
        ESP_LOGE(TAG, "%s 解压 CRC 不符：得到 %08lx，ZIP 记录 %08lx",
                 member->name, (unsigned long)actual_crc,
                 (unsigned long)member->crc32);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}
