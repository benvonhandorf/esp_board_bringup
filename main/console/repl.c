#include "repl.h"

#include "console_io.h"
#include "esp_bringup.h"
#include "menu.h"
#include "output.h"

#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_console.h"
#include "linenoise/linenoise.h"

#include "sdkconfig.h"

#define MAX_LINE_LEN     256
#define MAX_ARGS         32
#define COMMAND_QUEUE_LEN 4

/* The executor runs every command, including I2C scans and (later) WiFi
 * operations, so it needs more stack than a typical worker. */
#define EXECUTOR_STACK 6144
#define READER_STACK   4096

typedef struct {
    char *line;                /* heap-allocated; the executor frees it */
    SemaphoreHandle_t done;    /* optional; given once the command completes */
} command_item_t;

static QueueHandle_t command_queue;

static void executor_task(void *arg)
{
    (void)arg;

    command_item_t item;
    char *argv[MAX_ARGS];

    while (xQueueReceive(command_queue, &item, portMAX_DELAY) == pdTRUE) {
        /* esp_console_split_argv() is the tokenizer from IDF's console
         * component: it understands quoting and backslash escapes, so
         * `uart send "hello world"` arrives as a single argument. It
         * rewrites the buffer in place. */
        size_t argc = esp_console_split_argv(item.line, argv, MAX_ARGS);

        if (argc > 0) {
            bp_menu_execute((int)argc, argv);
        }

        free(item.line);

        if (item.done) {
            xSemaphoreGive(item.done);
        }
    }
}

int bp_console_submit(const char *line, bool wait)
{
    if (!command_queue || !line) {
        return -1;
    }

    command_item_t item = {
        .line = strdup(line),
        .done = NULL,
    };
    if (!item.line) {
        return -1;
    }

    SemaphoreHandle_t done = NULL;
    if (wait) {
        done = xSemaphoreCreateBinary();
        if (!done) {
            free(item.line);
            return -1;
        }
        item.done = done;
    }

    if (xQueueSend(command_queue, &item, pdMS_TO_TICKS(5000)) != pdTRUE) {
        bp_error("Console busy, command dropped: %s", line);
        free(item.line);
        if (done) {
            vSemaphoreDelete(done);
        }
        return -1;
    }

    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
    }

    return 0;
}

/* Tab completion for the token currently being typed. */
static void completion_cb(const char *buf, linenoiseCompletions *lc)
{
    const char *token = strrchr(buf, ' ');
    token = token ? token + 1 : buf;
    size_t prefix_len = (size_t)(token - buf);

    const char *candidates[32];
    size_t count = bp_menu_complete(token, candidates, 32);

    for (size_t i = 0; i < count; i++) {
        char completed[MAX_LINE_LEN];
        snprintf(completed, sizeof(completed), "%.*s%s",
                 (int)prefix_len, buf, candidates[i]);
        linenoiseAddCompletion(lc, completed);
    }
}

static void print_banner(void)
{
    bp_printf("\n");
    bp_printf("ESP Board Bringup on %s (console: %s)\n",
              CONFIG_IDF_TARGET, console_io_name());
    bp_printf("Type 'help' for available commands. "
              "Enter a menu name to drill in; 'back' to leave.\n\n");
}

/*
 * Probe the attached terminal for escape-sequence support and greet it.
 *
 * Called once the host is known to be attached, and again after a reconnect:
 * on USB-Serial-JTAG the board typically boots with nothing plugged in, and a
 * probe sent into the void would strand the console in dumb mode for the rest
 * of the session.
 */
static void greet_terminal(void)
{
    linenoiseSetDumbMode(0);
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }

    print_banner();

    if (linenoiseIsDumbMode()) {
        bp_printf("Terminal does not support escape sequences; "
                  "line editing, history and completion are disabled.\n\n");
    }
}

static void reader_task(void *arg)
{
    (void)arg;

    linenoiseSetMultiLine(1);
    linenoiseSetMaxLineLen(MAX_LINE_LEN);
    linenoiseHistorySetMaxLen(32);
    linenoiseSetCompletionCallback(&completion_cb);

    bool greeted = false;

    while (1) {
        if (!console_io_host_connected()) {
            /* Host went away; re-greet whoever attaches next. */
            greeted = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if (!greeted) {
            greet_terminal();
            greeted = true;
        }

        char *line = linenoise(bp_menu_prompt());
        if (!line) {
            /* Empty line or read error; just re-prompt. */
            continue;
        }

        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            bp_console_submit(line, true);
        }

        linenoiseFree(line);
    }
}

void bp_console_start(void)
{
    bp_output_init();
    console_io_init();

    command_queue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_item_t));
    ESP_ERROR_CHECK(command_queue ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(executor_task, "bp_exec", EXECUTOR_STACK, NULL, 3, NULL);

    /* The banner is printed by the reader once a terminal is actually attached. */
    xTaskCreate(reader_task, "bp_repl", READER_STACK, NULL, 2, NULL);
}
