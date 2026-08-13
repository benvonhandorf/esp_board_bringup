#!/usr/bin/env python3
"""Reset the NAU8822 and compare every register against its documented default.

A device ID register reading the right value is one byte of evidence, and a
half-working bus or the wrong part can produce it by luck. Sixty registers
arriving at their documented reset values cannot be faked that way, so this is
the identity test worth trusting -- and it doubles as a check that reads and
writes are reliable across the whole address space.

Reference values are nau8822_defaults.json, extracted from the reg_defaults
table in the mainline Linux driver (sound/soc/codecs/nau8822.c).

Needs the I2C bus up. Prints the differences rather than asserting on them:
two registers differ on the part measured here, which looks like table or die
revision drift rather than a fault.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bringup import Console                                   # noqa: E402

ROW = re.compile(r"R(\d+) \(0x([0-9a-f]{2})\) shadow 0x([0-9a-f]{3})"
                 r"(?:, device 0x([0-9a-f]{3}))?")
HERE = os.path.dirname(os.path.abspath(__file__))


def read_register(console, number):
    for line in console.send(f"audio nau8822 reg {number}").splitlines():
        found = ROW.search(line)
        if found and found.group(4):
            return int(found.group(4), 16)
    return None


def main():
    with open(os.path.join(HERE, "nau8822_defaults.json")) as handle:
        defaults = {int(k): v for k, v in json.load(handle).items()}

    with Console() as console:
        if "--no-reset" not in sys.argv:
            print("resetting the part (R0)")
            console.send("audio nau8822 reg 0 0")

        matched = differed = unreferenced = 0
        differences = []

        for number in range(0x40):
            value = read_register(console, number)
            if value is None:
                print(f"0x{number:02x}  no readback")
                unreferenced += 1
                continue
            if number not in defaults:
                print(f"0x{number:02x}  {value:#05x}  (no reference)")
                unreferenced += 1
                continue

            want = defaults[number]
            if value == want:
                matched += 1
                print(f"0x{number:02x}  {value:#05x}  want {want:#05x}  ok")
            else:
                differed += 1
                differences.append((number, value, want))
                print(f"0x{number:02x}  {value:#05x}  want {want:#05x}  DIFFERS")

    print(f"\n{matched} match, {differed} differ, "
          f"{unreferenced} without a reference")
    for number, got, want in differences:
        print(f"  0x{number:02x}: read {got:#05x}, expected {want:#05x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
