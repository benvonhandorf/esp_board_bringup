/*
 * Sensirion SHT4x humidity and temperature sensor.
 *
 * Command bytes, timings, CRC parameters and conversion formulas come from the
 * SHT4x datasheet, version 7.1 (March 2025).
 *
 * The bus protocol differs from a conventional register device: there is no
 * register pointer. A command byte is written as its own transaction, the
 * sensor is then given time to measure, and the result is read back in a
 * *separate* transaction. Issuing the read too early -- as a repeated-start
 * write-then-read does -- makes the sensor NACK the read header (datasheet
 * Table 8), so i2c_master_transmit_receive() cannot be used here.
 */
#include "esp_bringup.h"
#include "output.h"
#include "sht4x.h"
#include "i2c.h"

/* Table 8, Overview of I2C commands. */
#define CMD_MEASURE_HIGH  0xFD
#define CMD_MEASURE_MED   0xF6
#define CMD_MEASURE_LOW   0xE0
#define CMD_READ_SERIAL   0x89
#define CMD_SOFT_RESET    0x94

/* Heater commands: each runs the heater then takes a high-precision reading
 * just before switching it off. */
#define CMD_HEATER_200MW_1S    0x39
#define CMD_HEATER_200MW_100MS 0x32
#define CMD_HEATER_110MW_1S    0x2F
#define CMD_HEATER_110MW_100MS 0x24
#define CMD_HEATER_20MW_1S     0x1E
#define CMD_HEATER_20MW_100MS  0x15

/*
 * Table 5, System timing: the measurement takes at most 8.3 ms even at high
 * repeatability, so one value covers all three modes. See wait_ms() for why
 * this cannot simply be handed to pdMS_TO_TICKS().
 */
#define MEASURE_WAIT_MS 10
#define SERIAL_WAIT_MS  10
#define RESET_WAIT_MS   10

/* Heater pulses are 1.1 s / 0.11 s max, plus the measurement that follows. */
#define HEATER_LONG_WAIT_MS  1200
#define HEATER_SHORT_WAIT_MS 200

/* Part variants differ only in their fixed address: A=0x44, B=0x45, C=0x46. */
#define SHT4X_ADDR_FIRST   0x44
#define SHT4X_ADDR_LAST    0x46
#define SHT4X_ADDR_DEFAULT 0x44

/* Every reply is two 16-bit words, each followed by its own CRC. */
#define RESPONSE_LEN 6

#define XFER_TIMEOUT_MS 1000

/* Table 7: CRC-8, polynomial 0x31, initialised to 0xFF, no reflection,
 * no final XOR. The datasheet's worked example is CRC(0xBEEF) = 0x92. */
static uint8_t crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }

    return crc;
}

static void wait_ms(uint32_t ms)
{
    /*
     * vTaskDelay(n) guarantees only (n-1) full tick periods: the first tick
     * boundary can arrive immediately, so vTaskDelay(1) may return almost at
     * once. With the 10 ms tick used here that let the read overtake an 8.3 ms
     * measurement, and the sensor NACKed the read header. The extra tick makes
     * the delay at least the requested time.
     */
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

/*
 * Write a command, wait, and optionally read the reply.
 *
 * Pass wait_time_ms = 0 and length = 0 for commands with no response, such as
 * the soft reset.
 */
static esp_err_t send_command(uint8_t address, uint8_t command,
                              uint32_t wait_time_ms, uint8_t *buffer, size_t length)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(address, &dev);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_master_transmit(dev, &command, 1, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    wait_ms(wait_time_ms);

    if (length == 0) {
        return ESP_OK;
    }

    return i2c_master_receive(dev, buffer, length, XFER_TIMEOUT_MS);
}

/* Validate both CRCs and return the two data words. */
static bool decode_response(const uint8_t *raw, uint8_t address,
                            uint16_t *first, uint16_t *second)
{
    if (crc8(&raw[0], 2) != raw[2]) {
        bp_error("0x%02X: CRC error in the first word (got 0x%02X, computed 0x%02X)",
                 address, raw[2], crc8(&raw[0], 2));
        return false;
    }
    if (crc8(&raw[3], 2) != raw[5]) {
        bp_error("0x%02X: CRC error in the second word (got 0x%02X, computed 0x%02X)",
                 address, raw[5], crc8(&raw[3], 2));
        return false;
    }

    *first = (uint16_t)((raw[0] << 8) | raw[1]);
    *second = (uint16_t)((raw[3] << 8) | raw[4]);
    return true;
}

/*
 * Parse an optional leading address argument.
 *
 * Returns the index of the first argument that is not the address, so callers
 * accept "read", "read 0x45" and "read low" alike.
 *
 * `required_after` is how many arguments the command still needs once the
 * address has been taken. It is what separates "heater 200 1000" (no address,
 * 200 is the power) from "heater 0x45 200 1000" -- without it, a leading
 * numeric argument is indistinguishable from an omitted address.
 */
static int take_address(int argc, char **argv, int required_after,
                        uint8_t *address, bool *ok)
{
    *address = SHT4X_ADDR_DEFAULT;
    *ok = true;

    if (argc < 2) {
        return 1;
    }

    /* Too few arguments for argv[1] to be an address AND leave the command its
     * required operands, so it must be an operand itself. */
    if (argc - 2 < required_after) {
        return 1;
    }

    int value = 0;
    if (parse_num_arg(argv[1], &value) < 0) {
        return 1; /* not a number; leave it for the caller to interpret */
    }

    if (value < SHT4X_ADDR_FIRST || value > SHT4X_ADDR_LAST) {
        bp_error("Address must be 0x%02X-0x%02X (fixed by the part variant: "
                 "A=0x44, B=0x45, C=0x46)", SHT4X_ADDR_FIRST, SHT4X_ADDR_LAST);
        *ok = false;
        return 1;
    }

    *address = (uint8_t)value;
    return 2;
}

/* Datasheet section 4.6, equations 1 and 2. */
static double ticks_to_celsius(uint16_t ticks)
{
    return -45.0 + 175.0 * ticks / 65535.0;
}

static double ticks_to_humidity(uint16_t ticks)
{
    return -6.0 + 125.0 * ticks / 65535.0;
}

/* Shared by `read` and `heater`, both of which return a measurement. */
static int report_measurement(uint8_t address, uint8_t command, uint32_t wait_time_ms,
                              const char *description)
{
    uint8_t raw[RESPONSE_LEN];
    esp_err_t err = send_command(address, command, wait_time_ms, raw, sizeof(raw));
    if (err != ESP_OK) {
        bp_error("Reading 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    uint16_t temperature_ticks = 0;
    uint16_t humidity_ticks = 0;
    if (!decode_response(raw, address, &temperature_ticks, &humidity_ticks)) {
        return -1;
    }

    double celsius = ticks_to_celsius(temperature_ticks);
    double humidity = ticks_to_humidity(humidity_ticks);

    /* The conversion can yield non-physical values just outside 0-100 %RH.
     * The datasheet expects those to be cropped, but say so when it happens:
     * during bringup a wildly out-of-range value means a real problem. */
    double reported = humidity;
    if (reported > 100.0) {
        reported = 100.0;
    } else if (reported < 0.0) {
        reported = 0.0;
    }

    bp_printf("0x%02X  Temp %7.2f C (%7.2f F)  Humidity %6.2f %%RH\n",
              address, celsius, celsius * 9.0 / 5.0 + 32.0, reported);
    bp_printf("      raw T 0x%04X  RH 0x%04X  (%s)\n",
              temperature_ticks, humidity_ticks, description);

    if (reported != humidity) {
        bp_printf("      Note: uncropped humidity was %.2f %%RH, cropped to the "
                  "physical 0-100 range\n", humidity);
    }

    return 0;
}

int cmd_sht4x_read(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    int index = take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    uint8_t command = CMD_MEASURE_HIGH;
    const char *description = "high repeatability";

    if (index < argc) {
        const char *mode = argv[index];
        if (strcasecmp(mode, "high") == 0) {
            command = CMD_MEASURE_HIGH;
            description = "high repeatability";
        } else if (strcasecmp(mode, "medium") == 0 || strcasecmp(mode, "med") == 0) {
            command = CMD_MEASURE_MED;
            description = "medium repeatability";
        } else if (strcasecmp(mode, "low") == 0) {
            command = CMD_MEASURE_LOW;
            description = "low repeatability";
        } else {
            bp_error("Repeatability must be high, medium or low (got '%s')", mode);
            return -1;
        }
    }

    return report_measurement(address, command, MEASURE_WAIT_MS, description);
}

int cmd_sht4x_serial(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    uint8_t raw[RESPONSE_LEN];
    esp_err_t err = send_command(address, CMD_READ_SERIAL, SERIAL_WAIT_MS,
                                 raw, sizeof(raw));
    if (err != ESP_OK) {
        bp_error("No response from 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    uint16_t high = 0;
    uint16_t low = 0;
    if (!decode_response(raw, address, &high, &low)) {
        return -1;
    }

    /* The SHT4x has no ID register; a serial number that reads back with valid
     * CRCs is the available evidence that a real sensor is present. */
    bp_printf("0x%02X  Serial number 0x%04X%04X\n", address, high, low);
    return 0;
}

int cmd_sht4x_heater(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    int index = take_address(argc, argv, 2, &address, &ok);
    if (!ok) {
        return -1;
    }

    if (argc - index < 2) {
        bp_printf("Usage: heater [address] <power_mw> <duration_ms>\n");
        bp_printf("Power is 20, 110 or 200 mW; duration is 100 or 1000 ms.\n");
        return -1;
    }

    int power = 0;
    int duration = 0;
    if (parse_int_arg(argv[index], &power) < 0 ||
        parse_int_arg(argv[index + 1], &duration) < 0) {
        bp_error("Power and duration must be numbers");
        return -1;
    }

    /* Only the six combinations the device implements are available. */
    uint8_t command;
    if (duration == 1000) {
        switch (power) {
        case 200: command = CMD_HEATER_200MW_1S; break;
        case 110: command = CMD_HEATER_110MW_1S; break;
        case 20:  command = CMD_HEATER_20MW_1S; break;
        default:  power = -1; command = 0; break;
        }
    } else if (duration == 100) {
        switch (power) {
        case 200: command = CMD_HEATER_200MW_100MS; break;
        case 110: command = CMD_HEATER_110MW_100MS; break;
        case 20:  command = CMD_HEATER_20MW_100MS; break;
        default:  power = -1; command = 0; break;
        }
    } else {
        bp_error("Duration must be 100 or 1000 ms");
        return -1;
    }

    if (power < 0) {
        bp_error("Power must be 20, 110 or 200 mW");
        return -1;
    }

    bp_printf("Running the heater at %d mW for %d ms; the sensor measures once "
              "just before it switches off.\n", power, duration);

    uint32_t wait_time = (duration == 1000) ? HEATER_LONG_WAIT_MS : HEATER_SHORT_WAIT_MS;
    return report_measurement(address, command, wait_time, "after heating");
}

int cmd_sht4x_reset(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    bool ok = false;
    uint8_t address = 0;
    take_address(argc, argv, 0, &address, &ok);
    if (!ok) {
        return -1;
    }

    esp_err_t err = send_command(address, CMD_SOFT_RESET, RESET_WAIT_MS, NULL, 0);
    if (err != ESP_OK) {
        bp_error("Resetting 0x%02X: %s", address, esp_err_to_name(err));
        return -1;
    }

    bp_printf("0x%02X soft reset\n", address);
    return 0;
}
