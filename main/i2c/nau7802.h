#ifndef NAU7802_H
#define NAU7802_H

int cmd_nau7802_init(int argc, char **argv);
int cmd_nau7802_gain(int argc, char **argv);
int cmd_nau7802_rate(int argc, char **argv);
int cmd_nau7802_read(int argc, char **argv);
int cmd_nau7802_tare(int argc, char **argv);
int cmd_nau7802_calibrate(int argc, char **argv);
int cmd_nau7802_weight(int argc, char **argv);
int cmd_nau7802_status(int argc, char **argv);
int cmd_nau7802_input(int argc, char **argv);
int cmd_nau7802_drdy(int argc, char **argv);
int cmd_nau7802_ldomode(int argc, char **argv);
int cmd_nau7802_pgacap(int argc, char **argv);
int cmd_nau7802_raw(int argc, char **argv);
int cmd_nau7802_registers(int argc, char **argv);

#endif
