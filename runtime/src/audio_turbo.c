/* audio_turbo.c - the fast-forward's time compressor (audio_turbo.h).
 *
 * WSOLA, our own: grains of AT_N frames under a periodic Hann window, one
 * every AT_HS output frames (50% overlap: the two halves add up to 1). The
 * next grain's input position is the last one's plus the analysis hop Ha =
 * AT_HS x ratio, then moved by up to AT_DELTA frames to where it best matches
 * the last grain's natural continuation (its input right after the overlap),
 * so the waveforms meet in phase. The ratio is the measured input over the
 * output the clock asked for (any speed, also a varying one), corrected by
 * how far the input held is from what the next grain needs (its hop, the
 * search and the grain, plus a hop of margin): the delay added grows with
 * the speed, ~60 ms at 2x, ~0.2 s at 16x. */
#include "audio_turbo.h"

#include <math.h>
#include <string.h>

enum {
    AT_CAP = 1 << 17,          /* input ring: ~3 s at 44.1 kHz */
    AT_MASK = AT_CAP - 1,
    AT_N = 1024,               /* grain: 23 ms */
    AT_HS = AT_N / 2,          /* synthesis hop */
    AT_DELTA = 512,            /* search: +-11.6 ms (down to ~86 Hz) */
    AT_LC = AT_HS,             /* compared length: the overlap */
    AT_MARGIN = AT_N + AT_DELTA + AT_HS,   /* held past the next hop */
    AT_MEASURE = 4410,         /* ratio measured over 0.1 s of output */
};

static int16_t s_in[AT_CAP][2];
static int64_t s_end;            /* frames pushed (absolute) */
static int64_t s_rd;             /* direct: the next frame handed out */
static int     s_mode;           /* 0 direct, 1 compressing */
static int64_t s_prev;           /* compressing: the last grain's input start */
static float   s_pend[AT_HS][2]; /* its falling half, under the next grain */
static int16_t s_fin[AT_HS][2];  /* a finished hop not yet handed out */
static int     s_fin_n, s_fin_off;
static float   s_win[AT_N];
static int     s_win_ok;
static double  s_ratio = 1.0;
static int64_t s_in_since, s_out_since;   /* input, output asked for */
static uint64_t s_grains, s_jumps;

static void win_init(void) {
    if (s_win_ok) return;
    for (int i = 0; i < AT_N; i++)
        s_win[i] = 0.5f - 0.5f * cosf(6.283185307f * (float)i / (float)AT_N);
    s_win_ok = 1;
}

static inline float sx(int64_t i, int c) { return (float)s_in[i & AT_MASK][c] * (1.0f / 32768.0f); }
static inline float mono(int64_t i) { return (float)s_in[i & AT_MASK][0] + (float)s_in[i & AT_MASK][1]; }
static inline int16_t clamp16f(float v) {
    v *= 32768.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

void at_reset(void) {
    s_end = s_rd = s_prev = 0;
    s_mode = 0;
    s_fin_n = s_fin_off = 0;
    s_ratio = 1.0;
    s_in_since = s_out_since = 0;
    memset(s_pend, 0, sizeof s_pend);
}

void at_push(const int16_t *in, int frames) {
    for (int f = 0; f < frames; f++) {
        int16_t *d = s_in[s_end & AT_MASK];
        d[0] = in[f * 2];
        d[1] = in[f * 2 + 1];
        s_end++;
    }
    s_in_since += frames;
    /* Never let the ring lap what is still to be read (a pull that did not
     * come): direct drops the oldest, compressing jumps ahead (below). */
    if (s_mode == 0 && s_end - s_rd > AT_CAP - AT_N) s_rd = s_end - (AT_CAP - AT_N);
}

/* Normalized correlation of the candidate at p with the continuation at nat. */
static float corr(int64_t p, int64_t nat, int step) {
    float xy = 0.0f, yy = 1e-9f;
    for (int i = 0; i < AT_LC; i += step) {
        const float x = mono(nat + i), y = mono(p + i);
        xy += x * y;
        yy += y * y;
    }
    return xy / sqrtf(yy);
}

static int64_t best_pos(int64_t lo, int64_t hi, int64_t nat) {
    float best = -1e30f;
    int64_t bp = lo;
    for (int64_t p = lo; p <= hi; p += 4) {
        const float c = corr(p, nat, 2);
        if (c > best) { best = c; bp = p; }
    }
    const int64_t a = bp - 3 < lo ? lo : bp - 3, b = bp + 3 > hi ? hi : bp + 3;
    best = -1e30f;
    for (int64_t p = a; p <= b; p++) {
        const float c = corr(p, nat, 1);
        if (c > best) { best = c; bp = p; }
    }
    return bp;
}

static int take_fin(int16_t *out, int room) {
    int n = s_fin_n - s_fin_off;
    if (n > room) n = room;
    if (n <= 0) return 0;
    memcpy(out, s_fin[s_fin_off], (size_t)n * 2 * sizeof(int16_t));
    s_fin_off += n;
    return n;
}

static void measure(int asked) {
    s_out_since += asked;
    if (s_out_since < AT_MEASURE) return;
    double r = (double)s_in_since / (double)s_out_since;
    if (r < 0.5) r = 0.5;
    if (r > 64.0) r = 64.0;
    s_ratio = 0.6 * s_ratio + 0.4 * r;
    s_in_since = s_out_since = 0;
}

int at_pull(int16_t *out, int out_cap, int want, int compress) {
    win_init();
    int n = 0;
    if (!compress) {
        /* Back to the input as it came: the hop in hand first, then from the
         * last grain's natural continuation (the pending falling half plus
         * that input's rising half are the input itself). */
        if (s_mode == 1) { s_rd = s_prev + AT_HS; s_mode = 0; }
        n = take_fin(out, out_cap);
        while (n < out_cap && s_rd < s_end) {
            const int16_t *d = s_in[s_rd & AT_MASK];
            out[n * 2] = d[0];
            out[n * 2 + 1] = d[1];
            s_rd++;
            n++;
        }
        measure(n);
        return n;
    }
    if (want > out_cap) want = out_cap;
    if (s_mode == 0) {
        /* Start as if a grain ended on what was just played: its falling half
         * is the input from here on, so the first grain overlaps in phase. */
        if (s_end - s_rd < AT_HS) return take_fin(out, want);
        n = take_fin(out, want);
        for (int i = 0; i < AT_HS; i++)
            for (int c = 0; c < 2; c++) s_pend[i][c] = sx(s_rd + i, c) * s_win[AT_HS + i];
        s_prev = s_rd - AT_HS;
        s_mode = 1;
    } else {
        n = take_fin(out, want);
    }
    while (n < want) {
        /* A stall (the pulls stopped) left too much behind: start from near the
         * newest input; the pending half fades out under the new grain. */
        if (s_end - s_prev > AT_CAP / 2) {
            s_prev = s_end - (int64_t)(AT_HS * s_ratio) - AT_MARGIN;
            s_jumps++;
        }
        const int64_t nat = s_prev + AT_HS;
        const double target = (double)AT_HS * s_ratio + (double)AT_MARGIN;
        double ha = (double)AT_HS * s_ratio + ((double)(s_end - s_prev) - target) * 0.25;
        if (ha < 0.75 * AT_HS) ha = 0.75 * AT_HS;
        if (ha > 64.0 * AT_HS) ha = 64.0 * AT_HS;
        int64_t p0 = s_prev + (int64_t)ha;
        /* The speed fell (the ratio still lags): take the grain from the input
         * there is rather than wait for more, down to the shortest hop. */
        if (p0 + AT_DELTA + AT_N > s_end) {
            p0 = s_end - AT_DELTA - AT_N;
            if (p0 < s_prev + (int64_t)(0.75 * AT_HS)) break;   /* wait for input */
        }
        const int64_t lo = p0 - AT_DELTA;
        int64_t hi = p0 + AT_DELTA;
        if (hi + AT_N > s_end) hi = s_end - AT_N;
        if (hi < lo) break;
        const int64_t p = best_pos(lo, hi, nat);
        for (int i = 0; i < AT_HS; i++) {
            for (int c = 0; c < 2; c++) {
                s_fin[i][c] = clamp16f(s_pend[i][c] + sx(p + i, c) * s_win[i]);
                s_pend[i][c] = sx(p + AT_HS + i, c) * s_win[AT_HS + i];
            }
        }
        s_prev = p;
        s_grains++;
        s_fin_n = AT_HS;
        s_fin_off = 0;
        n += take_fin(out + n * 2, want - n);
    }
    measure(want);
    return n;
}

int at_backlog(void) {
    return (int)(s_mode ? s_end - s_prev - AT_HS : s_end - s_rd) + (s_fin_n - s_fin_off);
}

void at_stats(uint64_t *grains, double *ratio, uint64_t *jumps, int *mode) {
    *grains = s_grains;
    *ratio = s_ratio;
    *jumps = s_jumps;
    *mode = s_mode;
}
