

## Audio Codecs

Support outputting a simple tone or a frequency sweep over a set time period.

**Implemented** as the `audio` menu — see [docs/audio.md](docs/audio.md). Codecs are
submenus implementing the `audio_codec_t` vtable in `main/audio/audio.h`; adding
a part is a new file, a row in the registry in `audio.c`, and a submenu in
`menu_table.c`.

### NAU8822

Implemented: `audio nau8822`. Control over I2C at 0x1a/0x1b, audio over I2S,
requires MCLK. Both directions: playback through the DAC and speaker driver,
capture through the ADC via `audio nau8822 input <mic|line|off>` with analog
gain on `audio nau8822 gain`. The only part here that does both, and therefore
the only one that can run `audio loopback` acoustically.

Verified on hardware (2026-08-13) except the DAC and the analog outputs, which
need a transducer or a loopback jumper to observe. Confirmed: identity and the
full 64-register map against the mainline reset defaults, the init sequence,
every input-path register write, format encoding across seven rate/width
combinations, output volume, mute and routing, and — the part that matters —
the ADC producing real data whose noise floor tracks the input PGA to within
0.75 dB over 47 dB of range.

### NS4168

Implemented: `audio ns4168`. No control bus; optional shutdown pin.

## Microphones

Support both mono and stereo setups.
Sample the microphone for several seconds and report min, max, stdev of the audio data as well as an ASCII frequency plot for simple debugging and to ensure audio data is being received.

**Implemented** as `audio record`, `audio level` and `audio loopback` — see
[docs/audio.md](docs/audio.md). Both slots are always reported separately, so a mono
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

Implemented: `audio bus <bclk> <ws> <dout> din <pin> bits 32` receives from any
of them, and `audio sph0645` adds the Knowles part's own constraints — a fixed
oversampling ratio of 64 that forces 32-bit slots, a 2.048–4.096 MHz clock
limit that forces a 32–64 kHz sample rate, and a SELECT strap that decides
which slot it lands in. Each of those fails silently rather than loudly, which
is what earns the part a driver despite having no control bus.

Untested against real hardware; there is no SPH0645 on the bench. The 32-bit
capture path and every refusal have been exercised on the Cardputer through the
internal loopback.

Other 24/32-bit I2S microphones (INMP441, ICS-43434) work through the bus
command and `audio record` without a submenu.

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


