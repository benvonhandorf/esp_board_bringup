#ifndef CONSOLE_IO_H
#define CONSOLE_IO_H

#include <stdbool.h>

/*
 * Bring up the serial transport backing stdin/stdout so that blocking,
 * line-oriented reads work.
 *
 * Which peripheral this configures is decided at compile time from the
 * CONFIG_ESP_CONSOLE_* Kconfig choice, so the same source works on boards with
 * a native USB-Serial-JTAG port and on boards with a USB-to-UART bridge.
 */
void console_io_init(void);

/* Human readable name of the console transport, for the startup banner. */
const char *console_io_name(void);

/*
 * True when a host terminal is attached.
 *
 * This matters on native USB-Serial-JTAG: the board usually boots with nothing
 * plugged in, and probing the terminal for escape-sequence support before the
 * host attaches would fail and permanently drop the console into dumb mode.
 * Always true for a plain UART, where the peripheral is up from reset.
 */
bool console_io_host_connected(void);

#endif
