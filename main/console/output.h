#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdarg.h>
#include <stddef.h>

/*
 * Command output fan-out.
 *
 * The README requires that "the output of any command is sent to both
 * interfaces, not just the one that entered it". Command modules therefore
 * never call printf() directly: they call bp_printf(), which writes to the
 * local console *and* to every registered sink. The web interface registers a
 * sink that forwards to its WebSocket clients.
 */

/* Receives a chunk of already-formatted output. Must not call bp_printf(). */
typedef void (*bp_output_sink_fn)(const char *text, size_t len, void *ctx);

/* Set up the output mutex and redirect ESP_LOGx through the same fan-out. */
void bp_output_init(void);

/* Returns 0 on success, -1 if the sink table is full. */
int bp_output_sink_register(bp_output_sink_fn fn, void *ctx);
void bp_output_sink_unregister(bp_output_sink_fn fn, void *ctx);

/* printf() for command output. */
void bp_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void bp_vprintf(const char *fmt, va_list args);

/* Emit a pre-formatted buffer without going through printf formatting. */
void bp_write(const char *text, size_t len);

/*
 * Report a command failure in a uniform, machine-greppable way, so a host-side
 * script driving the board can tell success from failure without parsing prose.
 */
void bp_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
