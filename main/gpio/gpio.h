#ifndef GPIO_H
#define GPIO_H

int cmd_gpio_set(int argc, char** argv);
int cmd_gpio_read(int argc, char** argv);
int cmd_gpio_aread(int argc, char** argv);
int cmd_gpio_blink(int argc, char** argv);
int cmd_gpio_short(int argc, char** argv);
int cmd_gpio_rc(int argc, char** argv);

#endif
