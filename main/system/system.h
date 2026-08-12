#ifndef SYSTEM_H
#define SYSTEM_H

#include "esp_chip_info.h"

int cmd_system_info(int argc, char** argv);
int cmd_system_reset(int argc, char** argv);
int cmd_system_lfxtal(int argc, char** argv);

/*
 * Human-readable chip name, e.g. "ESP32-S3". Shared with the SD module, which
 * stamps it into the benchmark results file so a saved result can be tied back
 * to the board it came from.
 */
const char *bp_chip_model_name(esp_chip_model_t model);

#endif
