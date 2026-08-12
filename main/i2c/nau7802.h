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

#endif
