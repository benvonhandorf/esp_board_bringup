#ifndef SPI_H
#define SPI_H

#include "driver/spi_master.h"

int cmd_spi_bus(int argc, char** argv);
int cmd_spi_read(int argc, char** argv);
int cmd_spi_write(int argc, char** argv);

#endif
