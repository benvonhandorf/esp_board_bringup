

## Audio Codecs

Support outputting a simple tone or a frequency sweep over a set time period.

**Implemented** as the `audio` menu — see [README](README.md#audio). Codecs are
submenus implementing the `audio_codec_t` vtable in `main/audio/audio.h`; adding
a part is a new file, a row in the registry in `audio.c`, and a submenu in
`menu_table.c`.

### NAU8822

Implemented: `audio nau8822`. Control over I2C at 0x1a/0x1b, audio over I2S,
requires MCLK.

### NS4168

Implemented: `audio ns4168`. No control bus; optional shutdown pin.

## Microphones

Support both mono and stereo setups.
Sample the microphone for several seconds and report min, max, stdev of the audio data as well as an ASCII frequency plot for simple debugging and to ensure audio data is being received.

**Implemented** as `audio record`, `audio level` and `audio loopback` — see
[README](README.md#audio). Both slots are always reported separately, so a mono
part's slot is discovered rather than assumed. `record` gives min, max, mean,
stdev, RMS and peak plus a half-octave power spectrum; `loopback` plays a tone
and measures whether the input hears it, against a silent control.

### PDM Microphones

e.g. SPM1423

Implemented: `audio pdm <clk> <data>`. No control interface exists on these
parts, so there is no driver and no submenu — the transport is the whole of it.
Requires a hardware PDM-to-PCM filter, which the ESP32-C3 lacks.

### I2S Microphones

e.g. Knowles SPH0645LM4H-B I2S Microphone

Implemented: `audio bus <bclk> <ws> <dout> din <pin> bits 32`. Also driverless.

### Loopback

`audio loopback` needs a transmitter and a receiver at once, which is not
always possible: on the M5Stack Cardputer the microphone clock and the
speaker's word-select are the same GPIO, so that board cannot record its own
speaker. `audio bus ... din <dout pin>` is the fallback that always works — the
I2S driver loops the transmitter back internally, which tests the capture path
without testing anything outside the chip.

## Displays

For displays, run a test pattern over he displays showing different colors and drawing a grid over the display area.
Provide tooling to help determine offsets and orientation settings.

### ILI9488 IPS Display

### ST7789

### ST7789V2 - M5Stack Cardputer

## Input Accessories

### FT6236 Touch controller

Report when touch data is detected and X/Y coordinates of touches

### M5Stack Cardputer Keyboard Matrix


