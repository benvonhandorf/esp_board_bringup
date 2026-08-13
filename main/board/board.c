/*
 * Named board pinouts and per-subsystem presets.
 *
 * Each board is a submenu, and each subsystem it has a preset for is a command
 * inside it, so `board cardputer` prints what is available and changes
 * nothing, while `board cardputer audio` runs the setup. That falls out of the
 * existing menu machinery -- tab completion and `help` work on board names
 * with no special cases -- and it keeps the rule that selecting a board never
 * has side effects.
 *
 * A preset is stored as the command lines a person would have typed, and each
 * is echoed as it runs. The point is not to hide the commands but to stop
 * transcribing pin numbers by hand; after running one you can see exactly what
 * to type next time.
 */
#include "esp_bringup.h"
#include "output.h"
#include "board.h"
#include "menu.h"

#include "esp_console.h"
#include "sdkconfig.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

#define MAX_LINE_LEN 128
#define MAX_ARGS     24

typedef struct {
    const char *role;
    int pin;
    const char *note;  /* NULL when the pin is confirmed against hardware */
} board_pin_t;

typedef struct {
    const char *name;
    const char *chip;          /* CONFIG_IDF_TARGET this board is */
    const char *description;
    const board_pin_t *pins;
    size_t pin_count;
} board_t;

/* ------------------------------------------------------------------ */
/* Boards                                                              */
/* ------------------------------------------------------------------ */

/*
 * M5Stack Cardputer. Every pin here has now been exercised: the SD pins by the
 * benchmark table in docs/sd.md, the speaker pins by a tone, and the
 * microphone pins by clocking them from the wrong GPIO and watching the data
 * go completely static -- 48000 consecutive samples of -30935 with the clock
 * elsewhere, real varying audio with it on 43. That is what confirms 43 and 46
 * rather than the schematic's word for them.
 *
 * GPIO 43 carries the speaker's word-select and the microphone's clock, which
 * is a board decision with a consequence no amount of software can undo: the
 * two cannot run at the same time, so this board cannot record its own
 * speaker. `audio pdm` refuses rather than letting the second peripheral take
 * the pad away from the first.
 */
static const board_pin_t cardputer_pins[] = {
    {"SD CLK", 40, NULL},
    {"SD MOSI", 14, NULL},
    {"SD MISO", 39, NULL},
    {"SD CS", 12, NULL},
    {"Speaker BCLK", 41, NULL},
    {"Speaker WS/LRCK", 43, "also the microphone clock; only one at a time"},
    {"Speaker DATA", 42, "amplifier plays the RIGHT slot; left is silent"},
    {"Speaker SD/enable", -1, "not broken out; the amplifier is always on"},
    {"Mic PDM CLK", 43, "also the speaker WS line; only one at a time"},
    {"Mic PDM DATA", 46, "both slots carry the same mono signal"},
};

static const board_t cardputer = {
    .name = "cardputer",
    .chip = "esp32s3",
    .description = "M5Stack Cardputer: NS4168 speaker amplifier, SPM1423 PDM mic, microSD",
    .pins = cardputer_pins,
    .pin_count = ARRAY_COUNT(cardputer_pins),
};

/*
 * Seeed XIAO ESP32-S3 Sense. No speaker at all, so there is no audio output
 * preset -- but its microphone pins are its own, unlike the Cardputer's, so a
 * capture here does not have to give anything up first.
 */
static const board_pin_t xiao_pins[] = {
    {"SD CLK", 7, NULL},
    {"SD MOSI", 9, NULL},
    {"SD MISO", 8, NULL},
    {"SD CS", 21, NULL},
    {"Mic PDM CLK", 42, "from the schematic, not yet confirmed here"},
    {"Mic PDM DATA", 41, "from the schematic, not yet confirmed here"},
};

static const board_t xiao = {
    .name = "xiao",
    .chip = "esp32s3",
    .description = "Seeed XIAO ESP32-S3 Sense: PDM microphone and microSD, no speaker",
    .pins = xiao_pins,
    .pin_count = ARRAY_COUNT(xiao_pins),
};

static const board_t *const boards[] = {&cardputer, &xiao};

/* ------------------------------------------------------------------ */
/* Shared behaviour                                                    */
/* ------------------------------------------------------------------ */

static bool chip_matches(const board_t *board)
{
    if (strcmp(board->chip, CONFIG_IDF_TARGET) == 0) {
        return true;
    }
    bp_error("The %s is an %s board and this firmware is built for %s",
             board->name, board->chip, CONFIG_IDF_TARGET);
    bp_printf("Rebuild with 'idf.py set-target %s'.\n", board->chip);
    return false;
}

static int show_pins(const board_t *board)
{
    bp_printf("%s - %s\n", board->name, board->description);
    bp_printf("Built for %s%s\n", board->chip,
              strcmp(board->chip, CONFIG_IDF_TARGET) == 0
                  ? "" : "  <- this firmware is built for a different chip");

    bp_printf("\n%-18s %5s  %s\n", "role", "gpio", "note");
    for (size_t i = 0; i < board->pin_count; i++) {
        /* A negative pin means the signal exists on the board but is not
         * brought out to a GPIO, which is worth listing rather than omitting:
         * "there is no enable pin" is an answer, and a blank row is not. */
        if (board->pins[i].pin < 0) {
            bp_printf("%-18s %5s  %s\n", board->pins[i].role, "-",
                      board->pins[i].note ? board->pins[i].note : "");
        } else {
            bp_printf("%-18s %5d  %s\n", board->pins[i].role, board->pins[i].pin,
                      board->pins[i].note ? board->pins[i].note : "");
        }
    }
    bp_printf("\n");
    return 0;
}

/*
 * Run a preset's command lines.
 *
 * bp_menu_execute() is called directly rather than bp_console_submit(): this
 * already runs on the executor task, and asking that task to wait for itself
 * to drain the queue would deadlock. Executing here is also strictly ordered,
 * so a line that depends on the previous one having succeeded can be stopped
 * on failure.
 *
 * Presets contain only fully qualified commands, never a bare menu name, so
 * running one cannot move the menu the user is sitting in.
 */
static int apply(const board_t *board, const char *const *lines, size_t count)
{
    if (!chip_matches(board)) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        char buffer[MAX_LINE_LEN];
        char *argv[MAX_ARGS];

        if (strlen(lines[i]) >= sizeof(buffer)) {
            bp_error("Preset line is too long: %s", lines[i]);
            return -1;
        }
        strcpy(buffer, lines[i]);

        bp_printf("> %s\n", lines[i]);

        size_t argc = esp_console_split_argv(buffer, argv, MAX_ARGS);
        if (argc == 0) {
            continue;
        }

        if (bp_menu_execute((int)argc, argv) != 0) {
            bp_error("Preset stopped at '%s'", lines[i]);
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_board_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bp_printf("%-12s %-9s %s\n", "board", "chip", "description");
    for (size_t i = 0; i < ARRAY_COUNT(boards); i++) {
        bp_printf("%-12s %-9s %s\n", boards[i]->name, boards[i]->chip,
                  boards[i]->description);
    }
    bp_printf("\nEnter a board name for its pinout, or 'board <name> <subsystem>' "
              "to set it up.\n");
    return 0;
}

int cmd_board_cardputer_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&cardputer);
}

int cmd_board_cardputer_audio(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The NS4168's SD pin is not broken out to a GPIO on this board, so the
     * amplifier is permanently enabled and there is no pin to give `init`.
     * Attaching it anyway is still worth doing: it records what part is on the
     * other end, and `audio info` then says so.
     *
     * Measured on hardware: the amplifier plays the RIGHT slot only, so
     * `audio tone 1000 3 left` is silent here and is not a fault.
     */
    static const char *const lines[] = {
        "audio bus 41 43 42",
        "audio ns4168 init",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_cardputer_mic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The microphone clock is the speaker's word-select line, so this preset
     * and the audio one are mutually exclusive. `audio pdm` makes that case
     * itself and names the fix, so nothing is pre-empted here; running the two
     * in either order gives a clear answer rather than a silent one.
     */
    static const char *const lines[] = {
        "audio pdm 43 46",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_cardputer_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd spi 40 14 39 12",
    };
    return apply(&cardputer, lines, ARRAY_COUNT(lines));
}

int cmd_board_xiao_pins(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return show_pins(&xiao);
}

int cmd_board_xiao_sd(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "sd spi 7 9 8 21",
    };
    return apply(&xiao, lines, ARRAY_COUNT(lines));
}

int cmd_board_xiao_mic(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char *const lines[] = {
        "audio pdm 42 41",
    };
    return apply(&xiao, lines, ARRAY_COUNT(lines));
}
