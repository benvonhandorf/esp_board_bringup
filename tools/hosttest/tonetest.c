/* Host-side check of the integer DSP lifted verbatim from main/audio/tone.c. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define SINE_BITS  10
#define SINE_SIZE  (1 << SINE_BITS)
#define SINE_MASK  (SINE_SIZE - 1)

static int16_t sine_table[SINE_SIZE];

static void sine_table_init(void)
{
    for (int i = 0; i < SINE_SIZE; i++)
        sine_table[i] = (int16_t)lrint(32767.0 * sin(2.0 * M_PI * i / SINE_SIZE));
}

static inline int32_t sine_at(uint32_t phase)
{
    uint32_t index = phase >> (32 - SINE_BITS);
    uint32_t frac = (phase >> (32 - SINE_BITS - 8)) & 0xFF;
    int32_t a = sine_table[index];
    int32_t b = sine_table[(index + 1) & SINE_MASK];
    return a + (((b - a) * (int32_t)frac) >> 8);
}

static inline uint32_t phase_step(double hz, double rate)
{
    return (uint32_t)llrint(hz / rate * 4294967296.0);
}

static inline int32_t fade_gain(uint32_t n, uint32_t span)
{
    if (span == 0 || n >= span) return 32768;
    uint32_t phase = (uint32_t)(((uint64_t)n << 30) / span);
    int32_t s = sine_at(phase);
    return (s * s) >> 15;
}

static double instantaneous_hz(double f0, double f1, bool logarithmic,
                               double seconds, double elapsed)
{
    if (f1 == f0 || seconds <= 0.0) return f0;
    double t = elapsed / seconds;
    if (t > 1.0) t = 1.0;
    if (logarithmic) return f0 * pow(f1 / f0, t);
    return f0 + (f1 - f0) * t;
}

static void put_frame(void *buffer, size_t i, int32_t sample, uint8_t bits, int channel)
{
    int32_t left  = (channel == 2) ? 0 : sample;   /* 2 = RIGHT-only */
    int32_t right = (channel == 1) ? 0 : sample;   /* 1 = LEFT-only  */
    if (bits == 16) {
        int16_t *out = (int16_t *)buffer;
        out[2*i] = (int16_t)left; out[2*i+1] = (int16_t)right;
    } else {
        int shift = (bits == 24) ? 8 : 16;
        int32_t *out = (int32_t *)buffer;
        out[2*i] = left << shift; out[2*i+1] = right << shift;
    }
}

#define RATE 48000
#define BLOCK_FRAMES 256

/* tone.c computes this as FADE_MS * rate / 1000; at 48 kHz that is 240. */
#define FADE_MS 5
#define FADE_FRAMES ((uint32_t)((uint64_t)FADE_MS * RATE / 1000))

int main(void)
{
    sine_table_init();
    int fails = 0;

    /* 1. Table purity: interpolated lookup vs true sine, over a full sweep. */
    double worst = 0, energy = 0, err_energy = 0;
    for (uint64_t p = 0; p < 4294967296ULL; p += 65537) {
        double ideal = 32767.0 * sin(2.0 * M_PI * (double)p / 4294967296.0);
        double got = sine_at((uint32_t)p);
        double e = fabs(got - ideal);
        if (e > worst) worst = e;
        energy += ideal * ideal; err_energy += e * e;
    }
    double sinad_db = 10.0 * log10(energy / err_energy);
    printf("1. sine table: worst error %.1f LSB, error floor %.1f dB below signal\n",
           worst, sinad_db);
    if (sinad_db < 70.0) { printf("   FAIL: distortion worse than -70 dB\n"); fails++; }

    /* 2. Frequency accuracy: count zero crossings of a 1 kHz tone over 1 s. */
    uint32_t phase = 0, step = phase_step(1000.0, RATE);
    int crossings = 0; int32_t prev = sine_at(0);
    for (int n = 1; n < RATE; n++) {
        phase += step;
        int32_t s = sine_at(phase);
        if (prev < 0 && s >= 0) crossings++;
        prev = s;
    }
    printf("2. 1000 Hz for 1 s: %d rising zero crossings (want 1000)\n", crossings);
    if (abs(crossings - 1000) > 1) { printf("   FAIL\n"); fails++; }

    /* 3. Amplitude at level 25%% should be -12.04 dBFS. */
    int32_t amplitude = 25 * 32767 / 100;
    int32_t peak = 0;
    phase = 0; step = phase_step(1000.0, RATE);
    for (int n = 0; n < RATE; n++) {
        int32_t s = (sine_at(phase) * amplitude) >> 15;
        if (labs(s) > peak) peak = labs(s);
        phase += step;
    }
    double dbfs = 20.0 * log10((double)peak / 32767.0);
    printf("3. level 25%%: peak %d, %.2f dBFS (want -12.04)\n", peak, dbfs);
    if (fabs(dbfs + 12.04) > 0.2) { printf("   FAIL\n"); fails++; }

    /* 4. Fade is monotonic, starts at silence and reaches unity. */
    uint32_t fade = FADE_FRAMES;
    int32_t last = -1; bool monotonic = true;
    for (uint32_t n = 0; n <= fade; n++) {
        int32_t g = fade_gain(n, fade);
        if (g < last) monotonic = false;
        last = g;
    }
    printf("4. fade over %u frames: g(0)=%d g(mid)=%d g(end)=%d, monotonic %s\n",
           fade, fade_gain(0, fade), fade_gain(fade/2, fade), fade_gain(fade, fade),
           monotonic ? "yes" : "NO");
    if (fade_gain(0, fade) != 0 || fade_gain(fade, fade) != 32768 || !monotonic) {
        printf("   FAIL\n"); fails++;
    }
    /* Raised cosine is exactly half way up at the midpoint. */
    if (abs(fade_gain(fade/2, fade) - 16384) > 64) {
        printf("   FAIL: midpoint is not half gain\n"); fails++;
    }

    /* 5. No clipping when a full-scale tone meets a full-scale fade. */
    int32_t maxmag = 0;
    amplitude = 100 * 32767 / 100;
    phase = 0;
    for (int n = 0; n < RATE; n++) {
        int32_t s = (sine_at(phase) * amplitude) >> 15;
        s = (s * fade_gain((uint32_t)n < fade ? (uint32_t)n : fade, fade)) >> 15;
        if (labs(s) > maxmag) maxmag = labs(s);
        phase += step;
    }
    printf("5. level 100%%: peak magnitude %d (int16 limit 32767)\n", maxmag);
    if (maxmag > 32767) { printf("   FAIL: clips\n"); fails++; }

    /* 6. Log sweep hits both endpoints and is monotonic. */
    double f0 = 100, f1 = 10000, secs = 5.0;
    printf("6. log sweep %.0f->%.0f over %.0fs: t=0 %.1f, t=half %.1f, t=end %.1f\n",
           f0, f1, secs,
           instantaneous_hz(f0, f1, true, secs, 0.0),
           instantaneous_hz(f0, f1, true, secs, secs/2),
           instantaneous_hz(f0, f1, true, secs, secs));
    if (fabs(instantaneous_hz(f0, f1, true, secs, secs) - f1) > 1.0) {
        printf("   FAIL: does not reach the end frequency\n"); fails++;
    }
    /* Equal time per octave: the midpoint is the geometric mean. */
    if (fabs(instantaneous_hz(f0, f1, true, secs, secs/2) - sqrt(f0*f1)) > 1.0) {
        printf("   FAIL: not logarithmic\n"); fails++;
    }

    /* 7. Frame packing and buffer bounds for each width. */
    struct { uint8_t bits; size_t frame_bytes; } widths[] = {
        {16, 4}, {24, 8}, {32, 8},
    };
    for (size_t w = 0; w < 3; w++) {
        unsigned char buf[BLOCK_FRAMES * 8 + 16];
        memset(buf, 0xAA, sizeof(buf));
        size_t used = BLOCK_FRAMES * widths[w].frame_bytes;
        for (size_t i = 0; i < BLOCK_FRAMES; i++)
            put_frame(buf, i, 0x1234, widths[w].bits, 0);
        bool overran = false;
        for (size_t i = used; i < sizeof(buf); i++)
            if (buf[i] != 0xAA) overran = true;
        int32_t first = (widths[w].bits == 16) ? ((int16_t *)buf)[0] : ((int32_t *)buf)[0];
        printf("7.%zu %u-bit: frame %zu bytes, first slot 0x%08x, overrun %s\n",
               w, widths[w].bits, widths[w].frame_bytes, first, overran ? "YES" : "no");
        if (overran) { printf("   FAIL: wrote past the buffer\n"); fails++; }
    }
    /* Left-only must leave the right slot silent, and vice versa. */
    int16_t pair[2];
    put_frame(pair, 0, 1000, 16, 1);
    printf("7.L left-only frame: {%d, %d}\n", pair[0], pair[1]);
    if (pair[0] != 1000 || pair[1] != 0) { printf("   FAIL\n"); fails++; }
    put_frame(pair, 0, 1000, 16, 2);
    printf("7.R right-only frame: {%d, %d}\n", pair[0], pair[1]);
    if (pair[0] != 0 || pair[1] != 1000) { printf("   FAIL\n"); fails++; }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "all checks passed", fails);
    return fails != 0;
}
