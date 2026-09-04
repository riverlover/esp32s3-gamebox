/*
 * TF 卡：挂载 + 自检
 *
 * ⚠ 必须走 SPI3，不能挂 SPI2 —— display.c 里 SPI2 是 ST7789 独占的，而且核 1 的
 * blit_task 在连续 DMA 推条带。SD 事务插进同一条总线会直接抢掉推屏的时间预算
 * （每条带约 122 µs 固定开销那本账，见 display.c 文件头）。SPI3 完全空着。
 *
 * SPI3 在 ESP32-S3 上没有 IOMUX 原生脚，四根线一律走 GPIO matrix，上限约 40 MHz；
 * 而 SD 的 SPI 模式默认就跑 20 MHz，所以这个上限不构成瓶颈，引脚随便挑。
 *
 * 挂载失败时**故意不格式化**（format_if_mount_failed = false）：卡里是用户自己的
 * 东西，认不出来的原因九成是接线或信号完整性，格掉只会毁数据还掩盖真正的故障。
 *
 * 速度回退是有意设计的单变量对照实验：20 MHz 挂不上就自动退到 4 MHz 再试一次。
 *   两档都失败       -> 接线 / 供电问题（线接错、虚焊、没共地、卡没插到位）
 *   20 失败 4 成功   -> 接线是对的，是信号完整性：缺上拉、杜邦线太长、没有去耦电容
 * 隔着几层猜不如让固件自己把这个实验做掉。
 *
 * 挂上之后**不卸载**：ROM 全部从这张卡按需读取（rom_store.c），整个运行期间
 * 都要用。早期只做自检那一版是跑完就卸载的，现在不行了。
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sd_card.h"

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

/* 部分 TF 卡在 SPI 模式下拒绝 CRC_ON_OFF（CMD59），IDF 的 sdmmc_init_spi_crc
 * 会因此返回 ESP_ERR_NOT_SUPPORTED(0x106) 并整卡判死。SPI 规范里数据 CRC 对
 * 主机是可选的；社区与 IDFGH-14710 的做法是把 0x106 当成功继续挂载。
 * 用 --wrap 包一层，不改 IDF 源码。若后面读写校验仍失败，才回头查接线。 */
esp_err_t __real_sdmmc_init_spi_crc(sdmmc_card_t *card);
esp_err_t __wrap_sdmmc_init_spi_crc(sdmmc_card_t *card)
{
    esp_err_t err = __real_sdmmc_init_spi_crc(card);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW("sd_card",
                 "卡拒绝开启 SPI CRC（0x106），按可选处理继续挂载");
        return ESP_OK;
    }
    return err;
}

static const char *TAG = "sd_card";

/* 接线。模块丝印是 3V3/CS/MOSI/CLK/MISO/GND。 */
#define SD_PIN_CLK   39
#define SD_PIN_MOSI  41
#define SD_PIN_MISO  40
#define SD_PIN_CS    42

#define SD_HOST      SPI3_HOST

/* 先按默认 20 MHz 试，不行退到 4 MHz。见文件头的对照实验说明。 */
static const int SD_FREQ_KHZ[] = { 20000, 4000 };

/* 扇区读基准开关，见下面 read_benchmark() 的注释。换卡后想量这张卡快不快
 * 就打开；它自己要跑好几秒，平时别开。
 * SD_SMOKE_ONLY 构建会在 CMake 里 -DSD_BENCHMARK=1 强制打开。 */
#ifndef SD_BENCHMARK
#define SD_BENCHMARK 0
#endif

#define SD_TEST_FILE  SD_MOUNT_POINT "/sdtest.tmp"
static const char SD_TEST_TEXT[] = "esp32s3-gamebox sd bring-up\n";

static sdmmc_card_t *s_card;
static int s_used_khz;
static esp_err_t s_last_err = ESP_OK;

/* 这块便宜 breakout 板上常常只留了上拉电阻的焊盘、没有真的焊件，而 SD 协议要求
 * CS/MOSI/MISO 都有上拉。内部上拉约 45 kΩ 偏弱，只是让首次点亮的成功率高一些；
 * 真出现「有的卡认有的卡不认」再外挂 10 kΩ 到 3V3。 */
static void enable_internal_pullups(void)
{
    const int pins[] = { SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS };
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
}

/* 把常见错误码翻成「该去查哪根线」，比裸 ESP_ERR_xxx 有用得多。 */
static const char *mount_hint(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
        return "卡没响应：查 CS/CLK 是否接反或虚焊，卡是否插到底";
    case ESP_ERR_NOT_FOUND:
        return "没找到卡：查 3V3 和 GND，以及卡座是否供上电";
    case ESP_ERR_INVALID_RESPONSE:
        return "响应乱码：多半是 MISO/MOSI 接反，或者缺上拉";
    case ESP_ERR_INVALID_STATE:
        return "已经挂载过或总线被占用";
    case ESP_FAIL:
        return "找到卡但挂不上文件系统：卡没格式化成 FAT16/FAT32（exFAT 默认不支持）";
    case ESP_ERR_NOT_SUPPORTED:
        return "卡拒绝某条命令（常见于 SPI CRC）：先看是否已用 wrap 放过 0x106；仍失败再查接线/换卡";
    default:
        return "";
    }
}

esp_err_t sd_card_mount(void)
{
    if (s_card) return ESP_OK;

    ESP_LOGI(TAG, "挂载 TF 卡：SPI3  CLK=%d MOSI=%d MISO=%d CS=%d",
             SD_PIN_CLK, SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CS);

    enable_internal_pullups();

    const spi_bus_config_t bus = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* ROM 是分块读的（rom_store.c 的 READ_CHUNK），单次传输不超过这个数。 */
        .max_transfer_sz = 32 * 1024,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        s_last_err = err;
        ESP_LOGE(TAG, "SPI3 初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* ⚠ 绝不动用户的卡，见文件头 */
        /* 扫描目录和装 ROM 都是同时只开一个文件；留几个余量给以后的存档。 */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    for (unsigned i = 0; i < sizeof(SD_FREQ_KHZ) / sizeof(SD_FREQ_KHZ[0]); i++) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot         = SD_HOST;
        host.max_freq_khz = SD_FREQ_KHZ[i];

        sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
        dev.gpio_cs = SD_PIN_CS;
        dev.host_id = SD_HOST;

        err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &dev,
                                      &mount_cfg, &s_card);
        if (err == ESP_OK) {
            s_used_khz = SD_FREQ_KHZ[i];
            s_last_err = ESP_OK;
            break;
        }
        s_last_err = err;
        ESP_LOGW(TAG, "%d kHz 挂载失败：%s —— %s",
                 SD_FREQ_KHZ[i], esp_err_to_name(err), mount_hint(err));
        s_card = NULL;
    }

    if (!s_card) {
        ESP_LOGE(TAG, "TF 卡挂载失败，按上面的提示查接线；本次开机只有内嵌 ROM 可玩");
        spi_bus_free(SD_HOST);
        return err;
    }

    if (s_used_khz != SD_FREQ_KHZ[0]) {
        ESP_LOGW(TAG, "只有降到 %d kHz 才挂上：接线是对的，问题在信号完整性"
                      "（缺上拉 / 杜邦线太长 / 3V3 缺去耦电容）", s_used_khz);
    }

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "TF 卡就绪 @ %d kHz：%s，容量 %u MB，可用 %u MB",
                 s_used_khz, s_card->cid.name,
                 (unsigned)(total / (1024 * 1024)),
                 (unsigned)(freeb / (1024 * 1024)));
    }
    return ESP_OK;
}

bool sd_card_mounted(void)
{
    return s_card != NULL;
}

/* 屏上只有一行位置，hint 必须短；完整句子仍在串口 mount_hint()。 */
const char *sd_card_mount_hint(void)
{
    if (s_card) return "";
    switch (s_last_err) {
    case ESP_OK:
        return "";
    case ESP_ERR_TIMEOUT:
        return "卡无响应 查CS/CLK";
    case ESP_ERR_NOT_FOUND:
        return "未找到卡 查3V3/GND";
    case ESP_ERR_INVALID_RESPONSE:
        return "响应乱 查MOSI/MISO";
    case ESP_ERR_INVALID_STATE:
        return "总线占用";
    case ESP_FAIL:
        return "文件系统挂不上";
    case ESP_ERR_NOT_SUPPORTED:
        return "卡拒CRC/不支持";
    default:
        return "挂载失败 查接线";
    }
}

void sd_card_usage(uint64_t *used_bytes, uint64_t *total_bytes)
{
    if (used_bytes) *used_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    if (!s_card) return;

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb) != ESP_OK) return;
    if (used_bytes) *used_bytes = total - freeb;
    if (total_bytes) *total_bytes = total;
}

/* 列根目录。开了 CONFIG_FATFS_LFN_HEAP 之后这里显示的才是真名；
 * 没开长文件名时 "System Volume Information" 会显示成 "SYSTEM~1"。 */
static void list_root(void)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "根目录打不开");
        return;
    }

    int files = 0, dirs = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        /* IDF 的 struct dirent 里 d_name 是 256 字节，缓冲小于这个数
         * 会被 -Werror=format-truncation 拦下来。 */
        char path[300];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", ent->d_name);

        struct stat st;
        if (ent->d_type == DT_DIR) {
            dirs++;
            printf("  [DIR ] %s\n", ent->d_name);
        } else if (stat(path, &st) == 0) {
            files++;
            printf("  [%6lu] %s\n", (unsigned long)st.st_size, ent->d_name);
        } else {
            files++;
            printf("  [  ?  ] %s\n", ent->d_name);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "根目录共 %d 个文件、%d 个目录", files, dirs);
}

/* 写一个小文件再读回来比对，然后删掉。只有这一步过了才能说「读写都通」——
 * 挂载成功只证明能读扇区。
 *
 * ⚠ 必须检查 fclose 的返回值：fwrite 只把数据塞进 stdio 缓冲，真正落盘发生在
 * fclose 的 flush。第一版漏了这步，结果写失败一路溜到下一个 fopen 才暴露，
 * 报成「刚写的文件读不开」——错误信息指向了完全无关的地方，白查一轮。 */
static bool write_read_verify(void)
{
    errno = 0;
    FILE *fp = fopen(SD_TEST_FILE, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "建不了测试文件：%s", strerror(errno));
        return false;
    }

    size_t want = sizeof(SD_TEST_TEXT) - 1;
    errno = 0;
    size_t wrote = fwrite(SD_TEST_TEXT, 1, want, fp);
    int closed = fclose(fp);
    if (wrote != want || closed != 0) {
        ESP_LOGE(TAG, "写入失败：fwrite %u/%u 字节，fclose=%d（errno %d: %s）",
                 (unsigned)wrote, (unsigned)want, closed, errno, strerror(errno));
        unlink(SD_TEST_FILE);
        return false;
    }

    /* 分开验「目录项落了盘」和「数据读得回来」，这样下次再出问题能直接分辨
     * 是 FAT 表没写进去还是数据扇区坏。 */
    struct stat st;
    errno = 0;
    if (stat(SD_TEST_FILE, &st) != 0) {
        ESP_LOGE(TAG, "写完之后文件不存在（errno %d: %s）——目录项没落盘",
                 errno, strerror(errno));
        return false;
    }

    char buf[sizeof(SD_TEST_TEXT)] = { 0 };
    errno = 0;
    fp = fopen(SD_TEST_FILE, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "刚写的文件读不开（errno %d: %s）", errno, strerror(errno));
        unlink(SD_TEST_FILE);
        return false;
    }
    size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    bool ok = got == want && memcmp(buf, SD_TEST_TEXT, want) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "回读对不上：写 %u 字节（目录记 %ld），读回 %u 字节",
                 (unsigned)want, (long)st.st_size, (unsigned)got);
    }

    unlink(SD_TEST_FILE);   /* 自检不该在用户卡上留垃圾，失败路径也要删 */
    return ok;
}

/* 扇区读基准：把「固定开销」和「传输吞吐」分开。默认关闭，因为它自己就要跑
 * 好几秒（在慢卡上单扇区读一次就 80 ms）。换卡之后想知道这张卡快不快就打开。
 *
 * 判读方法（本机 2 GB SDSC 老卡的实测值）：
 *   CMD13（不传数据）    38 ms/次   <- 正常卡应当 <1 ms
 *   单扇区读             81 ms/次
 *   64 KB 一次读        119 ms（538 KB/s）
 * 固定开销占绝对大头，边际只有 0.30 ms/扇区。做过 20 MHz vs 4 MHz 的单变量
 * 对照：固定开销纹丝不动（38→40 ms），只有传输部分按时钟比例变慢 —— 所以那
 * 40 ms 是卡自己的命令响应延迟，不是接线或上拉的问题，降频治不了。
 *
 * 这个结论直接决定了 rom_store.c 的两个设计：扫描期一个文件都不开，
 * 装载时经内部 RAM 大块中转。事务数才是成本，字节数几乎免费。 */
static void read_benchmark(const char *tag)
{
    const int N = 32;
    uint8_t *buf = heap_caps_malloc(N * 512, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!buf) { ESP_LOGW(TAG, "基准缓冲分配失败"); return; }

    /* 挑一个数据区里的扇区，避开 0 号（引导扇区可能被缓存） */
    const uint32_t base = 1024;

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) {
        if (sdmmc_read_sectors(s_card, buf, base + i, 1) != ESP_OK) {
            ESP_LOGW(TAG, "单扇区读失败"); free(buf); return;
        }
    }
    int64_t one = esp_timer_get_time() - t0;

    t0 = esp_timer_get_time();
    if (sdmmc_read_sectors(s_card, buf, base + 64, N) != ESP_OK) {
        ESP_LOGW(TAG, "多扇区读失败"); free(buf); return;
    }
    int64_t many = esp_timer_get_time() - t0;

    /* 无数据命令（CMD13）单独计时：如果它也是 80 ms 量级，说明固定开销在
     * 每条命令上（主机侧/接线）；如果它很快，那 80 ms 就是卡响应读命令的
     * 内部延迟（卡本身慢）。这是区分「卡的锅」和「板子的锅」的关键一刀。 */
    t0 = esp_timer_get_time();
    for (int i = 0; i < N; i++) sdmmc_get_status(s_card);
    int64_t stat_us = esp_timer_get_time() - t0;

    /* 大块读：ROM 装载按 32 KB 一块，量一下真实吞吐 */
    uint8_t *big = heap_caps_malloc(64 * 1024, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    int64_t big_us = 0;
    if (big) {
        t0 = esp_timer_get_time();
        sdmmc_read_sectors(s_card, big, base + 256, 128);   /* 64 KB */
        big_us = esp_timer_get_time() - t0;
        free(big);
    }

    ESP_LOGI(TAG, "[%s] CMD13(无数据) %d 次共 %lld us（%lld us/次）；"
                  "64 KB 一次读 %lld us（%lld KB/s）",
             tag, N, (long long)stat_us, (long long)(stat_us / N),
             (long long)big_us,
             (long long)(big_us > 0 ? 64 * 1000000LL / big_us : 0));

    ESP_LOGI(TAG, "[%s] 扇区读基准：单扇区 %d 次共 %lld us（%lld us/次）；"
                  "一次读 %d 扇区共 %lld us（%lld KB/s）",
             tag, N, (long long)one, (long long)(one / N),
             N, (long long)many,
             (long long)(many > 0 ? (int64_t)N * 512 * 1000 / many : 0));
    free(buf);
}

esp_err_t sd_card_selftest(void)
{
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) return err;

#if SD_BENCHMARK
    read_benchmark("bench");
#endif

    printf("\n---------- TF 卡 ----------\n");
    sdmmc_card_print_info(stdout, s_card);
    printf("---------------------------\n");

    list_root();

    bool rw_ok = write_read_verify();
    ESP_LOGI(TAG, "读写校验 %s @ %d kHz", rw_ok ? "通过" : "失败", s_used_khz);

    /* 不卸载：ROM 接着要从这张卡读。 */
    return rw_ok ? ESP_OK : ESP_FAIL;
}
