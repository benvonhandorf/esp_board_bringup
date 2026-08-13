

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

Not implemented. The transport is already in place: `main/audio/i2s_bus.c`
carries an RX channel handle alongside TX and `audio bus` accepts a `din <pin>`
argument, so this needs a read path and a PDM init variant there, capture and
analysis commands in the `audio` menu, and `AUDIO_DIR_RX` codec entries — not a
restructure.

### PDM Microphones

e.g. SPM1423

### I2S Microphones

e.g. Knowles SPH0645LM4H-B I2S Microphone

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


