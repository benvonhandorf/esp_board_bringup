#ifndef I2C_H
#define I2C_H

#include "driver/i2c_master.h"

int cmd_i2c_bus(int argc, char** argv);
int cmd_i2c_scan(int argc, char** argv);
int cmd_i2c_read(int argc, char** argv);

#endif
