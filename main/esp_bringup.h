#ifndef ESP_BRINGUP_H
#define ESP_BRINGUP_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"

/*
 * Parse a pin specification into a caller-owned array of pin numbers.
 *
 * Accepts a single pin ("4"), an inclusive range ("0-5"), or a comma separated
 * list of either ("1,4,8-10"). On success *pins must be free()d by the caller.
 * Returns 0 on success, -1 on a malformed specification (in which case *pins is
 * NULL and *count is 0).
 */
int parse_pin_list(const char *pin_str, int **pins, int *count);

/*
 * Parse a whole token as a decimal integer. Unlike atoi() this rejects
 * non-numeric input instead of quietly returning 0, so "gpio read foo" is an
 * error rather than a read of pin 0. Returns 0 on success, -1 otherwise.
 */
int parse_int_arg(const char *token, int *out);

/*
 * As parse_int_arg(), but a "0x"/"0X" prefix selects hexadecimal. Everything
 * else is decimal -- deliberately *not* strtol()'s base 0, under which a bare
 * I2C address like "08" would be an invalid octal constant.
 */
int parse_num_arg(const char *token, int *out);

#endif
