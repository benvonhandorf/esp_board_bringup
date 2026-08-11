/*
 * TI INA237 current / voltage / power monitor.
 *
 * Register addresses and every scaling constant below come from the INA237
 * datasheet, SBOSA20A (revised May 2022). Section references are given where
 * the number is not self-evident.
 */
#include "esp_bringup.h"
#include "output.h"
#include "ina237.h"
#include "i2c.h"

/* Table 7-3, INA237 Registers. Note there is no DEVICE_ID register: the map
 * ends at MANUFACTURER_ID, unlike the otherwise similar INA238/INA228. */
#define REG_CONFIG          0x00
#define REG_ADC_CONFIG      0x01
#define REG_SHUNT_CAL       0x02
#define REG_VSHUNT          0x04
#define REG_VBUS            0x05
#define REG_DIETEMP         0x06
#define REG_CURRENT         0x07
#define REG_POWER           0x08  /* 24-bit */
#define REG_DIAG_ALRT       0x0B
#define REG_MANUFACTURER_ID 0x3E

#define MANUFACTURER_ID_TI  0x5449

/* Table 7-19, DIAG_ALRT register. */
#define DIAG_MATHOF  BIT(9)
#define DIAG_CNVRF   BIT(1)
#define DIAG_MEMSTAT BIT(0) /* 1 = normal, 0 = trim checksum error */

/* Table 8-1, ADC Full Scale Values, at ADCRANGE = 0. This driver leaves
 * ADCRANGE at its reset value and never writes CONFIG. */
#define VSHUNT_LSB_V   5.0e-6
#define VBUS_LSB_V     3.125e-3
#define DIETEMP_LSB_C  0.125
#define SHUNT_FULL_SCALE_V 0.16384

/* Equation 4: Power [W] = 0.2 x CURRENT_LSB x POWER. */
#define POWER_COEFFICIENT 0.2

/*
 * Equation 1: SHUNT_CAL = 819.2e6 x CURRENT_LSB x RSHUNT.
 *
 * Picking the maximum expected current so that it exactly fills the ADC range
 * gives CURRENT_LSB = SHUNT_FULL_SCALE_V / RSHUNT / 2^15, which is simply the
 * shunt LSB divided by the resistance. Substituting that back into Equation 1
 * cancels RSHUNT entirely, so SHUNT_CAL is the same constant for every shunt
 * value -- and happens to equal the register's own reset value of 0x1000.
 */
#define SHUNT_CAL_VALUE 4096

/* The A0/A1 pins select one of 16 addresses (Table 7-2). */
#define INA237_ADDR_FIRST 0x40
#define INA237_ADDR_LAST  0x4F

#define DEFAULT_SHUNT_OHMS 0.004

/* Matches MAX_CACHED_DEVICES in i2c.c, so every configured part keeps a
 * cached device handle. */
#define MAX_DEVICES 8

#define XFER_TIMEOUT_MS 1000

typedef struct {
    bool used;
    uint8_t address;
    double shunt_ohms;
    double current_lsb; /* amperes per CURRENT register count */
} ina237_device_t;

static ina237_device_t devices[MAX_DEVICES];

static double current_lsb_for(double shunt_ohms)
{
    return VSHUNT_LSB_V / shunt_ohms;
}

/*
 * Read a register.
 *
 * The INA237 needs the register pointer written before every read and does not
 * auto-increment across registers (section 7.5.1.1), so each value is its own
 * write-then-read transaction. Values arrive most significant byte first.
 */
static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint8_t reg,
                          uint8_t *buffer, size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buffer, len, XFER_TIMEOUT_MS);
}

static esp_err_t read_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t raw[2];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = (uint16_t)((raw[0] << 8) | raw[1]);
    }
    return err;
}

static esp_err_t read_reg24(i2c_master_dev_handle_t dev, uint8_t reg, uint32_t *out)
{
    uint8_t raw[3];
    esp_err_t err = read_reg(dev, reg, raw, sizeof(raw));
    if (err == ESP_OK) {
        *out = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
    }
    return err;
}

static esp_err_t write_reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t value)
{
    const uint8_t payload[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return i2c_master_transmit(dev, payload, sizeof(payload), XFER_TIMEOUT_MS);
}

static ina237_device_t *find_device(uint8_t address)
{
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && devices[i].address == address) {
            return &devices[i];
        }
    }
    return NULL;
}

static size_t configured_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used) {
            count++;
        }
    }
    return count;
}

/*
 * Register a part: confirm something INA237-shaped is really there, program
 * the calibration, and remember the shunt value. Re-running on a known address
 * updates it in place.
 */
static int configure_device(uint8_t address, double shunt_ohms, bool quiet)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        bp_error("Addressing 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    uint16_t manufacturer = 0;
    err = read_reg16(dev, REG_MANUFACTURER_ID, &manufacturer);
    if (err != ESP_OK) {
        bp_error("No response from 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    if (manufacturer != MANUFACTURER_ID_TI) {
        bp_error("0x%02X is not an INA237: MANUFACTURER_ID reads 0x%04X, expected 0x%04X",
                 address, manufacturer, MANUFACTURER_ID_TI);
        return -1;
    }

    err = write_reg16(dev, REG_SHUNT_CAL, SHUNT_CAL_VALUE);
    if (err != ESP_OK) {
        bp_error("Writing SHUNT_CAL to 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    ina237_device_t *entry = find_device(address);
    if (!entry) {
        for (size_t i = 0; i < MAX_DEVICES; i++) {
            if (!devices[i].used) {
                entry = &devices[i];
                break;
            }
        }
        if (!entry) {
            bp_error("Cannot track more than %d INA237s", MAX_DEVICES);
            return -1;
        }
    }

    entry->used = true;
    entry->address = address;
    entry->shunt_ohms = shunt_ohms;
    entry->current_lsb = current_lsb_for(shunt_ohms);

    if (!quiet) {
        bp_printf("0x%02X configured: shunt %.4f ohm, %.4f mA/LSB, range +/-%.2f A\n",
                  address, shunt_ohms, entry->current_lsb * 1000.0,
                  SHUNT_FULL_SCALE_V / shunt_ohms);
    }

    return 0;
}

int cmd_ina237_config(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc < 2) {
        bp_printf("Usage: config <address> [shunt_ohms]\n");
        bp_printf("Address is 0x%02X-0x%02X; shunt defaults to %.3f ohm.\n",
                  INA237_ADDR_FIRST, INA237_ADDR_LAST, DEFAULT_SHUNT_OHMS);
        return -1;
    }

    int address = 0;
    if (parse_num_arg(argv[1], &address) < 0 ||
        address < INA237_ADDR_FIRST || address > INA237_ADDR_LAST) {
        bp_error("Address must be 0x%02X-0x%02X (set by the A0/A1 pins)",
                 INA237_ADDR_FIRST, INA237_ADDR_LAST);
        return -1;
    }

    double shunt_ohms = DEFAULT_SHUNT_OHMS;
    if (argc > 2) {
        if (parse_double_arg(argv[2], &shunt_ohms) < 0 || shunt_ohms <= 0.0) {
            bp_error("Shunt resistance must be a positive number of ohms, e.g. 0.004");
            return -1;
        }
    }

    return configure_device((uint8_t)address, shunt_ohms, false);
}

static void report_health(uint16_t diag, uint16_t shunt_cal)
{
    if (!(diag & DIAG_MEMSTAT)) {
        bp_error("      Trim memory checksum error (DIAG_ALRT.MEMSTAT); "
                 "readings cannot be trusted");
    }
    if (diag & DIAG_MATHOF) {
        bp_error("      Arithmetic overflow (DIAG_ALRT.MATHOF); current and "
                 "power are invalid");
    }
    if (shunt_cal != SHUNT_CAL_VALUE) {
        bp_error("      SHUNT_CAL reads %u, expected %d; the device has been "
                 "reset since it was configured. Re-run 'config'.",
                 shunt_cal, SHUNT_CAL_VALUE);
    }
}

static int read_device(const ina237_device_t *entry)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(entry->address, &dev);
    if (err != ESP_OK) {
        bp_error("Addressing 0x%02X: %s", entry->address, esp_err_to_name(err));
        return -1;
    }

    uint16_t vbus_raw = 0, vshunt_raw = 0, dietemp_raw = 0, current_raw = 0;
    uint16_t shunt_cal = 0, diag = 0;
    uint32_t power_raw = 0;

    if ((err = read_reg16(dev, REG_VBUS, &vbus_raw)) != ESP_OK ||
        (err = read_reg16(dev, REG_VSHUNT, &vshunt_raw)) != ESP_OK ||
        (err = read_reg16(dev, REG_DIETEMP, &dietemp_raw)) != ESP_OK ||
        (err = read_reg16(dev, REG_CURRENT, &current_raw)) != ESP_OK ||
        (err = read_reg24(dev, REG_POWER, &power_raw)) != ESP_OK ||
        (err = read_reg16(dev, REG_SHUNT_CAL, &shunt_cal)) != ESP_OK ||
        (err = read_reg16(dev, REG_DIAG_ALRT, &diag)) != ESP_OK) {
        bp_error("Reading 0x%02X: %s", entry->address, esp_err_to_name(err));
        return -1;
    }

    /* VBUS is unsigned; VSHUNT and CURRENT are two's complement; DIETEMP is a
     * signed 12-bit value living in bits 15:4, so it is sign-extended as 16
     * bits first and then shifted down. */
    double bus_v = vbus_raw * VBUS_LSB_V;
    double shunt_v = (int16_t)vshunt_raw * VSHUNT_LSB_V;
    double temp_c = ((int16_t)dietemp_raw >> 4) * DIETEMP_LSB_C;
    double current_a = (int16_t)current_raw * entry->current_lsb;
    double power_w = power_raw * POWER_COEFFICIENT * entry->current_lsb;

    bp_printf("0x%02X  Bus %8.3f V  Current %8.4f A  Power %8.3f W\n",
              entry->address, bus_v, current_a, power_w);
    bp_printf("      Shunt %8.3f mV  Temp %5.1f C\n", shunt_v * 1000.0, temp_c);
    bp_printf("      SHUNT_CAL %u  CURRENT_LSB %.4f mA  shunt %.4f ohm\n",
              shunt_cal, entry->current_lsb * 1000.0, entry->shunt_ohms);

    report_health(diag, shunt_cal);
    return 0;
}

int cmd_ina237_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    if (argc > 1) {
        int address = 0;
        if (parse_num_arg(argv[1], &address) < 0 ||
            address < INA237_ADDR_FIRST || address > INA237_ADDR_LAST) {
            bp_error("Address must be 0x%02X-0x%02X (set by the A0/A1 pins)",
                     INA237_ADDR_FIRST, INA237_ADDR_LAST);
            return -1;
        }

        ina237_device_t *entry = find_device((uint8_t)address);
        if (!entry) {
            /* Reading an address nobody configured is the common quick path;
             * register it at the default shunt rather than refusing. */
            bp_printf("0x%02X is not configured; using the default %.3f ohm shunt.\n",
                      address, DEFAULT_SHUNT_OHMS);
            if (configure_device((uint8_t)address, DEFAULT_SHUNT_OHMS, true) < 0) {
                return -1;
            }
            entry = find_device((uint8_t)address);
        }

        return read_device(entry);
    }

    if (configured_count() == 0) {
        bp_error("No INA237s configured. Run 'ina237 config <address> [ohms]', "
                 "or 'ina237 read <address>' to use the default shunt.");
        return -1;
    }

    int failures = 0;
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (devices[i].used && read_device(&devices[i]) < 0) {
            failures++;
        }
    }

    return failures ? -1 : 0;
}

int cmd_ina237_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (configured_count() == 0) {
        bp_printf("No INA237s configured.\n");
        return 0;
    }

    bp_printf("%-8s %12s %14s %14s\n", "ADDRESS", "SHUNT (ohm)", "CURRENT_LSB", "FULL SCALE");
    for (size_t i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].used) {
            continue;
        }
        bp_printf("0x%02X     %12.4f %11.4f mA %11.2f A\n",
                  devices[i].address,
                  devices[i].shunt_ohms,
                  devices[i].current_lsb * 1000.0,
                  SHUNT_FULL_SCALE_V / devices[i].shunt_ohms);
    }

    return 0;
}
