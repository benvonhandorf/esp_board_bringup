#!/usr/bin/env python3
"""Check the NAU8822 output path: volume, mute, routing and the output mixers.

The mixer check is the one that matters beyond arithmetic. Only the DAC is
allowed to reach the output mixers, so that a tone which comes out of the part
can only have arrived over I2S; if the analog bypass were ever enabled by
accident, `audio tone` would appear to work while proving nothing.

Needs: the I2C bus up, and any I2S bus (the pins need not be correct).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bringup import Checks, Console                           # noqa: E402

ROW = re.compile(r"R(\d+) \(0x([0-9a-f]{2})\) shadow 0x([0-9a-f]{3})"
                 r"(?:, device 0x([0-9a-f]{3}))?")

PM2, PM3 = 0x02, 0x03
DAC_CONTROL = 0x0a
LEFT_MIXER, RIGHT_MIXER = 0x32, 0x33
LEFT_HP, LEFT_SPK = 0x34, 0x36


def read_register(console, number):
    for line in console.send(f"audio nau8822 reg {number}").splitlines():
        found = ROW.search(line)
        if found and found.group(4):
            return int(found.group(4), 16)
    return None


def main():
    checks = Checks("NAU8822 playback path")

    with Console() as console:
        console.send("audio bus 1 2 3 mclk 5")
        console.send("audio nau8822 init")

        def reg(number):
            return read_register(console, number)

        checks.section("volume maps onto the analog attenuator")
        console.send("audio volume 100")
        checks.equal("headphone at 0 dB", reg(LEFT_HP), 0x039)
        checks.equal("speaker at 0 dB", reg(LEFT_SPK), 0x039)
        console.send("audio volume 50")
        checks.equal("halved", reg(LEFT_HP), 0x01d)
        console.send("audio volume 0")
        checks.equal("zero mutes rather than attenuating", reg(LEFT_HP), 0x040)

        checks.section("mute drives the DAC soft mute")
        console.send("audio volume 60")
        console.send("audio mute on")
        checks.equal("soft mute set", reg(DAC_CONTROL), 0x040)
        console.send("audio mute off")
        checks.equal("soft mute clear", reg(DAC_CONTROL), 0x000)

        checks.section("route hp")
        console.send("audio nau8822 route hp")
        checks.equal("PM2 headphone drivers on", reg(PM2), 0x180)
        checks.equal("PM3 dac+mixers, no speaker", reg(PM3), 0x00f)
        checks.equal("speaker attenuator muted", reg(LEFT_SPK), 0x040)

        checks.section("route speaker is the mirror image")
        console.send("audio nau8822 route speaker")
        checks.equal("PM2 headphones off", reg(PM2), 0x000)
        checks.equal("PM3 speaker drivers on", reg(PM3), 0x06f)
        checks.equal("headphone attenuator muted", reg(LEFT_HP), 0x040)

        checks.section("route both")
        console.send("audio nau8822 route both")
        checks.equal("PM2 headphones back on", reg(PM2), 0x180)
        checks.equal("PM3 everything on", reg(PM3), 0x06f)

        checks.section("only the DAC reaches the output mixers")
        checks.equal("left mixer is DAC only", reg(LEFT_MIXER), 0x001)
        checks.equal("right mixer is DAC only", reg(RIGHT_MIXER), 0x001)

    return checks.report()


if __name__ == "__main__":
    sys.exit(main())
