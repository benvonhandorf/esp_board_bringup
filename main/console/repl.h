#ifndef REPL_H
#define REPL_H

#include <stdbool.h>

/*
 * Start the console: output fan-out, the command executor task, and the
 * interactive reader on the serial port.
 */
void bp_console_start(void);

/*
 * Queue a command line for execution.
 *
 * Every interface funnels through here, so commands from the serial console and
 * from the web interface are executed one at a time by a single task and their
 * output cannot interleave. Pass wait=true to block until the command has
 * finished (what the serial reader wants, so the next prompt appears after the
 * output rather than before it).
 *
 * Returns 0 if the line was queued.
 */
int bp_console_submit(const char *line, bool wait);

#endif
