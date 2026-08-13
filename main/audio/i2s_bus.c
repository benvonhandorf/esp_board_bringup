/*
 * I2S transport for the audio menu.
 *
 * This file owns the peripheral the way i2c.c owns the I2C bus: it is the only
 * place that talks to the I2S driver, and everything above it -- the signal
 * generator, the codec drivers -- works through the small API in audio.h. That
 * is also what keeps the chip guard to one file. A part with no I2S peripheral
 * still builds; the commands just report that it has none.
 *
 * Only the transmit channel is used today. The receive handle and the `din`
 * pin are carried through because the microphone support described in
 * HardwareSupport.md needs exactly this module with a read path added, and
 * retrofitting the channel pair afterwards would mean reworking every caller.
 */
#include "esp_bringup.h"
#include "output.h"
#include "audio.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_I2S_SUPPORTED

#include "driver/gpio.h"
#include "driver/i2s_std.h"

/* Six descriptors of 240 frames is the driver's own default and buys about
 * 30 ms of buffering at 48 kHz -- enough that a console printf cannot starve
 * the output. 240 is divisible by 3, which the driver requires at 24 bits. */
#define DMA_DESC_NUM  6
#define DMA_FRAME_NUM 240

static i2s_chan_handle_t tx_chan;
static i2s_chan_handle_t rx_chan;
static bool bus_open;
static bool tx_running;

static audio_format_t format;
static int pin_bclk = -1;
static int pin_ws = -1;
static int pin_dout = -1;
static int pin_din = -1;

/* 16-bit data sits in a 16-bit slot; 24- and 32-bit data both travel in a
 * 32-bit slot. Packing 24-bit data into three-byte slots is legal I2S and a
 * nuisance to generate, and most codecs are configured for 32-bit slots
 * anyway. */
static uint32_t slot_bits(void)
{
    return format.bits == 16 ? 16 : 32;
}

size_t audio_bus_frame_bytes(void)
{
    return 2 * (slot_bits() / 8);
}

bool audio_bus_ready(void)
{
    return bus_open;
}

bool audio_bus_require(void)
{
    if (!bus_open) {
        bp_error("I2S not initialized. Run 'audio bus <bclk> <ws> <dout>' first.");
        return false;
    }
    return true;
}

const audio_format_t *audio_bus_format(void)
{
    return bus_open ? &format : NULL;
}

void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk)
{
    if (bclk) *bclk = pin_bclk;
    if (ws)   *ws = pin_ws;
    if (dout) *dout = pin_dout;
    if (din)  *din = pin_din;
    if (mclk) *mclk = format.mclk_pin;
}

/*
 * What the driver actually programmed, rather than what was asked for.
 *
 * The clock tree divides an integer ratio out of a PLL, so a requested rate is
 * rarely produced exactly. i2s_channel_get_info() reports the real BCLK, and
 * the frame is two slots, so the sample rate follows. Reporting achieved
 * alongside requested is the same habit as `gpio pwm set` and `sd spi`.
 */
static bool chan_info(i2s_chan_info_t *info)
{
    if (!bus_open || !tx_chan) {
        return false;
    }
    return i2s_channel_get_info(tx_chan, info) == ESP_OK;
}

uint32_t audio_bus_actual_rate(void)
{
    i2s_chan_info_t info;
    if (!chan_info(&info) || info.bclk_hz == 0) {
        return 0;
    }
    return info.bclk_hz / (slot_bits() * 2);
}

uint32_t audio_bus_bclk_hz(void)
{
    i2s_chan_info_t info;
    return chan_info(&info) ? info.bclk_hz : 0;
}

uint32_t audio_bus_mclk_hz(void)
{
    i2s_chan_info_t info;
    return chan_info(&info) ? info.mclk_hz : 0;
}

void audio_bus_close(void)
{
    if (tx_chan) {
        if (tx_running) {
            i2s_channel_disable(tx_chan);
        }
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    if (rx_chan) {
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    tx_running = false;
    bus_open = false;
    pin_bclk = pin_ws = pin_dout = pin_din = -1;
    format.mclk_pin = -1;
}

esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt)
{
    /* Tearing down first is what makes `audio bus` re-runnable, for the same
     * reason create_bus() in i2c.c deletes before it creates. */
    audio_bus_close();

    format = *fmt;
    pin_bclk = bclk;
    pin_ws = ws;
    pin_dout = dout;
    pin_din = din;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    /* Send silence, not a stale buffer, if the generator ever falls behind. A
     * repeated buffer sounds like a perfectly good tone and would hide the
     * starvation that this tool exists to expose. */
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, din >= 0 ? &rx_chan : NULL);
    if (err != ESP_OK) {
        audio_bus_close();
        return err;
    }

    i2s_mclk_multiple_t multiple;
    switch (format.mclk_multiple) {
    case 384: multiple = I2S_MCLK_MULTIPLE_384; break;
    case 512: multiple = I2S_MCLK_MULTIPLE_512; break;
    case 256: multiple = I2S_MCLK_MULTIPLE_256; break;
    default:
        /* 24-bit data needs a multiple of 3 or the rate comes out inaccurate,
         * which the driver documents on the mclk_multiple field. */
        multiple = (format.bits == 24) ? I2S_MCLK_MULTIPLE_384 : I2S_MCLK_MULTIPLE_256;
        format.mclk_multiple = (format.bits == 24) ? 384 : 256;
        break;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = format.rate_hz,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = multiple,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)format.bits, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = format.mclk_pin >= 0 ? (gpio_num_t)format.mclk_pin : I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)bclk,
            .ws = (gpio_num_t)ws,
            .dout = dout >= 0 ? (gpio_num_t)dout : I2S_GPIO_UNUSED,
            .din = din >= 0 ? (gpio_num_t)din : I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    std_cfg.slot_cfg.slot_bit_width = (i2s_slot_bit_width_t)slot_bits();

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        audio_bus_close();
        return err;
    }

    if (rx_chan) {
        err = i2s_channel_init_std_mode(rx_chan, &std_cfg);
        if (err != ESP_OK) {
            audio_bus_close();
            return err;
        }
    }

    bus_open = true;
    return ESP_OK;
}

uint64_t audio_bus_buffered_frames(void)
{
    return (uint64_t)DMA_DESC_NUM * DMA_FRAME_NUM;
}

size_t audio_bus_block_frames(void)
{
    return DMA_FRAME_NUM;
}

esp_err_t audio_bus_tx_enable(bool enable)
{
    if (!tx_chan) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enable == tx_running) {
        return ESP_OK;
    }

    esp_err_t err = enable ? i2s_channel_enable(tx_chan) : i2s_channel_disable(tx_chan);
    if (err == ESP_OK) {
        tx_running = enable;
    }
    return err;
}

bool audio_bus_tx_enabled(void)
{
    return tx_running;
}

esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms)
{
    if (!tx_chan || !tx_running) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(tx_chan, data, bytes, written, timeout_ms);
}

#else /* !SOC_I2S_SUPPORTED */

/*
 * No I2S peripheral on this chip. The commands stay in the menu and report
 * why they cannot work, which is more useful during bring-up than a missing
 * command -- the same choice sd.c makes for `sd mmc` on the ESP32-C3.
 */

size_t audio_bus_frame_bytes(void) { return 4; }
bool audio_bus_ready(void) { return false; }

bool audio_bus_require(void)
{
    bp_error("%s has no I2S peripheral, so audio cannot be tested on it",
             CONFIG_IDF_TARGET);
    return false;
}

const audio_format_t *audio_bus_format(void) { return NULL; }

void audio_bus_pins(int *bclk, int *ws, int *dout, int *din, int *mclk)
{
    if (bclk) *bclk = -1;
    if (ws)   *ws = -1;
    if (dout) *dout = -1;
    if (din)  *din = -1;
    if (mclk) *mclk = -1;
}

uint32_t audio_bus_actual_rate(void) { return 0; }
uint32_t audio_bus_bclk_hz(void) { return 0; }
uint32_t audio_bus_mclk_hz(void) { return 0; }
uint64_t audio_bus_buffered_frames(void) { return 0; }
size_t audio_bus_block_frames(void) { return 240; }
void audio_bus_close(void) {}

esp_err_t audio_bus_open(int bclk, int ws, int dout, int din,
                         const audio_format_t *fmt)
{
    (void)bclk; (void)ws; (void)dout; (void)din; (void)fmt;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_bus_tx_enable(bool enable) { (void)enable; return ESP_ERR_NOT_SUPPORTED; }
bool audio_bus_tx_enabled(void) { return false; }

esp_err_t audio_bus_write(const void *data, size_t bytes, size_t *written,
                          uint32_t timeout_ms)
{
    (void)data; (void)bytes; (void)timeout_ms;
    if (written) {
        *written = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* SOC_I2S_SUPPORTED */
