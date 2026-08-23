/*
 * TF 卡上的游戏目录
 *
 * 开机递归扫描卡，按扩展名挑候选、按 **ROM 头** 判定平台（目录名只是给人看的，
 * 放错目录不影响结果），把目录建在 PSRAM；用户选中之后 rom_store_load() 才把
 * 那个文件读进来。
 *
 * 早期是从 flash 的 roms 分区 mmap 一整块打包镜像，entry->data 直接是 flash 指针，
 * 零拷贝。换到 SD 之后这个模型不成立了 —— 文件系统没法 mmap，字节必须显式读出来。
 * 所以现在 entry 里存路径，代价是每次开游戏多一次全量读盘（本机 EZSD1 实测
 * 1 MiB 约 3.3 秒，4 MiB 按吞吐量约 13 秒），换来的是容量从 13 MB 变成整张卡、
 * 加游戏不用重烧固件。
 *
 * ⚠ 扫描时**不打开任何文件**，平台只按扩展名定、大小只按 stat 取。这不是偷懒，
 * 是实测逼出来的：这张卡每条 SD 命令有约 40 ms 的固定就绪等待（无数据的 CMD13
 * 都要 40 ms，单扇区读 72 ms，而边际成本只有 0.30 ms/扇区）。原来每个文件都
 * open+读头+seek，39 个游戏要扫 14 秒。改成纯 readdir+stat 之后事务数掉一个量级。
 * ROM 头照样验，只是挪到 rom_store_load()——那时候反正要把整个文件读进来。
 * 代价是扩展名骗人的文件（尤其通用的 .bin）会出现在菜单里，选中时才报错。
 *
 * GB / GBC 靠扩展名分不准，但**这只影响菜单分组**：gbc_emu.c 根本不读
 * entry->system，gnuboy 自己从 ROM 头 0x143 判 CGB/SGB/DMG（gnuboy.c:234）。
 *
 * 所有失败都只是让 rom_store_init() 返回 0，不 abort：没插卡、卡挂不上、卡上没有
 * 合法 ROM，都不该让整块板子开不了机。调用方回退到编译期嵌入的 ROM。
 */

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rom_store.h"
#include "sd_card.h"

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "romstore";

/* 扫描分阶段计时。开机扫 39 个游戏一度要 14 秒，靠这个定位时间花在哪一步。
 * 定位完可以关掉，留着是因为换卡/换簇大小之后还得再量一次。 */
#define SCAN_PROFILE 0

#if SCAN_PROFILE
static int64_t s_t_readdir, s_t_stat;
static int     s_n_stat;
#define PROF_T0()      int64_t _p0 = esp_timer_get_time()
#define PROF_ADD(acc)  do { (acc) += esp_timer_get_time() - _p0; } while (0)
#else
#define PROF_T0()      do {} while (0)
#define PROF_ADD(acc)  do {} while (0)
#endif

/* 递归深度上限。卡上按 roms/<平台>/ 放最多也就两三层，给到 4 层够用，
 * 同时挡住「不小心把整张系统盘插进来」那种深树。 */
#define SCAN_MAX_DEPTH 4

/* 读 ROM 的分块大小，同时也是内部 RAM 反弹缓冲的大小。
 *
 * ⚠ 这个反弹缓冲不是可有可无的优化，是必需品：sdmmc_read_sectors() 发现目标
 * 缓冲不是 DMA-capable（PSRAM 一律不是）时，会退化成**一次一个 512 字节扇区**
 * 读再 memcpy（见 IDF 的 sdmmc_cmd.c）。本机实测单扇区读 72 ms，4 MiB 的 SNES
 * 卡带这样读要十分钟。先读进内部 RAM 让它走多扇区 DMA，再 memcpy 到 PSRAM，
 * 实测 64 KB 一次读能到 538 KB/s。
 *
 * 32 KB 是在内部 RAM 余量和摊薄固定开销之间取的折中；这块缓冲用完立刻释放。 */
#define READ_CHUNK (32 * 1024)

/* 防止一个坏掉的目录项声明出远超 PSRAM 的尺寸。当前最大卡带 DKC 是 4 MiB；
 * 留到 8 MiB 既覆盖合理的 SNES 卡，也绝不会做失控的大分配。 */
#define ROM_MAX_SIZE (8u * 1024u * 1024u)

#define NES_ROM_MIN_SIZE  (16 + 16 * 1024)
#define GB_ROM_MIN_SIZE   0x4000
#define SNES_ROM_MIN_SIZE 0x20000
#define SNES_LOROM_HEADER 0x7FC0
#define SNES_HIROM_HEADER 0xFFC0
#define GENESIS_ROM_MIN_SIZE 0x200

static rom_store_entry_t *s_entries;
static int s_count = -1;          /* -1 = 还没扫过 */

/* 名字和路径都从这块 PSRAM 池里切，避免每条目一次 malloc。 */
static char  *s_pool;
static size_t s_pool_used;
static size_t s_pool_size;

/* 递归共用一个路径缓冲：每层在栈上开 160 字节的话，main task 那 3584 字节的栈
 * 扛不住四层递归再叠 FATFS 自己的开销。同理头部缓冲也放静态区。
 * 扫描是单线程的，共用没有竞争。 */
static char    s_path[ROM_STORE_PATH_LEN];

static char *pool_dup(const char *src, size_t len)
{
    if (s_pool_used + len + 1 > s_pool_size) return NULL;
    char *dst = s_pool + s_pool_used;
    memcpy(dst, src, len);
    dst[len] = '\0';
    s_pool_used += len + 1;
    return dst;
}

/* ---------------- 平台判定 ---------------- */

/* SNES 没有 magic。业界通行的判据是内部头里那对校验和：
 * checksum ^ complement 必须等于 0xFFFF。再要求标题是可打印 ASCII，
 * 基本不会把随机数据认成卡带。h 指向 0x20 字节的内部头。 */
static bool snes_header_ok(const uint8_t *h)
{
    for (int i = 0; i < 21; i++) {
        uint8_t c = h[i];
        if (c != 0 && (c < 0x20 || c > 0x7E)) return false;
    }
    uint32_t comp = (uint32_t)h[0x1C] | ((uint32_t)h[0x1D] << 8);
    uint32_t ck   = (uint32_t)h[0x1E] | ((uint32_t)h[0x1F] << 8);
    return (ck ^ comp) == 0xFFFF;
}

/* GB 头校验和：0x134~0x14C 逐字节 check = check - v - 1，结果要等于 0x14D。 */
static bool gb_header_ok(const uint8_t *h, size_t size)
{
    if (size < 0x150 || size % 0x4000 != 0) return false;
    uint8_t check = 0;
    for (int i = 0x134; i <= 0x14C; i++) check = (uint8_t)(check - h[i] - 1);
    return check == h[0x14D];
}

/* 卡带类型（头 0x147）-> gnuboy 会选哪个 mapper。
 * components/gnuboy/hw.c 的 mbc_write() 只实现了一部分，没实现的那几种
 * 游戏写 bank 号时什么都不会发生，表现是**黑屏** —— 和崩溃、性能问题看着
 * 一模一样，极难判断。所以扫描时就把话说清楚。实测触发过：
 * Kirby Tilt 'n' Tumble（0x22，MBC7 + 加速度计）。 */
static const char *gb_unsupported_mapper(uint8_t cart_type)
{
    if (cart_type >= 11 && cart_type <= 13) return "MMM01";
    if (cart_type == 32) return "MBC6";
    if (cart_type == 34) return "MBC7";
    return NULL;
}

/* 内存里的完整 ROM 再验一次头。装载完调用，挡住「扫描时看着像、读进来是别的」
 * （文件在扫描后被换掉、读盘出错但 fread 没报错之类）。 */
static bool rom_header_ok(rom_system_t system, const uint8_t *rom, size_t size)
{
    static const uint8_t gb_logo_head[4] = {0xCE, 0xED, 0x66, 0x66};

    if (system == ROM_SYSTEM_NES) {
        return size >= NES_ROM_MIN_SIZE && memcmp(rom, "NES\x1a", 4) == 0;
    }
    if (system == ROM_SYSTEM_SNES) {
        if (size >= SNES_LOROM_HEADER + 0x20 &&
            snes_header_ok(rom + SNES_LOROM_HEADER)) return true;
        return size >= SNES_HIROM_HEADER + 0x20 &&
               snes_header_ok(rom + SNES_HIROM_HEADER);
    }
    if (system == ROM_SYSTEM_GENESIS) {
        return size >= GENESIS_ROM_MIN_SIZE && memcmp(rom + 0x100, "SEGA", 4) == 0;
    }
    return size >= 0x150 && memcmp(rom + 0x104, gb_logo_head, 4) == 0;
}

/* ---------------- 扫描 ---------------- */

/* 小写化的扩展名（含点）。认不出来的扩展名不报错，静默跳过 —— 卡上本来就
 * 会有一堆别的文件。只有压缩包例外：那是「以为放进去了其实没生效」的重灾区，
 * 单独提示一句。 */
static bool ext_is(const char *ext, const char *want)
{
    return strcasecmp(ext, want) == 0;
}

static bool ext_is_archive(const char *ext)
{
    return ext_is(ext, ".zip") || ext_is(ext, ".7z") || ext_is(ext, ".rar") ||
           ext_is(ext, ".gz")  || ext_is(ext, ".tar") || ext_is(ext, ".tgz");
}

/* 显示名：去掉路径和扩展名，再去掉 (...) [...] 那些区域/版本标记和开头的
 * "NN_" 排序前缀。和 pack_roms.py 的 display_name() 同一套规则。 */
static void display_name(const char *fname, const char *ext, char *out, size_t out_size)
{
    size_t stem_len = (size_t)(ext - fname);

    /* 先拷 stem，再原地删标记 */
    size_t n = 0;
    for (size_t i = 0; i < stem_len && n + 1 < out_size; i++) out[n++] = fname[i];
    out[n] = '\0';

    /* 删所有 (...) / [...]，连同它前面的空白 */
    size_t w = 0;
    for (size_t r = 0; out[r]; ) {
        if (out[r] == '(' || out[r] == '[') {
            char close = out[r] == '(' ? ')' : ']';
            size_t k = r + 1;
            while (out[k] && out[k] != ')' && out[k] != ']') k++;
            if (out[k] == close) {
                /* 回退掉刚写进去的尾部空白 */
                while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '\t')) w--;
                r = k + 1;
                continue;
            }
        }
        out[w++] = out[r++];
    }
    out[w] = '\0';

    /* 开头的 "NN_" 排序前缀 */
    char *p = out;
    if (w >= 3 && p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
        p[2] == '_') {
        memmove(out, out + 3, w - 3 + 1);
        w -= 3;
    }

    /* 去掉首尾空白 */
    size_t start = 0;
    while (out[start] == ' ' || out[start] == '\t') start++;
    while (w > start && (out[w - 1] == ' ' || out[w - 1] == '\t')) w--;
    out[w] = '\0';
    if (start) memmove(out, out + start, w - start + 1);

    /* 全是标记的极端情况，退回原始 stem */
    if (out[0] == '\0') {
        n = 0;
        for (size_t i = 0; i < stem_len && n + 1 < out_size; i++) out[n++] = fname[i];
        out[n] = '\0';
    }
}

/* 按扩展名和文件大小认一个候选。**不打开文件** —— 理由见文件头。
 * SNES 的 512 字节拷贝机头也能只从大小判出来（整卡带都是 0x400 的整数倍）。 */
static bool classify(const char *ext, size_t file_size,
                     rom_system_t *system, size_t *offset, size_t *rom_size)
{
    *offset = 0;
    *rom_size = file_size;

    if (ext_is(ext, ".nes")) {
        *system = ROM_SYSTEM_NES;
        return file_size >= NES_ROM_MIN_SIZE;
    }
    if (ext_is(ext, ".gb")) {
        *system = ROM_SYSTEM_GB;
        return file_size >= GB_ROM_MIN_SIZE;
    }
    if (ext_is(ext, ".gbc")) {
        *system = ROM_SYSTEM_GBC;
        return file_size >= GB_ROM_MIN_SIZE;
    }
    if (ext_is(ext, ".sfc") || ext_is(ext, ".smc")) {
        if (file_size % 0x400 == 512) {
            *offset = 512;
            *rom_size = file_size - 512;
        }
        *system = ROM_SYSTEM_SNES;
        return *rom_size >= SNES_ROM_MIN_SIZE;
    }
    if (ext_is(ext, ".md") || ext_is(ext, ".bin")) {
        *system = ROM_SYSTEM_GENESIS;
        return file_size >= GENESIS_ROM_MIN_SIZE;
    }
    return false;
}

static void try_add(const char *path, const char *fname)
{
    const char *ext = strrchr(fname, '.');
    if (!ext) return;

    if (ext_is_archive(ext)) {
        /* 以前这类文件是静默消失的，最难查 —— 用户以为放进去了。 */
        ESP_LOGW(TAG, "%s 是压缩包，设备上不解压，请先在电脑上解开", fname);
        return;
    }
    if (!ext_is(ext, ".nes") && !ext_is(ext, ".gb") && !ext_is(ext, ".gbc") &&
        !ext_is(ext, ".sfc") && !ext_is(ext, ".smc") && !ext_is(ext, ".md") &&
        !ext_is(ext, ".bin")) {
        return;     /* 卡上本来就有别的文件，不吭声 */
    }

    struct stat st;
    PROF_T0();
    int strc = stat(path, &st);
    PROF_ADD(s_t_stat);
#if SCAN_PROFILE
    s_n_stat++;
#endif
    if (strc != 0 || st.st_size <= 0) return;
    size_t file_size = (size_t)st.st_size;
    if (file_size > ROM_MAX_SIZE) {
        ESP_LOGW(TAG, "%s 有 %u MB，超过 %u MB 上限，跳过",
                 fname, (unsigned)(file_size / (1024 * 1024)),
                 (unsigned)(ROM_MAX_SIZE / (1024 * 1024)));
        return;
    }

    rom_system_t system;
    size_t offset, rom_size;
    if (!classify(ext, file_size, &system, &offset, &rom_size)) {
        ESP_LOGW(TAG, "%s 太小，不像能跑的卡带，跳过", fname);
        return;
    }

    char namebuf[ROM_STORE_NAME_LEN];
    display_name(fname, ext, namebuf, sizeof(namebuf));

    char *name = pool_dup(namebuf, strlen(namebuf));
    char *stored_path = name ? pool_dup(path, strlen(path)) : NULL;
    if (!stored_path) {
        ESP_LOGW(TAG, "名字/路径池满了，%s 之后的游戏不再收录", fname);
        return;
    }

    s_entries[s_count].name        = name;
    s_entries[s_count].path        = stored_path;
    s_entries[s_count].size        = rom_size;
    s_entries[s_count].file_offset = offset;
    s_entries[s_count].system      = system;
    s_count++;
}

/* 前缀是这几样的目录整棵跳过：`.` 是系统隐藏目录（含 macOS 的 .Spotlight-V100
 * 和 ._ 边车），`_` 和 `removed` 是「临时下架」约定，和 pack_roms.py 一致。 */
static bool skip_entry(const char *name)
{
    if (name[0] == '.' || name[0] == '_') return true;
    if (strncasecmp(name, "removed", 7) == 0) return true;
    if (strcasecmp(name, "System Volume Information") == 0) return true;
    return false;
}

/* s_path 里已经是当前目录的路径，len 是它的长度。 */
static void scan_dir(size_t len, int depth)
{
    DIR *dir = opendir(s_path);
    if (!dir) {
        ESP_LOGW(TAG, "打不开目录 %s", s_path);
        return;
    }

    struct dirent *ent;
    while (1) {
        PROF_T0();
        ent = readdir(dir);
        PROF_ADD(s_t_readdir);
        if (!ent || s_count >= ROM_STORE_MAX) break;
        if (skip_entry(ent->d_name)) continue;

        size_t n = strlen(ent->d_name);
        if (len + 1 + n + 1 > sizeof(s_path)) {
            ESP_LOGW(TAG, "路径太长，跳过 %s", ent->d_name);
            continue;
        }
        s_path[len] = '/';
        memcpy(s_path + len + 1, ent->d_name, n + 1);

        if (ent->d_type == DT_DIR) {
            if (depth < SCAN_MAX_DEPTH) scan_dir(len + 1 + n, depth + 1);
        } else {
            try_add(s_path, ent->d_name);
        }
        s_path[len] = '\0';
    }
    closedir(dir);
}

/* 先按平台、再按名字排。菜单靠 system 分组，组内顺序就是这里排出来的。 */
static int compare_entry(const void *a, const void *b)
{
    const rom_store_entry_t *x = a, *y = b;
    if (x->system != y->system) return (int)x->system - (int)y->system;
    return strcasecmp(x->name, y->name);
}

int rom_store_init(void)
{
    if (s_count >= 0) return s_count;
    s_count = 0;

    if (sd_card_mount() != ESP_OK) return 0;

    s_entries = heap_caps_calloc(ROM_STORE_MAX, sizeof(*s_entries),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pool_size = (size_t)ROM_STORE_MAX * (ROM_STORE_NAME_LEN + ROM_STORE_PATH_LEN);
    s_pool = heap_caps_malloc(s_pool_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries || !s_pool) {
        ESP_LOGE(TAG, "目录缓冲分配失败（需要 %u KB PSRAM）",
                 (unsigned)((ROM_STORE_MAX * sizeof(*s_entries) + s_pool_size) / 1024));
        free(s_entries); free(s_pool);
        s_entries = NULL; s_pool = NULL;
        return 0;
    }
    s_pool_used = 0;

    int64_t t0 = esp_timer_get_time();
    strcpy(s_path, SD_MOUNT_POINT);
    scan_dir(strlen(SD_MOUNT_POINT), 0);

    if (s_count == ROM_STORE_MAX) {
        ESP_LOGW(TAG, "已经收满 %d 个，卡上剩下的游戏不再扫描", ROM_STORE_MAX);
    }
    qsort(s_entries, (size_t)s_count, sizeof(*s_entries), compare_entry);

    int64_t total_ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "TF 卡：%d 个游戏可用（扫描耗时 %lld ms）", s_count,
             (long long)total_ms);
#if SCAN_PROFILE
    ESP_LOGI(TAG, "扫描分解 ms：readdir %lld  stat %lld（%d 次）",
             (long long)(s_t_readdir / 1000), (long long)(s_t_stat / 1000), s_n_stat);
#endif
    return s_count;
}

const rom_store_entry_t *rom_store_entry(int i)
{
    if (i < 0 || i >= s_count) return NULL;
    return &s_entries[i];
}

void rom_store_usage(uint64_t *used_bytes, uint64_t *capacity_bytes)
{
    sd_card_usage(used_bytes, capacity_bytes);
}

esp_err_t rom_store_load(const rom_store_entry_t *entry, size_t extra_bytes,
                         rom_store_image_t *out)
{
    if (!entry || !out || entry->size > SIZE_MAX - extra_bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint8_t *rom = heap_caps_malloc(entry->size + extra_bytes,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom) {
        ESP_LOGE(TAG, "%s 缓冲分配失败：需要 %u KB PSRAM",
                 entry->name, (unsigned)((entry->size + extra_bytes) / 1024));
        return ESP_ERR_NO_MEM;
    }

    int64_t t0 = esp_timer_get_time();
    int fd = open(entry->path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "打不开 %s（errno %d: %s，卡被拔了？）",
                 entry->path, errno, strerror(errno));
        free(rom);
        return ESP_ERR_NOT_FOUND;
    }
    if (entry->file_offset &&
        lseek(fd, (off_t)entry->file_offset, SEEK_SET) < 0) {
        ESP_LOGE(TAG, "%s 跳过拷贝机头失败（errno %d: %s）",
                 entry->name, errno, strerror(errno));
        close(fd);
        free(rom);
        return ESP_FAIL;
    }

    /* ⚠ 必须经内部 RAM 中转，不能让 read 直接写进 PSRAM。
     * sdmmc_read_sectors() 看到目标不是 DMA-capable 就退化成一次一个 512 字节
     * 扇区读 + memcpy（IDF 的 sdmmc_cmd.c）。本机实测单扇区 72 ms，4 MiB 这样
     * 读要十分钟。中转之后走多扇区 DMA，实测 538 KB/s。 */
    /* 这里故意用 POSIX open/read，不用 stdio fopen/fread。板上实测同一张卡裸读
     * 64 KB 有 533 KB/s，但 fread 把 16 KB 的 DMA 缓冲拆成单扇区事务，1 MiB
     * ROM 读了 84 秒（12 KB/s）；read 会把调用方缓冲直接交给 VFS/FatFs。
     * 换成 read 后 16 KB 中转是 5.6 秒，SNES 提前读取拿到 32 KB 后是 3.3 秒。 */
    /* 装载发生在 nes_emu_prealloc() 的 2x64 KB 和推屏条带 2x20 KB 之后，内部
     * RAM 已经很紧，32 KB 连续块不一定拿得到。块越大越划算（固定开销被摊薄），
     * 所以从大往小退，拿到多少算多少，实在不行退回直读 PSRAM 的慢路径。 */
    size_t chunk_size = READ_CHUNK;
    uint8_t *chunk = NULL;
    while (chunk_size >= 4096) {
        chunk = heap_caps_malloc(chunk_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (chunk) break;
        chunk_size /= 2;
    }
    if (!chunk) {
        ESP_LOGW(TAG, "内部 RAM 拿不出 4 KB 反弹缓冲，退回直读 PSRAM —— "
                      "会走单扇区路径，非常慢");
    } else if (chunk_size != READ_CHUNK) {
        ESP_LOGW(TAG, "反弹缓冲只拿到 %u KB（想要 %u KB），读盘会慢一些",
                 (unsigned)(chunk_size / 1024), (unsigned)(READ_CHUNK / 1024));
    }

    size_t done = 0;
    int read_errno = 0;
    while (done < entry->size) {
        size_t want = entry->size - done;
        ssize_t got;
        if (chunk) {
            if (want > chunk_size) want = chunk_size;
            got = read(fd, chunk, want);
            if (got > 0) memcpy(rom + done, chunk, (size_t)got);
        } else {
            got = read(fd, rom + done, want);
        }
        if (got < 0) {
            read_errno = errno;
            break;
        }
        if (got == 0) break;
        done += (size_t)got;
    }
    free(chunk);
    close(fd);

    if (read_errno) {
        ESP_LOGE(TAG, "%s 读取失败（errno %d: %s）",
                 entry->name, read_errno, strerror(read_errno));
        free(rom);
        return ESP_FAIL;
    }

    if (done != entry->size) {
        ESP_LOGE(TAG, "%s 只读到 %u/%u 字节", entry->name,
                 (unsigned)done, (unsigned)entry->size);
        free(rom);
        return ESP_ERR_INVALID_SIZE;
    }
    if (extra_bytes) memset(rom + entry->size, 0, extra_bytes);

    /* 扫描期只看扩展名，所以这里是 ROM 头的**唯一**一道关：扩展名骗人的文件
     * （尤其通用的 .bin）会一路进到菜单，选中时在这里被挡下。 */
    if (!rom_header_ok(entry->system, rom, entry->size)) {
        ESP_LOGE(TAG, "%s 的 ROM 头和扩展名对不上（%s），换个文件或改扩展名",
                 entry->name, entry->path);
        free(rom);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (entry->system == ROM_SYSTEM_GB || entry->system == ROM_SYSTEM_GBC) {
        const char *bad = gb_unsupported_mapper(rom[0x147]);
        if (bad) {
            ESP_LOGW(TAG, "%s 用的是 %s，gnuboy 的 hw.c 没实现 —— 进去多半是黑屏，"
                          "不是死机也不是性能问题", entry->name, bad);
        }
    }

    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "%s 已从卡读入：%u KB，耗时 %lld ms（%u KB/s，%u KB 块）",
             entry->name, (unsigned)(entry->size / 1024), (long long)ms,
             (unsigned)(ms > 0 ? entry->size / 1024 * 1000 / (unsigned)ms : 0),
             (unsigned)(chunk_size / 1024));

    out->data = rom;
    out->size = entry->size;
    out->crc32 = esp_crc32_le(0, rom, entry->size);
    out->owned = true;
    return ESP_OK;
}

void rom_store_image_release(rom_store_image_t *image)
{
    if (!image) return;
    if (image->owned) free(image->data);
    memset(image, 0, sizeof(*image));
}
