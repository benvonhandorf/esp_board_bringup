# Command reference

Every command the console accepts, one page per menu. The conventions that apply
everywhere — entering and leaving menus, case folding, `help`, the `ERR:` prefix
— are in the [project README](../README.md#menus).

| Menu | Covers | Submenus |
|---|---|---|
| [gpio](gpio.md) | Digital and analog pin access. `<pin>` accepts `4`, `0-5` or `1,4,8-10` | `pwm` |
| [wifi](wifi.md) | WiFi as a station or an access point, and throughput testing | |
| [i2c](i2c.md) | I2C master | `ina237`, `sht4x`, `nau7802` |
| [uart](uart.md) | Auxiliary UART (separate from this console) | |
| [spi](spi.md) | SPI master | |
| [sd](sd.md) | SD/MMC cards over SPI, 1-bit or 4-bit SD, with speed testing | |
| [audio](audio.md) | Audio over I2S: tone, sweep, microphone capture and loopback testing | `nau8822`, `ns4168`, `sph0645` |
| [board](board.md) | Known board pinouts and per-subsystem setup presets | `cardputer`, `xiao` |
| [system](system.md) | Device information and control | |

Descriptions match the one-liners the device itself prints for `help`; they are
the `.help` strings in `main/console/menu_table.c`.

## Where to start on an unknown board

**[Bringing up a new board](bringup.md)** is the guide: which tool to reach for
in which order, and what each stage is capable of proving. In short, `gpio
survey` first — it measures every pin and reports what each one looks like,
without you having to know the pinout. [`board list`](board.md#list) then tells
you whether this is a board the firmware already has a pinout for.

Which parts have been exercised against real hardware, and which are written but
unverified, is tracked in [HardwareSupport.md](../HardwareSupport.md).
