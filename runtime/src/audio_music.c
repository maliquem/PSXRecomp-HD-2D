/* audio_music.c - the music at its own tempo under the fast-forward
 * (audio_music.h). */
#include "audio_music.h"

#include <stdlib.h>
#include <string.h>

#include "audio_turbo.h"
#include "spu.h"
#include "spu_clone.h"

uint64_t psx_get_cycle_count(void);

enum {
    AM_CYC = 768,                  /* guest cycles per SPU frame (spu.c) */
    AM_XF = 44100 * 2 / 5,         /* the crossfade to the guest's sound: 0.4 s */
    AM_RS_FADE = 44100 / 4,        /* a catch-up's fade out, then in: 0.25 s each */
    AM_Q = 1 << 19,                /* accesses held for the music SPU */
    AM_BUF = 44100,
};
#define AM_CPS 33868800.0
/* Further behind than this, catch up anyway (a title that reports its song
 * changes catches up on those; this only bounds the accesses held). */
#define AM_MAX_LAG_S 300.0

typedef struct { uint64_t cycle; uint32_t addr, value; uint32_t kind; } AmEvent;

static uint32_t (*s_classify)(void);
static int s_state;                /* 0 idle, 1 running, 2 crossfading out */
static AmEvent *s_q;
static uint32_t s_qh, s_qt;        /* head (next to apply), tail */
static int s_q_over;
static uint64_t s_mclock;          /* the music SPU's clock */
static int s_inited;
static uint8_t *s_snap;
static uint32_t s_snap_cap;
static int s_xf_left;
static float s_mgain = 1.0f;       /* the music SPU's gain (a catch-up's fades) */
static int s_rs_phase;             /* 0 none, 1 fading out, 2 fading in */
static int s_changed;
static uint32_t s_resyncs, s_entries;
uint32_t g_am_dbg[8];   /* TCP audio_turbo "dbg" (debug_server.c) */
static int16_t s_sfx[AM_BUF * 2], s_mus[AM_BUF * 2];

void am_set_classifier(uint32_t (*music_voices)(void)) { s_classify = music_voices; }
int  am_ready(void) { return s_classify != NULL; }
int  am_active(void) { return s_state != 0; }
void am_music_changed(void) { if (s_state == 1) s_changed = 1; }

static inline int16_t sat16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* Voices `mask` out of a clone: keyed off (their release tails fade out; their
 * other registers stay: a driver that writes a register only when its own copy
 * changes, as BoF4's, would never give a zeroed volume back). */
static void silence(void (*wr)(uint32_t, uint32_t), uint32_t mask) {
    wr(0x1F801D8Cu, mask & 0xFFFFu);
    wr(0x1F801D8Eu, (mask >> 16) & 0xFFu);
}

/* The guest SPU as it is now, into a buffer. */
static const uint8_t *snapshot_live(uint32_t *len) {
    const uint32_t n = spu_snapshot_bytes();
    if (n > s_snap_cap) {
        uint8_t *p = (uint8_t *)realloc(s_snap, n);
        if (!p) return NULL;
        s_snap = p;
        s_snap_cap = n;
    }
    spu_snapshot_write(s_snap);
    *len = n;
    return s_snap;
}

/* A clone's sample RAM from the guest's (the snapshot has the registers and
 * voices only: a savestate keeps SPU RAM apart). */
static void copy_ram(uint8_t *to) { memcpy(to, spu_get_ram_ptr(), spu_get_ram_bytes()); }

/* The music SPU from the guest's present, without the effects' voices. */
static int music_from_live(const uint8_t *snap, uint32_t len) {
    s_mclock = psx_get_cycle_count();
    spum_set_clock(s_mclock);
    if (!spum_snapshot_read(snap, len)) return 0;
    copy_ram(spum_get_ram_ptr());
    silence(spum_write, ~s_classify() & 0xFFFFFFu);
    spum_output_flush();
    s_qh = s_qt = 0;
    s_q_over = 0;
    return 1;
}

static void q_push(uint32_t kind, uint32_t addr, uint32_t value) {
    if (s_q_over) return;
    if (s_qt - s_qh >= AM_Q) { s_q_over = 1; return; }   /* caught up below */
    AmEvent *e = &s_q[s_qt & (AM_Q - 1)];
    e->cycle = psx_get_cycle_count();
    e->kind = kind;
    e->addr = addr;
    e->value = value;
    s_qt++;
}

/* Every guest access to the SPU while running: the effects SPU now, the music
 * SPU later; a key-on goes to one or the other by its voices' owner. */
static void tap(int kind, uint32_t addr, uint32_t value) {
    if (kind == 0) {
        uint32_t mus = value;
        if (addr == 0x1F801D88u || addr == 0x1F801D8Au) {
            const uint32_t m = s_classify();
            const uint32_t half = addr == 0x1F801D88u ? (m & 0xFFFFu) : ((m >> 16) & 0xFFu);
            spus_write(addr, value & ~half);
            mus = value & half;
            for (uint32_t b = mus; b; b &= b - 1) g_am_dbg[3]++;
        } else {
            spus_write(addr, value);
        }
        q_push(0, addr, mus);
    } else if (kind == 1) {
        spus_dma_write(value);
        q_push(1, 0, value);
    } else {
        (void)spus_dma_read();
        q_push(2, 0, 0);
    }
}

static int enter(void) {
    if (!s_q) {
        s_q = (AmEvent *)malloc(sizeof(AmEvent) * AM_Q);
        if (!s_q) return 0;
    }
    if (!s_inited) { spus_init(); spum_init(); s_inited = 1; }
    uint32_t len = 0;
    const uint8_t *snap = snapshot_live(&len);
    if (!snap || !spus_snapshot_read(snap, len)) return 0;
    copy_ram(spus_get_ram_ptr());
    g_am_dbg[4] = s_classify();
    silence(spus_write, g_am_dbg[4] & 0xFFFFFFu);
    spus_output_flush();
    if (!music_from_live(snap, len)) return 0;
    s_mgain = 1.0f;
    s_rs_phase = 0;
    s_changed = 0;
    s_entries++;
    spu_io_tap = tap;
    s_state = 1;
    return 1;
}

static void stop(void) {
    spu_io_tap = NULL;
    s_state = 0;
    s_qh = s_qt = 0;
}

void am_reset(void) { stop(); }

/* The music SPU's next n frames at the wall clock's pace: the accesses held up
 * to there applied at their own guest cycles, the frames between them made. */
static void music_render(int16_t *out, int n) {
    const uint64_t target = s_mclock + (uint64_t)n * AM_CYC;
    while (s_qh != s_qt && s_q[s_qh & (AM_Q - 1)].cycle <= target) {
        const AmEvent *e = &s_q[s_qh & (AM_Q - 1)];
        if (e->cycle > s_mclock) { s_mclock = e->cycle; spum_set_clock(s_mclock); }
        if (e->kind == 0) spum_write(e->addr, e->value);
        else if (e->kind == 1) spum_dma_write(e->value);
        else (void)spum_dma_read();
        s_qh++;
        g_am_dbg[2]++;
    }
    s_mclock = target;
    spum_set_clock(s_mclock);
    spum_catch_up();
    int got = (int)spum_output_pop(out, (uint32_t)n);
    g_am_dbg[6] += (uint32_t)got;
    for (int i = 0; i < got * 2; i++) { const uint32_t a = (uint32_t)(out[i] < 0 ? -out[i] : out[i]); if (a > g_am_dbg[0]) g_am_dbg[0] = a; }
    if (got < n) memset(out + got * 2, 0, (size_t)(n - got) * 2 * sizeof(int16_t));
    /* a catch-up: fade out, take the guest's present, fade in */
    const uint64_t lag = psx_get_cycle_count() - s_mclock;
    if (s_rs_phase == 0 && (s_changed || s_q_over || (double)lag > AM_MAX_LAG_S * AM_CPS)) s_rs_phase = 1;
    for (int i = 0; i < n; i++) {
        if (s_rs_phase == 1) {
            s_mgain -= 1.0f / AM_RS_FADE;
            if (s_mgain <= 0.0f) {
                s_mgain = 0.0f;
                uint32_t len = 0;
                const uint8_t *snap = snapshot_live(&len);
                if (snap) music_from_live(snap, len);
                s_changed = 0;
                s_resyncs++;
                s_rs_phase = 2;
                /* the rest of this block from the new music SPU is not made yet: silent */
                for (int k = i; k < n; k++) out[k * 2] = out[k * 2 + 1] = 0;
                return;
            }
        } else if (s_rs_phase == 2) {
            s_mgain += 1.0f / AM_RS_FADE;
            if (s_mgain >= 1.0f) { s_mgain = 1.0f; s_rs_phase = 0; }
        }
        if (s_mgain < 1.0f) {
            out[i * 2] = (int16_t)((float)out[i * 2] * s_mgain);
            out[i * 2 + 1] = (int16_t)((float)out[i * 2 + 1] * s_mgain);
        }
    }
}

static int sfx_pull(void) {
    spus_catch_up();
    const int n = (int)spus_output_pop(s_sfx, AM_BUF);
    g_am_dbg[5] += (uint32_t)n;
    for (int i = 0; i < n * 2; i++) { const uint32_t a = (uint32_t)(s_sfx[i] < 0 ? -s_sfx[i] : s_sfx[i]); if (a > g_am_dbg[1]) g_am_dbg[1] = a; }
    return n;
}

int am_pump(const int16_t *live, int n_live, int16_t *out, int out_cap, int want, int ff) {
    if (out_cap > AM_BUF) out_cap = AM_BUF;
    if (s_state == 0) {
        if (!(ff && s_classify && enter())) {
            at_push(live, n_live);
            return at_pull(out, out_cap, 0, 0);
        }
    }
    if (s_state == 1 && !ff) {
        at_jump_direct();
        s_xf_left = AM_XF;
        s_state = 2;
    }
    const int ns = sfx_pull();
    at_push(s_sfx, ns);
    if (s_state == 1) {
        const int n = at_pull(out, out_cap, want, 1);
        music_render(s_mus, n);
        for (int i = 0; i < n * 2; i++) out[i] = sat16((int32_t)out[i] + s_mus[i]);
        return n;
    }
    /* crossfading out: (effects + music SPUs) -> the guest's own sound */
    int n = at_pull(out, out_cap, 0, 0);
    if (n > n_live) n = n_live;
    music_render(s_mus, n_live);
    int m = n_live < out_cap ? n_live : out_cap;
    for (int i = 0; i < m; i++) {
        const float g = 1.0f - (float)s_xf_left / (float)AM_XF;
        for (int c = 0; c < 2; c++) {
            const int32_t a = (i < n ? out[i * 2 + c] : 0) + s_mus[i * 2 + c];
            out[i * 2 + c] = sat16((int32_t)((float)live[i * 2 + c] * g + (float)a * (1.0f - g)));
        }
        if (s_xf_left > 0) s_xf_left--;
    }
    if (s_xf_left <= 0) {
        stop();
        at_reset();
    }
    return m;
}

void am_stats(int *state, double *lag_ms, uint32_t *queued, uint32_t *resyncs, uint32_t *entries) {
    *state = s_state;
    *lag_ms = s_state ? (double)(psx_get_cycle_count() - s_mclock) * 1000.0 / AM_CPS : 0.0;
    *queued = s_qt - s_qh;
    *resyncs = s_resyncs;
    *entries = s_entries;
}
