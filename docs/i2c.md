# I2C

[← Command reference](README.md) · [Project README](../README.md)

## `bus <scl> <sda>`

Initializes an I2C bus for use by future commands, specified by the `scl` and `sda` pins.

## `scan`

Scans the I2C bus enumerates the found devices in a table with the hexadecimal least significant digits along each column and the most significant digits as rows.  If a device is not found, the intersection should be left blank.  If one is found, print the address in hex at the intersection.

If any errors are present that prevent the bus from being properly scanned, such as missing pull up resistors, print the state of the bus that prevents scanning.

## `read <address> <bytes=1>`

Reads the specified number of bytes from the specified address on the bus.  If the number of bytes are not specified, one byte is read.

## INA237

Reached as `i2c ina237 ...`, or by entering the `i2c` menu and then `ina237`.
Drives TI INA237 current/voltage/power monitors, which occupy addresses
`0x40`–`0x4f` depending on how their A0/A1 pins are strapped. Several can be
present on one bus, each with its own shunt resistor.

The shunt input range is left at the device default of ±163.84 mV. Combined
with the shunt resistance this fixes the current resolution: at the default
0.004 Ω the device reads ±40.96 A at 1.25 mA per count.

### `config <address> [shunt_ohms]`

Registers a monitor and programs its calibration. The shunt resistance defaults
to **0.004 Ω**. Before accepting the device, `MANUFACTURER_ID` is read and must
report `0x5449`, so a wrong address or a different chip is reported rather than
silently producing plausible-looking numbers. Re-running on the same address
updates it in place.

Note that `MANUFACTURER_ID` is the only identification available: unlike the
INA238 and INA228, the INA237 has no `DEVICE_ID` register, so this check
confirms a TI part of this family but cannot distinguish the exact variant.

### `read [address]`

Reads the measurement registers and reports bus voltage, current and power,
along with shunt voltage and die temperature. With no address, every configured
monitor is read. With an address that has not been configured, the monitor is
registered on the spot using the default shunt.

The `DIAG_ALRT` register is checked on every read: an arithmetic overflow
(`MATHOF`) or a trim-memory checksum error (`MEMSTAT`) is reported, because
either one means the reported values cannot be trusted. `SHUNT_CAL` is also
read back, so a device that has reset since it was configured is flagged
instead of reporting mis-scaled current.

### `list`

Shows the configured monitors with their shunt resistance, current resolution
and full-scale range.

## SHT4x

Reached as `i2c sht4x ...`. Drives Sensirion SHT4x humidity and temperature
sensors. The address is fixed by the part variant — `0x44` for the A variant
(such as the SHT40-AD1B), `0x45` for B and `0x46` for C — so every command takes
an optional address that defaults to **0x44**.

Unlike the INA237 the SHT4x has no registers. A command byte is written, the
sensor is given time to measure, and the result is read back in a separate
transaction; reading too early makes the sensor NACK. Each 16-bit value carries
its own CRC-8, which is checked on every read, so corrupted data is reported
rather than converted into a plausible-looking measurement.

### `read [address] [high|medium|low]`

Measures temperature and relative humidity, reporting both in engineering units
along with the raw tick values. Repeatability defaults to `high`; lower settings
are faster and noisier. Humidity is cropped to the physical 0–100 %RH range, and
if cropping was necessary the uncropped value is shown too — during bringup a
wildly out-of-range reading is a signal, not noise.

### `serial [address]`

Reads the sensor's 32-bit serial number. The SHT4x has no ID register, so a
serial number that reads back with valid CRCs is the available evidence that a
real sensor is responding.

### `heater [address] <mW> <ms>`

Pulses the on-die heater and then reports the measurement the sensor takes just
before switching it off. Power is 20, 110 or 200 mW and duration is 100 or
1000 ms; only those six combinations exist in the device. Useful for driving off
condensation, and for confirming the part responds to a stimulus.

### `reset [address]`

Issues a soft reset.

## NAU7802

Reached as `i2c nau7802 ...`. Drives a Nuvoton NAU7802 24-bit bridge ADC as a
load cell front end. The address `0x2a` is fixed in silicon — there are no
address pins, so only one can be present per bus.

The normal bringup sequence is:

```
i2c nau7802 init drdy 7     # power up, self-calibrate, DRDY on GPIO 7
i2c nau7802 gain 128        # typical for a load cell's few mV of output
i2c nau7802 tare            # with the scale empty
i2c nau7802 calibrate 100   # with a known 100-unit mass on it
i2c nau7802 weight          # thereafter, in those units
```

### `init [ldo <volts>] [drdy <pin>]`

Resets the device, powers up the digital then analog sections, waits for the
power-up ready flag, and runs the internal offset calibration. Both options
describe how the board is wired, so they may be given in either order.

By default AVDD is taken from the pin, which is the chip's own default.
Boards that rely on the internal regulator — many load cell breakouts do —
need `init ldo 3.0`. This is not the default deliberately: enabling the
internal regulator on a board that already drives AVDD would put two sources on
one net.

`drdy <pin>` names the GPIO the device's DRDY output is wired to; see
[`drdy`](#drdy-pin-off) below for what it buys you. Without it the driver polls
the CR status bit over I2C, which works but cannot start the read at a known
point in the conversion.

### `drdy [<pin>|off]`

Shows or sets the GPIO wired to the device's DRDY output, or releases it with
`off`. Unlike the other commands this one needs neither `init` nor a bus — it
is a statement about how the board is wired, so it can be declared or revoked
without disturbing a converter that is already running.

**What it closes.** The NAU7802 writes its three result registers straight from
the conversion, with no shadow register and no read latch, and it does not care
that an I2C transaction is in flight — bus atomicity is not register
atomicity. A burst read that straddles the moment the device updates those
registers would return the top byte of one conversion stitched to the low bytes
of the next.

Without DRDY the driver polls CR on the FreeRTOS tick, so it learns a
conversion is ready anywhere in a 10–20 ms window after the fact. At 40 SPS and
above that is a whole conversion period or more, and the read then begins at an
unknown phase — potentially as the registers are being rewritten. Waiting on
the pin instead begins the read microseconds after the write, with most of a
conversion period of margin.

A stitched read has a distinctive signature: near zero the result alternates
between `0x0000xx` and `0xFFFFxx`, so the top byte comes from the wrong sample
and the value lands about ±65,500 away from its neighbours. **That signature
has never been observed on the sensor board** — runs of 250 samples at gain 1,
where the signal straddles zero and the top byte flips 40–60 times, produced
zero such outliers with polling or with DRDY. Treat this as a window that is
closed on principle, not as an explanation for a noisy reading; if readings are
noisy, the cause is somewhere else.

A wrong pin number fails cleanly rather than quietly: the input is pulled down,
so a pin that is not connected to DRDY reads low, times out, and says so. A
floating input left to sit high would instead look permanently ready and
return bad numbers.

```
i2c nau7802 drdy 7          # declare it
i2c nau7802 drdy            # show it, and the line's level right now
i2c nau7802 drdy off        # back to polling over I2C
```

One read still carries the old timing risk: if DRDY is already high when a
measurement starts, a result is sitting unread and there is no edge coming —
DRDY does not fall until the result is read — so the driver takes it at an
unknown phase. That can only be the first sample of a batch, since reading a
result drops the line and re-arms the edge for every sample after it.

### `status`

Reads `PU_CTRL`, `CTRL1`, `CTRL2` and the revision register back from the part
and reports each one as both the raw byte and what its bits mean, followed by
the tare and scale this session is holding.

Output after `init ldo 3.0` and `gain 128`:

```
Device revision 0x0F at 0x2A
PU_CTRL 0xBE  digital up, analog up, ready yes, data ready
AVDD source: internal LDO
CTRL1   0x2F  gain x128, LDO 3.0 V
CTRL2   0x00  10 SPS, calibration ok
Input channel: A
Data ready: DRDY on GPIO 7, now low
Tare 8421 counts; calibrated
Scale 214.7 counts per unit
```

**It does not require `init`, only a bus.** Every line above the tare is read
out of the chip, so this reports how the part is *actually* configured rather
than what this firmware believes it did — which is the whole point after the
ESP has been reset while the NAU7802 kept its power, or when something else set
the device up. Without `init` in this session the last two lines are replaced
by a note saying so; the register lines are still true.

That split is worth keeping in mind: the registers live in silicon, while the
tare offset and scale factor live only in this firmware's memory. A soft reset
of the ESP loses the calibration while leaving the chip configured and
converting, and `status` is how you see that state.

During bringup the fields that usually explain a problem are `ready` and
`data`: `ready no` means the analog section never came up, and `data pending`
means no conversion has completed, which on a part that is otherwise powered
points at conversions never having been started. `calibration ERROR` is the
chip's own `CAL_ERR` bit — the internal offset calibration failed, and every
reading after it is untrustworthy. `AVDD source` catches the wiring mistake
`init` is careful about: a board that feeds AVDD from a pin, reported here as
running from the internal LDO, has two sources on one net.

### `gain [1..128]` and `rate [10|20|40|80|320]`

Show or set the PGA gain and conversion rate. Both re-run the internal offset
calibration afterwards, because either change alters the analog path and
invalidates the existing calibration — without that, readings come back
swinging across most of the full-scale range.

**320 SPS is not usable on the board this was developed against.** It returns
values spanning the entire range regardless of calibration, while 10–80 SPS are
rock steady; the likely cause is that 320 SPS needs an external crystal rather
than the internal RC oscillator. The command warns when you select it.

### `ldomode [0|1]`

Shows or sets `REG0x1B[6]`, which picks the compensation for the internal
regulator's control loop. It has to match the capacitor the board fits on AVDD:

| | AVDD capacitor | trade |
|---|---|---|
| `0` (chip default) | ESR **below 1 Ω** | better DC accuracy, higher loop gain |
| `1` | ESR **up to 5 Ω** | more stable loop, lower DC gain |

Pin 16's description in the data sheet asks for "low ESR 1 ohm or less" because
that is what the default expects. A board fitting something with more ESR than
that and leaving this bit alone runs a marginally compensated regulator, which
would be a broadband noise source on the supply and the reference — after the
PGA, where no amount of gain or input rewiring reaches it.

Worth checking on an unfamiliar board; on the sensor board it makes no
measurable difference, so whatever is on AVDD there is comfortably inside the
default's 1 Ω.

Changing it re-runs the internal offset calibration, since the two modes settle
AVDD — the reference — at slightly different levels.

### `pgacap [on|off]`

Shows or sets `REG0x1C[7]`, which connects a filter capacitor across the
VIN2P/VIN2N pins to the PGA output. The data sheet offers it "for enhanced ENOB
at high PGA gain settings".

Two things it needs. The capacitor has to be physically fitted — **330 pF at
AVDD 3.3 V, 680 pF at 4.5 V** — and with none there this bit changes nothing at
all. And it consumes channel 2, whose pins become the filter node, so `input b`
after enabling it reads the capacitor rather than an input.

On the sensor board it changes nothing, which is the expected result for a
board with no such capacitor fitted.

### `read [samples]`, `tare [samples]`, `calibrate <known mass> [samples]`, `weight [samples]`

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
