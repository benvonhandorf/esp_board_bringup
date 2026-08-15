/*
 * The command tree. This is the single place where subsystems are wired into
 * the console; each module only exposes its cmd_* functions.
 */
#include "menu.h"

#include "audio.h"
#include "board.h"
#include "codec_nau8822.h"
#include "codec_ns4168.h"
#include "codec_sph0645.h"
#include "gpio.h"
#include "pwm.h"
#include "i2c.h"
#include "ina237.h"
#include "sht4x.h"
#include "nau7802.h"
#include "sd.h"
#include "spi.h"
#include "system.h"
#include "touch.h"
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
    {"read", "<pin> [up|down|none]", "Read the logic level of pin(s)", cmd_gpio_read},
    {"aread", "<pin>", "Read pin(s) with the ADC", cmd_gpio_aread},
    {"blink", "<pin> <count> [ms]", "Blink pin(s) for visual identification", cmd_gpio_blink},
    {"short", "<pin>", "Find pins shorted together, adjacent pairs first", cmd_gpio_short},
    {"rc", "<pin> [ref <pin> <kohms>]", "Measure each net's pull-up strength and capacitance", cmd_gpio_rc},
    {"survey", "[pin]", "Census every pin: pull-ups, driven lines, and what they suggest", cmd_gpio_survey},
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

static const bp_command_t nau7802_commands[] = {
    {"init", "[ldo <volts>]", "Power up, configure and self-calibrate", cmd_nau7802_init},
    {"status", "", "Show configuration and calibration state", cmd_nau7802_status},
    {"gain", "[1..128]", "Show or set the PGA gain", cmd_nau7802_gain},
    {"rate", "[10|20|40|80|320]", "Show or set the sample rate in SPS", cmd_nau7802_rate},
    {"read", "[samples]", "Averaged raw ADC counts", cmd_nau7802_read},
    {"tare", "[samples]", "Capture the zero offset with no load", cmd_nau7802_tare},
    {"calibrate", "<known mass> [samples]", "Derive the scale from a known mass", cmd_nau7802_calibrate},
    {"weight", "[samples]", "Report the load in calibrated units", cmd_nau7802_weight},
};

static const bp_menu_t nau7802_menu = {
    .name = "nau7802",
    .help = "Nuvoton NAU7802 24-bit bridge ADC / load cell at 0x2a",
    .commands = nau7802_commands,
    .command_count = ARRAY_COUNT(nau7802_commands),
};

static const bp_menu_t *const i2c_submenus[] = {&ina237_menu, &sht4x_menu, &nau7802_menu};

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
    {"free", "", "Release the SPI host so another module can use it", cmd_spi_free},
};

static const bp_menu_t spi_menu = {
    .name = "spi",
    .help = "SPI master",
    .commands = spi_commands,
    .command_count = ARRAY_COUNT(spi_commands),
};

static const bp_command_t sd_commands[] = {
    {"spi", "<clk> <mosi> <miso> <cs> [cd <pin>] [khz <freq>]", "Bring a card up over SPI", cmd_sd_spi},
    {"mmc", "<clk> <cmd> <d0> [<d1> <d2> <d3>] [cd <pin>] [khz <freq>]", "Bring a card up in 1-bit or 4-bit SD mode", cmd_sd_mmc},
    {"info", "", "Report the detected card", cmd_sd_info},
    {"bench", "[size_kb] [block_kb]", "Measure write and read speed through FAT", cmd_sd_bench},
    {"raw", "[size_kb] [block_kb] [start_sector]", "Measure read speed with no filesystem", cmd_sd_raw},
    {"sweep", "[max_khz] [size_kb] [block_kb]", "Find the fastest clock the card reads correctly at", cmd_sd_sweep},
    {"results", "[clear]", "Show or delete the saved results file", cmd_sd_results},
    {"close", "", "Unmount, release the card and free the bus", cmd_sd_close},
};

static const bp_menu_t sd_menu = {
    .name = "sd",
    .help = "SD/MMC cards over SPI, 1-bit or 4-bit SD, with speed testing",
    .commands = sd_commands,
    .command_count = ARRAY_COUNT(sd_commands),
};

static const bp_command_t nau8822_commands[] = {
    {"init", "[address]", "Power up and configure the codec (0x1a default)", cmd_nau8822_init},
    {"status", "", "Show identity, routing and volume", cmd_nau8822_status},
    {"reg", "<n> [value]", "Read or write a 9-bit register", cmd_nau8822_reg},
    {"route", "<hp|speaker|both>", "Choose which outputs are driven", cmd_nau8822_route},
    {"input", "<mic|line|off> [boost]", "Choose which input reaches the ADC", cmd_nau8822_input},
    {"gain", "[db]", "Show or set the analog gain on the selected input", cmd_nau8822_gain},
};

static const bp_menu_t nau8822_menu = {
    .name = "nau8822",
    .help = "Nuvoton NAU8822 stereo codec with speaker driver, I2C at 0x1a/0x1b",
    .commands = nau8822_commands,
    .command_count = ARRAY_COUNT(nau8822_commands),
};

static const bp_command_t ns4168_commands[] = {
    {"init", "[sd <pin>]", "Attach the amplifier and enable it", cmd_ns4168_init},
    {"status", "", "Show the enable pin and its state", cmd_ns4168_status},
};

static const bp_menu_t ns4168_menu = {
    .name = "ns4168",
    .help = "NS4168 mono I2S class-D amplifier (no control bus)",
    .commands = ns4168_commands,
    .command_count = ARRAY_COUNT(ns4168_commands),
};

static const bp_command_t audio_commands[] = {
    {"bus", "<bclk> <ws> <dout> [din <pin>] [mclk <pin>] [rate <hz>] [bits <n>]",
     "Initialize I2S and start its clocks", cmd_audio_bus},
    {"pdm", "<clk> <data> [rate <hz>]", "Receive from a PDM microphone", cmd_audio_pdm},
    {"info", "", "Show pins, format, clocks and the attached codec", cmd_audio_info},
    {"codecs", "", "List the parts this firmware can drive", cmd_audio_codecs},
    {"tone", "<hz> [seconds|continuous] [level <pct>] [left|right|both]",
     "Play a sine tone", cmd_audio_tone},
    {"sweep", "<start_hz> <end_hz> [seconds] [level <pct>] [log|linear]",
     "Sweep a sine tone between two frequencies", cmd_audio_sweep},
    {"stop", "", "End a continuous tone", cmd_audio_stop},
    {"record", "[seconds]", "Capture the input and report levels and a spectrum",
     cmd_audio_record},
    {"level", "[seconds]", "Live input level meter", cmd_audio_level},
    {"loopback", "[hz] [seconds] [level <pct>]",
     "Play a tone and measure whether the input hears it", cmd_audio_loopback},
    {"volume", "[pct]", "Show or set the codec's output volume", cmd_audio_volume},
    {"mute", "[on|off]", "Mute or unmute the codec", cmd_audio_mute},
    {"close", "", "Detach the codec and release I2S", cmd_audio_close},
};

static const bp_command_t sph0645_commands[] = {
    {"init", "[left|right] [sel <pin>]", "Attach the microphone and check the bus suits it", cmd_sph0645_init},
    {"status", "", "Show the slot, clock and what the part guarantees", cmd_sph0645_status},
};

static const bp_menu_t sph0645_menu = {
    .name = "sph0645",
    .help = "Knowles SPH0645LM4H-B I2S MEMS microphone (no control bus)",
    .commands = sph0645_commands,
    .command_count = ARRAY_COUNT(sph0645_commands),
};

static const bp_menu_t *const audio_submenus[] = {&nau8822_menu, &ns4168_menu,
                                                  &sph0645_menu};

static const bp_menu_t audio_menu = {
    .name = "audio",
    .help = "Audio over I2S: tone, sweep, microphone capture and loopback testing",
    .commands = audio_commands,
    .command_count = ARRAY_COUNT(audio_commands),
    .submenus = audio_submenus,
    .submenu_count = ARRAY_COUNT(audio_submenus),
};

static const bp_command_t cardputer_commands[] = {
    {"pins", "", "Show the known pinout", cmd_board_cardputer_pins},
    {"audio", "", "Set up I2S and the NS4168 speaker amplifier", cmd_board_cardputer_audio},
    {"mic", "", "Open the SPM1423 PDM microphone (releases the speaker)", cmd_board_cardputer_mic},
    {"sd", "", "Bring the microSD slot up over SPI", cmd_board_cardputer_sd},
};

static const bp_menu_t cardputer_menu = {
    .name = "cardputer",
    .help = "M5Stack Cardputer (esp32s3)",
    .commands = cardputer_commands,
    .command_count = ARRAY_COUNT(cardputer_commands),
};

static const bp_command_t xiao_commands[] = {
    {"pins", "", "Show the known pinout", cmd_board_xiao_pins},
    {"mic", "", "Open the PDM microphone", cmd_board_xiao_mic},
    {"sd", "", "Bring the microSD slot up over SPI", cmd_board_xiao_sd},
};

static const bp_menu_t xiao_menu = {
    .name = "xiao",
    .help = "Seeed XIAO ESP32-S3 Sense (esp32s3)",
    .commands = xiao_commands,
    .command_count = ARRAY_COUNT(xiao_commands),
};

static const bp_command_t board_commands[] = {
    {"list", "", "List the boards this firmware knows", cmd_board_list},
};

static const bp_menu_t *const board_submenus[] = {&cardputer_menu, &xiao_menu};

static const bp_menu_t board_menu = {
    .name = "board",
    .help = "Known board pinouts and per-subsystem setup presets",
    .commands = board_commands,
    .command_count = ARRAY_COUNT(board_commands),
    .submenus = board_submenus,
    .submenu_count = ARRAY_COUNT(board_submenus),
};

static const bp_command_t touch_commands[] = {
    {"watch", "<pads> [seconds]",
     "Calibrate touch pads and report all of them, live", cmd_touch_watch},
};

static const bp_menu_t touch_menu = {
    .name = "touch",
    .help = "Capacitive touch pads: calibrate a set and watch them live",
    .commands = touch_commands,
    .command_count = ARRAY_COUNT(touch_commands),
};

static const bp_command_t wifi_commands[] = {
    {"scan", "", "List nearby access points", cmd_wifi_scan},
    {"connect", "<AP> [password] [channel] [bssid]", "Join an access point", cmd_wifi_connect},
    {"ap", "<SSID> [password] [channel]", "Host an access point (or 'ap stop')", cmd_wifi_ap},
    {"autostart", "", "Join the stored network, or host an access point", cmd_wifi_autostart},
    {"status", "", "Show the current association and IP", cmd_wifi_status},
    {"iperf", "<server>[:<port>] [continuous]", "Measure throughput (iperf2 TCP)", cmd_wifi_iperf},
    {"netstats", "", "Show lwIP protocol drop/error counters", cmd_wifi_netstats},
};

static const bp_menu_t wifi_menu = {
    .name = "wifi",
    .help = "WiFi as a station or an access point, and throughput testing",
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
    &sd_menu,
    &audio_menu,
    &touch_menu,
    &board_menu,
};

const bp_menu_t bp_root_menu = {
    .name = "",
    .help = "ESP Board Bringup - exercise board peripherals before a BSP exists",
    .submenus = root_submenus,
    .submenu_count = ARRAY_COUNT(root_submenus),
};
