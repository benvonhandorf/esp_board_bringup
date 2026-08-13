#!/usr/bin/env python3
"""Check that the NAU8822 input commands put the silicon where they claim to.

Every value here is read back from the device, not from the driver's shadow
copy, so this tests the part and the bus as well as the code.

The last section is a regression test with a story. Routing originally wrote
Power Management 2 with only the output bits, and that register also holds the
ADC, its input mixer and its boost stage -- so changing the output route
silently powered the whole capture chain back down. It presented as "the
microphone stops working if you touch the outputs".

Needs: the I2C bus up, and any I2S bus (the pins need not be correct -- nothing
here listens, it only reads registers).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bringup import Checks, Console                           # noqa: E402

ROW = re.compile(r"R(\d+) \(0x([0-9a-f]{2})\) shadow 0x([0-9a-f]{3})"
                 r"(?:, device 0x([0-9a-f]{3}))?")

PM1, PM2 = 0x01, 0x02
INPUT_CONTROL, LEFT_PGA, RIGHT_PGA, LEFT_BOOST = 0x2c, 0x2d, 0x2e, 0x2f


def read_register(console, number):
    for line in console.send(f"audio nau8822 reg {number}").splitlines():
        found = ROW.search(line)
        if found and found.group(4):
            return int(found.group(4), 16)
    return None


def main():
    checks = Checks("NAU8822 capture path")

    with Console() as console:
        # Start from init. Gain is driver state that survives an input change
        # by design, so without this a run inherits the previous one's.
        console.send("audio bus 1 2 3 mclk 5")
        console.send("audio nau8822 init")

        def reg(number):
            return read_register(console, number)

        checks.section("input off, as init leaves it")
        console.send("audio nau8822 input off")
        checks.equal("PM1 no mic bias", reg(PM1), 0x00d)
        checks.equal("PM2 headphones only", reg(PM2), 0x180)
        checks.equal("input control disconnected", reg(INPUT_CONTROL), 0x000)
        checks.equal("left PGA muted", reg(LEFT_PGA), 0x040)
        checks.equal("boost mixer clear", reg(LEFT_BOOST), 0x000)

        checks.section("input mic")
        console.send("audio nau8822 input mic")
        checks.equal("PM1 mic bias on", reg(PM1), 0x01d)
        checks.equal("PM2 adc+pga+boost+hp", reg(PM2), 0x1bf)
        checks.equal("MICP+MICN on both channels", reg(INPUT_CONTROL), 0x033)
        checks.equal("left PGA 0 dB, unmuted", reg(LEFT_PGA), 0x010)
        checks.equal("right PGA 0 dB, unmuted", reg(RIGHT_PGA), 0x010)
        checks.equal("boost stage off", reg(LEFT_BOOST), 0x000)

        checks.section("gain quantises to 0.75 dB steps")
        console.send("audio nau8822 gain 12")
        checks.equal("+12 dB is code 32", reg(LEFT_PGA), 0x020)
        console.send("audio nau8822 gain 35.25")
        checks.equal("+35.25 dB is code 63", reg(LEFT_PGA), 0x03f)

        checks.section("input mic boost")
        output = console.send("audio nau8822 input mic boost")
        checks.equal("+20 dB boost bit set", reg(LEFT_BOOST), 0x100)
        checks.equal("gain survives the input change", reg(LEFT_PGA), 0x03f)
        reported = next((l.strip() for l in output.splitlines()
                         if l.strip().startswith("Gain")), "")
        checks.truth("reported gain includes the boost",
                     reported == "Gain +55.25 dB", repr(reported))
        console.send("audio nau8822 gain 0")

        checks.section("input line takes a different path entirely")
        console.send("audio nau8822 input line")
        checks.equal("PM1 mic bias off again", reg(PM1), 0x00d)
        checks.equal("PM2 adc+boost, no PGA", reg(PM2), 0x1b3)
        checks.equal("input mixer disconnected", reg(INPUT_CONTROL), 0x000)
        checks.equal("PGA muted out of the path", reg(LEFT_PGA), 0x040)
        checks.equal("L2 boost at 0 dB (code 6)", reg(LEFT_BOOST), 0x060)
        console.send("audio nau8822 gain -6")
        checks.equal("-6 dB is boost code 4", reg(LEFT_BOOST), 0x040)

        checks.section("routing must not clobber the capture chain")
        console.send("audio nau8822 route speaker")
        checks.equal("PM2 keeps adc+boost, drops hp", reg(PM2), 0x033)
        console.send("audio nau8822 route both")
        checks.equal("PM2 restores hp, keeps adc", reg(PM2), 0x1b3)

        checks.section("input off powers it back down")
        console.send("audio nau8822 input off")
        checks.equal("PM2 capture chain down", reg(PM2), 0x180)
        checks.equal("boost mixer clear", reg(LEFT_BOOST), 0x000)

        checks.section("bad arguments are refused, not ignored")
        for bad in ("audio nau8822 input line boost",
                    "audio nau8822 gain 40",
                    "audio nau8822 gain wibble",
                    "audio nau8822 input sideways"):
            output = console.send(bad)
            refused = any(l.startswith("ERR:") for l in output.splitlines())
            checks.truth(bad.replace("audio nau8822 ", ""), refused)

    return checks.report()


if __name__ == "__main__":
    sys.exit(main())
