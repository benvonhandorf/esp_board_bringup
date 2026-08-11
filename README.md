## ESP Board Bringup

This project is a CLI driven way to test any ESP series microcontroller board.  It uses the ESP command processing system to allow you to exercise different parts of the system directly, before a proper BSP is written.

The menu system and all outputs are visible on the primary serial port and a web interface hosted on the device, if it is connected to a network.  The output of any command is sent to both interfaces, not just the one that entered it.

## Building and running

```sh
idf.py set-target esp32c3        # first time only
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

Project configuration lives in `sdkconfig.defaults`; `sdkconfig` itself is generated
and gitignored. Delete it or run `idf.py reconfigure` after editing the defaults.

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

### GPIO

Commands in this menu take either a single pin number, a range of pins (e.g. `0-5`) or a comma separated list of pins as a <pin> parameter.  

#### `set <pin> <state>`

Sets the specified pin(s) to the requested state.  If pin is not already configured as an output pin, it will be configured at this time.

<state> options:
- true, 1, high: pin will be set high
- false, 0, low: pin will be set low

#### `read <pin>`

Returns a list of the specified pins and their logic levels.  If pin is not already configured as an input pin, it will be configured at this time.
Output will be one pin number per row, followed by a 0 or 1 indicating the logic level of the pin.

Sample: `gpio read 1,4` when GPIO 1 is low and GPIO 4 is high.
Output:
```
1: 0
4: 1
```

#### `aread <pin>`

Reads the analog value from the specified pin and returns a digital representation.
Both the raw ADC count and, where the chip carries the necessary calibration data
in eFuse, the equivalent voltage in millivolts are reported.

#### `blink <pin> <count> [period_ms]`

Blinks the specified pin LED count times for visual testing. The period defaults
to 500 ms. Accepts pin lists like the other GPIO commands.

#### PWM

Reached as `gpio pwm ...`, or by entering the `gpio` menu and then `pwm`.
Backed by LEDC; channels and timers are pooled, and outputs at the same
frequency share a timer.

##### `set <pin> <freq> <duty>`

Configures PWM on the specified pin with the given frequency (in Hz) and duty cycle (0-100%).
Re-running it on the same pin retunes that output in place. The duty resolution
is chosen to be the finest the clock allows at the requested frequency, and the
frequency actually achieved is reported (the timer divider quantizes it).

##### `stop <pin>`

Stops PWM output on the specified pin, leaving it low, and releases the channel.

### WiFi

This menu allows the user to perform actions on the wifi subsystem.

#### `scan`

Scans nearby wifi APs and outputs each AP including the RSSI and channel information for each.

#### `connect <AP> [Password]`

Connects to the specified access point.  Connection status and IP address are reported to the user, including any disconnections or changes in the future.
Disconnect reasons are decoded to readable text. Credentials are stored in NVS,
so the device reconnects on its own after a reset. Omit the password for an open
network.

#### `status`

Reports the current association — SSID, BSSID, RSSI, channel, security — plus
the IP address, gateway and netmask.

#### `iperf <server>[:<port>]`

Runs an iperf2 TCP test against the specified server, defaulting to port 5001.  Reports back throughput numbers.  Optionally, the user may specify `continuous` which will cause the test to run continually, reporting results every 5 seconds.

A continuous run is ended with `wifi iperf stop`. Because commands are executed
one at a time, the test runs in the background and the prompt stays usable while
it reports.

### I2C

#### `bus <scl> <sda>`

Initializes an I2C bus for use by future commands, specified by the `scl` and `sda` pins.

#### `scan`

Scans the I2C bus enumerates the found devices in a table with the hexadecimal least significant digits along each column and the most significant digits as rows.  If a device is not found, the intersection should be left blank.  If one is found, print the address in hex at the intersection.

If any errors are present that prevent the bus from being properly scanned, such as missing pull up resistors, print the state of the bus that prevents scanning.

#### `read <address> <bytes=1>`

Reads the specified number of bytes from the specified address on the bus.  If the number of bytes are not specified, one byte is read.

### UART

This is an auxiliary UART, always separate from the console port, so
initializing it can never take over the shell you are typing into.

#### `init <tx> <rx> <baud>`

Initializes a UART interface with the specified TX pin, RX pin, and baud rate.
Re-running it tears the old configuration down first, so pins and baud rate can
be changed freely.

#### `send <data>`

Sends the specified data string over the UART interface. Quote the argument to
send spaces (`send "hello world"`). The escapes `\n`, `\r`, `\t`, `\0`, `\\` and
`\xNN` are expanded, so devices expecting CR terminators can be driven directly.

#### `receive`

Receives and displays data from the UART interface, as a hex dump with an ASCII
column.

### SPI

#### `bus <clk> <mosi> <miso> [cs]`

Initializes an SPI bus with the specified clock, MOSI, and MISO pins.

The optional chip-select pin is an addition to the original specification:
without one the driver cannot select a peripheral, so reads return nothing but
bus noise. If it is omitted you must drive chip select yourself with `gpio set`.

#### `read <addr> <len>`

Reads the specified number of bytes from the given address on the SPI bus.
The address is transmitted as the first byte of a full-duplex transaction, and
the bytes clocked back during it are reported.

#### `write <addr> <data> [data...]`

Writes data to the specified address on the SPI bus. Data bytes may be given in
decimal or as `0x` hex.

### System

#### `info`

Displays system information including chip model, RAM, and flash size, plus the
silicon revision, station MAC, firmware and ESP-IDF versions, the reason for the
last reset, and uptime.

#### `reset`

Performs a soft reset of the device.

#### `lfxtal`

Attempt to configure the low frequency crystal oscillator.  Report the status of the oscillator.

The 32 kHz oscillator is enabled, given time to start, and then *measured* by
calibrating it against the main crystal. A dead oscillator makes that
calibration time out, which is what distinguishes "no crystal fitted" from a
working one — `rtc_clk_slow_freq_get_hz()` cannot be used here, as it returns a
nominal 32768 for whichever source is selected regardless of reality. Only if
the measurement is close to 32.768 kHz is the RTC slow clock actually switched
over to it; otherwise the previous clock source is left alone and the failure is
reported.

## Web interface

Once the device has an IP address, a single-page console is served on port 80
and advertised over mDNS as `esp-bringup.local`. Commands typed in the browser
travel over a WebSocket at `/ws`.

Both interfaces feed the same command queue and share one output fan-out, so
commands are executed one at a time and every line of output reaches the serial
port and every connected browser — regardless of where the command was entered.
