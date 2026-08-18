# Board

[← Command reference](README.md) · [Project README](../README.md)

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

## `list`

Lists the known boards. Each is a submenu; entering it shows the subsystems it
has presets for.

## `<board> pins`

Prints the pinout table. Pins carry a note where they came from a schematic
rather than from having been exercised here — every pin on the Cardputer has
now been used in anger, the microphone's by clocking it from a *different*
GPIO and watching the data go perfectly static, which is what distinguishes
"this is the clock pin" from "the schematic says so". A signal that
exists on the board but is not brought out to a GPIO is listed with a `-`
rather than omitted, because "there is no enable pin" is an answer and a
missing row is not.

## `<board> <subsystem>`

Runs that subsystem's setup. A preset is refused when the firmware is built for
a different chip than the board carries.

Currently `cardputer` (M5Stack Cardputer: `pins`, `audio`, `mic`, `sd`), `xiao` (Seeed XIAO ESP32-S3 Sense: `pins`, `mic`, `sd`), and `core_basic` (M5Stack Core Basic: `pins`, `audio`, `mic`, `sd`, `i2c`). The XIAO has no speaker, so it has no audio output preset.

**The Cardputer cannot record its own speaker.** GPIO 43 carries the speaker's word-select *and* the microphone's clock, so only one of the two can have the pad at a time — `board cardputer audio` and `board cardputer mic` are mutually exclusive, and `audio loopback` has nothing to work with there. `audio pdm` refuses with that explanation rather than letting the second peripheral quietly take the pin from the first, which is the failure worth preventing: it is not an error but a plausible silence, with the amplifier seeing a megahertz square wave where its frame clock used to be.

**The Core Basic shares GPIO13 between SD CS and speaker DOUT.** This is a board-level design decision: only one subsystem can use the pin at a time. Running `board core_basic sd` and then `board core_basic audio` (or vice versa) will reconfigure the pin for the new purpose.
