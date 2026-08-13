# Tools

Things that are not firmware but are worth keeping: a way to drive the console
from a script, tests that run on the build machine, and tests that run against
a board.

Set `BRINGUP_PORT` if the board is not on `/dev/ttyACM1`.

```sh
export BRINGUP_PORT=/dev/ttyACM0
```

## `bringup.py`

Drives the console over serial. Usable directly:

```sh
./bringup.py "system info" "gpio survey"
```

and importable by the tests as `Console` and `Checks`.

The one non-obvious part is how it decides a command has finished. Waiting for
the port to go quiet does not work: `audio tone 1000 3` prints nothing at all
for three seconds while it plays, so a quiet-timeout gives up in the middle of
it. It waits for the prompt instead.

## `hosttest/` — runs on the build machine

```sh
make -C hosttest
```

Compiles the audio signal processing with gcc and checks it against values
derived analytically — a sine of amplitude A has standard deviation A/√2,
halving amplitude is −6.02 dB, Parseval relates the bands to the total power.

**These catch a class of bug that hardware cannot.** The spectrum in
`capture.c` once probed each half-octave band at a single frequency; a tone
falling *between* two probes read 89 dB low. On a board that looks exactly like
a dead microphone, and the evidence points at the part rather than at the
arithmetic. This found it in seconds, and the fix — an FFT with every bin
assigned to exactly one band — is checked here too.

The DSP is **copied verbatim** out of `main/audio/tone.c` and
`main/audio/capture.c`, because those files cannot be compiled off-target. That
is the maintenance cost: change the firmware and the copy goes stale without
telling you. Re-copy the changed functions when you touch either file.

## `hwtest/` — runs against a board

Automated regression tests for the NAU8822 driver, all reading values back from
the device rather than from the driver's shadow copy, so they test the part and
the bus as well as the code.

| Script | What it establishes | Needs |
|---|---|---|
| `nau8822_registers.py` | identity, by comparing all 64 registers against their documented reset values | I2C bus |
| `nau8822_format.py` | `configure()` encodes word length, MCLK divider and sample rate correctly, across seven combinations | I2C bus, any I2S pins |
| `nau8822_playback.py` | volume, mute, routing, and that only the DAC reaches the output mixers | I2C bus, any I2S pins |
| `nau8822_capture.py` | every input-path register write, and the routing/capture regression below | I2C bus, any I2S pins |
| `nau8822_gain.py` | the analog input chain is really in circuit | I2C bus, **the real I2S pins** |

Only the last one captures audio, so only it needs the pins to be right. The
others exercise register encoding, where the I2S pins are irrelevant — which is
worth knowing, because it means most of a codec driver can be tested before the
audio wiring is understood.

Set up the bus first, then run them:

```sh
../bringup.py "i2c bus 7 6" "audio bus 11 12 9 din 10 mclk 8"
./nau8822_registers.py
./nau8822_format.py
./nau8822_playback.py
./nau8822_capture.py
./nau8822_gain.py "audio bus 11 12 9 din 10 mclk 8"
```

`nau8822_defaults.json` holds the reference reset values, extracted from the
`reg_defaults` table in the mainline Linux driver
(`sound/soc/codecs/nau8822.c`).

### Two techniques worth reusing

**A gain control is a test instrument.** `nau8822_gain.py` verifies a whole
analog input chain with nothing plugged into it: a working front end has noise
of its own, and if the amplifier is really in circuit then raising its gain
must raise that noise by the same number of decibels. Over the PGA's 47.25 dB
the measured floor rose 48.0 dB. It applies to any part with an input gain.

**A control measurement beats a plausible reading.** Powered down, the ADC
returns not a small number but *exactly* zero, and the same test found the
Cardputer's microphone by clocking it from the wrong pin and watching 48000
identical samples come back. `audio record` calls that case out by name for
this reason. A reading that looks reasonable is not evidence; a reading that
changes when you change one thing is.

## What is deliberately not here

A pin-permutation search that used the ADC as a detector to find the I2S
wiring. It found nothing, because it was built on the idea that pin capacitance
identifies connected pins — and it does not. Driven lines look like any other
idle input from inside the chip. `gpio survey` says so in its own output; the
lesson was worth keeping and the script was not.
