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
#include "esp_rom_crc.h"
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

/*
 * What was asked for. This deliberately survives teardown(), so the card can be
 * torn down and reopened at a different clock -- which is what `sd sweep` does
 * on every step -- without the caller re-entering the pins.
 */
static struct {
    sd_iface_t iface;
    int pins[6];
    int pin_count;
    int width;                          /* 1 or 4, for IFACE_MMC */
    int freq_khz;
} cfg;

/*
 * What the hardware currently has open, which is not the same thing: it is
 * IFACE_NONE between a teardown and the next open, and it is what decides which
 * host teardown() has to release.
 */
static sd_iface_t open_iface;
static sdmmc_card_t *card;
static sdmmc_host_t host;
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
    switch (cfg.iface) {
    case IFACE_SPI:
        return "SPI";
    case IFACE_MMC:
        return cfg.width == 4 ? "SD 4-bit" : "SD 1-bit";
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
 * Release the hardware, in the reverse of the order it was acquired, leaving
 * `cfg` alone. Called by `sd close`, at the top of `sd spi` / `sd mmc` so those
 * can be re-run with different pins the way `spi bus` and `i2c bus` can, on
 * every step of `sd sweep`, and on every init failure so a failed attempt never
 * leaves half-configured hardware behind.
 */
static void teardown(void)
{
    unmount_fat();

    free(card);
    card = NULL;

    if (open_iface == IFACE_SPI) {
        if (spi_dev >= 0) {
            sdspi_host_remove_device(spi_dev);
            spi_dev = -1;
        }
        if (host_inited) {
            sdspi_host_deinit();
        }
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else if (open_iface == IFACE_MMC) {
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

    open_iface = IFACE_NONE;
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

    bp_printf("Interface: %s on pins ", iface_name());
    for (int i = 0; i < cfg.pin_count; i++) {
        bp_printf("%s%d", i ? "," : "", cfg.pins[i]);
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
        bp_printf("Clock:     %.3f MHz requested %d kHz, negotiated ceiling %.3f MHz%s%s\n",
                  card->real_freq_khz / 1000.0, cfg.freq_khz, card->max_freq_khz / 1000.0,
                  card->is_ddr ? ", DDR" : "", card->is_uhs1 ? ", UHS-I" : "");
        /*
         * The CSD speed is what the card advertises after any high-speed switch,
         * and is therefore the line between running it in spec and overclocking
         * it. `sd sweep` finds out what it will actually tolerate.
         */
        bp_printf("Card rated: %.3f MHz per CSD%s\n", card->csd.tr_speed / 1e6,
                  card->real_freq_khz * 1000 > card->csd.tr_speed ? "  <-- OVERCLOCKED" : "");
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

static esp_err_t open_spi(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = cfg.pins[0],
        .mosi_io_num = cfg.pins[1],
        .miso_io_num = cfg.pins[2],
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_BLOCK_KB * 1024,
    };

    esp_err_t err = spi_bus_initialize(BP_SPI_HOST_ID, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    bus_owned = true;

    err = sdspi_host_init();
    if (err != ESP_OK) {
        return err;
    }
    host_inited = true;

    sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_config.host_id = BP_SPI_HOST_ID;
    dev_config.gpio_cs = cfg.pins[3];

    err = sdspi_host_init_device(&dev_config, &spi_dev);
    if (err != ESP_OK) {
        return err;
    }

    host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host.slot = spi_dev;
    host.max_freq_khz = cfg.freq_khz;
    return ESP_OK;
}

#if SOC_SDMMC_HOST_SUPPORTED
static esp_err_t open_mmc(void)
{
    host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host.max_freq_khz = cfg.freq_khz;
    host.slot = SDMMC_HOST_SLOT_1;

    /*
     * The bus width is driven by the slot, not by host.flags: sdmmc_fix_host_flags()
     * reads the slot width back and narrows the host flags to match, so setting
     * .width alone is what actually keeps a 1-bit slot from being switched to
     * 4-bit during initialization.
     */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = cfg.pins[0];
    slot_config.cmd = cfg.pins[1];
    slot_config.d0 = cfg.pins[2];
    if (cfg.width == 4) {
        slot_config.d1 = cfg.pins[3];
        slot_config.d2 = cfg.pins[4];
        slot_config.d3 = cfg.pins[5];
    }
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.width = (uint8_t)cfg.width;
    /* Boards being brought up frequently lack the external pull-ups the bus
     * needs; the internal ones are weak but usually enough to get a card to
     * answer, which is the point of the exercise. */
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        return err;
    }
    host_inited = true;

    return sdmmc_host_init_slot(host.slot, &slot_config);
}
#endif

/*
 * Bring the hardware up from `cfg` and probe the card. Quiet on success: the
 * sweep calls this once per clock step and does its own reporting.
 *
 * Always leaves the hardware released on failure, so the caller can go straight
 * on to the next attempt.
 */
static esp_err_t open_card(void)
{
    teardown();

    esp_err_t err;
    if (cfg.iface == IFACE_SPI) {
        open_iface = IFACE_SPI;
        err = open_spi();
    }
#if SOC_SDMMC_HOST_SUPPORTED
    else if (cfg.iface == IFACE_MMC) {
        open_iface = IFACE_MMC;
        err = open_mmc();
    }
#endif
    else {
        return ESP_ERR_INVALID_STATE;
    }

    if (err != ESP_OK) {
        teardown();
        return err;
    }

    card = calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        teardown();
        return ESP_ERR_NO_MEM;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        teardown();
        return err;
    }
    return ESP_OK;
}

/*
 * A card that answers CMD0 and CMD8 but then times out on ACMD41 has been asked
 * to power up and never finished. In practice on a bring-up bench that is
 * almost never the driver: the card has latched into SPI mode, or a connector is
 * marginal. Both cost minutes to find and seconds to fix, so say so rather than
 * leaving a bare ESP_ERR_TIMEOUT.
 */
static void explain_init_timeout(esp_err_t err)
{
    if (err != ESP_ERR_TIMEOUT) {
        return;
    }
    bp_printf("      The card did not complete power-up (ACMD41). Usually one of:\n");
    bp_printf("      - It is latched in SPI mode from an earlier 'sd spi'. A card\n");
    bp_printf("        leaves SPI mode only when its power is removed, and a board\n");
    bp_printf("        reset does not do that -- unplug and replug the board.\n");
    bp_printf("      - The card or an expansion connector is not fully seated.\n");
    bp_printf("      - The card is failing. Try another one.\n");
}

/*
 * ESP-IDF maps these three exact frequencies onto the UHS-I modes and rejects
 * them outright on a host without UHS support. Asking for 50000 kHz on an
 * ESP32-S3 therefore fails with a bare ESP_ERR_NOT_SUPPORTED that says nothing
 * about why, so say it here instead.
 */
static bool frequency_is_reserved(int freq_khz)
{
#if SOC_SDMMC_UHS_I_SUPPORTED
    (void)freq_khz;
    return false;
#else
    if (freq_khz == SDMMC_FREQ_DDR50 || freq_khz == SDMMC_FREQ_SDR50 ||
        freq_khz == SDMMC_FREQ_SDR104) {
        bp_error("%d kHz selects a UHS-I mode, which %s does not support. Ask for "
                 "a nearby rate such as %d kHz instead.",
                 freq_khz, CONFIG_IDF_TARGET, freq_khz - 1000);
        return true;
    }
    return false;
#endif
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
    if (frequency_is_reserved(freq_khz)) {
        return -1;
    }

    if (spi_menu_owns_host()) {
        bp_error("The 'spi' menu holds the SPI host. Release it with 'spi free' first.");
        return -1;
    }

    teardown();
    cfg.iface = IFACE_SPI;
    memcpy(cfg.pins, pins, sizeof(pins));
    cfg.pin_count = 4;
    cfg.width = 1;
    cfg.freq_khz = freq_khz;

    esp_err_t err = open_card();
    if (err != ESP_OK) {
        bp_error("Initializing the card over SPI at %d kHz: %s", freq_khz,
                 esp_err_to_name(err));
        explain_init_timeout(err);
        return -1;
    }

    bp_printf("Card up over SPI at %.3f MHz\n", card->real_freq_khz / 1000.0);
    return cmd_sd_info(0, NULL);
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
            bp_error("%s must be a pin number, not '%s'", names[pin_count], argv[index]);
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
    if (frequency_is_reserved(freq_khz)) {
        return -1;
    }

    teardown();
    cfg.iface = IFACE_MMC;
    memcpy(cfg.pins, pins, sizeof(int) * (size_t)pin_count);
    cfg.pin_count = pin_count;
    cfg.width = (pin_count == 6) ? 4 : 1;
    cfg.freq_khz = freq_khz;

    esp_err_t err = open_card();
    if (err != ESP_OK) {
        bp_error("Initializing the card over %s at %d kHz: %s", iface_name(), freq_khz,
                 esp_err_to_name(err));
        explain_init_timeout(err);
        return -1;
    }

    bp_printf("Card up over %s at %.3f MHz\n", iface_name(), card->real_freq_khz / 1000.0);
    return cmd_sd_info(0, NULL);
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

    if (cfg.iface == IFACE_NONE) {
        bp_printf("No SD card is initialized.\n");
        return 0;
    }
    teardown();
    cfg.iface = IFACE_NONE;
    cfg.pin_count = 0;
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

/* ------------------------------------------------------------------ */
/* Clock sweep                                                         */
/* ------------------------------------------------------------------ */

/*
 * Rates to try, in kHz. The host divides a fixed source clock, so most of these
 * land on the same actual frequency as a neighbour; the sweep reports what was
 * really produced and skips a step that repeats the previous one.
 *
 * 50000, 100000 and 200000 are deliberately absent: ESP-IDF treats those exact
 * values as requests for UHS-I modes and rejects them on hosts without UHS
 * support, which would look like a card failure rather than a host limitation.
 */
static const int freq_ladder[] = {
    20000, 24000, 26667, 30000, 33333, 36000, 40000,
    44000, 48000, 52000, 56000, 60000, 66667, 72000, 80000,
};

#define SWEEP_BASELINE_KHZ SDMMC_FREQ_DEFAULT
#define SWEEP_MAX_CONSECUTIVE_FAILURES 3

/*
 * How many consecutive steps may produce the same clock as the one before
 * before the sweep concludes the host, not the card, is the limit.
 *
 * It cannot be 1: the divider legitimately skips a step here and there (asking
 * for 30 MHz when 26.67 and 32 are the neighbouring divisors lands back on
 * 26.67, and the next step up still moves). Three in a row does not happen for
 * that reason -- it means every larger request is being clamped.
 */
#define SWEEP_MAX_CONSECUTIVE_DUPLICATES 3

/*
 * Read `blocks` blocks from sector 0 and return a CRC32 over the lot.
 *
 * A CRC rather than a kept copy of the reference: it costs 4 bytes instead of a
 * second buffer, which is what lets the verified region be large enough to be
 * worth something on a chip with a few hundred KB of RAM.
 */
static esp_err_t read_crc(uint8_t *buffer, size_t block_bytes, uint32_t blocks,
                          size_t sectors_per_block, uint32_t *out_crc,
                          bench_result_t *out_timing)
{
    uint32_t crc = 0;
    bench_result_t timing = {0};

    for (uint32_t i = 0; i < blocks; i++) {
        size_t sector = (size_t)i * sectors_per_block;

        int64_t start = esp_timer_get_time();
        esp_err_t err = sdmmc_read_sectors(card, buffer, sector, sectors_per_block);
        int64_t elapsed = esp_timer_get_time() - start;
        if (err != ESP_OK) {
            return err;
        }

        timing.bytes += block_bytes;
        timing.elapsed_us += elapsed;
        if (elapsed > timing.worst_block_us) {
            timing.worst_block_us = elapsed;
        }
        crc = esp_rom_crc32_le(crc, buffer, (uint32_t)block_bytes);
    }

    *out_crc = crc;
    if (out_timing) {
        *out_timing = timing;
    }
    return ESP_OK;
}

static double throughput_kib_s(const bench_result_t *r)
{
    if (r->elapsed_us <= 0) {
        return 0.0;
    }
    return ((double)r->bytes / 1024.0) / ((double)r->elapsed_us / 1e6);
}

int cmd_sd_sweep(int argc, char **argv)
{
    if (!require_card()) {
        return -1;
    }

    int max_khz = MAX_FREQ_KHZ;
    if (argc > 1 && (parse_int_arg(argv[1], &max_khz) < 0 ||
                     max_khz < SWEEP_BASELINE_KHZ || max_khz > MAX_FREQ_KHZ)) {
        bp_error("Ceiling must be %d-%d kHz", SWEEP_BASELINE_KHZ, MAX_FREQ_KHZ);
        return -1;
    }

    size_t total_bytes = 0, block_bytes = 0;
    if (take_sizes(argc, argv, 2, &total_bytes, &block_bytes) < 0) {
        return -1;
    }

    const size_t sector_size = (size_t)card->csd.sector_size;
    if (sector_size == 0 || block_bytes < sector_size) {
        bp_error("Block size must be at least the %u byte sector size", (unsigned)sector_size);
        return -1;
    }
    const size_t sectors_per_block = block_bytes / sector_size;
    block_bytes = sectors_per_block * sector_size;
    const uint32_t blocks = (uint32_t)(total_bytes / block_bytes);
    if (blocks == 0 || (uint64_t)blocks * sectors_per_block > (uint64_t)card->csd.capacity) {
        bp_error("Test region does not fit on the card");
        return -1;
    }

    /* Remember what to fall back to if every overclocked rate fails. */
    const int entry_freq_khz = cfg.freq_khz;

    uint8_t *buffer = heap_caps_malloc(block_bytes, MALLOC_CAP_DMA);
    if (!buffer) {
        bp_error("Out of DMA-capable memory for a %u KiB block",
                 (unsigned)(block_bytes / 1024));
        return -1;
    }

    bp_printf("Clock sweep over %s, %u KiB read per step from sector 0.\n",
              iface_name(), (unsigned)(blocks * block_bytes / 1024));
    bp_printf("Read-only: nothing is written to the card at an unverified clock.\n");

    /*
     * The drivers log the same lines on every single init, and the sweep does
     * one init per step; left alone that is a dozen copies of each interleaved
     * with the results table. Only the informational chatter is silenced --
     * warnings and errors still come through, because the reason a step failed
     * is exactly what the table cannot say on its own. Restored before
     * returning.
     */
    esp_log_level_set("SD_HOST", ESP_LOG_ERROR);
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);

    int result = -1;
    uint32_t reference_crc = 0;

    /*
     * Establish the reference at a rate the card is rated for. Read it twice: if
     * the card cannot reproduce its own data in spec then every comparison after
     * this is meaningless, and it is better to say so than to report a
     * confident-looking overclocking limit derived from noise.
     */
    cfg.freq_khz = SWEEP_BASELINE_KHZ;
    esp_err_t err = open_card();
    if (err != ESP_OK) {
        bp_error("Re-initializing at the %d kHz reference rate: %s",
                 SWEEP_BASELINE_KHZ, esp_err_to_name(err));
        goto done;
    }

    bench_result_t reference_timing = {0};
    err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &reference_crc,
                   &reference_timing);
    if (err == ESP_OK) {
        uint32_t again = 0;
        err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &again, NULL);
        if (err == ESP_OK && again != reference_crc) {
            bp_error("The card returned different data for the same sectors at the "
                     "in-spec %d kHz reference rate. It is not reliable enough to "
                     "sweep; nothing above this can be trusted either.",
                     SWEEP_BASELINE_KHZ);
            goto done;
        }
    }
    if (err != ESP_OK) {
        bp_error("Reading the reference at %d kHz: %s", SWEEP_BASELINE_KHZ,
                 esp_err_to_name(err));
        goto done;
    }

    const int rated_khz = card->csd.tr_speed / 1000;
    bp_printf("Reference CRC 0x%08X, stable over two passes. Card is rated for "
              "%.3f MHz.\n\n", (unsigned)reference_crc, rated_khz / 1000.0);
    bp_printf("  Requested     Actual   Read speed   Result\n");

    int best_requested_khz = 0;
    int best_actual_khz = 0;
    double best_kib_s = 0.0;
    int previous_actual_khz = -1;
    int consecutive_failures = 0;
    int consecutive_duplicates = 0;
    int first_failure_khz = 0;
    bool host_clamped = false;

    for (size_t i = 0; i < sizeof(freq_ladder) / sizeof(freq_ladder[0]); i++) {
        const int requested = freq_ladder[i];
        if (requested > max_khz) {
            break;
        }

        cfg.freq_khz = requested;
        err = open_card();
        if (err != ESP_OK) {
            bp_printf("  %7d kHz          -            -   init failed: %s\n",
                      requested, esp_err_to_name(err));
            if (!first_failure_khz) {
                first_failure_khz = requested;
            }
            if (++consecutive_failures >= SWEEP_MAX_CONSECUTIVE_FAILURES) {
                bp_printf("  ... stopping after %d consecutive failures.\n",
                          consecutive_failures);
                break;
            }
            continue;
        }

        const int actual = card->real_freq_khz;
        if (actual == previous_actual_khz) {
            bp_printf("  %7d kHz  %7.3f MHz            -   same clock as the previous "
                      "step, skipped\n", requested, actual / 1000.0);
            if (++consecutive_duplicates >= SWEEP_MAX_CONSECUTIVE_DUPLICATES) {
                host_clamped = true;
                break;
            }
            continue;
        }
        previous_actual_khz = actual;
        consecutive_duplicates = 0;

        uint32_t crc = 0;
        bench_result_t timing = {0};
        err = read_crc(buffer, block_bytes, blocks, sectors_per_block, &crc, &timing);

        if (err != ESP_OK) {
            bp_printf("  %7d kHz  %7.3f MHz            -   read failed: %s\n",
                      requested, actual / 1000.0, esp_err_to_name(err));
        } else if (crc != reference_crc) {
            /* This is the failure mode that matters: the transfer "succeeded"
             * and returned corrupt data. Without the CRC it would have been
             * recorded as a pass with a very good throughput figure. */
            bp_printf("  %7d kHz  %7.3f MHz   %8.1f KiB/s   DATA MISMATCH (CRC 0x%08X)\n",
                      requested, actual / 1000.0, throughput_kib_s(&timing), (unsigned)crc);
        } else {
            const double kib_s = throughput_kib_s(&timing);
            bp_printf("  %7d kHz  %7.3f MHz   %8.1f KiB/s   ok%s\n",
                      requested, actual / 1000.0, kib_s,
                      actual > rated_khz ? "  (overclocked)" : "");
            if (actual > best_actual_khz) {
                best_requested_khz = requested;
                best_actual_khz = actual;
                best_kib_s = kib_s;
            }
            consecutive_failures = 0;
            continue;
        }

        if (!first_failure_khz) {
            first_failure_khz = requested;
        }
        if (++consecutive_failures >= SWEEP_MAX_CONSECUTIVE_FAILURES) {
            bp_printf("  ... stopping after %d consecutive failures.\n", consecutive_failures);
            break;
        }
    }

    bp_printf("\n");
    if (best_actual_khz == 0) {
        bp_error("No rate passed, not even the %d kHz reference.", SWEEP_BASELINE_KHZ);
        cfg.freq_khz = entry_freq_khz;
    } else {
        bp_printf("Fastest verified: %.3f MHz at %.1f KiB/s (%.2f MiB/s), %.2fx the "
                  "reference.\n", best_actual_khz / 1000.0, best_kib_s,
                  best_kib_s / 1024.0,
                  throughput_kib_s(&reference_timing) > 0.0
                      ? best_kib_s / throughput_kib_s(&reference_timing) : 0.0);
        if (first_failure_khz) {
            bp_printf("First failure:    %d kHz requested.\n", first_failure_khz);
        } else if (host_clamped) {
            /*
             * Not the same thing as "the card is happy up here". Every larger
             * request produced the identical clock, so the card was never
             * actually driven any faster and its own limit is still unknown.
             */
            bp_printf("Limited by the host, not the card: every request above %d kHz "
                      "produced the same %.3f MHz, so the card was never clocked any "
                      "faster than that. %s cannot drive it harder on this interface.\n",
                      best_requested_khz, best_actual_khz / 1000.0, CONFIG_IDF_TARGET);
        } else {
            bp_printf("No failures up to the %d kHz ceiling that was asked for; raise "
                      "it to look further.\n", max_khz);
        }
        if (best_actual_khz > rated_khz) {
            bp_printf("This is %.3f MHz above the card's rated %.3f MHz. Passing one "
                      "read sweep is not a stability guarantee: overclocking is out of "
                      "spec, and margin varies with temperature, supply and wiring.\n",
                      (best_actual_khz - rated_khz) / 1000.0, rated_khz / 1000.0);
        }
        cfg.freq_khz = best_requested_khz;
        result = 0;
    }

    /* Leave the card usable at the best rate found, rather than wherever the
     * sweep happened to stop. */
    err = open_card();
    if (err != ESP_OK) {
        bp_error("Re-initializing at %d kHz after the sweep: %s", cfg.freq_khz,
                 esp_err_to_name(err));
        result = -1;
    } else {
        bp_printf("Card re-initialized at %.3f MHz.\n", card->real_freq_khz / 1000.0);
    }

done:
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
    esp_log_level_set("sdspi_transaction", ESP_LOG_INFO);
    free(buffer);
    return result;
}
