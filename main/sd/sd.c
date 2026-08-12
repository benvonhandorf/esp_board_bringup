/*
 * SD/MMC card bring-up and throughput measurement.
 *
 * A card can be reached three ways, and which one a board wired up is exactly
 * the sort of thing bring-up needs to establish: over SPI (the only option on
 * chips without an SD host peripheral, such as the ESP32-C3), or over the
 * dedicated SD host in 1-bit or 4-bit mode.
 *
 * Bring-up happens in two stages rather than through esp_vfs_fat_sdmmc_mount().
 * Those one-shot helpers do host init, card init and the FAT mount together and
 * unwind all of it if the mount fails -- which is the wrong behaviour here,
 * because a blank or non-FAT card should still report its identity and still be
 * measurable. So `sd spi` / `sd mmc` bring the interface up and initialize the
 * card, and FAT is only mounted when `sd bench` actually needs a filesystem.
 */
#include "esp_bringup.h"
#include "output.h"
#include "sd.h"
#include "spi.h"

#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#endif

#define SD_MOUNT_POINT "/sd"
#define SD_BENCH_FILE SD_MOUNT_POINT "/bench.tmp"
#define SD_MAX_OPEN_FILES 4

#define DEFAULT_SIZE_KB 512
#define DEFAULT_BLOCK_KB 16
#define MAX_SIZE_KB 16384
#define MAX_BLOCK_KB 128

/* Below SDMMC_FREQ_PROBING the host cannot divide down far enough. */
#define MIN_FREQ_KHZ 400
#define MAX_FREQ_KHZ 80000

typedef enum {
    IFACE_NONE,
    IFACE_SPI,
    IFACE_MMC,
} sd_iface_t;

static sdmmc_card_t *card;
static sdmmc_host_t host;
static sd_iface_t iface;
static int iface_width;                 /* 1 or 4, for IFACE_MMC */
static int iface_pins[6];               /* as given to `sd spi` / `sd mmc` */
static int iface_pin_count;
static bool host_inited;
static bool bus_owned;                  /* we called spi_bus_initialize() */
static sdspi_dev_handle_t spi_dev = -1;
static bool mounted;
static BYTE fat_pdrv = 0xFF;

bool sd_owns_spi_host(void)
{
    return bus_owned;
}

static bool require_card(void)
{
    if (!card) {
        bp_error("SD card not initialized. Run 'sd spi <clk> <mosi> <miso> <cs>' "
                 "or 'sd mmc <clk> <cmd> <d0>' first.");
        return false;
    }
    return true;
}

static const char *iface_name(void)
{
    switch (iface) {
    case IFACE_SPI:
        return "SPI";
    case IFACE_MMC:
        return iface_width == 4 ? "SD 4-bit" : "SD 1-bit";
    default:
        return "none";
    }
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */
/* ------------------------------------------------------------------ */

static void unmount_fat(void)
{
    if (!mounted) {
        return;
    }
    const char drive[3] = {(char)('0' + fat_pdrv), ':', '\0'};
    f_mount(NULL, drive, 0);
    esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
    ff_diskio_unregister(fat_pdrv);
    fat_pdrv = 0xFF;
    mounted = false;
}

/*
 * Release everything, in the reverse of the order it was acquired. Called both
 * by `sd close` and at the top of `sd spi` / `sd mmc`, so those commands can be
 * re-run with different pins the way `spi bus` and `i2c bus` can, and so a
 * failed init never leaves half-configured hardware behind.
 */
static void teardown(void)
{
    unmount_fat();

    free(card);
    card = NULL;

    if (iface == IFACE_SPI) {
        if (spi_dev >= 0) {
            sdspi_host_remove_device(spi_dev);
            spi_dev = -1;
        }
        if (host_inited) {
            sdspi_host_deinit();
        }
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else if (iface == IFACE_MMC) {
        if (host_inited) {
            sdmmc_host_deinit();
        }
    }
#endif
    host_inited = false;

    if (bus_owned) {
        spi_bus_free(BP_SPI_HOST_ID);
        bus_owned = false;
    }

    iface = IFACE_NONE;
    iface_width = 0;
    iface_pin_count = 0;
}

/* ------------------------------------------------------------------ */
/* Argument helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Consume an optional trailing "khz <freq>" starting at `index`, which must be
 * the end of the argument list.
 *
 * The frequency is a keyword pair rather than a bare trailing number because
 * `sd mmc` takes a variable number of pins (three for 1-bit, six for 4-bit): a
 * bare number could not be told apart from another pin. This mirrors the
 * `nau7802 init ldo 3.0` idiom.
 */
static int take_frequency(int argc, char **argv, int index, int *freq_khz)
{
    if (index >= argc) {
        return 0;
    }
    if (strcasecmp(argv[index], "khz") != 0) {
        bp_error("Unexpected argument '%s'; the only option is 'khz <freq>'", argv[index]);
        return -1;
    }
    if (index + 2 != argc) {
        bp_error("'khz' takes exactly one frequency in kHz");
        return -1;
    }
    if (parse_int_arg(argv[index + 1], freq_khz) < 0 ||
        *freq_khz < MIN_FREQ_KHZ || *freq_khz > MAX_FREQ_KHZ) {
        bp_error("Frequency must be %d-%d kHz", MIN_FREQ_KHZ, MAX_FREQ_KHZ);
        return -1;
    }
    return 0;
}

/* Every SD line is bidirectional, so all of them must be output-capable. */
static bool check_pins(const int *pins, int count, const char *const *names)
{
    for (int i = 0; i < count; i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) {
            bp_error("%s: GPIO %d is not an output-capable pin on this chip",
                     names[i], pins[i]);
            return false;
        }
    }
    return true;
}

/*
 * Parse the optional [size_kb] [block_kb] pair shared by `bench` and `raw`.
 * Returns 0 on success, -1 on a bad value.
 */
static int take_sizes(int argc, char **argv, int first, size_t *total_bytes, size_t *block_bytes)
{
    int size_kb = DEFAULT_SIZE_KB;
    int block_kb = DEFAULT_BLOCK_KB;

    if (argc > first && parse_int_arg(argv[first], &size_kb) < 0) {
        bp_error("Transfer size must be a whole number of KB");
        return -1;
    }
    if (argc > first + 1 && parse_int_arg(argv[first + 1], &block_kb) < 0) {
        bp_error("Block size must be a whole number of KB");
        return -1;
    }
    if (size_kb < 1 || size_kb > MAX_SIZE_KB) {
        bp_error("Transfer size must be 1-%d KB", MAX_SIZE_KB);
        return -1;
    }
    if (block_kb < 1 || block_kb > MAX_BLOCK_KB) {
        bp_error("Block size must be 1-%d KB", MAX_BLOCK_KB);
        return -1;
    }
    if (block_kb > size_kb) {
        block_kb = size_kb;
    }

    *block_bytes = (size_t)block_kb * 1024;
    /* Round the total down to a whole number of blocks so the maths is exact. */
    *total_bytes = ((size_t)size_kb * 1024 / *block_bytes) * *block_bytes;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Card information                                                    */
/* ------------------------------------------------------------------ */

static const char *card_type(void)
{
    if (card->is_sdio) {
        return "SDIO";
    }
    if (card->is_mmc) {
        return "MMC";
    }
    if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
        return "SDSC";
    }
    return (card->ocr & SD_OCR_S18_RA) ? "SDHC/SDXC (UHS-I)" : "SDHC/SDXC";
}

int cmd_sd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!require_card()) {
        return -1;
    }

    bp_printf("Interface: %s on pin%s", iface_name(), iface_pin_count == 1 ? " " : "s ");
    for (int i = 0; i < iface_pin_count; i++) {
        bp_printf("%s%d", i ? "," : "", iface_pins[i]);
    }
    bp_printf("\n");

    bp_printf("Type:      %s\n", card_type());
    bp_printf("Name:      %.8s\n", card->cid.name);

    /*
     * The SD CID packs the manufacture date into 12 bits as
     * (year - 2000) << 4 | month. MMC uses a different epoch, so only decode
     * the date for SD cards rather than print something plausible but wrong.
     */
    if (!card->is_mmc) {
        bp_printf("CID:       mfg 0x%02X, OEM 0x%04X, rev %d.%d, serial 0x%08X, made %04d-%02d\n",
                  (unsigned)card->cid.mfg_id, (unsigned)card->cid.oem_id,
                  (card->cid.revision >> 4) & 0xF, card->cid.revision & 0xF,
                  (unsigned)card->cid.serial,
                  2000 + (card->cid.date >> 4), card->cid.date & 0xF);
    } else {
        bp_printf("CID:       mfg 0x%02X, OEM 0x%04X, rev %d, serial 0x%08X\n",
                  (unsigned)card->cid.mfg_id, (unsigned)card->cid.oem_id,
                  card->cid.revision, (unsigned)card->cid.serial);
    }

    uint64_t bytes = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    bp_printf("Capacity:  %.2f GiB (%llu bytes, %d sectors of %d bytes)\n",
              (double)bytes / (1024.0 * 1024.0 * 1024.0), (unsigned long long)bytes,
              card->csd.capacity, card->csd.sector_size);
    bp_printf("CSD:       ver %d, read_bl_len %d, command classes 0x%03X\n",
              card->is_mmc ? card->csd.csd_ver : card->csd.csd_ver + 1,
              card->csd.read_block_len, (unsigned)card->csd.card_command_class);

    /*
     * real_freq_khz is what the host divider actually produced, which is not
     * what was asked for: the divider quantizes. max_freq_khz is the ceiling
     * the card itself reports, so the pair together says whether the interface
     * or the card is the limit.
     */
    if (card->real_freq_khz == 0) {
        bp_printf("Clock:     unknown\n");
    } else {
        bp_printf("Clock:     %.3f MHz (card limit %.3f MHz)%s%s\n",
                  card->real_freq_khz / 1000.0, card->max_freq_khz / 1000.0,
                  card->is_ddr ? ", DDR" : "", card->is_uhs1 ? ", UHS-I" : "");
    }

    bp_printf("Bus width: %d bit negotiated\n", 1 << card->log_bus_width);
    if (!card->is_mmc && !card->is_sdio) {
        bp_printf("SCR:       spec version %d, card supports%s%s\n",
                  card->scr.sd_spec,
                  (card->scr.bus_width & BIT(0)) ? " 1-bit" : "",
                  (card->scr.bus_width & BIT(2)) ? " 4-bit" : "");
        bp_printf("SSR:       %d-bit in use, allocation unit %" PRIu32 " KiB\n",
                  card->ssr.cur_bus_width ? 4 : 1, (uint32_t)card->ssr.alloc_unit_kb);
    }

    if (mounted) {
        uint64_t total = 0, freespace = 0;
        if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freespace) == ESP_OK) {
            bp_printf("FAT:       mounted at %s, %llu MiB total, %llu MiB free\n",
                      SD_MOUNT_POINT, (unsigned long long)(total / (1024 * 1024)),
                      (unsigned long long)(freespace / (1024 * 1024)));
        }
    } else {
        bp_printf("FAT:       not mounted ('sd bench' mounts it)\n");
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/* Common tail of `sd spi` and `sd mmc`: probe the card and report. */
static int finish_init(const char *what)
{
    card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        bp_error("Out of memory");
        teardown();
        return -1;
    }

    esp_err_t err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        bp_error("Initializing the card over %s: %s", what, esp_err_to_name(err));
        teardown();
        return -1;
    }

    bp_printf("Card up over %s at %.3f MHz\n", iface_name(), card->real_freq_khz / 1000.0);
    return cmd_sd_info(0, NULL);
}

int cmd_sd_spi(int argc, char **argv)
{
    if (argc < 5) {
        bp_printf("Usage: spi <clk> <mosi> <miso> <cs> [khz <freq>]\n");
        return -1;
    }

    int pins[4];
    static const char *const names[4] = {"CLK", "MOSI", "MISO", "CS"};
    for (int i = 0; i < 4; i++) {
        if (parse_int_arg(argv[i + 1], &pins[i]) < 0) {
            bp_error("%s must be a pin number", names[i]);
            return -1;
        }
    }

    int freq_khz = SDMMC_FREQ_DEFAULT;
    if (take_frequency(argc, argv, 5, &freq_khz) < 0) {
        return -1;
    }
    if (!check_pins(pins, 4, names)) {
        return -1;
    }

    if (spi_menu_owns_host()) {
        bp_error("The 'spi' menu holds the SPI host. Release it with 'spi free' first.");
        return -1;
    }

    teardown();
    iface = IFACE_SPI;

    const spi_bus_config_t bus_config = {
        .sclk_io_num = pins[0],
        .mosi_io_num = pins[1],
        .miso_io_num = pins[2],
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_BLOCK_KB * 1024,
    };

    esp_err_t err = spi_bus_initialize(BP_SPI_HOST_ID, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        bp_error("Initializing the SPI bus: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }
    bus_owned = true;

    err = sdspi_host_init();
    if (err != ESP_OK) {
        bp_error("Initializing the SD-over-SPI host: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }
    host_inited = true;

    sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_config.host_id = BP_SPI_HOST_ID;
    dev_config.gpio_cs = pins[3];

    err = sdspi_host_init_device(&dev_config, &spi_dev);
    if (err != ESP_OK) {
        bp_error("Attaching the card to the SPI bus: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }

    host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host.slot = spi_dev;
    host.max_freq_khz = freq_khz;

    memcpy(iface_pins, pins, sizeof(pins));
    iface_pin_count = 4;
    iface_width = 1;

    return finish_init("SPI");
}

#if SOC_SDMMC_HOST_SUPPORTED
int cmd_sd_mmc(int argc, char **argv)
{
    static const char *const names[6] = {"CLK", "CMD", "D0", "D1", "D2", "D3"};

    int pins[6];
    int pin_count = 0;
    int index = 1;
    while (index < argc && pin_count < 6 && strcasecmp(argv[index], "khz") != 0) {
        if (parse_int_arg(argv[index], &pins[pin_count]) < 0) {
            bp_error("%s must be a pin number, not '%s'",
                     pin_count < 6 ? names[pin_count] : "pin", argv[index]);
            return -1;
        }
        pin_count++;
        index++;
    }

    if (pin_count != 3 && pin_count != 6) {
        bp_printf("Usage: mmc <clk> <cmd> <d0> [<d1> <d2> <d3>] [khz <freq>]\n");
        bp_error("Give three pins for 1-bit mode or six for 4-bit mode");
        return -1;
    }

    int freq_khz = SDMMC_FREQ_DEFAULT;
    if (take_frequency(argc, argv, index, &freq_khz) < 0) {
        return -1;
    }
    if (!check_pins(pins, pin_count, names)) {
        return -1;
    }

    const int width = (pin_count == 6) ? 4 : 1;

    teardown();
    iface = IFACE_MMC;
    iface_width = width;

    host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host.max_freq_khz = freq_khz;
    host.slot = SDMMC_HOST_SLOT_1;

    /*
     * The bus width is driven by the slot, not by host.flags: sdmmc_fix_host_flags()
     * reads the slot width back and narrows the host flags to match, so setting
     * .width alone is what actually keeps a 1-bit slot from being switched to
     * 4-bit during initialization.
     */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = pins[0];
    slot_config.cmd = pins[1];
    slot_config.d0 = pins[2];
    if (width == 4) {
        slot_config.d1 = pins[3];
        slot_config.d2 = pins[4];
        slot_config.d3 = pins[5];
    }
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = (uint8_t)width;
    /* Boards being brought up frequently lack the external pull-ups the bus
     * needs; the internal ones are weak but usually enough to get a card to
     * answer, which is the point of the exercise. */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        bp_error("Initializing the SD host: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }
    host_inited = true;

    err = sdmmc_host_init_slot(host.slot, &slot_config);
    if (err != ESP_OK) {
        bp_error("Configuring the SD slot: %s", esp_err_to_name(err));
        teardown();
        return -1;
    }

    memcpy(iface_pins, pins, sizeof(int) * (size_t)pin_count);
    iface_pin_count = pin_count;

    return finish_init(width == 4 ? "SD 4-bit" : "SD 1-bit");
}
#else  /* !SOC_SDMMC_HOST_SUPPORTED */
int cmd_sd_mmc(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    bp_error("%s has no SD host peripheral, so 1-bit and 4-bit SD mode are not "
             "possible. Use 'sd spi' instead.", CONFIG_IDF_TARGET);
    return -1;
}
#endif

int cmd_sd_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (iface == IFACE_NONE) {
        bp_printf("No SD card is initialized.\n");
        return 0;
    }
    teardown();
    bp_printf("SD released.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* FAT mount                                                           */
/* ------------------------------------------------------------------ */

static bool mount_fat(void)
{
    if (mounted) {
        return true;
    }

    esp_err_t err = ff_diskio_get_drive(&fat_pdrv);
    if (err != ESP_OK || fat_pdrv == 0xFF) {
        bp_error("No free FATFS drive slot: %s", esp_err_to_name(err));
        fat_pdrv = 0xFF;
        return false;
    }
    ff_diskio_register_sdmmc(fat_pdrv, card);

    const char drive[3] = {(char)('0' + fat_pdrv), ':', '\0'};
    const esp_vfs_fat_conf_t conf = {
        .base_path = SD_MOUNT_POINT,
        .fat_drive = drive,
        .max_files = SD_MAX_OPEN_FILES,
    };

    FATFS *fs = NULL;
    err = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK) {
        bp_error("Registering %s with the VFS: %s", SD_MOUNT_POINT, esp_err_to_name(err));
        ff_diskio_unregister(fat_pdrv);
        fat_pdrv = 0xFF;
        return false;
    }

    FRESULT res = f_mount(fs, drive, 1);
    if (res != FR_OK) {
        esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
        ff_diskio_unregister(fat_pdrv);
        fat_pdrv = 0xFF;
        if (res == FR_NO_FILESYSTEM) {
            bp_error("The card has no FAT filesystem, so the file benchmark cannot "
                     "run. 'sd info' and 'sd raw' still work.");
        } else {
            bp_error("Mounting FAT: FatFs error %d", res);
        }
        return false;
    }

    mounted = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Benchmarks                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t bytes;
    int64_t elapsed_us;      /* time in the transfer calls only */
    int64_t worst_block_us;  /* slowest single block */
} bench_result_t;

/*
 * Report one direction.
 *
 * The worst single block is reported alongside the average because SD cards
 * stall for tens of milliseconds while they do internal housekeeping; on a
 * board that has to keep up with a sensor or a camera, that worst case is the
 * number that decides whether the design works, and an average hides it.
 */
static void report(const char *label, const bench_result_t *r, size_t block_bytes)
{
    double seconds = (double)r->elapsed_us / 1e6;
    double kib = (double)r->bytes / 1024.0;

    if (seconds <= 0.0) {
        bp_printf("%-6s %6.0f KiB in under 1 us -- too small to measure\n", label, kib);
        return;
    }

    bp_printf("%-6s %6.0f KiB in %7.3f s = %8.1f KiB/s (%.2f MiB/s), "
              "worst %u KiB block %.1f ms\n",
              label, kib, seconds, kib / seconds, kib / seconds / 1024.0,
              (unsigned)(block_bytes / 1024), (double)r->worst_block_us / 1000.0);
}

/* Fill a block with a position-dependent pattern, stamped with the block index
 * so a filesystem that hands back the wrong block is caught rather than scored. */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t block_index)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i * 31u + 7u);
    }
    if (len >= sizeof(uint32_t)) {
        memcpy(buf, &block_index, sizeof(uint32_t));
    }
}

int cmd_sd_bench(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 1, &total_bytes, &block_bytes) < 0) {
        return -1;
    }
    if (!mount_fat()) {
        return -1;
    }

    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);

    uint8_t *pattern = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    uint8_t *readback = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!pattern || !readback) {
        bp_error("Out of DMA-capable memory for a %u KiB block",
                 (unsigned)(block_bytes / 1024));
        free(pattern);
        free(readback);
        return -1;
    }

    bp_printf("FAT benchmark on %s: %u KiB in %u KiB blocks over %s at %.3f MHz\n",
              SD_BENCH_FILE, (unsigned)(total_bytes / 1024),
              (unsigned)(block_bytes / 1024), iface_name(), card->real_freq_khz / 1000.0);

    int result = -1;
    bench_result_t write_result = {0};
    bench_result_t read_result = {0};

    FILE *f = fopen(SD_BENCH_FILE, "wb");
    if (!f) {
        bp_error("Creating %s: %s", SD_BENCH_FILE, strerror(errno));
        goto done;
    }

    for (uint32_t i = 0; i < blocks; i++) {
        fill_pattern(pattern, block_bytes, i);
        int64_t start = esp_timer_get_time();
        size_t written = fwrite(pattern, 1, block_bytes, f);
        int64_t elapsed = esp_timer_get_time() - start;

        if (written != block_bytes) {
            bp_error("Writing block %u: %s", (unsigned)i, strerror(errno));
            fclose(f);
            goto cleanup_file;
        }
        write_result.bytes += written;
        write_result.elapsed_us += elapsed;
        if (elapsed > write_result.worst_block_us) {
            write_result.worst_block_us = elapsed;
        }
    }

    /*
     * The flush is inside the measurement on purpose. Without it the card is
     * still absorbing the tail of the data when the clock stops, and the
     * reported write speed is the speed of filling a RAM buffer.
     */
    int64_t flush_start = esp_timer_get_time();
    bool flushed = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    int64_t flush_us = esp_timer_get_time() - flush_start;
    write_result.elapsed_us += flush_us;
    fclose(f);

    if (!flushed) {
        bp_error("Flushing %s: %s", SD_BENCH_FILE, strerror(errno));
        goto cleanup_file;
    }

    report("Write", &write_result, block_bytes);
    bp_printf("       final flush %.1f ms\n", (double)flush_us / 1000.0);

    f = fopen(SD_BENCH_FILE, "rb");
    if (!f) {
        bp_error("Reopening %s: %s", SD_BENCH_FILE, strerror(errno));
        goto cleanup_file;
    }

    for (uint32_t i = 0; i < blocks; i++) {
        int64_t start = esp_timer_get_time();
        size_t got = fread(readback, 1, block_bytes, f);
        int64_t elapsed = esp_timer_get_time() - start;

        if (got != block_bytes) {
            bp_error("Reading block %u: short read of %u bytes", (unsigned)i, (unsigned)got);
            fclose(f);
            goto cleanup_file;
        }
        read_result.bytes += got;
        read_result.elapsed_us += elapsed;
        if (elapsed > read_result.worst_block_us) {
            read_result.worst_block_us = elapsed;
        }

        /* Verified outside the timed region above -- the compare is charged to
         * the CPU, not to the card. */
        fill_pattern(pattern, block_bytes, i);
        if (memcmp(pattern, readback, block_bytes) != 0) {
            bp_error("Block %u read back different from what was written; the "
                     "throughput numbers above cannot be trusted.", (unsigned)i);
            fclose(f);
            goto cleanup_file;
        }
    }
    fclose(f);

    report("Read", &read_result, block_bytes);
    bp_printf("Data verified: %u KiB read back byte-for-byte.\n",
              (unsigned)(read_result.bytes / 1024));
    result = 0;

cleanup_file:
    /* The card is left as it was found; this is a bring-up tool, not a
     * destructive one. */
    if (unlink(SD_BENCH_FILE) != 0 && errno != ENOENT) {
        bp_error("Removing %s: %s", SD_BENCH_FILE, strerror(errno));
        result = -1;
    }

done:
    free(pattern);
    free(readback);
    return result;
}

int cmd_sd_raw(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 1, &total_bytes, &block_bytes) < 0) {
        return -1;
    }

    int start_sector = 0;
    if (argc > 3 && (parse_int_arg(argv[3], &start_sector) < 0 || start_sector < 0)) {
        bp_error("Start sector must be a non-negative sector number");
        return -1;
    }

    const size_t sector_size = (size_t)card->csd.sector_size;
    if (sector_size == 0 || block_bytes < sector_size) {
        bp_error("Block size must be at least the %u byte sector size",
                 (unsigned)sector_size);
        return -1;
    }

    /* Whole sectors only: sdmmc_read_sectors() counts in sectors. */
    const size_t sectors_per_block = block_bytes / sector_size;
    block_bytes = sectors_per_block * sector_size;
    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);
    if (blocks == 0) {
        bp_error("Transfer size is smaller than one block");
        return -1;
    }

    const uint64_t last_sector = (uint64_t)start_sector + (uint64_t)blocks * sectors_per_block;
    if (last_sector > (uint64_t)card->csd.capacity) {
        bp_error("Sectors %d-%llu are past the end of the card (%d sectors)",
                 start_sector, (unsigned long long)last_sector - 1, card->csd.capacity);
        return -1;
    }

    uint8_t *buffer = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!buffer) {
        bp_error("Out of DMA-capable memory for a %u KiB block",
                 (unsigned)(block_bytes / 1024));
        return -1;
    }

    /*
     * Read-only, so it is safe to run anywhere on the card, and it measures the
     * interface without the filesystem's allocation and metadata overhead in
     * the way -- the difference between this and `sd bench` is what FAT costs.
     */
    bp_printf("Raw read of %u KiB from sector %d in %u KiB blocks over %s at %.3f MHz\n",
              (unsigned)(blocks * block_bytes / 1024), start_sector,
              (unsigned)(block_bytes / 1024), iface_name(), card->real_freq_khz / 1000.0);

    bench_result_t result = {0};
    for (uint32_t i = 0; i < blocks; i++) {
        size_t sector = (size_t)start_sector + (size_t)i * sectors_per_block;

        int64_t start = esp_timer_get_time();
        esp_err_t err = sdmmc_read_sectors(card, buffer, sector, sectors_per_block);
        int64_t elapsed = esp_timer_get_time() - start;

        if (err != ESP_OK) {
            bp_error("Reading %u sectors at %u: %s", (unsigned)sectors_per_block,
                     (unsigned)sector, esp_err_to_name(err));
            free(buffer);
            return -1;
        }
        result.bytes += block_bytes;
        result.elapsed_us += elapsed;
        if (elapsed > result.worst_block_us) {
            result.worst_block_us = elapsed;
        }
    }

    report("Read", &result, block_bytes);
    free(buffer);
    return 0;
}
