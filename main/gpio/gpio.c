#include "esp_bringup.h"
#include "output.h"
#include "gpio.h"

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

#define DEFAULT_BLINK_PERIOD_MS 500
#define MAX_BLINK_COUNT 1000

/* One handle per ADC unit, created on first use. Channels are configured
 * lazily as pins are requested; configuring channel 0 once and then reading
 * whichever channel a pin maps to (as the previous version did) returns
 * garbage for every pin except the one on channel 0. */
static adc_oneshot_unit_handle_t adc_units[SOC_ADC_PERIPH_NUM];
static adc_cali_handle_t adc_cali[SOC_ADC_PERIPH_NUM];
static uint32_t adc_configured_channels[SOC_ADC_PERIPH_NUM];

static bool check_pin(int pin, bool need_output)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        bp_error("GPIO %d does not exist on this chip", pin);
        return false;
    }
    if (need_output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        bp_error("GPIO %d is input-only on this chip", pin);
        return false;
    }
    return true;
}

static esp_err_t configure_pin(int pin, gpio_mode_t mode)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

int cmd_gpio_set(int argc, char **argv)
{
    if (argc < 3) {
        bp_printf("Usage: set <pin> <state>\n");
        return -1;
    }

    int state;
    if (strcasecmp(argv[2], "true") == 0 || strcmp(argv[2], "1") == 0 ||
        strcasecmp(argv[2], "high") == 0) {
        state = 1;
    } else if (strcasecmp(argv[2], "false") == 0 || strcmp(argv[2], "0") == 0 ||
               strcasecmp(argv[2], "low") == 0) {
        state = 0;
    } else {
        bp_error("Invalid state '%s'. Use true/false, 1/0 or high/low", argv[2]);
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (parse_pin_list(argv[1], &pins, &count) < 0) {
        bp_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        if (!check_pin(pin, true)) {
            failures++;
            continue;
        }

        esp_err_t err = configure_pin(pin, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            bp_error("Configuring GPIO %d: %s", pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        gpio_set_level(pin, state);
        bp_printf("%d: %s\n", pin, state ? "high" : "low");
    }

    free(pins);
    return failures ? -1 : 0;
}

int cmd_gpio_read(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: read <pin>\n");
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (parse_pin_list(argv[1], &pins, &count) < 0) {
        bp_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        if (!check_pin(pin, false)) {
            failures++;
            continue;
        }

        esp_err_t err = configure_pin(pin, GPIO_MODE_INPUT);
        if (err != ESP_OK) {
            bp_error("Configuring GPIO %d: %s", pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        /* README sample output: "<pin>: <level>", one pin per row. */
        bp_printf("%d: %d\n", pin, gpio_get_level(pin));
    }

    free(pins);
    return failures ? -1 : 0;
}

/* Bring up the unit handle, the channel, and (if available) calibration. */
static esp_err_t prepare_adc(adc_unit_t unit, adc_channel_t channel)
{
    if (unit >= SOC_ADC_PERIPH_NUM) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!adc_units[unit]) {
        const adc_oneshot_unit_init_cfg_t init_config = {.unit_id = unit};
        esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_units[unit]);
        if (err != ESP_OK) {
            adc_units[unit] = NULL;
            return err;
        }
    }

    if (!(adc_configured_channels[unit] & BIT(channel))) {
        const adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12, /* widest input range: roughly 0-3.1V */
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        esp_err_t err = adc_oneshot_config_channel(adc_units[unit], channel, &config);
        if (err != ESP_OK) {
            return err;
        }
        adc_configured_channels[unit] |= BIT(channel);
    }

    /* Calibration turns raw counts into millivolts. Not every chip ships with
     * the required eFuse data, so a failure here is not fatal. */
    if (!adc_cali[unit]) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        const adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali[unit]);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        const adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali[unit]);
#endif
    }

    return ESP_OK;
}

int cmd_gpio_aread(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: aread <pin>\n");
        return -1;
    }

    int *pins = NULL;
    int count = 0;
    if (parse_pin_list(argv[1], &pins, &count) < 0) {
        bp_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    int failures = 0;
    for (int i = 0; i < count; i++) {
        int pin = pins[i];

        adc_unit_t unit;
        adc_channel_t channel;
        if (adc_oneshot_io_to_channel(pin, &unit, &channel) != ESP_OK) {
            bp_error("GPIO %d is not an ADC-capable pin", pin);
            failures++;
            continue;
        }

        esp_err_t err = prepare_adc(unit, channel);
        if (err != ESP_OK) {
            bp_error("Preparing ADC%d channel %d for GPIO %d: %s",
                     unit + 1, channel, pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        int raw = 0;
        err = adc_oneshot_read(adc_units[unit], channel, &raw);
        if (err != ESP_OK) {
            bp_error("Reading GPIO %d: %s", pin, esp_err_to_name(err));
            failures++;
            continue;
        }

        int millivolts = 0;
        if (adc_cali[unit] &&
            adc_cali_raw_to_voltage(adc_cali[unit], raw, &millivolts) == ESP_OK) {
            bp_printf("%d: %d raw, %d mV (ADC%d channel %d)\n",
                      pin, raw, millivolts, unit + 1, channel);
        } else {
            bp_printf("%d: %d raw (ADC%d channel %d, uncalibrated)\n",
                      pin, raw, unit + 1, channel);
        }
    }

    free(pins);
    return failures ? -1 : 0;
}

int cmd_gpio_blink(int argc, char **argv)
{
    if (argc < 3) {
        bp_printf("Usage: blink <pin> <count> [period_ms]\n");
        return -1;
    }

    int count;
    if (parse_int_arg(argv[2], &count) < 0 || count < 1 || count > MAX_BLINK_COUNT) {
        bp_error("Count must be 1-%d", MAX_BLINK_COUNT);
        return -1;
    }

    int period = DEFAULT_BLINK_PERIOD_MS;
    if (argc > 3 && (parse_int_arg(argv[3], &period) < 0 || period < 10 || period > 10000)) {
        bp_error("Period must be 10-10000 ms");
        return -1;
    }

    int *pins = NULL;
    int pin_count = 0;
    if (parse_pin_list(argv[1], &pins, &pin_count) < 0) {
        bp_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    /* Configure everything up front so a bad pin fails before any blinking. */
    for (int i = 0; i < pin_count; i++) {
        if (!check_pin(pins[i], true)) {
            free(pins);
            return -1;
        }
        esp_err_t err = configure_pin(pins[i], GPIO_MODE_OUTPUT);
        if (err != ESP_OK) {
            bp_error("Configuring GPIO %d: %s", pins[i], esp_err_to_name(err));
            free(pins);
            return -1;
        }
    }

    bp_printf("Blinking %d pin(s) %d times at %d ms...\n", pin_count, count, period);

    for (int i = 0; i < count; i++) {
        for (int level = 1; level >= 0; level--) {
            for (int p = 0; p < pin_count; p++) {
                gpio_set_level(pins[p], level);
            }
            /* vTaskDelay yields, so the rest of the system keeps running. */
            vTaskDelay(pdMS_TO_TICKS(period / 2));
        }
    }

    bp_printf("Blink complete\n");
    free(pins);
    return 0;
}
