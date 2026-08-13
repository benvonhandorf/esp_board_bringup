# AGENTS.md

Orientation for coding agents. Read this first; it should save you most of a
codebase scan. `README.md` is the user-facing manual — every command is
documented there in detail, so consult it instead of reverse-engineering
behaviour from source.

## What this is

An ESP-IDF application that turns any ESP32-series board into a CLI-driven
bring-up rig: you exercise GPIO, I2C, SPI, UART, SD and WiFi from a shell before
a BSP exists. The same shell is reachable over the primary serial port *and* over
a web page the device hosts; output from a command goes to both interfaces
regardless of which one typed it.

Targets built against: **ESP32-C3** (dev default) and **ESP32-S3** (Seeed XIAO
ESP32-S3 Sense). ESP-IDF v6.0.1.

## Build and flash

```sh
idf.py set-target esp32c3        # or esp32s3; first time only, and to switch
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

- `sdkconfig` and `sdkconfig.old` are generated and gitignored. Edit
  `sdkconfig.defaults` (shared) or `sdkconfig.defaults.<target>` (per chip),
  then `idf.py reconfigure`.
- `.vscode/settings.json` sets `IDF_TARGET` as an env var, which outranks
  everything else. Switching targets without updating it fails the build with a
  CMake cache mismatch. Build from a plain shell to avoid this.
- `managed_components/` is gitignored and fetched from `dependencies.lock`
  (`espressif/iperf-cmd`, `espressif/mdns`). Never edit anything under it.
- There is no test suite and no host-side build. Verification is `idf.py build`
  plus running commands on hardware.

## Layout

```
main/
  main.c              app_main() -> bp_console_start(), then queues `wifi autostart`
  esp_bringup.{c,h}   common includes + argument parsers (pin lists, ints, doubles)
  console/            the shell itself
    menu.{c,h}        hierarchical command tree, dispatch, prompt, tab completion
    menu_table.c      THE wiring point: every menu and command is declared here
    repl.c            linenoise reader task + command queue + executor task
    console_io.c      picks the serial transport from CONFIG_ESP_CONSOLE_*
    output.{c,h}      bp_printf/bp_error fan-out to console + registered sinks
  audio/              i2s_bus.c (transport), tone.c (signal generation),
                      capture.c (input statistics, FFT, tone detection),
                      audio.c (the menu + codec registry),
                      codec_nau8822.c, codec_ns4168.c, codec_sph0645.c
  board/              board.c named pinouts and per-subsystem setup presets
  gpio/               gpio.c (set/read/aread/blink/short/rc), pwm.c (LEDC)
  i2c/                i2c.c (bus/scan/read) + drivers: ina237.c, sht4x.c, nau7802.c
  spi/spi.c           SPI master
  sd/sd.c             SD over SPI / 1-bit / 4-bit SD, benchmarks, clock sweep (largest file)
  uart/uart.c         auxiliary UART, separate from the console
  wifi/wifi.c         station + soft AP, iperf; starts the web server once either has an address
  web/                web.c HTTP+WebSocket console, index.html embedded via EMBED_FILES
```

**The subdirectories under `main/` are plain source folders, not ESP-IDF
components.** IDF only auto-discovers components under `components/` or
`EXTRA_COMPONENT_DIRS`. A new file must be added to the `SRCS` list in
`main/CMakeLists.txt` or it is silently never compiled.

`main` builds with `-Wall -Wextra -Werror`; IDF and managed components do not
(they would not build clean), which is why the flags are set on the component
and not project-wide.

## How a command works

1. `repl.c` reads a line (linenoise) or `web.c` receives one over the WebSocket.
   Both call `bp_console_submit()`, which queues it for a single executor task —
   so commands never run concurrently, and modules need no locking of their own.
2. `bp_menu_execute()` walks the tree in `menu_table.c`, case-folding *command
   tokens only* (argument values such as WiFi passwords keep their case), and
   either drills into a menu, pops with `back`/`exit`/`quit`, or calls a command.
3. The command function receives `argv[0] == its own name`. So `gpio set 19 true`
   reaches `cmd_gpio_set()` as `argc=3`, `argv={"set","19","true"}` — the first
   real argument is `argv[1]`.

### Adding a command

- Implement `int cmd_<module>_<name>(int argc, char **argv)` in the module's
  `.c`, declare it in the module's `.h`.
- Add a row to the relevant `bp_command_t[]` in `menu_table.c` (name, usage
  string, one-line help, function).
- Document it in `README.md` under the matching `####` heading. Commits in this
  repo change code and README together; keep that.

### Conventions to follow

- **Never call `printf()`** from command code — use `bp_printf()`, or the
  output never reaches the web interface. `ESP_LOGx` is already rerouted through
  the same fan-out.
- **Failures go through `bp_error()`**, which prefixes `ERR:` so a host-side
  script can detect failure without parsing prose. Return `-1`; return `0` on
  success. Usage/arity complaints are printed as `Usage: ...`.
- **Parse with the helpers in `esp_bringup.h`**, never `atoi()`:
  `parse_pin_list()` (accepts `4`, `0-5`, `1,4,8-10`; caller frees),
  `parse_int_arg()`, `parse_num_arg()` (`0x` prefix means hex), and
  `parse_double_arg()`. They reject trailing garbage so `gpio read foo` is an
  error rather than a read of pin 0.
- Output is line-oriented and script-friendly: one record per line, e.g.
  `1: 0`.

## Cross-module constraints worth knowing before you edit

- **One SPI host.** `BP_SPI_HOST_ID` (`SPI2_HOST` in `spi.h`) is shared by the
  `spi` and `sd` menus; on the C3 it is the only general-purpose host. Each
  refuses rather than stealing it, via the mirrored `spi_menu_owns_host()` and
  `sd_owns_spi_host()`.
- **SD host peripheral is chip-dependent.** `sd mmc` is compiled behind
  `SOC_SDMMC_HOST_SUPPORTED`; it exists on the S3 and not on the C3. Anything
  touching `driver/sdmmc_host.h` needs that guard.
- **SD bring-up is deliberately two-stage.** `sd spi` / `sd mmc` init the host
  and card; FAT is mounted only when `sd bench` needs it. Do not "simplify" this
  into `esp_vfs_fat_sdmmc_mount()` — a blank or non-FAT card must still report
  its identity and still be measurable.
- **`audio` is a capability menu, not a bus menu**, like `sd`. `audio/i2s_bus.c`
  is the *only* file that includes `driver/i2s_*.h`, which is what keeps the
  `#if SOC_I2S_SUPPORTED` guard to one file — the `#else` half provides stubs so
  everything above it builds unchanged on a chip with no I2S. Both current
  targets have I2S, so that branch is dead code on them; force the guard to
  `#if 0` and build once if you change it.
- **Codec drivers implement the `audio_codec_t` vtable** in `audio.h` and
  register in the array at the top of `audio.c`. Optional entries may be NULL
  (the NS4168 has no volume control), and returning `ESP_ERR_NOT_SUPPORTED`
  means the driver has already reported the reason, so the caller stays quiet.
  Adding a part: new file, one row in the registry, one submenu in
  `menu_table.c`, one README section.
- **Two codecs attach at once, one per direction**, held in `tx_codec` and
  `rx_codec`. Attaching displaces only parts contending for a direction the
  incoming one needs, and a bidirectional part occupies both. This is a
  correctness requirement, not a convenience: `detach()` powers a part down —
  the NS4168's drives its enable pin low — so a single slot would make
  attaching a microphone silently stop the amplifier. Anything iterating the
  two slots must skip the duplicate when one part holds both.
- **A part with no control bus can still deserve a driver.** The test is not
  "has registers" but "can it fail silently": the NS4168 has an enable pin and
  a channel-select quirk, and the SPH0645 has a fixed oversampling ratio, a
  clock range outside which it sleeps, and a SELECT strap. Each of those
  produces plausible-looking data rather than an error, which is exactly what a
  bringup tool should catch. A part with no such traps — a PDM microphone, an
  INMP441 — needs no driver and should not get one.
- **`audio nau8822` spans two menus.** Control is I2C and audio is I2S, so it
  needs `i2c bus` *and* `audio bus` up first, and `audio bus` must have been
  given an `mclk` pin. Each check names the fix, because the dependency is not
  visible from the command being typed.
- **The NAU8822's power-management registers span both directions.** The
  headphone drivers share Power Management 2 with the ADC and its front end, so
  `apply_power()` recomputes the whole word from the whole of the driver's
  state. Writing only the output bits — as the routing code originally did —
  silently powered the capture chain back down on every route change. Never
  write one of these registers from a function that knows about one direction.
- **The NAU8822's ADC starts off, on purpose.** Microphone and line inputs take
  different paths, different gain registers and different bias, and which is
  fitted is a board fact. Guessing wrong puts mic bias on a line output or
  20 dB of gain into a clip, so `input` demands the choice.
- **`audio bus` leaves the transmitter running**, sending silence. Codecs mute
  or reset when their clocks stop, and cycling the clock around every tone pops
  an amplifier. Do not "optimise" this into enabling TX only while playing.
- **The I2S wire is always two slots.** `I2S_SLOT_MODE_MONO` is deliberately not
  offered; a stereo part fed one-slot frames plays an octave down at half speed.
  Capture reports both slots separately for the same reason it matters on
  output: which one a mono part lands in is a board fact to be discovered.
- **There is one receiver, opened two ways.** `audio bus ... din <pin>` gives a
  standard-mode receiver sharing BCLK and WS with the transmitter; `audio pdm`
  gives a PDM one on a channel of its own. Opening the standard bus releases a
  PDM microphone, because the two share the module's sample rate — capture and
  playback must run at one rate or a loopback measurement means nothing.
- **PDM needs `SOC_I2S_SUPPORTS_PDM2PCM`, not just `SOC_I2S_SUPPORTS_PDM_RX`.**
  The ESP32-C3 has the second and not the first, so it would deliver a raw
  bitstream that every statistic in `capture.c` would misreport as sound;
  `audio_bus_open_pdm()` refuses there rather than returning plausible numbers.
- **`din == dout` is an internal loopback**, not an error to reject. The I2S
  driver wires the transmitter back to the receiver through the pad, which is
  the only capture test needing no hardware, so `pins_distinct()` exempts that
  pair and the result is labelled as proving nothing external.
- **Analysis runs after the capture, never during it.** `capture.c` streams
  integer statistics while reading and does the FFT afterwards on a buffered
  window. A filter bank running per sample would starve the DMA on a chip with
  no FPU. Do not move the transform into the read loop.
- **The spectrum is band power, not per-band probes.** Every FFT bin is assigned
  to exactly one half-octave band. An earlier version sampled each band at one
  frequency and read a tone falling between two probes 89 dB low; the host test
  in the scratchpad covers that case, so keep it covered.
- **`audio loopback` measures the target frequency twice**, silent then playing,
  and reports the difference. Only the change is evidence — a single reading
  cannot separate a microphone hearing the speaker from a supply whining near
  the test frequency.
- **A pin conflict between capture and playback is a real board case**, not a
  hypothetical: on the Cardputer the microphone clock and the speaker's
  word-select are both GPIO 43. `pdm_pins_free()` refuses, because the
  unguarded failure is a plausible silence rather than an error.
- **Board presets call `bp_menu_execute()` directly**, never
  `bp_console_submit(line, true)` — a preset already runs on the executor task,
  and waiting there for that task to drain its own queue deadlocks.
- **I2C device handles are cached** per address in `i2c.c`. Drivers
  (`ina237.c`, `sht4x.c`, `nau7802.c`) call `i2c_require_bus()` and
  `i2c_device_handle()` rather than opening their own bus.
- **The console must be the chip's PRIMARY serial device.** On USB-Serial-JTAG
  boards (`/dev/ttyACM*`) that means `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`; a
  secondary console is output-only and would never receive keystrokes.
- **The web server starts from `wifi.c`** — on `IP_EVENT_STA_GOT_IP` as a
  station, and on `WIFI_EVENT_AP_START` as an access point (the AP owns
  `192.168.4.1` immediately, so there is no lease to wait for). `bp_web_start()`
  is idempotent because both paths can fire in one session. It registers an
  output sink; `httpd_ws_send_frame_async()` must run on the HTTP task, so the
  sink hands chunks over with `httpd_queue_work()`.
- **Station and AP mode are exclusive** in `wifi.c` — `WIFI_MODE_STA` or
  `WIFI_MODE_AP`, never `APSTA`, because one radio cannot hold two channels and
  an APSTA setup silently drags the AP onto the station's channel. `connect`,
  `ap` and `scan` each check `ap_active` and switch or refuse rather than
  letting the driver return a bare `ESP_ERR_WIFI_MODE`.
- **Boot networking is a queued command, not a call.** `app_main()` submits
  `wifi autostart` through `bp_console_submit()` so it runs on the executor task
  like anything typed. Calling into `wifi.c` directly from `app_main` would race
  a user command against the shared event group — the single-executor invariant
  is what lets these modules skip locking.
- `web/index.html` is embedded into the binary via `EMBED_FILES`; there is no
  filesystem behind it.
