#include "output.h"

#include "esp_bringup.h"

#include "freertos/semphr.h"

#define MAX_SINKS 4

/* Output is formatted once into this buffer, then handed to stdout and every
 * sink, so all interfaces see byte-identical text. Guarded by output_mutex. */
#define FORMAT_BUF_SIZE 512

typedef struct {
    bp_output_sink_fn fn;
    void *ctx;
} sink_t;

static sink_t sinks[MAX_SINKS];
static size_t sink_count;
static SemaphoreHandle_t output_mutex;
static char format_buf[FORMAT_BUF_SIZE];

/* ESP_LOGx output is routed here so log lines reach the web interface too. */
static int log_vprintf(const char *fmt, va_list args)
{
    bp_vprintf(fmt, args);
    return 0;
}

void bp_output_init(void)
{
    if (output_mutex) {
        return;
    }
    output_mutex = xSemaphoreCreateRecursiveMutex();
    esp_log_set_vprintf(log_vprintf);
}

int bp_output_sink_register(bp_output_sink_fn fn, void *ctx)
{
    if (sink_count == MAX_SINKS) {
        return -1;
    }

    xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    sinks[sink_count].fn = fn;
    sinks[sink_count].ctx = ctx;
    sink_count++;
    xSemaphoreGiveRecursive(output_mutex);
    return 0;
}

void bp_output_sink_unregister(bp_output_sink_fn fn, void *ctx)
{
    xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    for (size_t i = 0; i < sink_count; i++) {
        if (sinks[i].fn == fn && sinks[i].ctx == ctx) {
            sinks[i] = sinks[sink_count - 1];
            sink_count--;
            break;
        }
    }
    xSemaphoreGiveRecursive(output_mutex);
}

void bp_write(const char *text, size_t len)
{
    if (len == 0) {
        return;
    }

    /* bp_output_init() may not have run yet during very early startup. */
    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    fwrite(text, 1, len, stdout);

    for (size_t i = 0; i < sink_count; i++) {
        sinks[i].fn(text, len, sinks[i].ctx);
    }

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }
}

void bp_vprintf(const char *fmt, va_list args)
{
    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    int len = vsnprintf(format_buf, sizeof(format_buf), fmt, args);
    if (len > 0) {
        /* vsnprintf() reports the length it *would* have written. */
        size_t written = (size_t)len < sizeof(format_buf) ? (size_t)len
                                                          : sizeof(format_buf) - 1;
        bp_write(format_buf, written);
    }

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }
}

void bp_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    bp_vprintf(fmt, args);
    va_end(args);
}

void bp_error(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    if (output_mutex) {
        xSemaphoreTakeRecursive(output_mutex, portMAX_DELAY);
    }

    bp_printf("ERR: ");
    bp_vprintf(fmt, args);
    bp_printf("\n");

    if (output_mutex) {
        xSemaphoreGiveRecursive(output_mutex);
    }

    va_end(args);
}
