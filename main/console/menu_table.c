/*
 * The command tree. This is the single place where subsystems are wired into
 * the console; each module only exposes its cmd_* functions.
 */
#include "menu.h"

#include "gpio.h"
#include "pwm.h"
#include "i2c.h"
#include "ina237.h"
#include "sht4x.h"
#include "spi.h"
#include "system.h"
#include "uart.h"
#include "wifi.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const bp_command_t system_commands[] = {
    {"info", "", "Chip model, RAM, flash and firmware details", cmd_system_info},
    {"reset", "", "Soft-reset the device", cmd_system_reset},
    {"lfxtal", "", "Configure and report the 32kHz crystal", cmd_system_lfxtal},
};

static const bp_menu_t system_menu = {
    .name = "system",
    .help = "Device information and control",
    .commands = system_commands,
    .command_count = ARRAY_COUNT(system_commands),
};

static const bp_command_t gpio_commands[] = {
    {"set", "<pin> <state>", "Drive pin(s) high or low", cmd_gpio_set},
    {"read", "<pin>", "Read the logic level of pin(s)", cmd_gpio_read},
    {"aread", "<pin>", "Read pin(s) with the ADC", cmd_gpio_aread},
    {"blink", "<pin> <count> [ms]", "Blink pin(s) for visual identification", cmd_gpio_blink},
};

static const bp_command_t pwm_commands[] = {
    {"set", "<pin> <freq> <duty>", "Drive a pin with PWM (duty 0-100%)", cmd_pwm_set},
    {"stop", "<pin>", "Stop PWM and release the channel", cmd_pwm_stop},
};

static const bp_menu_t pwm_menu = {
    .name = "pwm",
    .help = "Hardware PWM via LEDC",
    .commands = pwm_commands,
    .command_count = ARRAY_COUNT(pwm_commands),
};

static const bp_menu_t *const gpio_submenus[] = {&pwm_menu};

static const bp_menu_t gpio_menu = {
    .name = "gpio",
    .help = "Digital and analog pin access. <pin> accepts 4, 0-5 or 1,4,8-10",
    .commands = gpio_commands,
    .command_count = ARRAY_COUNT(gpio_commands),
    .submenus = gpio_submenus,
    .submenu_count = ARRAY_COUNT(gpio_submenus),
};

static const bp_command_t i2c_commands[] = {
    {"bus", "<scl> <sda>", "Initialize the I2C bus on the given pins", cmd_i2c_bus},
    {"scan", "", "Probe the bus and tabulate responding devices", cmd_i2c_scan},
    {"read", "<address> [bytes]", "Read bytes from a device", cmd_i2c_read},
};

static const bp_command_t ina237_commands[] = {
    {"config", "<address> [ohms]", "Register a monitor (shunt defaults to 0.004 ohm)", cmd_ina237_config},
    {"read", "[address]", "Report bus voltage, current and power", cmd_ina237_read},
    {"list", "", "Show configured monitors and their calibration", cmd_ina237_list},
};

static const bp_menu_t ina237_menu = {
    .name = "ina237",
    .help = "TI INA237 current/voltage/power monitors at 0x40-0x4f",
    .commands = ina237_commands,
    .command_count = ARRAY_COUNT(ina237_commands),
};

static const bp_command_t sht4x_commands[] = {
    {"read", "[address] [high|medium|low]", "Measure temperature and humidity", cmd_sht4x_read},
    {"serial", "[address]", "Read the sensor serial number", cmd_sht4x_serial},
    {"heater", "[address] <mW> <ms>", "Pulse the heater, then measure", cmd_sht4x_heater},
    {"reset", "[address]", "Soft-reset the sensor", cmd_sht4x_reset},
};

static const bp_menu_t sht4x_menu = {
    .name = "sht4x",
    .help = "Sensirion SHT4x humidity/temperature sensors at 0x44-0x46",
    .commands = sht4x_commands,
    .command_count = ARRAY_COUNT(sht4x_commands),
};

static const bp_menu_t *const i2c_submenus[] = {&ina237_menu, &sht4x_menu};

static const bp_menu_t i2c_menu = {
    .name = "i2c",
    .help = "I2C master",
    .commands = i2c_commands,
    .submenus = i2c_submenus,
    .submenu_count = ARRAY_COUNT(i2c_submenus),
    .command_count = ARRAY_COUNT(i2c_commands),
};

static const bp_command_t uart_commands[] = {
    {"init", "<tx> <rx> <baud>", "Initialize the auxiliary UART", cmd_uart_init},
    {"send", "<data>", "Transmit a string", cmd_uart_send},
    {"receive", "", "Show data received since the last call", cmd_uart_receive},
};

static const bp_menu_t uart_menu = {
    .name = "uart",
    .help = "Auxiliary UART (separate from this console)",
    .commands = uart_commands,
    .command_count = ARRAY_COUNT(uart_commands),
};

static const bp_command_t spi_commands[] = {
    {"bus", "<clk> <mosi> <miso> [cs]", "Initialize the SPI bus", cmd_spi_bus},
    {"read", "<addr> <len>", "Read bytes from an address", cmd_spi_read},
    {"write", "<addr> <data>...", "Write bytes to an address", cmd_spi_write},
};

static const bp_menu_t spi_menu = {
    .name = "spi",
    .help = "SPI master",
    .commands = spi_commands,
    .command_count = ARRAY_COUNT(spi_commands),
};

static const bp_command_t wifi_commands[] = {
    {"scan", "", "List nearby access points", cmd_wifi_scan},
    {"connect", "<AP> [password]", "Join an access point", cmd_wifi_connect},
    {"status", "", "Show the current association and IP", cmd_wifi_status},
    {"iperf", "<server>[:<port>] [continuous]", "Measure throughput (iperf2 TCP)", cmd_wifi_iperf},
};

static const bp_menu_t wifi_menu = {
    .name = "wifi",
    .help = "Station-mode WiFi and throughput testing",
    .commands = wifi_commands,
    .command_count = ARRAY_COUNT(wifi_commands),
};

static const bp_menu_t *const root_submenus[] = {
    &system_menu,
    &gpio_menu,
    &wifi_menu,
    &i2c_menu,
    &uart_menu,
    &spi_menu,
};

const bp_menu_t bp_root_menu = {
    .name = "",
    .help = "ESP Board Bringup - exercise board peripherals before a BSP exists",
    .submenus = root_submenus,
    .submenu_count = ARRAY_COUNT(root_submenus),
};
