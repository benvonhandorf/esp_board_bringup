## ESP Board Bringup

This project is a CLI driven way to test any ESP series microcontroller board.  It uses the ESP command processing system to allow you to exercise different parts of the system directly, before a proper BSP is written.

The menu system and all outputs are visible on the primary serial port and a web interface hosted on the device, if it is connected to a network.  The output of any command is sent to both interfaces, not just the one that entered it.

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

#### INA237

Reached as `i2c ina237 ...`, or by entering the `i2c` menu and then `ina237`.
Drives TI INA237 current/voltage/power monitors, which occupy addresses
`0x40`–`0x4f` depending on how their A0/A1 pins are strapped. Several can be
present on one bus, each with its own shunt resistor.

The shunt input range is left at the device default of ±163.84 mV. Combined
with the shunt resistance this fixes the current resolution: at the default
0.004 Ω the device reads ±40.96 A at 1.25 mA per count.

##### `config <address> [shunt_ohms]`

Registers a monitor and programs its calibration. The shunt resistance defaults
to **0.004 Ω**. Before accepting the device, `MANUFACTURER_ID` is read and must
report `0x5449`, so a wrong address or a different chip is reported rather than
silently producing plausible-looking numbers. Re-running on the same address
updates it in place.

Note that `MANUFACTURER_ID` is the only identification available: unlike the
INA238 and INA228, the INA237 has no `DEVICE_ID` register, so this check
confirms a TI part of this family but cannot distinguish the exact variant.

##### `read [address]`

Reads the measurement registers and reports bus voltage, current and power,
along with shunt voltage and die temperature. With no address, every configured
monitor is read. With an address that has not been configured, the monitor is
registered on the spot using the default shunt.

The `DIAG_ALRT` register is checked on every read: an arithmetic overflow
(`MATHOF`) or a trim-memory checksum error (`MEMSTAT`) is reported, because
either one means the reported values cannot be trusted. `SHUNT_CAL` is also
read back, so a device that has reset since it was configured is flagged
instead of reporting mis-scaled current.

##### `list`

Shows the configured monitors with their shunt resistance, current resolution
and full-scale range.

#### SHT4x

Reached as `i2c sht4x ...`. Drives Sensirion SHT4x humidity and temperature
sensors. The address is fixed by the part variant — `0x44` for the A variant
(such as the SHT40-AD1B), `0x45` for B and `0x46` for C — so every command takes
an optional address that defaults to **0x44**.

Unlike the INA237 the SHT4x has no registers. A command byte is written, the
sensor is given time to measure, and the result is read back in a separate
transaction; reading too early makes the sensor NACK. Each 16-bit value carries
its own CRC-8, which is checked on every read, so corrupted data is reported
rather than converted into a plausible-looking measurement.

##### `read [address] [high|medium|low]`

Measures temperature and relative humidity, reporting both in engineering units
along with the raw tick values. Repeatability defaults to `high`; lower settings
are faster and noisier. Humidity is cropped to the physical 0–100 %RH range, and
if cropping was necessary the uncropped value is shown too — during bringup a
wildly out-of-range reading is a signal, not noise.

##### `serial [address]`

Reads the sensor's 32-bit serial number. The SHT4x has no ID register, so a
serial number that reads back with valid CRCs is the available evidence that a
real sensor is responding.

##### `heater [address] <mW> <ms>`

Pulses the on-die heater and then reports the measurement the sensor takes just
before switching it off. Power is 20, 110 or 200 mW and duration is 100 or
1000 ms; only those six combinations exist in the device. Useful for driving off
condensation, and for confirming the part responds to a stimulus.

##### `reset [address]`

Issues a soft reset.

#### NAU7802

Reached as `i2c nau7802 ...`. Drives a Nuvoton NAU7802 24-bit bridge ADC as a
load cell front end. The address `0x2a` is fixed in silicon — there are no
address pins, so only one can be present per bus.

The normal bringup sequence is:

```
i2c nau7802 init            # power up, self-calibrate
i2c nau7802 gain 128        # typical for a load cell's few mV of output
i2c nau7802 tare            # with the scale empty
i2c nau7802 calibrate 100   # with a known 100-unit mass on it
i2c nau7802 weight          # thereafter, in those units
```

##### `init [ldo <volts>]`

Resets the device, powers up the digital then analog sections, waits for the
power-up ready flag, and runs the internal offset calibration.

By default AVDD is taken from the pin, which is the chip's own default.
Boards that rely on the internal regulator — many load cell breakouts do —
need `init ldo 3.0`. This is not the default deliberately: enabling the
internal regulator on a board that already drives AVDD would put two sources on
one net.

##### `gain [1..128]` and `rate [10|20|40|80|320]`

Show or set the PGA gain and conversion rate. Both re-run the internal offset
calibration afterwards, because either change alters the analog path and
invalidates the existing calibration — without that, readings come back
swinging across most of the full-scale range.

**320 SPS is not usable on the board this was developed against.** It returns
values spanning the entire range regardless of calibration, while 10–80 SPS are
rock steady; the likely cause is that 320 SPS needs an external crystal rather
than the internal RC oscillator. The command warns when you select it.

##### `read [samples]`, `tare [samples]`, `calibrate <known mass> [samples]`, `weight [samples]`

`read` reports the averaged raw count, the spread across the samples, and the
percentage of full scale. An absolute voltage is deliberately not reported: it
would depend on REFP−REFN, which this driver has no way to know.

`tare` captures the zero offset, `calibrate` derives the scale factor from a
known mass, and `weight` reports the load in whatever unit was used to
calibrate. `weight` also reports the sample spread converted into those units,
so every reading carries an indication of its own noise.

Two guards worth knowing about. Any reading pinned at full scale is reported as
a saturation error rather than a large number — during bringup that usually
means the bridge is disconnected, unexcited or miswired. And `calibrate`
refuses when the reading has not moved clear of the noise, since calibrating
against noise yields an absurd scale factor that silently corrupts every later
weight.

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

#### `free`

Releases the SPI host and the pins, without a reset. The SD menu competes for
the same host — on chips with only one general purpose SPI host, such as the
ESP32-C3, there would otherwise be no way to move from `spi bus` to `sd spi`
within a session.

### SD

Brings a card up over any of the three interfaces a board might have wired, and
measures how fast it actually goes. Which interface a board provides, and
whether the card keeps up, is exactly the kind of thing that has to be
established before a BSP exists.

Bring-up is deliberately in two stages. `sd spi` and `sd mmc` initialize the
card and nothing more; the filesystem is only mounted when `sd bench` needs one.
That way a blank, corrupt or non-FAT card still reports its identity through
`sd info` and is still measurable through `sd raw`, instead of the whole thing
unwinding because there was no FAT partition to mount.

**Neither benchmark writes to the card outside a filesystem.** `sd bench` creates
`/sd/bench.tmp`, reads it back and deletes it; `sd raw` is read-only. Nothing
that was on the card is disturbed.

#### `spi <clk> <mosi> <miso> <cs> [khz <freq>]`

Brings the card up over SPI. This is the only option on chips without an SD host
peripheral — notably the ESP32-C3, where `sd mmc` is not compiled in at all.

The frequency is a `khz` keyword pair rather than a trailing number because
`mmc` below takes a variable number of pins, and a bare number could not be told
apart from another pin. It defaults to 20 MHz; 40 MHz is the high-speed rate.
What the host divider actually produced is reported, since it quantizes.

Re-running the command tears the previous configuration down first, so pins and
frequency can be changed freely.

#### `mmc <clk> <cmd> <d0> [<d1> <d2> <d3>] [khz <freq>]`

Brings the card up on the dedicated SD host. Three pins select 1-bit mode, six
select 4-bit — the pin count is what picks the width, so there is no separate
and forgettable width argument.

The width is applied to the *slot*, not to the host flags. ESP-IDF reads the
slot width back and narrows the host to match, so setting the slot is what
actually stops a 1-bit slot from being switched to 4-bit partway through
initialization.

Internal pull-ups are enabled on the bus. They are weak and no substitute for
proper external ones, but a board being brought up frequently has none fitted at
all, and getting the card to answer is the point of the exercise.

Note that a card put into SPI mode by `sd spi` **stays** in SPI mode until its
power is removed; that is the card's behaviour, not this tool's. A board reset
does not do it. Test SD mode first, or physically power-cycle in between.

#### `info`

Reports the detected card: the interface and pins in use, card type, product
name, the CID (manufacturer, OEM, revision, serial and manufacture date),
capacity and sector geometry from the CSD, negotiated bus width, and the
filesystem's total and free space when one is mounted.

Two clocks are reported, not one. The first is what the host is really clocking
the card at, the second is the ceiling the card itself advertises — together
they say whether the interface or the card is the limit.

#### `bench [size_kb] [block_kb]`

Mounts FAT if it is not already mounted, then writes, reads back and deletes
`/sd/bench.tmp`. Defaults to 512 KB in 16 KB blocks.

The write measurement includes the final flush. Without it the card is still
absorbing the tail of the data when the clock stops, and the figure reported is
the speed of filling a RAM buffer rather than the speed of the card.

Every block is verified against what was written, and each carries its block
index, so a filesystem handing back the wrong block is reported rather than
scored. The comparison is done outside the timed region and is not charged to
the card.

#### `raw [size_kb] [block_kb] [start_sector]`

Reads whole sectors straight off the card with no filesystem in the way. The
gap between this and `bench` is what FAT costs.

It is read-only, so it is safe to run anywhere on the card. Blocks are rounded
down to whole sectors and the range is checked against the card's capacity.

#### `sweep [max_khz] [size_kb] [block_kb]`

Steps the clock up and reports the fastest rate the card still returns *correct*
data at. Defaults to a ceiling of 80 MHz and 512 KiB read per step.

This exists because an overclocked card usually does not fail — it succeeds and
hands back corrupt data. A sweep that only checked for errors would report a
confident, entirely wrong answer. So the sweep first reads the test region at
20 MHz, a rate every card is rated for, and keeps a CRC32 of it; every faster
step re-reads the same sectors and compares. A CRC rather than a kept copy
because it costs four bytes instead of a second buffer, which is what lets the
verified region be big enough to mean something.

The reference is read **twice** and the two must agree. A card that cannot
reproduce its own data in spec makes every later comparison meaningless, and
that is worth saying outright rather than deriving an overclocking limit from
noise.

It is **read-only**. Nothing is written to the card at a clock that has not been
verified, because a corrupt write is not recoverable the way a corrupt read is.

Two things the report is careful to distinguish:

- **The card failed** — a data mismatch, a read error, or an init failure. The
  first such rate is reported.
- **The host ran out of clock.** The dividers quantize, so several requested
  rates land on the same actual one; that is normal and those steps are skipped.
  But when *every* larger request produces an identical clock, the card was
  never driven any faster and its real limit is still unknown. That is reported
  as a host limitation, not as a pass.

Steps that pass above the card's CSD-rated speed are marked `(overclocked)`.
Passing one read sweep is not a stability guarantee — it is out of spec, and the
margin varies with temperature, supply and wiring.

Afterwards the card is left initialized at the fastest verified rate, so
`sd bench` and `sd raw` measure it without re-entering anything.

#### `close`

Unmounts, releases the card and frees the bus.

#### Reported numbers

Both benchmarks report the average throughput **and the slowest single block**.
The worst case is not a footnote: SD cards stall for tens of milliseconds while
they do internal housekeeping, and on a board that has to keep up with a sensor
or a camera that stall is what decides whether the design works. An average
hides it entirely.

Sanity-check the result against the interface ceiling — at the 20 MHz default
that is 2.5 MB/s on SPI or 1-bit SD, and 10 MB/s on 4-bit SD, scaling with the
clock. A number above the ceiling means something was measured other than the
card.

Measured on the board this was developed against, a XIAO ESP32-S3 Sense with a
4 GB SDHC card (`SD04G`, rated 25 MHz), 512 KiB in 16 KiB blocks:

| | SPI @ 20 MHz | SD 1-bit @ 20 MHz | SD 1-bit @ 40 MHz |
|---|---|---|---|
| Raw read | 1.45 MiB/s | 2.20 MiB/s | 4.19 MiB/s |
| FAT read | 1.43 MiB/s | 2.19 MiB/s | 4.08 MiB/s |
| FAT write | 0.39 MiB/s | 0.39 MiB/s | 1.43 MiB/s |
| Worst write block | 27.7 ms | 849.5 ms | 33.7 ms |

`sd sweep` found the limits to be 20 MHz over SPI and 40 MHz over SD 1-bit. Both
are worth understanding, because neither is the card:

- **Over SPI it stops at 20 MHz.** At 24 MHz and above the high-speed switch
  fails re-reading the CSD and the bus starts reporting command CRC errors. The
  pins used here (7, 8, 9) are not the ESP32-S3's IOMUX pins for SPI2, so the
  signals go through the GPIO matrix and the extra input delay is what runs out.
- **Over SD 1-bit it stops at 40 MHz** — but that is the host clamping, not the
  card. Every request above 40 MHz produced exactly 40.000 MHz, so the card was
  never actually driven faster and 40 MHz is not known to be its limit. It was
  still 15 MHz above its rated 25 MHz and read correctly throughout.

Note the 849.5 ms worst-case write block at 20 MHz, against 33.7 ms for the same
card at 40 MHz. That is not the clock — it is the card pausing for internal
housekeeping, landing in one run and not the other, and it is large enough to
drag the whole 20 MHz write average below the SPI one. It is precisely why the
worst block is reported.

#### Example: Seeed XIAO ESP32-S3 Sense

The Sense expansion board's microSD is on CLK/SCK `GPIO7`, CMD/MOSI `GPIO9`,
D0/MISO `GPIO8` and CS `GPIO21`:

```
sd spi 7 9 8 21     # SD over SPI
sd close
sd mmc 7 9 8        # SD 1-bit on the same pins
```

D1, D2 and D3 are not brought out on that board, so 4-bit mode cannot be
exercised there even though the command supports it.

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
