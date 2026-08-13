## ESP Board Bringup

This project is a CLI driven way to test any ESP series microcontroller board.  It uses the ESP command processing system to allow you to exercise different parts of the system directly, before a proper BSP is written.

The menu system and all outputs are visible on the primary serial port and a web interface hosted on the device.  The output of any command is sent to both interfaces, not just the one that entered it.

The device reaches that web interface either by joining an existing network or, when there is none to join, by hosting its own access point — which it does automatically at boot, so a board is usable over WiFi without a serial session at all.  See [WiFi](#wifi).

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

#### `read <pin> [up|down|none]`

Returns a list of the specified pins and their logic levels.  If pin is not already configured as an input pin, it will be configured at this time.
Output will be one pin number per row, followed by a 0 or 1 indicating the logic level of the pin.

The optional internal pull defaults to `none`, which is what a bare `read` has
always done. `up` is the useful one during bringup: without a pull, a pin that is
driven low and a pin that is connected to nothing both tend to read 0. With the
pull-up fighting it, anything still reading 0 is genuinely being held down.

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

#### `short <pin>`

Finds pins that are shorted together. Give it the pins to test — the same
`4`, `0-5` or `1,4,8-10` forms as every other GPIO command — and it drives each
one low in turn while reading the others with pull-ups enabled. A pin that
follows another down shares a net with it.

**Adjacent pins are tested and reported first.** A solder bridge at the package
is far and away the most common way two nets get tied together, and neighbouring
GPIO numbers are usually neighbouring pads, so that is the answer worth seeing
before a wall of results. (Usually, not always — GPIO numbering does not track
the package outline exactly, so the full matrix follows.)

A short is only reported when **both** pins pull each other down. The symmetry
requirement matters: a pin held low by the board — a grounded net, a card-detect
switch, an output driving low — follows everything and fights nothing, and a
naive rule reports it as shorted to every pin in the list. Those are listed
separately as untestable instead.

Pins that must never be driven are skipped with the reason given: flash and PSRAM
pins, the USB or UART console pins, and anything a peripheral currently holds.
The first group would hang the chip and the second would cut off the connection
these results are printed to; ESP-IDF's own `esp_gpio_is_reserved()` supplies the
list, so it stays right per chip and per board configuration.

Because it drives every pin listed, do not run it against a bus another device
may be driving at the same time. Pins are left as passive inputs afterwards.

Sample, on a board where D0 and SCK of an SD slot were bridged at the pads:

```
> gpio short 38-44
Adjacent pins (where a solder bridge usually lands):
  GPIO 39 <-> GPIO 40   SHORTED
Remaining pairs:
  15 pairs tested, all clear
Held low whatever is driven, so not testable and not a short:
  GPIO 44
```

That short stalled SD card init in a way that looked nothing like a wiring
fault — the host never got its clock started, because it waits for the data bus
to go idle and every CLK low was dragging D0 down with it.

#### `rc <pin> [ref <pin> <kohms>]`

Measures the **pull-up strength and capacitance of each net**. `short` answers
"are these two nets the same net"; this answers "is this net pulled up, how
hard, and how much is hanging off it" — which a logic read cannot, because a
10k pull-up to a healthy rail and a 10k pull-up whose far end is floating both
read as `1`.

It drives the pin low, releases it, and times the rise. The net charges through
whatever pulls it up, so the rise is R×C. Measuring twice, the second time with
the internal pull-up (~45k) added in parallel, gives two equations for the two
unknowns and uses the on-chip pull-up as the reference:

```
t_ext = k·R_ext·C        t_par = k·(R_ext ‖ R_int)·C
t_ext / t_par = (R_ext + R_int) / R_int
```

so `R_ext = R_int · (t_ext/t_par − 1)`, and `C` follows. No meter needed.

**The internal pull-up is the one thing that cannot be measured from the
inside**, and every result scales with it. Nominally 45k, it varies widely with
process and temperature — on the ESP32-S3 measured here it is actually 35k. So
uncalibrated results are worth about a factor of two, which is still enough to
separate the cases this exists for: a signal net is single-digit pF, a net tied
to an unpowered rail's decoupling capacitor is tens of nF.

**Ratios between pins are exact regardless**, because the threshold and the
capacitance cancel in `t_ext/t_par`. Two pins that differ by 2× really do differ
by 2×, even when neither absolute value can be trusted. Read the table that way
before reading the numbers.

To get absolute accuracy, name a pin whose pull-up you know and the measurement
runs backwards to solve for `R_int`, which then applies to every other pin:

```
> gpio rc 38-44 ref 41 50
Internal pull-up measured as 34.9k against the 50 reference on GPIO 41.
```

A good reference is a net with a known fitted resistor and nothing else on it.

Timing is done by releasing the pad and sampling it once at a chosen delay,
binary-searching for the crossing, with the delay-to-sample cost calibrated out
by driving the same pin push-pull. Polling `gpio_get_level()` in a loop instead
only resolves one iteration — about 290 ns here, the same order as the rise
being measured, which quantises every net to the same two numbers.

Sample, on the SD slot of an ESP32-S3 board with 50k pull-ups fitted on CMD
and DAT0–3, calibrated against one of them, with a card in the slot:

```
> gpio rc 38-44 ref 41 50
Internal pull-up measured as 34.9k against the 50 reference on GPIO 41.

 pin    external   with int    pull-up      net C
  38      650 ns      250 ns    55.9 k      8.4 pF     D1
  39      650 ns      269 ns    49.5 k      9.5 pF     D0
  40        none      462 ns       none      9.6 pF    CLK
  41      650 ns      275 ns    47.6 k      9.8 pF     CMD
  42      369 ns      200 ns    29.5 k      9.0 pF     D3
  43      650 ns      238 ns    60.7 k      7.7 pF     D2
  44          --         --         --         --      DET
```

Reading that: **CLK correctly has no pull-up** — the host always drives it
push-pull, and a pull-up there would be a design error. **DET is held low** by
the card-detect switch, which is how you know a card is seated. CMD and DAT0–2
average 53k against a fitted 50k, so the scatter is the measurement, not the
board.

**DAT3 at 29.5k is the interesting one, and it is correct.** Solving
`50k ‖ X = 29.5k` gives 72k, which is the pull-up *inside the card* used for
card detection before the host knows anything about the card — the SD spec puts
it at 10–90k. Every SD slot with a card in it should show DAT3 at roughly half
its neighbours. Pull the card and it rises to match them.

Every net is 8–10 pF: a bare pin and a short trace, nothing unexpected hanging
off the bus.

Holding the other lines low with `gpio set` and re-measuring one of them is a
way to ask whether a rail is real: if a net's pull-up is being fed parasitically
through its neighbours, collapsing them changes the answer. If it does not
budge, the rail is genuinely sourced.

Same cautions as `short`: it drives the pin, so nothing else may be driving it,
and interrupts are masked for up to 5 ms per sample.

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

The radio is either a station or an access point, never both: a single radio
cannot sit on two channels, and an AP+station setup would silently drag the AP
onto whatever channel the station associated on. `connect` therefore stops a
running AP and `ap` drops a station association, each saying so as it happens.

#### `scan`

Scans nearby wifi APs and outputs each AP including the RSSI and channel information for each.

Scanning is a station-mode operation, so it is refused while an access point is
running; stop it with `wifi ap stop` first.

#### `connect <AP> [Password]`

Connects to the specified access point.  Connection status and IP address are reported to the user, including any disconnections or changes in the future.
Disconnect reasons are decoded to readable text. Credentials are stored in NVS,
so `autostart` rejoins the same network after a reset. Omit the password for an
open network. A running access point is stopped first.

#### `ap <SSID> [password] [channel]`

Hosts an access point so a laptop or phone can join the board directly and reach
the web interface with no network in the middle — which is the point on a bench
where there is no usable AP to join.

The password is optional; omit it for an open network. WPA2 requires at least 8
characters, and the SSID is limited to 32. The channel defaults to 1 and must be
1–13. Because the password is positional, an open network on a specific channel
is spelled with an empty password: `wifi ap bench "" 6`.

Clients get addresses over DHCP from the device, which is `192.168.4.1`. The web
console starts as soon as the AP is up — unlike station mode there is no lease to
wait for — so `http://192.168.4.1/` is live immediately. Clients joining and
leaving are reported as they happen, with their MAC address and AID.

#### `ap stop`

Stops the access point and returns the radio to station mode. The web server
stays bound and becomes reachable again as soon as a station association
provides an address.

Note that stopping the AP disconnects any client using it — including the
browser you typed the command into, if you reached the web console that way.

#### `autostart`

Joins the network stored in NVS, or hosts an access point when there is none.

This runs automatically at boot, so a board is reachable over the web interface
without a serial session at all. A board that has been through `wifi connect`
comes back on the same network after a reset; a fresh one, or one whose network
is out of range, raises its own access point instead:

- SSID `esp-bringup-<xxxxxx>`, where the suffix is the low three bytes of the
  board's soft AP MAC address, so several boards on a bench stay distinct
- password `bringup1234` (compiled in, so it is a speed bump rather than
  security — but better than an open console on a shared bench)
- channel 1, address `192.168.4.1`

The stored-network attempt uses a 10 second timeout rather than the 20 seconds
`connect` allows, so a board that cannot reach its network starts hosting
promptly. The command can be re-run by hand at any time.

#### `status`

Reports the current association — SSID, BSSID, RSSI, channel, security — plus
the IP address, gateway and netmask.

While an access point is running it reports that instead: the SSID, channel,
security and address being advertised, followed by each connected client with
its MAC address, DHCP-leased IP and signal strength.

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
`/sd/bench.tmp`. Defaults to 512 KB in 16 KB blocks. The result is appended to
the results file described below.

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

The read is read-only, so it is safe to run anywhere on the card. Blocks are
rounded down to whole sectors and the range is checked against the card's
capacity. The result is appended to the results file described below, which is
the one write it does make.

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

The measurement is **read-only**. Nothing is written to the card at a clock that
has not been verified, because a corrupt write is not recoverable the way a
corrupt read is. The results file is written afterwards, once the card has been
reopened at the fastest rate that passed.

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

The rating is re-read at every step rather than measured once, because it is not
constant. ESP-IDF only attempts the CMD6 high-speed switch when the host asks
for more than 20 MHz; below that the card stays in Default Speed and reports
25 MHz, and after the switch it reports 50 MHz. Comparing every step against the
figure seen at the 20 MHz reference would label perfectly in-spec High Speed
operation as overclocking.

Afterwards the card is left initialized at the fastest verified rate, so
`sd bench` and `sd raw` measure it without re-entering anything.

#### `results [clear]`

Prints the saved results file back, or with `clear` deletes it. Worth having
because on a bringup bench the board is usually the only thing holding the card,
so there is no convenient way to pull it and read the file on a host.

#### `close`

Unmounts, releases the card and frees the bus.

#### The results file

`bench`, `raw` and `sweep` each append their output to **`/sd/sdbench.txt`** when
they succeed. Nothing is overwritten, so a card accumulates a history and can be
carried between boards.

Each entry is stamped with enough identity to say where it came from, which is
the point — a bare table of numbers found on a card months later is worthless:

```
================================================================
SD card clock sweep / overclocking test
Uptime:    25 s when run. The board has no RTC, so entries are in
           file order, not wall-clock order.
Chip:      ESP32-S3 rev v0.2, 2 cores
Flash:     8192 KB
MAC (STA): d8:3b:da:45:4c:ec  <- identifies this board
Firmware:  esp_board_bringup 3500f4b-dirty (built Aug 12 2026 10:44:18)
ESP-IDF:   v6.0.1
Interface: SPI on CLK=GPIO7, MOSI=GPIO9, MISO=GPIO8, CS=GPIO21
Clock:     20.000 MHz (requested 20000 kHz), card rated 25.000 MHz
Card:      SD04G, SDHC/SDXC, 3.68 GiB, CID mfg 0x27 serial 0x7C559EEF, made 2015-05
----------------------------------------------------------------
```

The station MAC is the part that is genuinely unique per board; the chip model
and revision say which design, and the pins are named by role so the wiring
harness is recorded too. For a sweep, the whole rate-by-rate table follows,
including which steps were overclocked and where it stopped and why.

The body is captured with the same formatting calls that produced the console
output, so what is saved cannot drift from what was displayed.

Saving is **best effort**. The measurement has already succeeded by the time the
file is written, so a card with no filesystem produces a plain note rather than
an `ERR:` line — a host script parsing `ERR:` should not be told a good
benchmark failed because there was nowhere to record it.

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

And on an M5Stack Cardputer (SPI only — the slot breaks out just CLK 40, MOSI
14, MISO 39, CS 12) with an 8 GB SanDisk `SA08G` from 2015:

| | SPI @ 20 MHz |
|---|---|
| Raw read | 1.42 MiB/s |
| FAT read | 0.93 MiB/s |
| FAT write | 0.21 MiB/s |
| Worst write block | 885.7 ms |

The raw read matching the XIAO's 1.45 MiB/s across two different cards is the
useful part: at 20 MHz over SPI the bus is the limit, not the card. Where the
cards differ is writes — 0.21 MiB/s with an 885 ms worst-case block is this
particular card's flash management, and it is why `bench` reports the worst
block alongside the average.

`sd sweep` found the limits to be 20 MHz over SPI and 40 MHz over SD 1-bit. Both
are worth understanding, because neither is the card — and neither is an
overclock, despite 40 MHz being above the 25 MHz the card advertises at default
speed:

- **Over SPI it stops at 20 MHz, and this is a protocol threshold rather than a
  speed limit.** Initialization fails in `sdmmc_enable_hs_mode_and_check()`
  re-reading the CSD. That looks like a signal-integrity ceiling and is not one:

  `sdmmc_init_card_hs_mode` runs *before* `sdmmc_init_host_frequency`, so the
  failing SEND_CSD is issued at the 400 kHz initialization clock — the bus never
  reaches the requested rate. The switch is attempted only when
  `host.max_freq_khz > SDMMC_FREQ_DEFAULT`, which is 20000 exactly, so what
  triggers it is crossing a constant in software.

  Confirmed directly: **20000 kHz initializes and 20001 kHz does not**, with the
  same `send_csd returned 0x108`. A 1 kHz difference cannot matter electrically,
  and the SPI divider quantises both to the same clock anyway.

  What the card does is accept the CMD6 high-speed switch and then fail the
  SEND_CSD that follows it. `sdmmc_init_card_hs_mode` tolerates
  `ESP_ERR_NOT_SUPPORTED` by falling back to 20 MHz, but treats every other
  error as fatal, so this aborts the whole init instead of degrading. Two
  different cards on two different boards behave identically, so it is not
  specific to a card, a board, or the GPIO matrix.
- **Over SD 1-bit it stops at 40 MHz** — that is the host, not the card, and it
  is a fixed divider rather than a negotiated rate. ESP-IDF's
  `sd_host_slot_get_clk_dividers()` maps *every* request of 40 MHz or more onto
  `host_div = 4`, i.e. 160 MHz / 4 = exactly 40 MHz, so the card was never
  driven faster and its own limit is unknown. See the note below.

  Note that 40 MHz is **not** an overclock for this card: once it accepts the
  high-speed switch its CSD reports 50 MHz, so the whole sweep ran in spec.

##### Why 40 MHz is the SD-mode ceiling

The ESP32-S3's SD host is fed only by PLL_160M or the 40 MHz crystal — there is
no 200 MHz source, and `SOC_SDMMC_UHS_I_SUPPORTED` is not defined for it, so
UHS-I (SDR50 at 100 MHz, SDR104 at 208 MHz) does not exist on this chip. That
puts SD High Speed, specified to a 50 MHz maximum, at the top of what the part
can do.

Dividing 160 MHz by an integer, the options either side of that limit are
160/4 = 40 MHz and 160/3 = 53.3 MHz. The latter is over the 50 MHz High Speed
ceiling, so 40 MHz is the fastest in-spec rate the clock tree can produce, and
ESP-IDF hard-codes it for any request at or above 40 MHz.

So the 40 MHz wall is not a silicon divider limit — the hardware could emit
53.3 MHz or 80 MHz — but there is no way to ask for those through the public
API, and both would be outside the SD specification.

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

### Audio

Plays a tone or a frequency sweep out of an I2S codec or amplifier, captures
from a microphone or an ADC, and reports what the hardware actually did with
either.

This is a capability menu rather than a bus menu — it owns the I2S transport
the way `sd` owns whichever bus a card is on. The generic commands here never
mention a specific part: they work through a small codec interface, so a
NAU8822 with sixty registers and an NS4168 with one enable pin are driven by
the same `audio tone`, `audio volume` and `audio mute`. Parts appear as
submenus (`audio nau8822`, `audio ns4168`, `audio sph0645`) and attach
themselves when initialized.

**Two parts can be attached at once, one per direction.** A board may well have
an amplifier and a microphone on the same bus, and testing one against the
other is the whole point of `audio loopback`. Attaching a part only displaces
one that needs the same direction; a part that works both ways, like the
NAU8822, occupies both slots on its own. A single slot would be worse than
limiting — detaching powers a part down, so attaching a microphone would
silently stop the amplifier.

**The wire is always two slots wide.** I2S has a genuine mono slot mode, but a
stereo part fed one-slot frames plays everything an octave down at half speed,
and every part this targets expects a two-slot frame. So there is no mono
option; put the signal in one channel with `left` or `right` instead, which is
also how you find out which slot a mono amplifier is listening to.

#### `bus <bclk> <ws> <dout> [din <pin>] [mclk <pin>] [rate <hz>] [bits <16|24|32>] [mclkmult <n>]`

Initializes I2S on the given pins. The rate defaults to 48000 Hz and the width
to 16 bits. Re-running it tears the old configuration down first, so pins and
format can be changed freely; an attached codec is reconfigured to match.

`mclk` is needed by codecs that cannot synthesise a clock from BCLK alone — the
NAU8822 is one — and unnecessary for a simple amplifier. `mclkmult` sets MCLK
as a multiple of the sample rate; it defaults to 256, or 384 at 24 bits, which
the driver requires to be a multiple of 3 for the rate to come out accurate.

`din` adds a receiver sharing the same BCLK and WS — an I2S microphone such as
the SPH0645 (which has a submenu of its own, below), or a codec's ADC. A PDM
microphone is a different mode and has its own command, `audio pdm`.

**Setting `din` to the same pin as `dout` is an internal loopback.** The I2S
driver notices and feeds the transmitter straight back to the receiver through
the pad, with nothing connected. That is the one capture test that needs no
hardware at all, which makes it the right first move when a microphone reads
silent: it separates a broken receive path from a broken part.

**The transmitter starts immediately and keeps running.** With nothing queued
the DMA sends silence, so BCLK and WS are live from this point on. That is what
a codec wants — most of them mute or reset when their clocks stop, and starting
and stopping the clock around every tone makes an amplifier pop — and it means
the lines can be put on a scope before any signal is played.

The clocks are reported as configured, not as requested:

```
> audio bus 41 43 42
I2S initialized on BCLK=41, WS=43, DOUT=42
Format:  48000 Hz requested, 48000 Hz configured, 16-bit, 2 slots
Clocks:  BCLK 1.536 MHz, MCLK not routed to a pin
Transmitting silence, so the clocks are live for the codec.
```

#### `info`

Shows the pins, format, clocks, whether output is running, and the attached
codec with its own status.

#### `codecs`

Lists the parts this firmware knows how to drive, which direction each works
in, and whether it needs MCLK.

#### `tone <hz> [seconds|continuous] [level <pct>] [left|right|both]`

Plays a sine tone. The duration defaults to 3 seconds and the level to 25 % of
full scale (−12 dBFS) — loud enough to hear, quiet enough not to hurt.

`continuous` is the interruptible form. Commands execute one at a time, so a
finite tone occupies the shell — serial *and* web — for its whole duration;
a continuous one runs on its own task and is ended with `audio stop`.

The signal is generated from a 1024-entry sine table with interpolation rather
than by calling `sinf()`. The S3 has an FPU and would manage; the C3 has none,
and softfloat there would spend a large part of a core generating the test
signal, which is precisely when the question is whether the *hardware* keeps
up. The table's error floor measures 90 dB below the signal, far under anything
an amplifier under test contributes. Phase is a full 32-bit accumulator, so no
frequency has to divide into the buffer length and block boundaries are
inaudible, and every tone gets a 5 ms raised-cosine fade at each end so it does
not thump the speaker.

```
> audio tone 1000 3
Played 1000.0 Hz at 25% (-12.0 dBFS), 144000 frames
Sample rate: 48000 requested, 48000 configured, 47999 measured
```

**The measured rate is the one worth reading.** The configured rate says what
the divider was set to; the measured one says how fast samples actually left.
It is timed between two moments when the DMA queue is equally full, so the
buffer cancels out — timing the whole run instead would charge the prefill to
the measurement and under-report by the buffer depth. A shortfall means
something starved the DMA.

#### `sweep <start_hz> <end_hz> [seconds] [level <pct>] [log|linear] [left|right|both]`

Sweeps between two frequencies over 5 seconds by default. Logarithmic unless
told otherwise, because equal time per octave is how audio hardware is judged.
Frequency steps once per block of 256 frames — 5 ms at 48 kHz — with phase
carried across, so there is no clicking at the steps.

#### `stop`

Ends a continuous tone, ramping it down rather than cutting it.

#### `pdm <clk> <data> [rate <hz>]`

Opens a PDM microphone — the SPM1423 on the Cardputer, the one on the XIAO
Sense. PDM is not I2S with different pins: the part is clocked at a few
megahertz on a single line and sends one bit per clock, and the peripheral's
decimation filter turns that back into PCM. So it gets its own receiver rather
than joining the standard bus, and its own command.

The rate is the *decimated* rate, and it sets the microphone's own clock at 64
times itself — 48 kHz gives 3.072 MHz. That matters, because a MEMS PDM part
typically specifies 1 to 3.25 MHz and drops into a low-power mode below that,
where it looks exactly like a dead microphone. The command reports the clock it
produced and says so when it lands outside that range.

If the standard bus is already open, the microphone inherits its sample rate.
Capture and playback share one rate deliberately: a loopback measured at two
different rates would not mean anything.

```
> audio pdm 43 46
Input:   PDM on CLK=43, DATA=46, decimated to 48000 Hz 16-bit
         Microphone clock 3.072 MHz
         Receiver running
```

**A PDM microphone has no control interface, so it has no driver here.** There
are no registers, no address, no enable — the transport is the whole of it.
That is why there is no `audio spm1423` submenu to match `audio ns4168`: the
NS4168 earned a driver by having one pin, and this part does not have even
that. The same applies to an I2S microphone, which `audio bus ... din <pin>`
covers outright.

On the ESP32-C3 this refuses. The chip can clock a PDM microphone but has no
PDM-to-PCM filter, so what arrives is the raw one-bit stream; every statistic
below would be measuring the modulator rather than the sound, and would look
like plausible numbers while meaning nothing.

#### `record [seconds]`

Captures the input and reports what was in it. Two seconds by default.

Both slots are always reported separately. A mono microphone fills one of them
and which one is a board fact, discovered the same way `audio tone ... left`
discovers which slot a mono amplifier plays.

```
> audio record 2
Captured 96000 frames at 48000 Hz from PDM microphone, 16-bit (full scale 32768)
slot           min         max      mean      stdev        rms      peak
left          -412         398      -1.8       58.3     -55.0 dB  -38.3 dB
right            0           0       0.0        0.0    -120.0 dB -120.0 dB
```

RMS is quoted about the mean, which is why the column is also labelled stdev. A
microphone with a DC offset is normal and says nothing about the signal;
counting that offset as level would make a silent part look like a loud one.

For scale, the Cardputer's SPM1423 in a quiet room sits between −62 and −70
dBFS and rises to about −33 dBFS when someone talks at it, on a DC offset of
roughly 1300 counts. A **stuck** input is called out explicitly rather than
left to be inferred, because it is the commonest way for one to be wrong and it
reads confusingly otherwise — RMS floors at −120 dB while peak sits near 0 dB,
peak being measured from zero for headroom and a dead line being nearly all
offset:

```
> audio record 1
slot           min         max      mean      stdev          rms       peak
left        -30935      -30935  -30935.0        0.0    -120.0 dB    -0.5 dB
The left slot never changed -- every one of 48000 samples read -30935. Nothing
is modulating this input: check that the part is clocked and that the data pin
is the right one.
```

Below the table is a spectrum in half-octave bands. Every FFT bin lands in
exactly one band, so a band is the *total power* in that half octave rather
than the level at its centre — which means a tone anywhere inside a band shows
up at its real level, and the bands sum to the broadband RMS above them. Both
of those are checked host-side; the first version of this probed each band at
one frequency and read a tone between two probes 89 dB low, which is the kind
of display that gets a working microphone thrown away.

#### `level [seconds]`

A meter rather than a measurement: one line per 100 ms with a bar per slot.
`record` answers "what is the input doing"; this answers "does it react to
me", which is the question you actually have while tapping a microphone, and
it needs the answer to arrive while your finger is still moving.

#### `loopback [hz] [seconds] [level <pct>]`

Plays a tone and measures whether the input hears it. This is the end-to-end
test: output, air or wire, input, all in one command.

It measures the same frequency **twice** — once with the output silent, once
with it playing — and reports the difference. That control is the whole point.
A single measurement with the tone running cannot tell a microphone that hears
the speaker from one sitting next to a switching supply that happens to whine
near the test frequency; only the change between the two is evidence.

```
> audio loopback 1000
Testing whether the input hears 1000 Hz at 25% from the output.

slot          quiet    playing   change
left         -120.0     -120.0      0.0
right         -78.4      -13.1     65.3

Heard it: 1000 Hz rose 65.3 dB in the right slot when the output started.
```

A rise of 12 dB or more counts as heard, 6 to 12 dB as marginal, and less than
that as not heard. A negative result comes with the checks to make in the order
they are cheapest to answer.

Both a transmitter and a receiver have to be up, and on some boards that is
impossible — see the Cardputer below.

#### `volume [pct]` and `mute [on|off]`

Delegated to the attached codec. A part with no volume control says so and
points at the generator's own `level` instead, which is a digital attenuation
and always available.

#### `close`

Detaches the codec and releases I2S. The codec goes first: an amplifier still
enabled when its clocks stop thumps the speaker.

#### NAU8822

Reached as `audio nau8822 ...`. Drives the Nuvoton NAU8822 stereo codec with
speaker driver — the only part here that both plays and records, and so the
only one that can run `audio loopback` through real air rather than through the
chip's internal loopback. Its speaker driver and its ADC are on the same I2S
bus with no shared pin, which the Cardputer's microphone and speaker are not.

Control is I2C at `0x1a` (CSB low) or `0x1b` (CSB high), so
**the `i2c` menu's bus must be up first** — run `i2c bus <scl> <sda>` before
`audio nau8822 init`. Audio arrives over I2S, so `audio bus` must be up too,
and it must have an `mclk` pin: the part is a slave and has no way to make a
system clock from BCLK alone. Without it every register write succeeds and
nothing comes out, so `init` refuses rather than leaving you to find that.

Registers are 7 address bits and 9 data bits packed into two bytes. The driver
keeps a **shadow copy** of everything it writes. That is not an optimisation:
several registers pack unrelated fields, and the family this part is
register-compatible with — the WM8978 — is write-only, so without a shadow
there would be no way to change one field without destroying its neighbours.

Identity is handled the same careful way `sht4x serial` is. The device ID
register is read first; if it answers, the part is identified outright. If it
does not, an acknowledged write is the only remaining evidence, and both `init`
and `status` say which of the two happened rather than implying more than was
established.

##### `init [address]`

Resets the part, powers up the reference, bias and I/O buffer, configures the
audio interface and clocking for the current `audio bus` format, sets the DAC
to full scale, routes only the DAC into the output mixers — so a tone that
comes out came from I2S and nowhere else — enables thermal shutdown on the
speaker driver, and brings up both outputs.

The capture side comes up configured but **unpowered and disconnected**. See
`input` below for why that is not an oversight.

Volume is carried by the analog output attenuator, not the digital DAC volume:
attenuating digitally throws away bits, and the question on a bench is usually
whether the analog path works at all. 100 % is 0 dB; the six steps of gain
above that are deliberately not reachable.

##### `input <mic|line|off> [boost]` and `gain [db]`

The capture side. This is the one part here that records *and* plays, so it
occupies both codec slots on its own and `audio record` works through it once
an input is selected.

**`init` deliberately leaves the ADC off.** The part has two input paths and
they are not interchangeable — a microphone goes in differentially on
MICP/MICN through the input mixer and the PGA, wanting mic bias and usually the
+20 dB boost; a line signal goes in on L2/R2 straight to the boost mixer,
skipping the PGA, wanting neither. Which is fitted is a board fact the driver
cannot read, and guessing wrong is not harmless: mic bias on a line output is
pointless at best, and 20 dB of gain on one clips instantly. So it waits to be
told.

`gain` follows whichever path is selected, because they are different
amplifiers with different ranges: the microphone PGA covers −12 to +35.25 dB in
0.75 dB steps, and the line input −15 to +3 dB in steps of 3. Both quantise, so
the value reported back is the one actually programmed rather than the one
asked for. `boost` is the separate +20 dB stage ahead of the ADC and only
exists on the microphone path — asking for it on the line input is refused
rather than ignored, since silently dropping it would leave you believing in
gain that is not there.

The ADC's high-pass filter is left on. An electret or MEMS input carries a DC
offset that is not signal, and the part can remove it in hardware — which is
the same problem `audio record` works around in software by quoting RMS about
the mean.

Selecting an input warns if the I2S bus has no receive line, because the ADC
then has nowhere to send samples and `audio record` would report a dead input
on a codec that is working perfectly. The full setup is `audio bus <bclk> <ws>
<dout> din <pin> mclk <pin>`, with `din` on the codec's ADCOUT.

**How this was verified without a microphone.** With nothing connected to the
inputs the ADC still sees its own front-end noise, and if the PGA is genuinely
in circuit then raising its gain has to raise that noise by the same number of
decibels. It does: over the PGA's full 47.25 dB the measured floor rose 48.0 dB,
and the +20 dB boost stage added 19.5 dB. At the bottom of the range the rise
lags slightly — 5.0 dB for a commanded 6 — which is the converter's own floor
showing through underneath, exactly as it should. The control is the same one
that confirmed the Cardputer's microphone: powered down, the ADC returns not a
small number but *exactly* zero.

The line path can only be half-checked this way. Its gain sits after the point
where the ADC's noise enters, so with no source connected the floor barely
moves; the registers demonstrably take effect, but the dB scale needs a real
signal to confirm.

##### `status`, `reg <n> [value]`, `route <hp|speaker|both>`

`reg` reads or writes any of the 64 registers, showing both the shadow value
and the live one where the part reads back, and flagging a difference between
them. `route` chooses which output drivers are powered.

#### NS4168

Reached as `audio ns4168 ...`. A mono I2S class-D amplifier with **no control
bus at all** — it takes BCLK, LRCK and SDATA and drives a speaker, and
everything configurable about it is set by resistors on the board. It is the
useful counterexample for the codec interface: if a part with one pin and no
registers fits the same abstraction as a codec with sixty, the abstraction is
at the right level. It implements mute and nothing else, and `audio volume`
says plainly that there is no volume to set.

##### `init [sd <pin>]`

Attaches the amplifier and enables it. The I2S clocks must already be running:
an amplifier enabled into a dead bus latches onto whatever the lines happen to
be doing, and the ESP32 leaves them low.

`sd` is the shutdown pin. Give it and `audio mute` works; omit it and the part
is assumed hard-enabled, which is the case on boards that do not break it out —
the Cardputer is one.

Note that on many designs **the SD pin doubles as channel select** through a
resistor divider, so which slot the amplifier plays is a board decision rather
than a software one. `audio tone 1000 3 left` followed by the same with `right`
is the quick way to find out which. On the Cardputer the answer is **right**:
a left-only tone is silent there, and that is the board, not a fault.

##### `status`

Shows the enable pin and its state. It also says outright that nothing here
confirms the part really is an NS4168 — with no control bus, there is no way to
ask.

#### SPH0645

Reached as `audio sph0645 ...`. The Knowles SPH0645LM4H-B I2S MEMS microphone,
and the input-side counterpart to the NS4168: also no control bus, also no
registers. So why does it get a driver at all, when `audio bus ... din <pin>`
already receives I2S?

Because the datasheet imposes three constraints that are invisible from the
command line and that fail *quietly* rather than loudly:

- **The oversampling ratio is fixed at 64**, so WS must be BCLK/64. Two 32-bit
  slots give exactly that. Ask for 16-bit slots and the frame is 32 clocks, the
  part sees word-select at twice the rate it expects, and what comes back is
  not silence but plausible-looking rubbish.
- **The clock must be 2.048–4.096 MHz**, which at 64 clocks per frame means a
  sample rate of 32–64 kHz and nothing else. Below 900 kHz the part
  deliberately sleeps and tri-states its data pin, which reads as a dead
  microphone rather than as a misconfiguration.
- **SELECT decides which slot it lands in** — low drives the line while
  word-select is low, which in I2S is the left slot. It is usually strapped on
  the board, so the software cannot read it and has to be told.

`init [left|right] [sel <pin>]` checks the first two against the live bus and
records the third. Give `sel` a GPIO and it drives it, which turns "which slot
is my microphone in" from a question into an experiment you can run twice.

`status` reports the slot, the measured bit clock against the datasheet range,
and what the part guarantees: 24-bit words with 18 bits of real precision and
the low bits always zero — worth stating, because it means zeros down there are
the part working to specification rather than something truncating in this
firmware. It also gives the sensitivity (94 dB SPL reads −26 dBFS) so there is
something to compare a recording against, and notes that a single microphone on
a bus needs a 100 kΩ pull-down on the data line: without it, the half of the
frame the part tri-states floats to whatever the bus capacitance holds, and the
other slot mirrors the first instead of reading silence.

**Not verified against a real part.** There is no SPH0645 on the bench, so this
is transcribed from the Rev B datasheet. The 32-bit capture path it depends on
*has* been exercised on hardware through the internal loopback, and so have all
of its refusals; the part itself has not.

Other 24/32-bit I2S microphones — the INMP441, the ICS-43434 — work through the
same `audio bus ... din <pin> bits 32` and the same `audio record`, without
this submenu. They differ in their clock limits, so the checks here are the
SPH0645's rather than universal.

### Board

Known board pinouts, and presets that replay the commands which use them.

Every command in this project takes explicit pin numbers, which is right for a
tool whose job is to find out how a board is wired. Once a board *has* been
worked out, though, retyping its pinout is only a way to make mistakes.

**Selecting a board changes nothing.** `board cardputer` lists what is
available; only `board cardputer audio` runs anything, and it echoes each line
as it goes, so the preset is a shortcut for the commands rather than a
replacement for knowing them:

```
> board cardputer audio
> audio bus 41 43 42
I2S initialized on BCLK=41, WS=43, DOUT=42
...
> audio ns4168 init
NS4168 attached; no SD pin given, so it is assumed hard-enabled
```

#### `list`

Lists the known boards. Each is a submenu; entering it shows the subsystems it
has presets for.

#### `<board> pins`

Prints the pinout table. Pins carry a note where they came from a schematic
rather than from having been exercised here — every pin on the Cardputer has
now been used in anger, the microphone's by clocking it from a *different*
GPIO and watching the data go perfectly static, which is what distinguishes
"this is the clock pin" from "the schematic says so". A signal that
exists on the board but is not brought out to a GPIO is listed with a `-`
rather than omitted, because "there is no enable pin" is an answer and a
missing row is not.

#### `<board> <subsystem>`

Runs that subsystem's setup. A preset is refused when the firmware is built for
a different chip than the board carries.

Currently `cardputer` (M5Stack Cardputer: `pins`, `audio`, `mic`, `sd`) and
`xiao` (Seeed XIAO ESP32-S3 Sense: `pins`, `mic`, `sd`). The XIAO has no
speaker, so it has no audio output preset.

**The Cardputer cannot record its own speaker.** GPIO 43 carries the speaker's
word-select *and* the microphone's clock, so only one of the two can have the
pad at a time — `board cardputer audio` and `board cardputer mic` are mutually
exclusive, and `audio loopback` has nothing to work with there. `audio pdm`
refuses with that explanation rather than letting the second peripheral quietly
take the pin from the first, which is the failure worth preventing: it is not
an error but a plausible silence, with the amplifier seeing a megahertz square
wave where its frame clock used to be.

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
[WiFi](#wifi) for the default SSID and password.

Both interfaces feed the same command queue and share one output fan-out, so
commands are executed one at a time and every line of output reaches the serial
port and every connected browser — regardless of where the command was entered.
