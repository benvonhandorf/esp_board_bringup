## ESP Board Bringup

This project is a CLI driven way to test any ESP series microcontroller board.  It uses the ESP command processing system to allow you to exercise different parts of the system directly, before a proper BSP is written.

The menu system and all outputs are visible on the primary serial port and a web interface hosted on the device.  The output of any command is sent to both interfaces, not just the one that entered it.

The device reaches that web interface either by joining an existing network or, when there is none to join, by hosting its own access point — which it does automatically at boot, so a board is usable over WiFi without a serial session at all.  See [WiFi](docs/wifi.md).

## Building and running

```sh
idf.py set-target esp32c3        # or esp32s3; first time only, and to switch
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

Project configuration lives in `sdkconfig.defaults`; `sdkconfig` itself is generated
and gitignored. Delete it or run `idf.py reconfigure` after editing the defaults.

Chip-specific settings live in `sdkconfig.defaults.<target>`, which ESP-IDF layers
on top of the shared file — flash size differs between the boards, and the ESP32-C3
has no SD host peripheral, so `sd mmc` is compiled out there. The target named in
`sdkconfig.defaults` is only the default guess used when no `sdkconfig` exists;
`idf.py set-target` overrides it.

One caveat when switching: `.vscode/settings.json` sets `IDF_TARGET` as an
environment variable, which outranks everything else and will fail the build with a
CMake cache mismatch. Update it to match, or build from a plain shell.

The console is the chip's **primary** serial device. On boards with a native
USB-Serial-JTAG port (which enumerate as `/dev/ttyACM*`) that must be
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`; a secondary console is output-only and
would never receive your keystrokes. Line editing, history and tab completion
come from linenoise and are enabled once a terminal is attached.

## Menus

Each menu below allows you to work with a particular subsystem.  You can either drill into a menu with the menu name or execute a command within that menu by prepending the menu name.  
e.g. `gpio` will push into the GPIO menu, while `gpio set 19 true` will turn GPIO pin 19 on as an output pin

All commands are case insensitive, but perfer lower case. Only the command
tokens are case folded — arguments such as WiFi passwords and UART payloads
keep their case exactly as typed.

You may leave any sub menu using any of the following commands: `back`, `exit`, `quit`

`help` lists the current menu; `help <menu>` lists another one. Commands report
failures as a line beginning with `ERR:`, so a host-side script can tell success
from failure without parsing prose.

### The menus

Each menu has its own page in **[docs/](docs/README.md)**:

| Menu | Covers | Submenus |
|---|---|---|
| [gpio](docs/gpio.md) | Digital and analog pin access. `<pin>` accepts `4`, `0-5` or `1,4,8-10` | `pwm` |
| [wifi](docs/wifi.md) | WiFi as a station or an access point, and throughput testing | |
| [i2c](docs/i2c.md) | I2C master | `ina237`, `sht4x`, `nau7802` |
| [uart](docs/uart.md) | Auxiliary UART (separate from this console) | |
| [spi](docs/spi.md) | SPI master | |
| [sd](docs/sd.md) | SD/MMC cards over SPI, 1-bit or 4-bit SD, with speed testing | |
| [audio](docs/audio.md) | Audio over I2S: tone, sweep, microphone capture and loopback testing | `nau8822`, `ns4168`, `sph0645` |
| [board](docs/board.md) | Known board pinouts and per-subsystem setup presets | `cardputer`, `xiao` |
| [system](docs/system.md) | Device information and control | |

On a board you do not know, start with [`gpio survey`](docs/gpio.md#survey-pin):
it measures every pin and reports what each one looks like before you have a
pinout. What has been verified against real hardware is tracked in
[HardwareSupport.md](HardwareSupport.md).

## Web interface

Once the device has an address, a single-page console is served on port 80 and
advertised over mDNS as `esp-bringup.local`. Commands typed in the browser
travel over a WebSocket at `/ws`.

That address comes from either mode. As a station it is whatever DHCP handed
out, reported by `wifi connect` and `wifi status`. As an access point it is
always `192.168.4.1`, and the console is up the moment the AP is — there is no
lease to wait for. If mDNS does not resolve (it often will not over an AP the
client just joined), use the numeric address.

Because `wifi autostart` runs at boot, a board with no reachable network is
serving this page over its own AP within a few seconds of power-up. See
[WiFi](docs/wifi.md) for the default SSID and password.

Both interfaces feed the same command queue and share one output fan-out, so
commands are executed one at a time and every line of output reaches the serial
port and every connected browser — regardless of where the command was entered.
