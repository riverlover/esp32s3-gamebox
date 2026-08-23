/*
 * TF 卡自检
 *
 * ⚠ 必须走 SPI3，不能挂 SPI2 —— display.c 里 SPI2 是 ST7789 独占的，而且核 1 的
 * blit_task 在连续 DMA 推条带。SD 事务插进同一条总线会直接抢掉推屏的时间预算
 * （每条带约 122 µs 固定开销那本账，见 display.c 文件头）。SPI3 完全空着。
 *
 * SPI3 在 ESP32-S3 上没有 IOMUX 原生脚，四根线一律走 GPIO matrix，上限约 40 MHz；
 * 而 SD 的 SPI 模式默认就跑 20 MHz，所以这个上限不构成瓶颈，引脚随便挑。
 *
 * 挂载失败时**故意不格式化**（format_if_mount_failed = false）：这是自检，卡里
 * 是用户自己的东西，认不出来的原因九成是接线或信号完整性，格掉只会毁数据还掩盖
 * 真正的故障。
 *
 * 速度回退是有意设计的单变量对照实验：20 MHz 失败就自动退到 4 MHz 再试一次。
 *   两档都失败       -> 接线 / 供电问题（线接错、虚焊、没共地、卡没插到位）
 *   20 失败 4 成功   -> 接线是对的，是信号完整性：缺上拉、杜邦线太长、没有去耦电容
 * 隔着几层猜不如让固件自己把这个实验做掉。
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
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_card";

/* 接线。模块丝印是 3V3/CS/MOSI/CLK/MISO/GND，注意 MOSI 和 MISO 是对调过的那一版。 */
#define SD_PIN_CLK   39
#define SD_PIN_MOSI  41
#define SD_PIN_MISO  40
#define SD_PIN_CS    42

#define SD_HOST      SPI3_HOST
#define SD_MOUNT     "/sd"

/* 先按默认 20 MHz 试，不行退到 4 MHz。见文件头的对照实验说明。 */
static const int SD_FREQ_KHZ[] = { 20000, 4000 };

#define SD_TEST_FILE  SD_MOUNT "/sdtest.tmp"
static const char SD_TEST_TEXT[] = "esp32s3-gamebox sd bring-up\n";

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
    default:
        return "";
    }
}

/* 列根目录。默认 FATFS 配置是 CONFIG_FATFS_LFN_NONE，只认 8.3 短名，所以
 * "Super Mario World.sfc" 会显示成 "SUPERM~1.SFC" —— 这不是读错了，是长文件名
 * 支持没开。真要用 SD 当 ROM 来源，得先在 sdkconfig.defaults 里开 FATFS_LFN_HEAP。 */
static void list_root(void)
{
    DIR *dir = opendir(SD_MOUNT);
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
        snprintf(path, sizeof(path), SD_MOUNT "/%s", ent->d_name);

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
    ESP_LOGI(TAG, "根目录共 %d 个文件、%d 个目录（短名显示是 8.3 限制，不是读错）",
             files, dirs);
}

/* 写一个小文件再读回来比对，然后删掉。只有这一步过了才能说「读写都通」——
 * 挂载成功只证明能读扇区。
 *
 * ⚠ 必须检查 fclose 的返回值：fwrite 只把数据塞进 stdio 缓冲，真正落盘发生在
 * fclose 的 flush。第一版漏了这一步，结果写失败一路溜到下一个 fopen 才暴露，
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

esp_err_t sd_card_selftest(void)
{
    ESP_LOGI(TAG, "自检开始：SPI3  CLK=%d MOSI=%d MISO=%d CS=%d",
             SD_PIN_CLK, SD_PIN_MOSI, SD_PIN_MISO, SD_PIN_CS);

    enable_internal_pullups();

    const spi_bus_config_t bus = {
        .mosi_io_num     = SD_PIN_MOSI,
        .miso_io_num     = SD_PIN_MISO,
        .sclk_io_num     = SD_PIN_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI3 初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* ⚠ 自检绝不动用户的卡，见文件头 */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    int used_khz = 0;
    bool rw_ok = false, listed = false;

    /* 降速重试覆盖「挂不上」和「挂上了写不了」两种失败：写比读对时序更敏感，
     * 只在挂载这一步做回退会漏掉「读得了写不了」那一档，而那恰恰是信号完整性
     * 问题最典型的表现。 */
    for (unsigned i = 0; i < sizeof(SD_FREQ_KHZ) / sizeof(SD_FREQ_KHZ[0]); i++) {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot         = SD_HOST;
        host.max_freq_khz = SD_FREQ_KHZ[i];

        sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
        dev.gpio_cs = SD_PIN_CS;
        dev.host_id = SD_HOST;

        err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &dev, &mount_cfg, &card);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%d kHz 挂载失败：%s —— %s",
                     SD_FREQ_KHZ[i], esp_err_to_name(err), mount_hint(err));
            continue;
        }
        used_khz = SD_FREQ_KHZ[i];

        if (!listed) {
            printf("\n---------- TF 卡 ----------\n");
            sdmmc_card_print_info(stdout, card);
            printf("---------------------------\n");

            uint64_t total = 0, freeb = 0;
            if (esp_vfs_fat_info(SD_MOUNT, &total, &freeb) == ESP_OK) {
                ESP_LOGI(TAG, "FAT 容量 %u MB，可用 %u MB",
                         (unsigned)(total / (1024 * 1024)),
                         (unsigned)(freeb / (1024 * 1024)));
            }
            list_root();
            listed = true;
        }

        rw_ok = write_read_verify();
        if (rw_ok) break;

        ESP_LOGW(TAG, "%d kHz 读得了写不了，降速再试一次", SD_FREQ_KHZ[i]);
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
        card = NULL;
    }

    if (!card) {
        ESP_LOGE(TAG, "所有速度档都没通过，按上面的提示先查接线");
        spi_bus_free(SD_HOST);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    if (used_khz != SD_FREQ_KHZ[0]) {
        ESP_LOGW(TAG, "只有降到 %d kHz 才通过：接线是对的，问题在信号完整性"
                      "（缺上拉 / 杜邦线太长 / 3V3 缺去耦电容）", used_khz);
    }
    ESP_LOGI(TAG, "读写校验通过 @ %d kHz", used_khz);

    /* 卸载并交还 SPI3：自检跑完就该零占用，别让它影响模拟器的内存账。 */
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, card);
    spi_bus_free(SD_HOST);

    ESP_LOGI(TAG, "自检结束：TF 卡可用");
    return ESP_OK;
}
