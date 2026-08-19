/*
 * Nuvoton NAU7802 24-bit bridge ADC, used here to drive a load cell.
 *
 * Register addresses, bit positions and encodings come from the NAU7802 data
 * sheet, revision 1.5. The device address 0x2A is fixed in silicon -- there are
 * no address pins.
 *
 * Unlike the SHT4x this is a conventional register device, and unlike the
 * INA237 it auto-increments on a burst read, so the three ADC result bytes can
 * be fetched in one transaction.
 */
#include "esp_bringup.h"
#include "output.h"
#include "nau7802.h"
#include "i2c.h"

#include "driver/gpio.h"
#include "freertos/semphr.h"

#define NAU7802_ADDRESS 0x2A

/* Section 10, Summary Device Register Map. */
#define REG_PU_CTRL    0x00
#define REG_CTRL1      0x01
#define REG_CTRL2      0x02
#define REG_OCAL1_B2   0x03
#define REG_GCAL1_B3   0x06
#define REG_ADCO_B2    0x12

#define REG_PGA        0x1B
#define REG_POWER_CTRL 0x1C

#define REG_DEVICE_REV 0x1F

/*
 * REG0x1B[6] LDOMODE picks the compensation for the internal regulator's
 * control loop, and it has to match the capacitor the board actually fits on
 * AVDD:
 *
 *   0 (default) - ESR below 1 ohm. Better DC accuracy, higher loop gain.
 *   1           - ESR up to 5 ohms. More stable loop, lower DC gain.
 *
 * Pin 16's own description in the data sheet asks for "low ESR 1 ohm or less"
 * because that is what the default expects. A board that fits something with
 * more ESR than that and leaves this bit alone runs a marginally compensated
 * regulator, which is a broadband noise source sitting on the supply and the
 * reference -- after the PGA, so no amount of gain or input wiring changes it.
 */
#define PGA_LDOMODE BIT(6)

/*
 * REG0x1C[7] PGA_CAP_EN connects a filter capacitor across the VIN2P/VIN2N
 * pins to the PGA output, which the data sheet offers "for enhanced ENOB at
 * high PGA gain settings". It needs the capacitor to be physically fitted --
 * 330 pF at AVDD 3.3 V, 680 pF at 4.5 V -- and it consumes channel 2, whose
 * pins become the filter node.
 */
#define POWER_PGA_CAP_EN BIT(7)

/*
 * REG0x15 (chopper clock, ADC common mode) is deliberately left at its
 * power-up value.
 *
 * The datasheet marks every REG_CHPS encoding except '11' as Reserved, which
 * suggests the '00' the device powers up with should be corrected. Measured on
 * hardware, the opposite is true: the default gives a rock-steady zero at
 * 10-80 SPS, while writing 0x30 rails the converter at negative full scale on
 * every rate. Writing it read-modify-write is worse still, because REG0x15
 * shares its read path with OTP[32:24] (selected by REG0x1B[7]), so the
 * read-back is not necessarily the register being written.
 *
 * Trusting the silicon over a preliminary datasheet table here.
 */
#define REG_ADC_CTRL 0x15 /* documented above; intentionally not written */

/* REG0x00 PU_CTRL. CR and PUR are read-only status. */
#define PU_CTRL_RR    BIT(0) /* register reset, level triggered */
#define PU_CTRL_PUD   BIT(1) /* power up digital */
#define PU_CTRL_PUA   BIT(2) /* power up analog */
#define PU_CTRL_PUR   BIT(3) /* power up ready (RO) */
#define PU_CTRL_CS    BIT(4) /* cycle start */
#define PU_CTRL_CR    BIT(5) /* cycle ready: ADC data available (RO) */
#define PU_CTRL_OSCS  BIT(6)
#define PU_CTRL_AVDDS BIT(7) /* 1 = internal LDO, 0 = AVDD pin */

/* REG0x01 CTRL1: GAINS[2:0], VLDO[5:3]. */
#define CTRL1_GAINS_SHIFT 0
#define CTRL1_GAINS_MASK  0x07
#define CTRL1_VLDO_SHIFT  3
#define CTRL1_VLDO_MASK   0x38

/* REG0x02 CTRL2: CALMOD[1:0], CALS, CAL_ERR, CRS[6:4], CHS. */
#define CTRL2_CALMOD_MASK  0x03
#define CTRL2_CALS         BIT(2)
#define CTRL2_CAL_ERR      BIT(3)
#define CTRL2_CRS_SHIFT    4
#define CTRL2_CRS_MASK     0x70
/*
 * CHS is bit 7, the top of the register -- bit 0 is CALMOD[0]. Getting this
 * wrong does not fail loudly: writing bit 0 selects a calibration mode instead
 * of a channel, and run_calibration() then clears CALMOD back to 00, so the
 * channel silently never changes and every reading comes from channel 1 no
 * matter what `input` was told. `status` reading the same wrong bit back
 * agrees with itself and reports channel A throughout.
 */
#define CTRL2_CHS          BIT(7)

/* CALMOD 00 = internal offset calibration, the one to run at power-up. */
#define CALMOD_OFFSET_INTERNAL 0x00

/* The result is 24-bit two's complement: section 9.2 gives full scale as
 * +/-0.5 x (REFP - REFN) / gain, so the range is bipolar. */
#define ADC_FULL_SCALE 8388608.0 /* 2^23 */

#define XFER_TIMEOUT_MS 1000

/* Worst case is 10 SPS, i.e. 100 ms per conversion; allow generous margin so a
 * misconfigured rate reports a timeout rather than hanging. */
#define CONVERSION_TIMEOUT_MS 1000
#define CALIBRATION_TIMEOUT_MS 2000

#define MAX_SAMPLES 200
#define DEFAULT_SAMPLES 10

/* GAINS[2:0] encodings, index = register value. */
static const int gain_values[8] = {1, 2, 4, 8, 16, 32, 64, 128};

/* CRS[2:0] encodings. Only these five are defined; 100-110 are unused. */
static const int rate_values[8] = {10, 20, 40, 80, -1, -1, -1, 320};

/* VLDO[2:0] encodings, in millivolts. */
static const int ldo_millivolts[8] = {4500, 4200, 3900, 3600, 3300, 3000, 2700, 2400};

static bool initialized;
static int32_t tare_offset;      /* raw counts with no load */
static double counts_per_unit;   /* scale factor from `calibrate` */
static bool calibrated;

/*
 * DRDY. The device raises this pin when a conversion lands and drops it when
 * the result registers are read, which is the same information as PU_CTRL.CR
 * but available without an I2C transaction and without polling latency.
 *
 * That latency is the point. The CR poll below runs on the FreeRTOS tick, so it
 * can return anywhere in a 10-20 ms window after the conversion actually
 * completed; at 40 SPS and above that is a whole conversion period or more, and
 * the burst read of the result then starts at an unknown phase -- sometimes
 * right as the device overwrites the registers, which yields a result stitched
 * together from two conversions. Waiting on the pin instead starts the read
 * microseconds after the registers were written, with most of a conversion
 * period of margin before the next write.
 *
 * -1 means no pin is wired up (or none has been declared), in which case the
 * driver falls back to polling CR exactly as before.
 */
static int drdy_pin = -1;
static SemaphoreHandle_t drdy_signal;
static bool isr_service_ready;

static esp_err_t read_regs(uint8_t reg, uint8_t *buffer, size_t len)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(NAU7802_ADDRESS, &dev);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_transmit_receive(dev, &reg, 1, buffer, len, XFER_TIMEOUT_MS);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *value)
{
    return read_regs(reg, value, 1);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_device_handle(NAU7802_ADDRESS, &dev);
    if (err != ESP_OK) {
        return err;
    }

    const uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(dev, payload, sizeof(payload), XFER_TIMEOUT_MS);
}

static esp_err_t update_reg(uint8_t reg, uint8_t clear_mask, uint8_t set_mask)
{
    uint8_t value = 0;
    esp_err_t err = read_reg(reg, &value);
    if (err != ESP_OK) {
        return err;
    }

    value = (uint8_t)((value & ~clear_mask) | set_mask);
    return write_reg(reg, value);
}

static bool require_init(void)
{
    if (!initialized) {
        bp_error("NAU7802 not initialized. Run 'nau7802 init' first.");
        return false;
    }
    return true;
}

/* Wait for a PU_CTRL status bit, polling on the FreeRTOS tick. */
static esp_err_t wait_for_bit(uint8_t reg, uint8_t mask, bool set, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited <= timeout_ms; waited += 10) {
        uint8_t value = 0;
        esp_err_t err = read_reg(reg, &value);
        if (err != ESP_OK) {
            return err;
        }
        if (((value & mask) != 0) == set) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    return ESP_ERR_TIMEOUT;
}

static void IRAM_ATTR drdy_isr(void *arg)
{
    (void)arg;

    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(drdy_signal, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Stop using the pin and hand it back in its reset state. */
static void release_drdy(void)
{
    if (drdy_pin < 0) {
        return;
    }

    gpio_isr_handler_remove(drdy_pin);
    gpio_set_intr_type(drdy_pin, GPIO_INTR_DISABLE);
    gpio_reset_pin(drdy_pin);
    drdy_pin = -1;
}

static esp_err_t configure_drdy(int pin)
{
    if (!GPIO_IS_VALID_GPIO(pin)) {
        bp_error("GPIO %d does not exist on this chip", pin);
        return ESP_ERR_INVALID_ARG;
    }

    if (drdy_signal == NULL) {
        drdy_signal = xSemaphoreCreateBinary();
        if (drdy_signal == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    release_drdy();

    /*
     * The device drives DRDY push-pull, so the pull-down is not there to hold
     * the line. It is there for the pin that turns out not to be connected to
     * it: a floating input can sit high and make every read look instantly
     * ready, which is indistinguishable from a working pin until the numbers
     * come out wrong. Pulled down, a wrong pin number times out and says so.
     */
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Shared across the whole application, and installed at most once. Calling
     * it a second time returns ESP_ERR_INVALID_STATE, which is harmless -- but
     * it logs at error level on the way out, and on this firmware the console
     * *is* the product, so simply re-pointing DRDY at another pin would print
     * an alarming line about a condition that is not a problem.
     */
    if (!isr_service_ready) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) {
            err = ESP_OK; /* another module got there first */
        }
        if (err != ESP_OK) {
            return err;
        }
        isr_service_ready = true;
    }

    err = gpio_isr_handler_add(pin, drdy_isr, NULL);
    if (err != ESP_OK) {
        /* Leave no half-claimed pin behind: it is configured for an edge
         * nothing is listening for. */
        gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
        gpio_reset_pin(pin);
        return err;
    }

    drdy_pin = pin;
    return ESP_OK;
}

/* Wait for the device to raise DRDY. Only called with a pin configured. */
static esp_err_t wait_for_drdy(uint32_t timeout_ms)
{
    /*
     * Drop a signal left over from a conversion nobody read. Doing this before
     * sampling the level rather than after is what makes the sequence safe: an
     * edge arriving between the two still leaves the semaphore given, so the
     * take below returns immediately instead of missing the wake-up.
     */
    xSemaphoreTake(drdy_signal, 0);

    /*
     * Already high means a result is sitting in the registers unread, and there
     * will be no edge to wait for -- DRDY does not fall until the result is
     * read, so a conversion completing under a full register does not produce
     * one. Take it now. This read is the one case that keeps the old timing
     * risk, since its phase within the conversion is unknown; it can only
     * happen on the first sample of a batch, because reading the result drops
     * the line and re-arms the edge for every sample after it.
     */
    if (gpio_get_level(drdy_pin) == 1) {
        return ESP_OK;
    }

    if (xSemaphoreTake(drdy_signal, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/*
 * Take one conversion.
 *
 * The datasheet is explicit that reading ADCO without CR set latches and shifts
 * out the *previous* result, so readiness is established first -- otherwise an
 * averaged read would silently be an average of one sample repeated.
 */
static esp_err_t read_raw(int32_t *out)
{
    esp_err_t err;

    if (drdy_pin >= 0) {
        err = wait_for_drdy(CONVERSION_TIMEOUT_MS);
        if (err == ESP_ERR_TIMEOUT) {
            bp_error("DRDY (GPIO %d) never went high. Check that pin is really "
                     "wired to the device's DRDY output, or run 'drdy off' to "
                     "go back to polling the CR status bit over I2C.", drdy_pin);
        }
    } else {
        err = wait_for_bit(REG_PU_CTRL, PU_CTRL_CR, true, CONVERSION_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        return err;
    }

    /* Burst read: the NAU7802 auto-increments, so B2/B1/B0 come out in order. */
    uint8_t raw[3];
    err = read_regs(REG_ADCO_B2, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    uint32_t value = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];

    /* Sign-extend the 24-bit two's complement result into int32_t. */
    if (value & 0x800000) {
        value |= 0xFF000000;
    }

    *out = (int32_t)value;
    return ESP_OK;
}

/* Average `samples` conversions; load cell readings are noisy enough that a
 * single sample is rarely meaningful. */
static int read_average(int samples, double *average, int32_t *minimum, int32_t *maximum)
{
    double total = 0.0;
    int32_t low = INT32_MAX;
    int32_t high = INT32_MIN;

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = read_raw(&value);
        if (err != ESP_OK) {
            bp_error("Reading conversion %d of %d: %s", i + 1, samples,
                     esp_err_to_name(err));
            return -1;
        }

        total += value;
        if (value < low) {
            low = value;
        }
        if (value > high) {
            high = value;
        }
    }

    *average = total / samples;
    *minimum = low;
    *maximum = high;

    /*
     * A converter pinned at either rail is not measuring anything. During
     * bringup this usually means the bridge is disconnected or miswired, the
     * excitation is missing, or the gain is too high for the signal -- all of
     * which otherwise show up as a large, confident-looking number.
     */
    if (low <= -(int32_t)(ADC_FULL_SCALE - 1) || high >= (int32_t)(ADC_FULL_SCALE - 1)) {
        bp_error("ADC is saturated at full scale. Check that the bridge is "
                 "connected and excited, and that the gain is not too high.");
    }

    return 0;
}

static int take_sample_count(int argc, char **argv, int index, int *samples)
{
    *samples = DEFAULT_SAMPLES;

    if (argc <= index) {
        return 0;
    }
    if (parse_int_arg(argv[index], samples) < 0 ||
        *samples < 1 || *samples > MAX_SAMPLES) {
        bp_error("Sample count must be 1-%d", MAX_SAMPLES);
        return -1;
    }
    return 0;
}

/* Run the internal offset calibration and report the device's own verdict. */
static int run_calibration(void)
{
    esp_err_t err = update_reg(REG_CTRL2, CTRL2_CALMOD_MASK | CTRL2_CALS,
                               CALMOD_OFFSET_INTERNAL | CTRL2_CALS);
    if (err != ESP_OK) {
        bp_error("Starting internal calibration: %s", esp_err_to_name(err));
        return -1;
    }

    /* CALS is an action bit: the device clears it when calibration completes. */
    err = wait_for_bit(REG_CTRL2, CTRL2_CALS, false, CALIBRATION_TIMEOUT_MS);
    if (err != ESP_OK) {
        bp_error("Internal calibration did not finish: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t ctrl2 = 0;
    if (read_reg(REG_CTRL2, &ctrl2) == ESP_OK && (ctrl2 & CTRL2_CAL_ERR)) {
        bp_error("Internal calibration reported an error (CTRL2.CAL_ERR). "
                 "Check the bridge wiring and the reference voltage.");
        return -1;
    }

    return 0;
}

/* Map a voltage like "3.0" onto a VLDO encoding. Prints the legal set itself. */
static int parse_ldo_volts(const char *token, int *index)
{
    double volts = 0.0;
    if (parse_double_arg(token, &volts) < 0) {
        bp_error("LDO voltage must be a number");
        return -1;
    }

    int millivolts = (int)(volts * 1000.0 + 0.5);
    for (int i = 0; i < 8; i++) {
        if (ldo_millivolts[i] == millivolts) {
            *index = i;
            return 0;
        }
    }

    bp_printf("LDO voltage must be one of:");
    for (int i = 0; i < 8; i++) {
        bp_printf(" %.1f", ldo_millivolts[i] / 1000.0);
    }
    bp_printf("\n");
    return -1;
}

static void print_init_usage(void)
{
    bp_printf("Usage: init [ldo <volts>] [drdy <pin>]\n");
    bp_printf("Without 'ldo', AVDD is taken from the pin (chip default).\n");
    bp_printf("Without 'drdy', conversions are detected by polling over I2C.\n");
}

int cmd_nau7802_init(int argc, char **argv)
{
    if (!i2c_require_bus()) {
        return -1;
    }

    /*
     * AVDD source. The chip default is the external AVDD pin, and that is kept
     * here: switching the internal regulator on while a board already drives
     * AVDD would put two sources on one net. Boards that rely on the internal
     * LDO (many load cell breakouts do) need 'init ldo <volts>'.
     */
    bool use_ldo = false;
    int ldo_index = 0;
    int requested_drdy = -1;

    /* Both options describe how the board is wired, so either order is fine
     * and neither is required. */
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "ldo") == 0) {
            if (++i >= argc) {
                bp_error("Give the LDO voltage, e.g. 'init ldo 3.0'");
                return -1;
            }
            if (parse_ldo_volts(argv[i], &ldo_index) < 0) {
                return -1;
            }
            use_ldo = true;
        } else if (strcasecmp(argv[i], "drdy") == 0) {
            if (++i >= argc) {
                bp_error("Give the GPIO the DRDY pin is wired to, e.g. 'init drdy 7'");
                return -1;
            }
            if (parse_int_arg(argv[i], &requested_drdy) < 0) {
                bp_error("DRDY must be a GPIO number");
                return -1;
            }
        } else {
            print_init_usage();
            return -1;
        }
    }

    /* Claim the pin before touching the device, so a bad pin number fails
     * without having half-configured the converter. */
    if (requested_drdy >= 0 && configure_drdy(requested_drdy) != ESP_OK) {
        bp_error("Configuring GPIO %d as the DRDY input failed", requested_drdy);
        return -1;
    }

    uint8_t revision = 0;
    esp_err_t err = read_reg(REG_DEVICE_REV, &revision);
    if (err != ESP_OK) {
        bp_error("No response from 0x%02X: %s", NAU7802_ADDRESS, esp_err_to_name(err));
        return -1;
    }

    /* RR is level triggered: raise it to enter reset, drop it to leave. */
    if (write_reg(REG_PU_CTRL, PU_CTRL_RR) != ESP_OK ||
        write_reg(REG_PU_CTRL, 0) != ESP_OK) {
        bp_error("Resetting the device failed");
        return -1;
    }

    /* Digital first; PUA is only valid once PUD is set. */
    if (write_reg(REG_PU_CTRL, PU_CTRL_PUD) != ESP_OK) {
        bp_error("Powering up the digital section failed");
        return -1;
    }

    err = wait_for_bit(REG_PU_CTRL, PU_CTRL_PUR, true, CONVERSION_TIMEOUT_MS);
    if (err != ESP_OK) {
        bp_error("Device never reported power-up ready (PU_CTRL.PUR): %s",
                 esp_err_to_name(err));
        return -1;
    }

    uint8_t pu_ctrl = PU_CTRL_PUD | PU_CTRL_PUA;
    if (use_ldo) {
        pu_ctrl |= PU_CTRL_AVDDS;
    }
    if (write_reg(REG_PU_CTRL, pu_ctrl) != ESP_OK) {
        bp_error("Powering up the analog section failed");
        return -1;
    }

    if (use_ldo &&
        update_reg(REG_CTRL1, CTRL1_VLDO_MASK,
                   (uint8_t)(ldo_index << CTRL1_VLDO_SHIFT)) != ESP_OK) {
        bp_error("Setting the LDO voltage failed");
        return -1;
    }


    initialized = true;
    tare_offset = 0;
    calibrated = false;
    counts_per_unit = 0.0;

    if (run_calibration() < 0) {
        initialized = false;
        return -1;
    }

    /* Begin continuous conversions. */
    if (update_reg(REG_PU_CTRL, 0, PU_CTRL_CS) != ESP_OK) {
        bp_error("Starting conversions failed");
        initialized = false;
        return -1;
    }

    bp_printf("NAU7802 ready at 0x%02X (device revision 0x%02X)\n",
              NAU7802_ADDRESS, revision);
    if (drdy_pin >= 0) {
        bp_printf("Conversions signalled by DRDY on GPIO %d\n", drdy_pin);
    } else {
        bp_printf("No DRDY pin: conversions are detected by polling the CR bit "
                  "over I2C, which cannot start the read at a known point in "
                  "the conversion. Wire DRDY and use 'init drdy <pin>' if the "
                  "readings show occasional large outliers.\n");
    }
    if (use_ldo) {
        bp_printf("AVDD from the internal LDO at %.1f V\n",
                  ldo_millivolts[ldo_index] / 1000.0);
    } else {
        bp_printf("AVDD taken from the pin. If the load cell reads nothing, this "
                  "board may need the internal regulator: 'init ldo 3.0'\n");
    }
    bp_printf("Internal offset calibration passed. Run 'tare' with no load, then "
              "'calibrate <known mass>'.\n");
    return 0;
}

int cmd_nau7802_gain(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        uint8_t ctrl1 = 0;
        if (read_reg(REG_CTRL1, &ctrl1) != ESP_OK) {
            bp_error("Reading CTRL1 failed");
            return -1;
        }
        bp_printf("Gain is x%d\n", gain_values[ctrl1 & CTRL1_GAINS_MASK]);
        return 0;
    }

    int requested = 0;
    if (parse_int_arg(argv[1], &requested) < 0) {
        bp_error("Gain must be a number");
        return -1;
    }

    for (int i = 0; i < 8; i++) {
        if (gain_values[i] == requested) {
            if (update_reg(REG_CTRL1, CTRL1_GAINS_MASK,
                           (uint8_t)(i << CTRL1_GAINS_SHIFT)) != ESP_OK) {
                bp_error("Setting the gain failed");
                return -1;
            }
            /* Changing the analog path invalidates the offset calibration. */
            bp_printf("Gain set to x%d; re-running internal offset calibration\n",
                      requested);
            return run_calibration();
        }
    }

    bp_printf("Gain must be one of:");
    for (int i = 0; i < 8; i++) {
        bp_printf(" %d", gain_values[i]);
    }
    bp_printf("\n");
    return -1;
}

int cmd_nau7802_input(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        uint8_t ctrl2 = 0;
        if (read_reg(REG_CTRL2, &ctrl2) != ESP_OK) {
            bp_error("Reading CTRL2 failed");
            return -1;
        }
        bp_printf("Input channel: %s\n", (ctrl2 & CTRL2_CHS) ? "B" : "A");
        return 0;
    }

    if (strcasecmp(argv[1], "a") == 0) {
        if (update_reg(REG_CTRL2, CTRL2_CHS, 0) != ESP_OK) {
            bp_error("Switching to input A failed");
            return -1;
        }
        bp_printf("Input channel set to A; re-running internal offset calibration\n");
        return run_calibration();
    }

    if (strcasecmp(argv[1], "b") == 0) {
        if (update_reg(REG_CTRL2, CTRL2_CHS, CTRL2_CHS) != ESP_OK) {
            bp_error("Switching to input B failed");
            return -1;
        }
        bp_printf("Input channel set to B; re-running internal offset calibration\n");
        return run_calibration();
    }

    bp_error("Input must be 'a' or 'b'");
    return -1;
}

/*
 * Deliberately does not require `init`, or even a bus: this is a statement
 * about how the board is wired, and it is useful to be able to declare or
 * revoke it without disturbing a converter that is already running.
 */
int cmd_nau7802_drdy(int argc, char **argv)
{
    if (argc < 2) {
        if (drdy_pin < 0) {
            bp_printf("No DRDY pin; conversions are detected by polling the CR "
                      "status bit over I2C\n");
        } else {
            bp_printf("DRDY on GPIO %d, reading %s right now\n", drdy_pin,
                      gpio_get_level(drdy_pin) ? "high (a result is waiting)"
                                               : "low (no result waiting)");
        }
        return 0;
    }

    if (strcasecmp(argv[1], "off") == 0) {
        release_drdy();
        bp_printf("DRDY released; back to polling the CR status bit over I2C\n");
        return 0;
    }

    int pin = 0;
    if (parse_int_arg(argv[1], &pin) < 0) {
        bp_printf("Usage: drdy [<pin>|off]\n");
        return -1;
    }

    if (configure_drdy(pin) != ESP_OK) {
        bp_error("Configuring GPIO %d as the DRDY input failed", pin);
        return -1;
    }

    bp_printf("DRDY on GPIO %d\n", pin);

    /*
     * A pin that is low here is the normal case -- the last result was read --
     * so silence is not evidence either way. Saying nothing at all about it
     * would leave a typo to surface later as a timeout mid-measurement.
     */
    bp_printf("Run 'read' to confirm it: a wrong pin times out rather than "
              "reporting bad numbers.\n");
    return 0;
}

int cmd_nau7802_ldomode(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    uint8_t pga = 0;
    if (read_reg(REG_PGA, &pga) != ESP_OK) {
        bp_error("Reading the PGA register failed");
        return -1;
    }

    if (argc < 2) {
        bool stable = (pga & PGA_LDOMODE) != 0;
        bp_printf("LDOMODE %d: the AVDD capacitor %s\n", stable ? 1 : 0,
                  stable ? "may have ESR up to 5 ohms"
                         : "must have ESR below 1 ohm");
        bp_printf("%s\n", stable
                  ? "More stable regulator loop, lower DC gain."
                  : "Better DC accuracy, higher loop gain. This is the chip "
                    "default, and it is only correct if the board's AVDD "
                    "capacitor really is below 1 ohm ESR.");
        return 0;
    }

    int mode = 0;
    if (parse_int_arg(argv[1], &mode) < 0 || (mode != 0 && mode != 1)) {
        bp_printf("Usage: ldomode [0|1]\n");
        bp_printf("  0  AVDD capacitor ESR below 1 ohm (chip default)\n");
        bp_printf("  1  AVDD capacitor ESR up to 5 ohms\n");
        return -1;
    }

    if (update_reg(REG_PGA, PGA_LDOMODE, mode ? PGA_LDOMODE : 0) != ESP_OK) {
        bp_error("Setting LDOMODE failed");
        return -1;
    }

    /* The two modes settle AVDD at slightly different levels, and AVDD is the
     * reference, so the existing offset calibration no longer applies. */
    bp_printf("LDOMODE set to %d; re-running internal offset calibration\n", mode);
    return run_calibration();
}

int cmd_nau7802_pgacap(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    uint8_t power = 0;
    if (read_reg(REG_POWER_CTRL, &power) != ESP_OK) {
        bp_error("Reading the power control register failed");
        return -1;
    }

    if (argc < 2) {
        bp_printf("PGA output bypass capacitor: %s\n",
                  (power & POWER_PGA_CAP_EN) ? "enabled" : "disabled");
        return 0;
    }

    bool enable;
    if (strcasecmp(argv[1], "on") == 0) {
        enable = true;
    } else if (strcasecmp(argv[1], "off") == 0) {
        enable = false;
    } else {
        bp_printf("Usage: pgacap [on|off]\n");
        return -1;
    }

    if (update_reg(REG_POWER_CTRL, POWER_PGA_CAP_EN,
                   enable ? POWER_PGA_CAP_EN : 0) != ESP_OK) {
        bp_error("Setting the PGA output bypass capacitor failed");
        return -1;
    }

    if (enable) {
        /*
         * Worth saying every time rather than once in the docs: with no
         * capacitor fitted this quietly does nothing, and it takes channel 2
         * away whether or not it helps.
         */
        bp_printf("PGA output bypass capacitor enabled. This needs a capacitor "
                  "fitted across VIN2P/VIN2N (330 pF at AVDD 3.3 V, 680 pF at "
                  "4.5 V); with none there it changes nothing. Channel B now "
                  "reads the filter node, not an input.\n");
    } else {
        bp_printf("PGA output bypass capacitor disabled; channel B is an input "
                  "again\n");
    }

    bp_printf("Re-running internal offset calibration\n");
    return run_calibration();
}

int cmd_nau7802_raw(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int arg_idx = 1;
    int samples = 1;
    if (arg_idx < argc) {
        if (parse_int_arg(argv[arg_idx], &samples) < 0) {
            bp_error("Invalid sample count: %s", argv[arg_idx]);
            return -1;
        }
        arg_idx++;
    }

    const char *channel = "a";
    if (arg_idx < argc) {
        if (strcasecmp(argv[arg_idx], "a") != 0 && strcasecmp(argv[arg_idx], "b") != 0) {
            bp_error("Channel must be 'a' or 'b': %s", argv[arg_idx]);
            return -1;
        }
        channel = argv[arg_idx];
    }

    if (strcasecmp(channel, "b") == 0) {
        if (update_reg(REG_CTRL2, CTRL2_CHS, CTRL2_CHS) != ESP_OK) {
            bp_error("Switching to input B failed");
            return -1;
        }
        run_calibration();
    } else {
        if (update_reg(REG_CTRL2, CTRL2_CHS, 0) != ESP_OK) {
            bp_error("Switching to input A failed");
            return -1;
        }
        run_calibration();
    }

    bp_printf("Raw ADC readings (channel %s):\n", channel);

    for (int i = 0; i < samples; i++) {
        int32_t value = 0;
        esp_err_t err = read_raw(&value);
        if (err != ESP_OK) {
            bp_error("Reading sample %d: %s", i + 1, esp_err_to_name(err));
            return -1;
        }

        double percent = value * 100.0 / ADC_FULL_SCALE;
        bp_printf("  %8ld  (%.4f%% of full scale)\n", value, percent);
    }

    return 0;
}

int cmd_nau7802_rate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        uint8_t ctrl2 = 0;
        if (read_reg(REG_CTRL2, &ctrl2) != ESP_OK) {
            bp_error("Reading CTRL2 failed");
            return -1;
        }
        int rate = rate_values[(ctrl2 & CTRL2_CRS_MASK) >> CTRL2_CRS_SHIFT];
        if (rate < 0) {
            bp_printf("Sample rate register holds an undefined encoding\n");
        } else {
            bp_printf("Sample rate is %d SPS\n", rate);
        }
        return 0;
    }

    int requested = 0;
    if (parse_int_arg(argv[1], &requested) < 0) {
        bp_error("Sample rate must be a number");
        return -1;
    }

    for (int i = 0; i < 8; i++) {
        if (rate_values[i] == requested) {
            if (update_reg(REG_CTRL2, CTRL2_CRS_MASK,
                           (uint8_t)(i << CTRL2_CRS_SHIFT)) != ESP_OK) {
                bp_error("Setting the sample rate failed");
                return -1;
            }
            /* The conversion rate changes the modulator and filter settings, so
             * the existing offset calibration no longer applies. Skipping this
             * leaves the ADC returning values that swing across most of the
             * full-scale range. */
            bp_printf("Sample rate set to %d SPS; re-running internal offset "
                      "calibration\n", requested);

            /* Measured on the board this was developed against: 10-80 SPS are
             * rock steady, while 320 SPS returns values swinging across the
             * whole range. Likely an oscillator limitation -- 320 SPS may need
             * an external crystal (PU_CTRL.OSCS) rather than the internal RC. */
            if (requested == 320) {
                bp_printf("Note: 320 SPS has been observed returning invalid, "
                          "full-range data with the internal RC oscillator. "
                          "Check the reading before trusting it.\n");
            }

            return run_calibration();
        }
    }

    bp_printf("Sample rate must be one of: 10 20 40 80 320 SPS\n");
    return -1;
}

int cmd_nau7802_read(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    double average = 0.0;
    int32_t low = 0;
    int32_t high = 0;
    if (read_average(samples, &average, &low, &high) < 0) {
        return -1;
    }

    /* Percent of full scale is wiring-independent; an absolute voltage would
     * depend on REFP-REFN, which this driver has no way to know. */
    bp_printf("Raw %10.1f counts  (%d samples, spread %ld, %.4f%% of full scale)\n",
              average, samples, (long)(high - low),
              average * 100.0 / ADC_FULL_SCALE);

    if (tare_offset != 0) {
        bp_printf("     %10.1f counts net of tare\n", average - tare_offset);
    }

    return 0;
}

int cmd_nau7802_tare(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    double average = 0.0;
    int32_t low = 0;
    int32_t high = 0;
    if (read_average(samples, &average, &low, &high) < 0) {
        return -1;
    }

    tare_offset = (int32_t)average;
    bp_printf("Tare set to %ld counts (%d samples, spread %ld)\n",
              (long)tare_offset, samples, (long)(high - low));
    return 0;
}

int cmd_nau7802_calibrate(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (argc < 2) {
        bp_printf("Usage: calibrate <known mass>\n");
        bp_printf("Run 'tare' with the scale empty first, then place a known "
                  "mass and run this. The unit is whatever you use here.\n");
        return -1;
    }

    double known = 0.0;
    if (parse_double_arg(argv[1], &known) < 0 || known == 0.0) {
        bp_error("The known mass must be a non-zero number");
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 2, &samples) < 0) {
        return -1;
    }

    double average = 0.0;
    int32_t low = 0;
    int32_t high = 0;
    if (read_average(samples, &average, &low, &high) < 0) {
        return -1;
    }

    double net = average - tare_offset;

    /*
     * Reject a calibration derived from noise. With nothing on the cell the net
     * change is a fraction of a count, which would still produce a scale factor
     * -- an absurd one that then makes every later weight meaningless. Require
     * the change to stand well clear of the observed sample spread.
     */
    double noise = (double)(high - low);
    double magnitude = net < 0.0 ? -net : net;

    if (magnitude < 10.0 * (noise + 1.0)) {
        bp_error("The reading moved only %.1f counts from the tare, which is "
                 "within the noise (%.0f counts of spread). Is the mass on the "
                 "cell, and was 'tare' run while it was empty?", net, noise);
        return -1;
    }

    counts_per_unit = net / known;
    calibrated = true;

    bp_printf("Calibrated: %.1f counts per unit (%.1f counts for %.4f units)\n",
              counts_per_unit, net, known);
    return 0;
}

int cmd_nau7802_weight(int argc, char **argv)
{
    if (!require_init()) {
        return -1;
    }

    if (!calibrated) {
        bp_error("Not calibrated. Run 'tare' with no load, then "
                 "'calibrate <known mass>'.");
        return -1;
    }

    int samples = 0;
    if (take_sample_count(argc, argv, 1, &samples) < 0) {
        return -1;
    }

    double average = 0.0;
    int32_t low = 0;
    int32_t high = 0;
    if (read_average(samples, &average, &low, &high) < 0) {
        return -1;
    }

    double net = average - tare_offset;
    double mass = net / counts_per_unit;

    /* Convert the sample spread into the same units so the reading comes with
     * an honest indication of its own noise. */
    double noise = (high - low) / counts_per_unit;

    bp_printf("Weight %12.4f units  (+/-%.4f noise over %d samples, %.1f raw counts)\n",
              mass, noise / 2.0, samples, net);
    return 0;
}

int cmd_nau7802_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus()) {
        return -1;
    }

    uint8_t pu_ctrl = 0, ctrl1 = 0, ctrl2 = 0, revision = 0;
    if (read_reg(REG_PU_CTRL, &pu_ctrl) != ESP_OK ||
        read_reg(REG_CTRL1, &ctrl1) != ESP_OK ||
        read_reg(REG_CTRL2, &ctrl2) != ESP_OK ||
        read_reg(REG_DEVICE_REV, &revision) != ESP_OK) {
        bp_error("No response from 0x%02X", NAU7802_ADDRESS);
        return -1;
    }

    int rate = rate_values[(ctrl2 & CTRL2_CRS_MASK) >> CTRL2_CRS_SHIFT];

    bp_printf("Device revision 0x%02X at 0x%02X\n", revision, NAU7802_ADDRESS);
    bp_printf("PU_CTRL 0x%02X  digital %s, analog %s, ready %s, data %s\n",
              pu_ctrl,
              (pu_ctrl & PU_CTRL_PUD) ? "up" : "down",
              (pu_ctrl & PU_CTRL_PUA) ? "up" : "down",
              (pu_ctrl & PU_CTRL_PUR) ? "yes" : "no",
              (pu_ctrl & PU_CTRL_CR) ? "ready" : "pending");
    bp_printf("AVDD source: %s\n",
              (pu_ctrl & PU_CTRL_AVDDS) ? "internal LDO" : "AVDD pin");
    bp_printf("CTRL1   0x%02X  gain x%d, LDO %.1f V\n", ctrl1,
              gain_values[ctrl1 & CTRL1_GAINS_MASK],
              ldo_millivolts[(ctrl1 & CTRL1_VLDO_MASK) >> CTRL1_VLDO_SHIFT] / 1000.0);
    bp_printf("CTRL2   0x%02X  %d SPS, calibration %s\n", ctrl2,
              rate, (ctrl2 & CTRL2_CAL_ERR) ? "ERROR" : "ok");
    bp_printf("Input channel: %s\n", (ctrl2 & CTRL2_CHS) ? "B" : "A");
    if (drdy_pin >= 0) {
        bp_printf("Data ready: DRDY on GPIO %d, now %s\n", drdy_pin,
                  gpio_get_level(drdy_pin) ? "high" : "low");
    } else {
        bp_printf("Data ready: no DRDY pin, polling the CR bit over I2C\n");
    }

    uint8_t pga = 0, power = 0;
    if (read_reg(REG_PGA, &pga) == ESP_OK &&
        read_reg(REG_POWER_CTRL, &power) == ESP_OK) {
        bp_printf("PGA     0x%02X  LDOMODE %d (AVDD capacitor ESR up to %s)\n",
                  pga, (pga & PGA_LDOMODE) ? 1 : 0,
                  (pga & PGA_LDOMODE) ? "5 ohms" : "1 ohm");
        bp_printf("POWER   0x%02X  PGA output bypass capacitor %s\n", power,
                  (power & POWER_PGA_CAP_EN) ? "enabled" : "disabled");
    }

    if (initialized) {
        bp_printf("Tare %ld counts; %s\n", (long)tare_offset,
                  calibrated ? "calibrated" : "not calibrated");
        if (calibrated) {
            bp_printf("Scale %.1f counts per unit\n", counts_per_unit);
        }
    } else {
        bp_printf("Not initialized by this session; run 'nau7802 init'\n");
    }

    return 0;
}

int cmd_nau7802_registers(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!i2c_require_bus()) {
        return -1;
    }

    bp_printf("Register dump for NAU7802 at 0x%02X:\n", NAU7802_ADDRESS);

    for (int reg = 0; reg <= 0x0B; reg++) {
        uint8_t value = 0;
        if (read_reg(reg, &value) == ESP_OK) {
            bp_printf("  REG%02X: 0x%02X\n", reg, value);
        } else {
            bp_printf("  REG%02X: <error reading>\n", reg);
        }
    }

    return 0;
}
