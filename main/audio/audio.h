#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Audio bring-up: an I2S transport, a test signal generator, and codec drivers.
 *
 * The layering mirrors the rest of this project. i2s_bus.c owns the peripheral
 * the way i2c.c owns the I2C bus, and part drivers build on it rather than
 * opening their own -- see i2c_require_bus() / i2c_device_handle() for the
 * pattern being followed.
 *
 * Deliberately no I2S driver types appear in this header. i2s_bus.c is the only
 * file that includes driver/i2s_std.h, which is what lets the whole module be
 * compiled out on a chip with no I2S peripheral without touching anything else.
 */

/* ------------------------------------------------------------------ */
/* Stream format                                                       */
/* ------------------------------------------------------------------ */

/*
 * Shared by playback and (later) capture, so a microphone reuses the bus
 * configuration rather than describing its own.
 */
typedef struct {
    uint32_t rate_hz;      /* requested sample rate */
    uint8_t bits;          /* 16, 24 or 32 valid data bits per sample */
    int mclk_pin;          /* -1 when the board does not wire one */
    int mclk_multiple;     /* 256 or 384; 0 picks one to suit the bit width */
} audio_format_t;

/*
 * The wire is always two slots wide.
 *
 * I2S_SLOT_MODE_MONO exists, but it changes the frame the codec sees, and a
 * stereo part fed one-slot frames plays everything an octave down at half
 * speed. Every part this tool targets expects a two-slot frame, so the slot
 * count is not a user-visible option; put the signal in one channel with the
 * left/right argument to `audio tone` instead, which is also how you find out
 * which slot a mono amplifier is listening to.
 */

/* Which directions a part can work in. TX only for now; RX is what the
 * microphone support in HardwareSupport.md will set. */
typedef enum {
    AUDIO_DIR_TX = 1 << 0,
    AUDIO_DIR_RX = 1 << 1,
} audio_dir_t;

/* ------------------------------------------------------------------ */
/* Codec interface                                                     */
/* ------------------------------------------------------------------ */

/*
 * What every audio part looks like to the generic commands.
 *
 * The optional entries really are optional: the NS4168 is an amplifier with no
 * control interface at all, so it leaves set_volume NULL and the `audio volume`
 * command explains that rather than failing obscurely. Adding a part is a new
 * file exporting one of these, a row in the registry in audio.c, and a submenu
 * in menu_table.c.
 */
typedef struct audio_codec {
    const char *name;
    const char *description;   /* one line, shown by `audio codecs` */
    audio_dir_t directions;
    bool needs_mclk;           /* refuse to attach on a bus with no MCLK pin */

    /*
     * Every call below may return ESP_ERR_NOT_SUPPORTED to mean "I have
     * already told the user why this cannot work". The generic commands stay
     * quiet in that case rather than appending an error name that says less
     * than the driver's own message did.
     */

    /* Evidence a real part is present. NULL when the part cannot be probed. */
    esp_err_t (*probe)(void);
    /* Match the part to the bus format. Called on attach and after `audio bus`. */
    esp_err_t (*configure)(const audio_format_t *fmt);
    esp_err_t (*set_volume)(int percent);  /* NULL: no volume control */
    esp_err_t (*set_mute)(bool mute);      /* NULL: no mute control */
    void (*status)(void);                  /* part-specific detail */
    void (*detach)(void);                  /* release pins, power down */
} audio_codec_t;

/* Make this part the target of the generic commands. Replaces any previous. */
void audio_codec_attach(const audio_codec_t *codec);
void audio_codec_detach(void);
const audio_codec_t *audio_codec_active(void);

/* ------------------------------------------------------------------ */
/* Transport (i2s_bus.c)                                               */
/* ------------------------------------------------------------------ */

bool audio_bus_ready(void);

/* True if the bus is up; otherwise reports the usual error and returns false.
 * The mirror of i2c_require_bus(). */
bool audio_bus_require(void);

/* (Re)open the bus. din < 0 means no receive line. Tears down any existing
 * configuration first, so the command is re-runnable. */
esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt);
void audio_bus_close(void);

const audio_format_t *audio_bus_format(void);
void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk);

/* Bytes per frame: slots x bits/8. Frames are always two slots wide -- mono is
 * carried as the same sample in both, because a codec expecting a stereo frame
 * plays a one-slot frame at half speed and an octave down. */
size_t audio_bus_frame_bytes(void);

/* Clocks the driver actually programmed, from i2s_channel_get_info(). The
 * sample rate is derived from BCLK, so it is what the hardware is doing rather
 * than what was asked for; the dividers quantise. Zero when unknown. */
uint32_t audio_bus_actual_rate(void);
uint32_t audio_bus_bclk_hz(void);
uint32_t audio_bus_mclk_hz(void);

/* Frames the DMA holds when its queue is full. The signal generator needs it
 * to know when writes have become rate-limited by the hardware rather than by
 * the buffer filling up, which is where an honest rate measurement starts. */
uint64_t audio_bus_buffered_frames(void);

/*
 * Frames the generator should write at a time: one DMA descriptor.
 *
 * This is not a free choice. The DMA frees space one whole descriptor at a
 * time, so a write of any other size returns with the queue at an occupancy
 * that depends on where the block boundary happened to fall. Matching the
 * descriptor makes every write return at the same queue state, which is what
 * lets the rate measurement in tone.c cancel the buffer exactly instead of
 * carrying a fixed offset of up to one descriptor.
 */
size_t audio_bus_block_frames(void);

esp_err_t audio_bus_tx_enable(bool enable);
bool audio_bus_tx_enabled(void);
esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Test signals (tone.c)                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    AUDIO_CHANNEL_BOTH,
    AUDIO_CHANNEL_LEFT,
    AUDIO_CHANNEL_RIGHT,
} audio_channel_t;

typedef struct {
    double start_hz;
    double end_hz;      /* equal to start_hz for a steady tone */
    bool logarithmic;   /* sweeps only */
    double seconds;     /* 0 means continuous, ended by audio_play_stop() */
    int level_pct;      /* digital amplitude, 1-100 */
    audio_channel_t channel;
} audio_signal_t;

/*
 * Play a signal. A finite duration blocks until it finishes and then reports;
 * a continuous one starts a task and returns. Reports its own errors, and
 * returns 0 or -1 like a command.
 */
int audio_play(const audio_signal_t *signal);
int audio_play_stop(void);
bool audio_playing(void);

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int cmd_audio_bus(int argc, char **argv);
int cmd_audio_info(int argc, char **argv);
int cmd_audio_codecs(int argc, char **argv);
int cmd_audio_tone(int argc, char **argv);
int cmd_audio_sweep(int argc, char **argv);
int cmd_audio_stop(int argc, char **argv);
int cmd_audio_volume(int argc, char **argv);
int cmd_audio_mute(int argc, char **argv);
int cmd_audio_close(int argc, char **argv);

#endif
