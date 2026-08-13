/* Host-side check of the analysis lifted verbatim from main/audio/capture.c. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define AUDIO_BANDS 20
#define ANALYSIS_FRAMES 4096
#define BAND_BASE_HZ 31.25
#define ANALYSIS_FULL_SCALE 32768.0
#define AUDIO_DBFS_FLOOR (-120.0)

static double audio_dbfs(double amplitude, double full_scale)
{
    if (full_scale <= 0.0 || amplitude <= 0.0) return AUDIO_DBFS_FLOOR;
    double db = 20.0 * log10(amplitude / full_scale);
    return db < AUDIO_DBFS_FLOOR ? AUDIO_DBFS_FLOOR : db;
}

typedef struct { double coeff, sinw, cosw, s1, s2; } goertzel_t;

static void goertzel_init(goertzel_t *g, double hz, double rate)
{
    double w = 2.0 * M_PI * hz / rate;
    g->cosw = cos(w);
    g->sinw = sin(w);
    g->coeff = 2.0 * g->cosw;
    g->s1 = g->s2 = 0.0;
}

static inline void goertzel_push(goertzel_t *g, double x)
{
    double s0 = x + g->coeff * g->s1 - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;
}

static double goertzel_amplitude(const goertzel_t *g, uint32_t n)
{
    if (n == 0) return 0.0;
    double real = g->s1 - g->s2 * g->cosw;
    double imag = g->s2 * g->sinw;
    return 4.0 * sqrt(real * real + imag * imag) / (double)n;
}

/* --- fft() and analyse(), lifted verbatim from main/audio/capture.c --- */
typedef struct {
    uint32_t rate;
    double detect_hz;
    double tone_dbfs[2];
    uint32_t band_count;
    double band_hz[AUDIO_BANDS];
    double band_dbfs[2][AUDIO_BANDS];
    uint64_t analysed;
} cap_t;

static void fft(float *re, float *im, uint32_t n)
{
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (uint32_t len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * M_PI / (double)len;
        double wr = cos(angle);
        double wi = sin(angle);
        uint32_t half = len / 2;

        for (uint32_t i = 0; i < n; i += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (uint32_t k = 0; k < half; k++) {
                float ur = re[i + k];
                float ui = im[i + k];
                float xr = re[i + k + half];
                float xi = im[i + k + half];
                float vr = (float)(xr * cr - xi * ci);
                float vi = (float)(xr * ci + xi * cr);

                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + half] = ur - vr;
                im[i + k + half] = ui - vi;

                double next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
    }
}

/*
 * Convert summed bin power into the amplitude of the sinusoid that would carry
 * it.
 *
 * A Hann-windowed tone at bin centre puts N/4 of its amplitude in that bin and
 * N/8 in each neighbour, so its power sums to 3/32 of (A*N)^2 -- and the same
 * constant relates power to amplitude for noise spread across the band, which
 * is what makes one scale usable for both. Every bin belongs to exactly one
 * band, so nothing is counted twice and nothing falls through.
 */
static double band_power(double power, uint32_t n)
{
    if (n == 0 || power <= 0.0) {
        return 0.0;
    }
    return sqrt(power) / ((double)n * sqrt(3.0 / 32.0));
}


static void analyse(cap_t *cap, const int16_t *window, uint32_t n,
                    float *re, float *im)
{
    if (n < 64) {
        return;   /* too short for any of this to mean anything */
    }

    double rate = (double)cap->rate;
    uint32_t bands = 0;

    /* Half-octave centres, stopping short of Nyquist where the anti-alias
     * response of whatever is upstream is unknown anyway. */
    for (uint32_t i = 0; i < AUDIO_BANDS; i++) {
        double hz = BAND_BASE_HZ * pow(2.0, i / 2.0);
        if (hz >= rate * 0.45) {
            break;
        }
        cap->band_hz[bands++] = hz;
    }
    cap->band_count = bands;

    /* The FFT wants a power of two, and the capture may have stopped short of
     * one; analyse the largest that fits. */
    uint32_t fft_n = 1;
    while (fft_n * 2 <= n) {
        fft_n *= 2;
    }

    for (uint32_t ch = 0; ch < 2; ch++) {
        goertzel_t detector;
        if (cap->detect_hz > 0.0) {
            goertzel_init(&detector, cap->detect_hz, rate);
        }

        /* Hann by recurrence: cos((k+1)a) = 2 cos(a) cos(ka) - cos((k-1)a).
         * Two adds and a multiply per sample instead of a cosine. It costs
         * 1.4 dB of scalloping instead of 3.9, and keeps the skirts of a loud
         * tone out of unrelated bands. */
        double a = 2.0 * M_PI / (double)fft_n;
        double c = 2.0 * cos(a);
        double u_prev = 1.0;          /* cos(0) */
        double u = cos(a);

        for (uint32_t k = 0; k < fft_n; k++) {
            double w = 0.5 - 0.5 * u_prev;
            double x = window[2 * k + ch] * w;

            if (cap->detect_hz > 0.0) {
                goertzel_push(&detector, x);
            }
            if (re) {
                re[k] = (float)x;
                im[k] = 0.0f;
            }

            double next = c * u - u_prev;
            u_prev = u;
            u = next;
        }

        cap->tone_dbfs[ch] = (cap->detect_hz > 0.0)
            ? audio_dbfs(goertzel_amplitude(&detector, fft_n), ANALYSIS_FULL_SCALE)
            : AUDIO_DBFS_FLOOR;

        if (!re || bands == 0) {
            continue;
        }

        fft(re, im, fft_n);

        /*
         * Every positive-frequency bin is assigned to the band whose centre it
         * is nearest in log frequency, which is what the half-octave spacing
         * means: the bands tile the spectrum rather than sampling it. Bin
         * zero is skipped -- that is DC, already reported as the mean, and it
         * would otherwise dominate the bottom band on any part with an offset.
         */
        double power[AUDIO_BANDS] = {0};
        for (uint32_t k = 1; k < fft_n / 2; k++) {
            double hz = (double)k * rate / (double)fft_n;
            int band = (int)lrint(2.0 * log2(hz / BAND_BASE_HZ));
            if (band < 0 || band >= (int)bands) {
                continue;
            }
            power[band] += (double)re[k] * re[k] + (double)im[k] * im[k];
        }

        for (uint32_t i = 0; i < bands; i++) {
            cap->band_dbfs[ch][i] =
                audio_dbfs(band_power(power[i], fft_n), ANALYSIS_FULL_SCALE);
        }
    }

    cap->analysed = fft_n;
}

/* --- streaming stats, verbatim from slot_push / the reduction --- */
typedef struct { int32_t min, max; int64_t sum; double sum_sq; uint64_t clipped; } acc_t;

static void slot_push(acc_t *a, int32_t s, int32_t limit)
{
    if (s < a->min) a->min = s;
    if (s > a->max) a->max = s;
    a->sum += s;
    a->sum_sq += (double)s * (double)s;
    if (s >= limit || s <= -limit) a->clipped++;
}

/* ------------------------------------------------------------------ */

static int failures;

static void check(const char *what, double got, double want, double tol)
{
    bool ok = fabs(got - want) <= tol;
    printf("%-58s %9.3f (want %.3f +-%.3f) %s\n", what, got, want, tol,
           ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static void check_le(const char *what, double got, double limit)
{
    bool ok = got <= limit;
    printf("%-58s %9.3f (want <= %.3f)        %s\n", what, got, limit,
           ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static int16_t *make_tone(uint32_t n, double rate, double hz, double amp,
                          double dc, int channel)
{
    int16_t *w = calloc(n * 2, sizeof(int16_t));
    for (uint32_t k = 0; k < n; k++) {
        double v = amp * sin(2.0 * M_PI * hz * k / rate) + dc;
        int32_t s = (int32_t)lrint(v);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        if (channel != 1) w[2 * k] = (int16_t)s;
        if (channel != 0) w[2 * k + 1] = (int16_t)s;
    }
    return w;
}

int main(void)
{
    const uint32_t n = ANALYSIS_FRAMES;
    static float re_buf[ANALYSIS_FRAMES], im_buf[ANALYSIS_FRAMES];
    const double rate = 48000.0;

    /* 1. A tone exactly on a probe frequency, at a known level. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 1000.0};
        int16_t *w = make_tone(n, rate, 1000.0, 8192.0, 0.0, 2);
        analyse(&cap, w, n, re_buf, im_buf);
        check("1kHz at -12.04 dBFS, detector", cap.tone_dbfs[0], -12.04, 0.15);
        check("1kHz at -12.04 dBFS, both slots equal",
              cap.tone_dbfs[0] - cap.tone_dbfs[1], 0.0, 0.001);
        /* 31.25 * 2^(10/2) = 1000 exactly, so probe 10 is the tone. */
        check("probe 10 is 1000 Hz", cap.band_hz[10], 1000.0, 0.001);
        check("probe 10 level", cap.band_dbfs[0][10], -12.04, 0.15);
        check_le("probe 9 (707 Hz) leakage", cap.band_dbfs[0][9], -60.0);
        check_le("probe 11 (1414 Hz) leakage", cap.band_dbfs[0][11], -60.0);
        free(w);
    }

    /* 2. Off-probe frequency: the Hann window is what keeps this honest. A
     *    rectangular window would scallop by up to 3.9 dB here. */
    {
        double worst = 0.0;
        for (double hz = 990.0; hz <= 1010.0; hz += 0.7) {
            cap_t cap = {.rate = 48000, .detect_hz = hz};
            int16_t *w = make_tone(n, rate, hz, 8192.0, 0.0, 2);
            analyse(&cap, w, n, re_buf, im_buf);
            double err = fabs(cap.tone_dbfs[0] - (-12.04));
            if (err > worst) worst = err;
            free(w);
        }
        check_le("worst error over 990-1010 Hz, detector on the tone", worst, 0.2);
    }

    /* 3. A tone between two probes still shows up on both, near the right
     *    level -- that is what stops a bar chart hiding a signal. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 0.0};
        int16_t *w = make_tone(n, rate, 1189.2, 8192.0, 0.0, 2);  /* midway 1000/1414 */
        analyse(&cap, w, n, re_buf, im_buf);
        double best = fmax(cap.band_dbfs[0][10], cap.band_dbfs[0][11]);
        check_le("tone midway between probes, best probe within 12 dB",
                 -12.04 - best, 12.0);
        free(w);
    }

    /* 4. Silence reads the floor, not a small number that looks like signal. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 1000.0};
        int16_t *w = calloc(n * 2, sizeof(int16_t));
        analyse(&cap, w, n, re_buf, im_buf);
        check("silence, detector", cap.tone_dbfs[0], AUDIO_DBFS_FLOOR, 0.001);
        free(w);
    }

    /* 5. A DC offset must not register as a 1 kHz tone. A microphone with an
     *    offset is normal; counting it as signal would break every loopback. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 1000.0};
        int16_t *w = make_tone(n, rate, 1000.0, 8192.0, 3000.0, 2);
        analyse(&cap, w, n, re_buf, im_buf);
        check("1kHz with 3000 counts of DC, detector unchanged",
              cap.tone_dbfs[0], -12.04, 0.15);
        check_le("that DC does not light the 31 Hz probe",
                 cap.band_dbfs[0][0], -40.0);
        free(w);
    }

    /* 6. One slot only: the loopback verdict depends on telling them apart. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 1000.0};
        int16_t *w = make_tone(n, rate, 1000.0, 8192.0, 0.0, 1);  /* right only */
        analyse(&cap, w, n, re_buf, im_buf);
        check("right-only tone, right slot", cap.tone_dbfs[1], -12.04, 0.15);
        check("right-only tone, left slot", cap.tone_dbfs[0], AUDIO_DBFS_FLOOR, 0.001);
        free(w);
    }

    /* 7. Level scales as it should: halving the amplitude is -6 dB. */
    {
        cap_t a = {.rate = 48000, .detect_hz = 1000.0};
        cap_t b = {.rate = 48000, .detect_hz = 1000.0};
        int16_t *wa = make_tone(n, rate, 1000.0, 8192.0, 0.0, 2);
        int16_t *wb = make_tone(n, rate, 1000.0, 4096.0, 0.0, 2);
        analyse(&a, wa, n, re_buf, im_buf);
        analyse(&b, wb, n, re_buf, im_buf);
        check("halving amplitude is -6.02 dB",
              a.tone_dbfs[0] - b.tone_dbfs[0], 6.02, 0.05);
        free(wa);
        free(wb);
    }

    /* 8. Probe list stops below Nyquist at every rate offered. */
    {
        const uint32_t rates[] = {8000, 16000, 44100, 48000, 96000, 192000};
        double worst_ratio = 0.0;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
            cap_t cap = {.rate = rates[i], .detect_hz = 0.0};
            int16_t *w = calloc(n * 2, sizeof(int16_t));
            analyse(&cap, w, n, re_buf, im_buf);
            double top = cap.band_hz[cap.band_count - 1];
            double ratio = top / (rates[i] / 2.0);
            if (ratio > worst_ratio) worst_ratio = ratio;
            if (cap.band_count == 0) {
                printf("rate %u produced no probes                             FAIL\n",
                       rates[i]);
                failures++;
            }
            free(w);
        }
        check_le("highest probe as a fraction of Nyquist, worst rate",
                 worst_ratio, 0.9);
    }

    /* 9. Streaming stats against a signal whose mean and stdev are known:
     *    a sine of amplitude A about an offset has stdev A/sqrt(2). */
    {
        acc_t acc = {.min = INT32_MAX, .max = INT32_MIN};
        const uint32_t count = 48000;
        for (uint32_t k = 0; k < count; k++) {
            double v = 8192.0 * sin(2.0 * M_PI * 1000.0 * k / rate) + 1000.0;
            slot_push(&acc, (int32_t)lrint(v), 32767);
        }
        double mean = (double)acc.sum / count;
        double var = acc.sum_sq / count - mean * mean;
        check("mean of a sine about 1000 counts", mean, 1000.0, 1.0);
        check("stdev is A/sqrt(2)", sqrt(var), 8192.0 / sqrt(2.0), 2.0);
        check("no false clipping at 9192 peak", (double)acc.clipped, 0.0, 0.0);
    }

    /* 10. Clipping is counted when it happens. */
    {
        acc_t acc = {.min = INT32_MAX, .max = INT32_MIN};
        for (uint32_t k = 0; k < 1000; k++) {
            double v = 40000.0 * sin(2.0 * M_PI * 1000.0 * k / rate);
            int32_t s = (int32_t)lrint(v);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            slot_push(&acc, s, 32767);
        }
        check_le("clipping is noticed", -(double)acc.clipped, -100.0);
    }


    /* 11. Energy conservation: the bands should account for the signal's own
     *     power, which is the property that lets the bar chart be checked
     *     against the RMS column above it. */
    {
        cap_t cap = {.rate = 48000, .detect_hz = 0.0};
        int16_t *w = calloc(n * 2, sizeof(int16_t));
        srand(1);
        double sum_sq = 0.0;
        for (uint32_t k = 0; k < n; k++) {
            /* Noise, band-limited by construction to well inside the probes. */
            double v = 0.0;
            for (int h = 3; h < 400; h++)
                v += 40.0 * sin(2.0 * M_PI * h * 47.0 * k / rate + h);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            w[2 * k] = w[2 * k + 1] = (int16_t)lrint(v);
            sum_sq += v * v;
        }
        analyse(&cap, w, n, re_buf, im_buf);
        double band_total = 0.0;
        for (uint32_t i = 0; i < cap.band_count; i++) {
            double amp = pow(10.0, cap.band_dbfs[0][i] / 20.0) * ANALYSIS_FULL_SCALE;
            band_total += amp * amp / 2.0;      /* sinusoid power from amplitude */
        }
        double signal_power = sum_sq / n;
        check("summed band power vs signal power, dB",
              10.0 * log10(band_total / signal_power), 0.0, 1.0);
        free(w);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
