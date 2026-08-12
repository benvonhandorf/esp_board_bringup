#include "esp_bringup.h"
#include "output.h"
#include "gpio.h"

#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_rom_sys.h"
#include "soc/io_mux_reg.h"
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

/*
 * Which internal pull, if any, to hold an input at while reading it.
 *
 * Without one a read cannot tell a pin that is driven low from a pin that is
 * not connected to anything: both tend to read 0. Enabling the pull-up makes
 * that distinction, which is the difference between "this net is shorted to
 * ground" and "this net is floating" during bringup.
 */
typedef enum {
    PULL_NONE,
    PULL_UP,
    PULL_DOWN,
} pin_pull_t;

static esp_err_t configure_pin_pull(int pin, gpio_mode_t mode, pin_pull_t pull)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode,
        .pull_up_en = (pull == PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}

static esp_err_t configure_pin(int pin, gpio_mode_t mode)
{
    return configure_pin_pull(pin, mode, PULL_NONE);
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
        bp_printf("Usage: read <pin> [up|down|none]\n");
        return -1;
    }

    /*
     * The optional pull defaults to none, which is what a bare `read` has always
     * done. `up` is the useful one during bringup: a pin that still reads 0 with
     * the internal pull-up fighting it is genuinely being held low, whereas a
     * floating pin goes high.
     */
    pin_pull_t pull = PULL_NONE;
    if (argc > 2) {
        if (strcasecmp(argv[2], "up") == 0) {
            pull = PULL_UP;
        } else if (strcasecmp(argv[2], "down") == 0) {
            pull = PULL_DOWN;
        } else if (strcasecmp(argv[2], "none") == 0) {
            pull = PULL_NONE;
        } else {
            bp_error("Pull must be 'up', 'down' or 'none', not '%s'", argv[2]);
            return -1;
        }
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

        esp_err_t err = configure_pin_pull(pin, GPIO_MODE_INPUT, pull);
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

/* ------------------------------------------------------------------ */
/* Short detection                                                     */
/* ------------------------------------------------------------------ */

/*
 * Settling time after changing a pin before reading its neighbours.
 *
 * The observers are held by the internal pull-up, which is weak -- tens of
 * kiloohms against whatever capacitance the board has. 45k into 100pF is a few
 * microseconds, so 200us is generous without making a 64-pin scan slow.
 */
#define SHORT_SETTLE_US 200

/*
 * Pins the scan must never drive.
 *
 * Driving a flash or PSRAM pin hangs the chip instantly, and driving the console
 * pins cuts off the very connection the results are printed to. ESP-IDF already
 * tracks the first group: spi_flash and esp_psram reserve their pins at startup,
 * so esp_gpio_is_reserved() catches them along with any peripheral currently
 * holding a pin. The console pins it does not track, so they are named here.
 */
static bool pin_is_drivable(int pin, const char **why)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        *why = "input-only";
        return false;
    }
    if (esp_gpio_is_reserved(BIT64(pin))) {
        *why = "reserved for flash, PSRAM or a peripheral in use";
        return false;
    }
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && defined(USB_INT_PHY0_DM_GPIO_NUM)
    if (pin == USB_INT_PHY0_DM_GPIO_NUM || pin == USB_INT_PHY0_DP_GPIO_NUM) {
        *why = "USB console";
        return false;
    }
#endif
#ifdef CONFIG_ESP_CONSOLE_UART_TX_GPIO
    if (pin == CONFIG_ESP_CONSOLE_UART_TX_GPIO || pin == CONFIG_ESP_CONSOLE_UART_RX_GPIO) {
        *why = "console UART";
        return false;
    }
#endif
    return true;
}

/* Park every pin in the set as a pulled-up input: the state observers need. */
static void park_as_observers(const int *pins, int count)
{
    for (int i = 0; i < count; i++) {
        configure_pin_pull(pins[i], GPIO_MODE_INPUT, PULL_UP);
    }
}

/*
 * Drive pins[driven] low and record which of the others follow it down.
 * `mask` gets a bit set per observer index that read low.
 */
static void probe_from(const int *pins, int count, int driven, uint64_t *mask)
{
    configure_pin_pull(pins[driven], GPIO_MODE_OUTPUT, PULL_NONE);
    gpio_set_level(pins[driven], 0);
    esp_rom_delay_us(SHORT_SETTLE_US);

    for (int i = 0; i < count; i++) {
        if (i != driven && gpio_get_level(pins[i]) == 0) {
            *mask |= BIT64(i);
        }
    }

    /* Back to an observer before moving on, so only one pin is ever driven. */
    configure_pin_pull(pins[driven], GPIO_MODE_INPUT, PULL_UP);
    esp_rom_delay_us(SHORT_SETTLE_US);
}

int cmd_gpio_short(int argc, char **argv)
{
    if (argc < 2) {
        bp_printf("Usage: short <pin>\n");
        return -1;
    }

    int *requested = NULL;
    int requested_count = 0;
    if (parse_pin_list(argv[1], &requested, &requested_count) < 0) {
        bp_error("Invalid pin specification: %s", argv[1]);
        return -1;
    }

    /* Drop anything unsafe to drive, saying which and why rather than
     * silently testing a smaller set than was asked for. */
    int *pins = calloc((size_t)requested_count, sizeof(int));
    if (!pins) {
        bp_error("Out of memory");
        free(requested);
        return -1;
    }
    int count = 0;
    for (int i = 0; i < requested_count; i++) {
        const char *why = NULL;
        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            duplicate = duplicate || (pins[j] == requested[i]);
        }

        if (duplicate) {
            /* Without this a repeated pin is compared against itself, follows
             * itself down, and gets reported as shorted to itself. */
            bp_printf("Skipping GPIO %d: listed more than once\n", requested[i]);
        } else if (!GPIO_IS_VALID_GPIO(requested[i])) {
            bp_printf("Skipping GPIO %d: does not exist on this chip\n", requested[i]);
        } else if (!pin_is_drivable(requested[i], &why)) {
            bp_printf("Skipping GPIO %d: %s\n", requested[i], why);
        } else {
            pins[count++] = requested[i];
        }
    }
    free(requested);

    if (count < 2) {
        bp_error("Need at least two testable pins; got %d", count);
        free(pins);
        return -1;
    }

    uint64_t *observed = calloc((size_t)count, sizeof(uint64_t));
    if (!observed) {
        bp_error("Out of memory");
        free(pins);
        return -1;
    }

    bp_printf("Short scan across %d pins. Each is driven low in turn while the others\n"
              "are read with pull-ups, so a pin that follows shares a net with it.\n"
              "A short is only reported when both pins pull each other down.\n", count);
    bp_printf("Note: this briefly drives every pin listed. Do not run it on a bus\n"
              "      another device may be driving at the same time.\n\n");

    park_as_observers(pins, count);

    /*
     * A pin already sitting low fights nothing and follows everything, so it
     * would otherwise be reported as shorted to every other pin. Find those
     * first and leave them out of the pairing.
     */
    uint64_t stuck_low = 0;
    esp_rom_delay_us(SHORT_SETTLE_US);
    for (int i = 0; i < count; i++) {
        if (gpio_get_level(pins[i]) == 0) {
            stuck_low |= BIT64(i);
        }
    }

    for (int i = 0; i < count; i++) {
        if (!(stuck_low & BIT64(i))) {
            probe_from(pins, count, i, &observed[i]);
        }
    }

    /* A real short is symmetric, and neither end may be a stuck-low pin. */
    #define SHORTED(a, b) (!(stuck_low & BIT64(a)) && !(stuck_low & BIT64(b)) && \
                           (observed[a] & BIT64(b)) && (observed[b] & BIT64(a)))

    /*
     * Adjacent pins first. A solder bridge at the package is by far the most
     * common way two nets get tied together, and neighbouring GPIO numbers are
     * usually neighbouring pads -- so this is the answer worth seeing first.
     */
    int adjacent_pairs = 0, adjacent_shorts = 0;
    bp_printf("Adjacent pins (where a solder bridge usually lands):\n");
    for (int a = 0; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
            if (pins[b] - pins[a] != 1) {
                continue;
            }
            adjacent_pairs++;
            if (SHORTED(a, b)) {
                bp_printf("  GPIO %d <-> GPIO %d   SHORTED\n", pins[a], pins[b]);
                adjacent_shorts++;
            }
        }
    }
    if (adjacent_pairs == 0) {
        bp_printf("  no consecutively numbered pins in the set\n");
    } else if (adjacent_shorts == 0) {
        bp_printf("  %d pair%s tested, all clear\n",
                  adjacent_pairs, adjacent_pairs == 1 ? "" : "s");
    }

    int other_pairs = 0, other_shorts = 0;
    bp_printf("Remaining pairs:\n");
    for (int a = 0; a < count; a++) {
        for (int b = a + 1; b < count; b++) {
            if (pins[b] - pins[a] == 1) {
                continue;
            }
            other_pairs++;
            if (SHORTED(a, b)) {
                bp_printf("  GPIO %d <-> GPIO %d   SHORTED\n", pins[a], pins[b]);
                other_shorts++;
            }
        }
    }
    if (other_pairs == 0) {
        bp_printf("  none to test\n");
    } else if (other_shorts == 0) {
        bp_printf("  %d pair%s tested, all clear\n", other_pairs, other_pairs == 1 ? "" : "s");
    }

    if (stuck_low) {
        bp_printf("Held low whatever is driven, so not testable and not a short:\n ");
        for (int i = 0; i < count; i++) {
            if (stuck_low & BIT64(i)) {
                bp_printf(" GPIO %d", pins[i]);
            }
        }
        bp_printf("\n  A grounded net, a card-detect switch or an output driving low.\n");
    }

    #undef SHORTED

    /* Leave the pins passive rather than holding pull-ups on someone's bus. */
    for (int i = 0; i < count; i++) {
        configure_pin_pull(pins[i], GPIO_MODE_INPUT, PULL_NONE);
    }

    const int total_shorts = adjacent_shorts + other_shorts;
    bp_printf("\n%d short%s found.\n", total_shorts, total_shorts == 1 ? "" : "s");

    free(observed);
    free(pins);
    return total_shorts ? -1 : 0;
}
