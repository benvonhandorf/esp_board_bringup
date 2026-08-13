#!/usr/bin/env python3
"""Verify the NAU8822 analog input chain with nothing plugged into it.

The technique generalises to any part with an input gain, and it is worth
knowing: a working analog front end has noise of its own, and if the amplifier
is genuinely in circuit then raising its gain must raise that noise by the same
number of decibels. No signal source is needed -- the amplifier is the
instrument. A part that merely accepted the register writes without them
reaching anything analog gives a flat line.

Expect the rise to lag slightly at the bottom of the range: there the
converter's own floor dominates and does not scale with the PGA. That is the
result being physically right rather than suspiciously perfect.

The control at the end is the one that makes the rest mean something. Powered
down, the ADC returns not a small number but exactly zero.

Needs the real I2S pins, since this one actually captures. Pass the bus command
if yours differ from the carrier this was written against:

    ./nau8822_gain.py "audio bus 11 12 9 din 10 mclk 8"
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bringup import Console                                   # noqa: E402

DEFAULT_BUS = "audio bus 11 12 9 din 10 mclk 8"
ROW = re.compile(r"^(left|right)\s+(-?\d+)\s+(-?\d+)\s+(-?[\d.]+)\s+"
                 r"(-?[\d.]+)\s+(-?[\d.]+) dB")


def noise_floor(console):
    """Return (rms dBFS, stdev counts) for the left slot."""
    for line in console.send("audio record 0.5").splitlines():
        found = ROW.match(line.strip())
        if found and found.group(1) == "left":
            return float(found.group(6)), float(found.group(5))
    return None, None


def main():
    bus = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BUS

    with Console() as console:
        console.send("audio close")
        console.send(bus)
        console.send("audio nau8822 init")

        print("microphone path, PGA in circuit")
        console.send("audio nau8822 input mic")
        print(f"{'set dB':>8} {'rms dBFS':>10} {'stdev':>9} {'rise':>8}")
        base = None
        for decibels in (-12, -6, 0, 6, 12, 18, 24, 30, 35.25):
            console.send(f"audio nau8822 gain {decibels}")
            rms, stdev = noise_floor(console)
            if rms is None:
                print(f"{decibels:>8}   no capture -- are the I2S pins right?")
                return 1
            if base is None:
                base = rms
            print(f"{decibels:>8} {rms:>10.1f} {stdev:>9.1f} {rms - base:>+8.1f}")
        span = rms - base
        print(f"  commanded +47.25 dB over the range, measured {span:+.1f} dB")

        print("\n+20 dB boost stage on top of a 0 dB PGA")
        console.send("audio nau8822 input mic")
        console.send("audio nau8822 gain 0")
        plain, _ = noise_floor(console)
        console.send("audio nau8822 input mic boost")
        console.send("audio nau8822 gain 0")
        boosted, _ = noise_floor(console)
        print(f"  {plain:.1f} dBFS -> {boosted:.1f} dBFS, "
              f"rise {boosted - plain:+.1f} dB (datasheet says 20)")

        # The line path's gain sits after the point where the converter's noise
        # enters, so with no source connected the floor barely moves. The
        # registers demonstrably take effect; the scale needs a real signal.
        print("\nline path (PGA bypassed) -- expect a compressed response")
        console.send("audio nau8822 input line")
        print(f"{'set dB':>8} {'rms dBFS':>10} {'rise':>8}")
        base = None
        for decibels in (-15, -9, -3, 0, 3):
            console.send(f"audio nau8822 gain {decibels}")
            rms, _ = noise_floor(console)
            if base is None:
                base = rms
            print(f"{decibels:>8} {rms:>10.1f} {rms - base:>+8.1f}")

        print("\ncontrol: ADC powered down")
        console.send("audio nau8822 input off")
        rms, stdev = noise_floor(console)
        print(f"  rms {rms:.1f} dBFS, stdev {stdev:.1f} "
              f"({'exactly zero, as it should be' if stdev == 0.0 else 'NOT zero'})")
        return 0 if stdev == 0.0 else 1


if __name__ == "__main__":
    sys.exit(main())
