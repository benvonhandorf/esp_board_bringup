/*
 * The `audio` menu.
 *
 * This is a capability menu rather than a bus menu -- it owns the I2S
 * transport, the test signals, and whichever codec is currently attached. `sd`
 * is the existing example of the same shape: named after what it tests, not
 * after the peripheral it happens to drive.
 *
 * The generic commands here never mention a specific part. They work through
 * the audio_codec_t vtable in audio.h, so a codec with sixty I2C registers and
 * an amplifier with one enable pin are driven by the same `audio tone`,
 * `audio volume` and `audio mute`.
 */
#include "esp_bringup.h"
#include "output.h"
#include "audio.h"
#include "codec_nau8822.h"
#include "codec_ns4168.h"

#include "driver/gpio.h"

#define DEFAULT_RATE_HZ  48000
#define DEFAULT_BITS     16
#define DEFAULT_LEVEL_PCT 25
#define DEFAULT_SECONDS   3.0

#define MIN_RATE_HZ  8000
#define MAX_RATE_HZ  192000
#define MAX_SECONDS  600.0
#define MIN_TONE_HZ  1.0

/* Every part the firmware knows how to drive. Adding one is a new file, one
 * row here, and one submenu in menu_table.c. */
static const audio_codec_t *const codec_registry[] = {
    &nau8822_codec,
    &ns4168_codec,
};
#define CODEC_COUNT (sizeof(codec_registry) / sizeof(codec_registry[0]))

static const audio_codec_t *active_codec;
static int last_volume_pct = -1;

/* ------------------------------------------------------------------ */
/* Codec attachment                                                    */
/* ------------------------------------------------------------------ */

void audio_codec_attach(const audio_codec_t *codec)
{
    if (active_codec && active_codec != codec && active_codec->detach) {
        active_codec->detach();
    }
    active_codec = codec;
    last_volume_pct = -1;
}

void audio_codec_detach(void)
{
    if (active_codec && active_codec->detach) {
        active_codec->detach();
    }
    active_codec = NULL;
    last_volume_pct = -1;
}

const audio_codec_t *audio_codec_active(void)
{
    return active_codec;
}

/* ------------------------------------------------------------------ */
/* audio bus                                                           */
/* ------------------------------------------------------------------ */

/*
 * Consume the optional trailing keyword pairs. Pins are positional because
 * three of them are always required; everything else is a keyword pair, so
 * options can be given in any order and none of them can be confused with a
 * pin. This is the same idiom as `sd spi ... khz <freq>`.
 */
static int take_bus_options(int argc, char **argv, int index,
                            int *din, audio_format_t *fmt)
{
    while (index < argc) {
        if (index + 1 >= argc) {
            bp_error("'%s' needs a value", argv[index]);
            return -1;
        }

        const char *key = argv[index];
        const char *value = argv[index + 1];

        if (strcasecmp(key, "din") == 0) {
            if (parse_int_arg(value, din) < 0 || !GPIO_IS_VALID_GPIO(*din)) {
                bp_error("DIN must be a valid pin number, not '%s'", value);
                return -1;
            }
        } else if (strcasecmp(key, "mclk") == 0) {
            if (parse_int_arg(value, &fmt->mclk_pin) < 0 ||
                !GPIO_IS_VALID_OUTPUT_GPIO(fmt->mclk_pin)) {
                bp_error("MCLK must be an output-capable pin, not '%s'", value);
                return -1;
            }
        } else if (strcasecmp(key, "rate") == 0) {
            int rate = 0;
            if (parse_int_arg(value, &rate) < 0 ||
                rate < MIN_RATE_HZ || rate > MAX_RATE_HZ) {
                bp_error("Sample rate must be %d-%d Hz", MIN_RATE_HZ, MAX_RATE_HZ);
                return -1;
            }
            fmt->rate_hz = (uint32_t)rate;
        } else if (strcasecmp(key, "bits") == 0) {
            int bits = 0;
            if (parse_int_arg(value, &bits) < 0 ||
                (bits != 16 && bits != 24 && bits != 32)) {
                bp_error("Bit width must be 16, 24 or 32");
                return -1;
            }
            fmt->bits = (uint8_t)bits;
        } else if (strcasecmp(key, "mclkmult") == 0) {
            int mult = 0;
            if (parse_int_arg(value, &mult) < 0 ||
                (mult != 128 && mult != 192 && mult != 256 && mult != 384 &&
                 mult != 512 && mult != 768)) {
                bp_error("MCLK multiple must be 128, 192, 256, 384, 512 or 768");
                return -1;
            }
            fmt->mclk_multiple = mult;
        } else {
            bp_error("Unexpected argument '%s'; options are 'din <pin>', "
                     "'mclk <pin>', 'rate <hz>', 'bits <16|24|32>' and "
                     "'mclkmult <n>'", key);
            return -1;
        }
        index += 2;
    }
    return 0;
}

static bool pins_distinct(const int *pins, const char *const *names, int count)
{
    for (int i = 0; i < count; i++) {
        if (pins[i] < 0) {
            continue;
        }
        for (int j = i + 1; j < count; j++) {
            if (pins[i] == pins[j]) {
                bp_error("%s and %s cannot both be GPIO %d", names[i], names[j],
                         pins[i]);
                return false;
            }
        }
    }
    return true;
}

static void print_format(void)
{
    const audio_format_t *fmt = audio_bus_format();
    if (!fmt) {
        return;
    }

    uint32_t configured = audio_bus_actual_rate();
    bp_printf("Format:  %lu Hz requested", (unsigned long)fmt->rate_hz);
    if (configured) {
        bp_printf(", %lu Hz configured", (unsigned long)configured);
    }
    bp_printf(", %u-bit, 2 slots\n", fmt->bits);

    /*
     * Report what the dividers actually produced, not what was asked for. The
     * clock tree divides an integer ratio out of a PLL, so a requested rate is
     * often unreachable exactly -- the same reason `gpio pwm set` reports the
     * frequency it achieved and `sd spi` reports its real clock.
     */
    uint32_t bclk = audio_bus_bclk_hz();
    uint32_t mclk = audio_bus_mclk_hz();
    bp_printf("Clocks:  BCLK %.3f MHz", bclk / 1e6);
    if (fmt->mclk_pin >= 0) {
        bp_printf(", MCLK %.3f MHz on GPIO %d (x%d)\n", mclk / 1e6,
                  fmt->mclk_pin, fmt->mclk_multiple);
    } else {
        bp_printf(", MCLK not routed to a pin\n");
    }
}

int cmd_audio_bus(int argc, char **argv)
{
    if (argc < 4) {
        bp_printf("Usage: bus <bclk> <ws> <dout> [din <pin>] [mclk <pin>] "
                  "[rate <hz>] [bits <16|24|32>] [mclkmult <n>]\n");
        return -1;
    }

    int bclk = 0;
    int ws = 0;
    int dout = 0;
    if (parse_int_arg(argv[1], &bclk) < 0 || parse_int_arg(argv[2], &ws) < 0 ||
        parse_int_arg(argv[3], &dout) < 0) {
        bp_error("BCLK, WS and DOUT must be pin numbers");
        return -1;
    }

    int din = -1;
    audio_format_t fmt = {
        .rate_hz = DEFAULT_RATE_HZ,
        .bits = DEFAULT_BITS,
        .mclk_pin = -1,
        .mclk_multiple = 0,
    };
    if (take_bus_options(argc, argv, 4, &din, &fmt) < 0) {
        return -1;
    }

    const int pins[] = {bclk, ws, dout, din, fmt.mclk_pin};
    const char *const names[] = {"BCLK", "WS", "DOUT", "DIN", "MCLK"};
    for (int i = 0; i < 5; i++) {
        if (i == 3) {
            continue; /* DIN is an input, checked separately below */
        }
        if (pins[i] >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i])) {
            bp_error("%s: GPIO %d is not an output-capable pin on this chip",
                     names[i], pins[i]);
            return -1;
        }
    }
    if (din >= 0 && !GPIO_IS_VALID_GPIO(din)) {
        bp_error("DIN: GPIO %d is not a valid pin on this chip", din);
        return -1;
    }
    if (!pins_distinct(pins, names, 5)) {
        return -1;
    }

    /* Reconfiguring the transport under a running tone would tear the channel
     * out from under the player task. */
    if (audio_playing()) {
        audio_play_stop();
    }

    esp_err_t err = audio_bus_open(bclk, ws, dout, din, &fmt);
    if (err != ESP_OK) {
        bp_error("Initializing I2S: %s", esp_err_to_name(err));
        return -1;
    }

    /*
     * Start the transmitter immediately and leave it running.
     *
     * With no data queued the DMA sends silence, so the bit clock and word
     * select run continuously from this point. That is what a codec wants:
     * most of them mute or reset themselves when their clocks stop, and
     * starting and stopping the clock around every tone makes an amplifier
     * pop. It also means the lines can be put on a scope before any signal is
     * played.
     */
    err = audio_bus_tx_enable(true);
    if (err != ESP_OK) {
        bp_error("Starting the I2S transmitter: %s", esp_err_to_name(err));
        return -1;
    }

    bp_printf("I2S initialized on BCLK=%d, WS=%d, DOUT=%d", bclk, ws, dout);
    if (din >= 0) {
        bp_printf(", DIN=%d", din);
    }
    bp_printf("\n");
    print_format();
    bp_printf("Transmitting silence, so the clocks are live for the codec.\n");

    /* An attached part was configured for the old format; bring it in line. */
    if (active_codec && active_codec->configure) {
        err = active_codec->configure(audio_bus_format());
        if (err != ESP_OK) {
            bp_error("Reconfiguring %s for the new format: %s",
                     active_codec->name, esp_err_to_name(err));
            return -1;
        }
        bp_printf("Reconfigured %s for the new format.\n", active_codec->name);
    }

    return 0;
}

int cmd_audio_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!audio_bus_ready()) {
        bp_printf("I2S is not initialized. Run 'audio bus <bclk> <ws> <dout>'.\n");
    } else {
        int bclk = 0;
        int ws = 0;
        int dout = 0;
        int din = 0;
        int mclk = 0;
        audio_bus_pins(&bclk, &ws, &dout, &din, &mclk);

        bp_printf("Pins:    BCLK=%d, WS=%d, DOUT=%d", bclk, ws, dout);
        if (din >= 0) {
            bp_printf(", DIN=%d", din);
        }
        if (mclk >= 0) {
            bp_printf(", MCLK=%d", mclk);
        }
        bp_printf("\n");
        print_format();
        bp_printf("Output:  %s\n", audio_bus_tx_enabled()
                  ? (audio_playing() ? "playing" : "running, silent") : "stopped");
    }

    if (!active_codec) {
        bp_printf("Codec:   none attached (a bare I2S DAC or amplifier needs "
                  "none)\n");
        return 0;
    }

    bp_printf("Codec:   %s - %s\n", active_codec->name, active_codec->description);
    if (last_volume_pct >= 0) {
        bp_printf("Volume:  %d%%\n", last_volume_pct);
    }
    if (active_codec->status) {
        active_codec->status();
    }
    return 0;
}

int cmd_audio_codecs(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bp_printf("%-10s %-7s %-5s %s\n", "name", "dir", "mclk", "part");
    for (size_t i = 0; i < CODEC_COUNT; i++) {
        const audio_codec_t *c = codec_registry[i];
        const char *dir = (c->directions == (AUDIO_DIR_TX | AUDIO_DIR_RX)) ? "in/out"
                        : (c->directions & AUDIO_DIR_RX) ? "in" : "out";
        bp_printf("%-10s %-7s %-5s %s%s\n", c->name, dir,
                  c->needs_mclk ? "yes" : "no", c->description,
                  c == active_codec ? "  <- attached" : "");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test signals                                                        */
/* ------------------------------------------------------------------ */

/*
 * Trailing options shared by `tone` and `sweep`.
 *
 * A bare number is the duration and may appear once; everything else is a
 * keyword. That keeps `tone 1000 3 level 50 left` readable while leaving no
 * argument that could be mistaken for another.
 */
static int take_signal_options(int argc, char **argv, int index,
                               audio_signal_t *sig, bool is_sweep)
{
    bool duration_set = false;

    while (index < argc) {
        const char *token = argv[index];

        if (strcasecmp(token, "level") == 0) {
            if (index + 1 >= argc) {
                bp_error("'level' needs a percentage");
                return -1;
            }
            int pct = 0;
            if (parse_int_arg(argv[index + 1], &pct) < 0 || pct < 1 || pct > 100) {
                bp_error("Level must be 1-100 percent of full scale");
                return -1;
            }
            sig->level_pct = pct;
            index += 2;
            continue;
        }

        if (strcasecmp(token, "left") == 0) {
            sig->channel = AUDIO_CHANNEL_LEFT;
        } else if (strcasecmp(token, "right") == 0) {
            sig->channel = AUDIO_CHANNEL_RIGHT;
        } else if (strcasecmp(token, "both") == 0) {
            sig->channel = AUDIO_CHANNEL_BOTH;
        } else if (is_sweep && strcasecmp(token, "log") == 0) {
            sig->logarithmic = true;
        } else if (is_sweep && strcasecmp(token, "linear") == 0) {
            sig->logarithmic = false;
        } else if (!is_sweep && strcasecmp(token, "continuous") == 0) {
            sig->seconds = 0.0;
            duration_set = true;
        } else {
            double seconds = 0.0;
            if (duration_set || parse_double_arg(token, &seconds) < 0) {
                bp_error("Unexpected argument '%s'", token);
                return -1;
            }
            if (seconds <= 0.0 || seconds > MAX_SECONDS) {
                bp_error("Duration must be greater than 0 and at most %.0f seconds",
                         MAX_SECONDS);
                return -1;
            }
            sig->seconds = seconds;
            duration_set = true;
        }
        index++;
    }

    return 0;
}

static bool frequency_ok(double hz, const char *what)
{
    /* Above Nyquist the tone aliases down to something else entirely, which
     * during bring-up looks like a fault in the hardware rather than in the
     * request. */
    const audio_format_t *fmt = audio_bus_format();
    double nyquist = (fmt ? fmt->rate_hz : DEFAULT_RATE_HZ) / 2.0;

    if (hz < MIN_TONE_HZ || hz >= nyquist) {
        bp_error("%s must be between %.0f Hz and %.0f Hz (half the sample rate)",
                 what, MIN_TONE_HZ, nyquist);
        return false;
    }
    return true;
}

int cmd_audio_tone(int argc, char **argv)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (argc < 2) {
        bp_printf("Usage: tone <hz> [seconds|continuous] [level <pct>] "
                  "[left|right|both]\n");
        return -1;
    }

    double hz = 0.0;
    if (parse_double_arg(argv[1], &hz) < 0) {
        bp_error("Frequency must be a number, not '%s'", argv[1]);
        return -1;
    }
    if (!frequency_ok(hz, "Frequency")) {
        return -1;
    }

    audio_signal_t signal = {
        .start_hz = hz,
        .end_hz = hz,
        .logarithmic = false,
        .seconds = DEFAULT_SECONDS,
        .level_pct = DEFAULT_LEVEL_PCT,
        .channel = AUDIO_CHANNEL_BOTH,
    };
    if (take_signal_options(argc, argv, 2, &signal, false) < 0) {
        return -1;
    }

    return audio_play(&signal);
}

int cmd_audio_sweep(int argc, char **argv)
{
    if (!audio_bus_require()) {
        return -1;
    }
    if (argc < 3) {
        bp_printf("Usage: sweep <start_hz> <end_hz> [seconds] [level <pct>] "
                  "[log|linear] [left|right|both]\n");
        return -1;
    }

    double start = 0.0;
    double end = 0.0;
    if (parse_double_arg(argv[1], &start) < 0 || parse_double_arg(argv[2], &end) < 0) {
        bp_error("Start and end frequencies must be numbers");
        return -1;
    }
    if (!frequency_ok(start, "Start frequency") || !frequency_ok(end, "End frequency")) {
        return -1;
    }
    if (start == end) {
        bp_error("Start and end frequencies are the same; use 'tone' for that");
        return -1;
    }

    audio_signal_t signal = {
        .start_hz = start,
        .end_hz = end,
        /* Equal time per octave, which is how audio hardware is judged. */
        .logarithmic = true,
        .seconds = 5.0,
        .level_pct = DEFAULT_LEVEL_PCT,
        .channel = AUDIO_CHANNEL_BOTH,
    };
    if (take_signal_options(argc, argv, 3, &signal, true) < 0) {
        return -1;
    }

    return audio_play(&signal);
}

int cmd_audio_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return audio_play_stop();
}

/* ------------------------------------------------------------------ */
/* Generic codec operations                                            */
/* ------------------------------------------------------------------ */

static const audio_codec_t *require_codec(const char *operation)
{
    if (!active_codec) {
        bp_error("No codec attached, so there is nothing to %s", operation);
        bp_printf("Run 'audio codecs' for the parts this firmware knows, then "
                  "attach one (for example 'audio nau8822 init').\n");
        return NULL;
    }
    return active_codec;
}

int cmd_audio_volume(int argc, char **argv)
{
    const audio_codec_t *codec = require_codec("set the volume on");
    if (!codec) {
        return -1;
    }

    if (!codec->set_volume) {
        /*
         * Not a failure of the command so much as of the hardware, and worth
         * saying plainly: a fixed-gain amplifier has nowhere to put a volume
         * setting, but the generator can still be turned down.
         */
        bp_error("%s has no volume control", codec->name);
        bp_printf("Use the digital level instead: 'audio tone 1000 3 level 10'.\n");
        return -1;
    }

    if (argc < 2) {
        if (last_volume_pct < 0) {
            bp_printf("Volume has not been set this session.\n");
        } else {
            bp_printf("Volume %d%%\n", last_volume_pct);
        }
        return 0;
    }

    int pct = 0;
    if (parse_int_arg(argv[1], &pct) < 0 || pct < 0 || pct > 100) {
        bp_error("Volume must be 0-100 percent");
        return -1;
    }

    esp_err_t err = codec->set_volume(pct);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1; /* the driver has already explained; see audio_codec_t */
    }
    if (err != ESP_OK) {
        bp_error("Setting the volume on %s: %s", codec->name, esp_err_to_name(err));
        return -1;
    }

    last_volume_pct = pct;
    bp_printf("Volume %d%%\n", pct);
    return 0;
}

int cmd_audio_mute(int argc, char **argv)
{
    const audio_codec_t *codec = require_codec("mute");
    if (!codec) {
        return -1;
    }

    if (!codec->set_mute) {
        bp_error("%s has no mute control", codec->name);
        return -1;
    }

    bool mute = true;
    if (argc > 1) {
        if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "true") == 0) {
            mute = true;
        } else if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "false") == 0) {
            mute = false;
        } else {
            bp_error("Expected 'on' or 'off', not '%s'", argv[1]);
            return -1;
        }
    }

    esp_err_t err = codec->set_mute(mute);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return -1; /* the driver has already explained; see audio_codec_t */
    }
    if (err != ESP_OK) {
        bp_error("%s %s: %s", mute ? "Muting" : "Unmuting", codec->name,
                 esp_err_to_name(err));
        return -1;
    }

    bp_printf("%s %s\n", codec->name, mute ? "muted" : "unmuted");
    return 0;
}

int cmd_audio_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (audio_playing()) {
        audio_play_stop();
    }

    /*
     * The codec goes first. An amplifier still enabled when its clocks stop
     * thumps the speaker, and a codec left powered on a dead bus is the kind
     * of state that makes the next bring-up attempt confusing.
     */
    if (active_codec) {
        bp_printf("Detaching %s\n", active_codec->name);
        audio_codec_detach();
    }

    if (!audio_bus_ready()) {
        bp_printf("I2S was not initialized.\n");
        return 0;
    }

    audio_bus_close();
    bp_printf("I2S released.\n");
    return 0;
}
