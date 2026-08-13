#!/usr/bin/env python3
"""Check that nau8822_configure() programs the interface registers correctly.

R4 carries the I2S format and word length, R6 the MCLK divider, R7 the sample
rate. All three are recomputed every time `audio bus` runs, and getting any of
them wrong produces silence or noise rather than an error -- so they are worth
checking against silicon rather than against the shadow copy.

Needs: the I2C bus up and the codec initialized. The I2S pins need not be
correct; only the register encoding is under test.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bringup import Checks, Console                           # noqa: E402

ROW = re.compile(r"R(\d+) \(0x([0-9a-f]{2})\) shadow 0x([0-9a-f]{3})"
                 r"(?:, device 0x([0-9a-f]{3}))?")

INTERFACE, CLOCKING, RATE = 0x04, 0x06, 0x07

# label, options for `audio bus`, expected R4, R6, R7.
# R4 = I2S format (0x2 << 3) | word length << 5; R7 = rate index << 1.
CASES = [
    ("48000 Hz, 16-bit, x256", "bits 16 rate 48000",                0x010, 0x000, 0x000),
    ("48000 Hz, 24-bit, x384", "bits 24 rate 48000",                0x050, 0x020, 0x000),
    ("48000 Hz, 32-bit, x256", "bits 32 rate 48000",                0x070, 0x000, 0x000),
    ("32000 Hz, 16-bit, x256", "bits 16 rate 32000",                0x010, 0x000, 0x002),
    ("16000 Hz, 16-bit, x256", "bits 16 rate 16000",                0x010, 0x000, 0x006),
    ("8000 Hz, 16-bit, x256",  "bits 16 rate 8000",                 0x010, 0x000, 0x00a),
    ("48000 Hz, 16-bit, x512", "bits 16 rate 48000 mclkmult 512",   0x010, 0x040, 0x000),
]


def read_register(console, number):
    for line in console.send(f"audio nau8822 reg {number}").splitlines():
        found = ROW.search(line)
        if found and found.group(4):
            return int(found.group(4), 16)
    return None


def main():
    checks = Checks("NAU8822 interface format encoding")

    with Console() as console:
        for label, options, want4, want6, want7 in CASES:
            checks.section(label)
            console.send(f"audio bus 1 2 3 mclk 5 {options}")
            checks.equal("R4 format and word length",
                         read_register(console, INTERFACE), want4)
            checks.equal("R6 clocking, MCLK divider",
                         read_register(console, CLOCKING), want6)
            checks.equal("R7 sample rate code",
                         read_register(console, RATE), want7)

    return checks.report()


if __name__ == "__main__":
    sys.exit(main())
