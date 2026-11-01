/*
 * present_phase.c — see present_phase.h.
 *
 * Clock: the QueryPerformanceCounter ticks SDL_GetPerformanceCounter returns on
 * Windows (the pacer's deadlines are in it). Elsewhere the hold stays inactive.
 */
#include "present_phase.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdatomic.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* ---- D3DKMT (gdi32, loaded at run time; layouts from the public WDK) ---- */
typedef LONG KmtStatus;
typedef UINT KmtHandle;
typedef struct { HDC hDc; KmtHandle hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } KmtOpenFromHdc;
typedef struct { KmtHandle hAdapter; KmtHandle hDevice; UINT VidPnSourceId; } KmtWaitVblank;
typedef struct { KmtHandle hAdapter; } KmtClose;
typedef KmtStatus (APIENTRY *KmtOpenFn)(KmtOpenFromHdc *);
typedef KmtStatus (APIENTRY *KmtWaitFn)(const KmtWaitVblank *);
typedef KmtStatus (APIENTRY *KmtCloseFn)(const KmtClose *);

static KmtOpenFn  s_kopen;
static KmtWaitFn  s_kwait;
static KmtCloseFn s_kclose;

static uint64_t qpc(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
static double s_freq;   /* ticks per second */

/* ---- the vblank grid (vblank thread writes, a seqlock) ---- */
static _Atomic uint32_t s_gseq;
static _Atomic uint64_t s_g_anchor, s_g_last, s_g_period_bits;   /* ticks; double bits */
static _Atomic uint64_t s_vblanks, s_vb_fail, s_vb_reopen;
static _Atomic(void *) s_hwnd;

typedef struct { uint64_t anchor, last; double period; } Grid;

static void grid_write(uint64_t anchor, uint64_t last, double period) {
    uint64_t bits;
    memcpy(&bits, &period, sizeof bits);
    atomic_fetch_add(&s_gseq, 1);
    atomic_store(&s_g_anchor, anchor);
    atomic_store(&s_g_last, last);
    atomic_store(&s_g_period_bits, bits);
    atomic_fetch_add(&s_gseq, 1);
}
static int grid_read(Grid *g) {
    for (int tries = 0; tries < 8; tries++) {
        const uint32_t a = atomic_load(&s_gseq);
        if (a & 1u) continue;
        g->anchor = atomic_load(&s_g_anchor);
        g->last = atomic_load(&s_g_last);
        const uint64_t bits = atomic_load(&s_g_period_bits);
        if (atomic_load(&s_gseq) != a) continue;
        memcpy(&g->period, &bits, sizeof g->period);
        return g->period > 0.0 && g->last != 0;
    }
    return 0;
}

static int cmp_i64(const void *a, const void *b) {
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}
static int cmp_dbl(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The grid from the last vblank times: the period over the last ~2 s (whole
 * refreshes counted, so a missed wakeup does not matter), the phase from the
 * earliest wakeups of the last 64 (a wakeup is never early, only late). */
enum { VB_RING = 256 };
static uint64_t s_vb[VB_RING];
static unsigned s_vbn;

static void vb_note(uint64_t t) {
    s_vb[s_vbn++ % VB_RING] = t;
    atomic_fetch_add(&s_vblanks, 1);
    const unsigned n = s_vbn < VB_RING ? s_vbn : VB_RING;
    if (n < 12) return;
    double iv[32];
    int ni = 0;
    for (unsigned i = 1; i < n && ni < 32; i++) {
        const uint64_t a = s_vb[(s_vbn - i) % VB_RING], b = s_vb[(s_vbn - i - 1) % VB_RING];
        iv[ni++] = (double)(a - b);
    }
    qsort(iv, (size_t)ni, sizeof iv[0], cmp_dbl);
    const double pm = iv[ni / 2];
    if (pm <= 0.0) return;
    const uint64_t newest = s_vb[(s_vbn - 1) % VB_RING], oldest = s_vb[(s_vbn - n) % VB_RING];
    const double span = (double)(newest - oldest);
    const double k = floor(span / pm + 0.5);
    double period = k > 0.0 ? span / k : pm;
    if (fabs(period - pm) > pm * 0.05) period = pm;
    int64_t res[64];
    const unsigned m = n < 64 ? n : 64;
    for (unsigned i = 0; i < m; i++) {
        const double d = (double)(int64_t)(s_vb[(s_vbn - 1 - i) % VB_RING] - newest);
        res[i] = (int64_t)(d - floor(d / period + 0.5) * period);
    }
    qsort(res, m, sizeof res[0], cmp_i64);
    grid_write((uint64_t)((int64_t)newest + res[m / 10]), newest, period);
}

static DWORD WINAPI vblank_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    KmtHandle adapter = 0;
    UINT source = 0;
    HMONITOR mon = NULL;
    uint64_t checked = 0;
    for (;;) {
        const uint64_t now = qpc();
        if (!adapter || (double)(now - checked) > s_freq) {
            checked = now;
            HWND hwnd = (HWND)atomic_load(&s_hwnd);
            HMONITOR m = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : NULL;
            if (m != mon || !adapter) {
                if (adapter) { KmtClose c = { adapter }; s_kclose(&c); adapter = 0; }
                mon = m;
                MONITORINFOEXW mi;
                memset(&mi, 0, sizeof mi);
                mi.cbSize = sizeof mi;
                if (m && GetMonitorInfoW(m, (MONITORINFO *)&mi)) {
                    HDC dc = CreateDCW(NULL, mi.szDevice, NULL, NULL);
                    if (dc) {
                        KmtOpenFromHdc o;
                        memset(&o, 0, sizeof o);
                        o.hDc = dc;
                        if (s_kopen(&o) == 0) { adapter = o.hAdapter; source = o.VidPnSourceId; }
                        DeleteDC(dc);
                    }
                }
                s_vbn = 0;   /* another screen: a new grid */
                atomic_fetch_add(&s_vb_reopen, 1);
            }
            if (!adapter) { Sleep(100); continue; }
        }
        KmtWaitVblank w = { adapter, 0, source };
        const KmtStatus st = s_kwait(&w);
        const uint64_t t = qpc();
        if (st != 0) {
            atomic_fetch_add(&s_vb_fail, 1);
            KmtClose c = { adapter };
            s_kclose(&c);
            adapter = 0;
            Sleep(20);
            continue;
        }
        vb_note(t);
    }
    return 0;
}

/* ---- the hold (composing thread) ---- */
enum { RD_RING = 120 };
static int64_t  s_rd[RD_RING];         /* ready - ref, ticks */
static unsigned s_rdn;
static HANDLE   s_timer;
static _Atomic uint64_t s_guest_period;   /* ticks, from the emulation thread */
static _Atomic int64_t  s_err;            /* slack - half a refresh, ticks */
static _Atomic int      s_err_fresh;
static _Atomic uint64_t s_engaged_at;     /* last hold */
static _Atomic int      s_mult;           /* refreshes per guest frame (0: not a multiple) */
/* stats */
static _Atomic uint64_t s_holds, s_held_ticks, s_late, s_miss, s_skipped_ref, s_corr_n;
static _Atomic int64_t  s_corr_sum, s_last_err, s_last_R;
static _Atomic int      s_inactive_why;   /* 0 active, 1 env, 2 no vblanks, 3 not a multiple, 4 no period */

static int s_env = -1, s_started = 0;
static int64_t s_gpu_allow;   /* ticks added to the readiness (the GPU's tail) */

static void start_once(void) {
    if (s_started) return;
    s_started = 1;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_freq = (double)f.QuadPart;
    const char *g = getenv("PSX_PRESENT_PHASE_GPU_MS");
    const double gms = g && *g ? atof(g) : 2.0;
    s_gpu_allow = (int64_t)(gms * s_freq / 1000.0);
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    if (gdi) {
        s_kopen = (KmtOpenFn)(void *)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
        s_kwait = (KmtWaitFn)(void *)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent");
        s_kclose = (KmtCloseFn)(void *)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    }
    if (!s_kopen || !s_kwait || !s_kclose) {
        fprintf(stdout, "psxrecomp: present phase: no D3DKMT vblank events; presents as before\n");
        return;
    }
    s_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!s_timer) s_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, vblank_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    fprintf(stdout, "psxrecomp: present phase: vblank thread on (PSX_PRESENT_PHASE=0 turns it off)\n");
}

static void wait_until(uint64_t t) {
    for (;;) {
        const uint64_t now = qpc();
        if (now >= t) return;
        const double us = (double)(t - now) * 1e6 / s_freq;
        if (us < 200.0) return;   /* close enough: the swap lands on the same refresh */
        if (s_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((us - 100.0) * 10.0);
            if (due.QuadPart >= 0) due.QuadPart = -1;
            if (SetWaitableTimer(s_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(s_timer, 100);
                continue;
            }
        }
        Sleep(1);
    }
}

int present_phase_hold(uint64_t ref, void *native_window) {
    if (s_env < 0) {
        const char *e = getenv("PSX_PRESENT_PHASE");
        s_env = !(e && *e == '0');
    }
    if (!s_env) { atomic_store(&s_inactive_why, 1); return 0; }
    if (!native_window) return 0;
    atomic_store(&s_hwnd, native_window);
    start_once();
    if (!s_kwait) { atomic_store(&s_inactive_why, 2); return 0; }
    const uint64_t now = qpc();
    Grid g;
    if (!grid_read(&g) || (double)(now - g.last) > s_freq * 0.25) {
        atomic_store(&s_inactive_why, 2);
        return 0;
    }
    const uint64_t gper = atomic_load(&s_guest_period);
    if (!gper) { atomic_store(&s_inactive_why, 4); return 0; }
    const double ratio = (double)gper / g.period;
    const int n = (int)floor(ratio + 0.5);
    if (n < 2 || fabs(ratio - n) > 0.01 * n) {
        atomic_store(&s_mult, 0);
        atomic_store(&s_inactive_why, 3);
        return 0;
    }
    atomic_store(&s_mult, n);
    atomic_store(&s_inactive_why, 0);
    atomic_store(&s_engaged_at, now);
    if (!ref || ref > now + gper || now - ref > 4 * gper) {
        atomic_fetch_add(&s_skipped_ref, 1);   /* a reanchored or unknown deadline */
        return 1;
    }
    s_rd[s_rdn++ % RD_RING] = (int64_t)(now - ref);
    const unsigned cnt = s_rdn < RD_RING ? s_rdn : RD_RING;
    int64_t tmp[RD_RING];
    memcpy(tmp, s_rd, cnt * sizeof tmp[0]);
    qsort(tmp, cnt, sizeof tmp[0], cmp_i64);
    const int64_t R = tmp[(cnt * 9) / 10] + s_gpu_allow;
    atomic_store(&s_last_R, R);
    const double P = g.period;
    const double x = (double)(int64_t)(ref - g.anchor) + (double)R;   /* from the anchor */
    const double mid0 = P * 0.5;
    const double k = ceil((x - mid0) / P);
    const double S = mid0 + k * P;
    const int64_t err = (int64_t)((S - x) - P * 0.5);
    atomic_store(&s_err, err);
    atomic_store(&s_last_err, err);
    atomic_store(&s_err_fresh, 1);
    const uint64_t st = (uint64_t)((int64_t)g.anchor + (int64_t)S);
    atomic_fetch_add(&s_holds, 1);
    if (now < st) {
        wait_until(st);
        atomic_fetch_add(&s_held_ticks, qpc() - now);
    } else {
        atomic_fetch_add(&s_late, 1);
        if ((double)(now - st) > P * 0.5) atomic_fetch_add(&s_miss, 1);   /* past the vblank */
    }
    return 1;
}

enum { GAP_MAX = 9 };
static _Atomic uint64_t s_gap[GAP_MAX + 1], s_gpu_late;
static int64_t s_last_slot = INT64_MIN;
void present_phase_note_done(uint64_t swap, uint64_t done, void *native_window) {
    if (native_window) atomic_store(&s_hwnd, native_window);
    start_once();
    Grid g;
    if (!s_kwait || !grid_read(&g)) return;
    const uint64_t t = done > swap ? done : swap;
    if (done > swap + (uint64_t)(g.period * 0.5)) atomic_fetch_add(&s_gpu_late, 1);
    /* the refresh it lands on, in ticks (the anchor moves every vblank) */
    const int64_t slot = (int64_t)g.anchor +
                         (int64_t)(ceil((double)(int64_t)(t - g.anchor) / g.period) * g.period);
    if (s_last_slot != INT64_MIN) {
        int64_t d = (int64_t)floor((double)(slot - s_last_slot) / g.period + 0.5);
        if (d < 0) d = 0;
        if (d > GAP_MAX) d = GAP_MAX;
        atomic_fetch_add(&s_gap[d], 1);
    }
    s_last_slot = slot;
}

int64_t present_phase_correction(uint64_t period_ticks) {
    atomic_store(&s_guest_period, period_ticks);
    if (!s_started || !s_kwait) return 0;
    const uint64_t at = atomic_load(&s_engaged_at);
    if (!at || (double)(qpc() - at) > s_freq * 0.1) return 0;
    if (!atomic_exchange(&s_err_fresh, 0)) return 0;
    const int64_t e = atomic_load(&s_err);
    int64_t adj = e / 10;
    const int64_t cap = (int64_t)period_ticks * 3 / 1000;
    if (adj > cap) adj = cap;
    if (adj < -cap) adj = -cap;
    atomic_fetch_add(&s_corr_sum, adj);
    atomic_fetch_add(&s_corr_n, 1);
    return adj;
}

int present_phase_active(void) {
    if (!s_started || !s_kwait) return 0;
    const uint64_t at = atomic_load(&s_engaged_at);
    return at && (double)(qpc() - at) < s_freq * 0.1;
}

int present_phase_json(char *out, size_t cap) {
    Grid g;
    const int have = s_started && grid_read(&g);
    const double ms = s_freq > 0.0 ? 1000.0 / s_freq : 0.0;
    const uint64_t holds = atomic_load(&s_holds);
    const uint64_t cn = atomic_load(&s_corr_n);
    int n = snprintf(out, cap,
        "\"active\":%d,\"inactive_why\":%d,\"refresh_hz\":%.4f,\"mult\":%d,\"vblanks\":%llu,"
        "\"vb_fail\":%llu,\"vb_reopen\":%llu,\"holds\":%llu,\"held_ms_avg\":%.3f,\"late\":%llu,"
        "\"miss\":%llu,\"skipped_ref\":%llu,\"ready_ms\":%.3f,\"err_ms\":%.3f,"
        "\"corr_avg_us\":%.3f,\"corr_n\":%llu",
        present_phase_active(), atomic_load(&s_inactive_why),
        have && g.period > 0.0 ? s_freq / g.period : 0.0, atomic_load(&s_mult),
        (unsigned long long)atomic_load(&s_vblanks), (unsigned long long)atomic_load(&s_vb_fail),
        (unsigned long long)atomic_load(&s_vb_reopen), (unsigned long long)holds,
        holds ? (double)atomic_load(&s_held_ticks) * ms / (double)holds : 0.0,
        (unsigned long long)atomic_load(&s_late), (unsigned long long)atomic_load(&s_miss),
        (unsigned long long)atomic_load(&s_skipped_ref),
        (double)atomic_load(&s_last_R) * ms, (double)atomic_load(&s_last_err) * ms,
        cn ? (double)atomic_load(&s_corr_sum) * ms * 1000.0 / (double)cn : 0.0,
        (unsigned long long)cn);
    if (n > 0 && (size_t)n < cap) {
        n += snprintf(out + n, cap - (size_t)n, ",\"gpu_late\":%llu,\"gaps\":[",
                      (unsigned long long)atomic_load(&s_gpu_late));
        for (int i = 0; i <= GAP_MAX && (size_t)n < cap; i++)
            n += snprintf(out + n, cap - (size_t)n, "%s%llu", i ? "," : "",
                          (unsigned long long)atomic_load(&s_gap[i]));
        if ((size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "]");
    }
    return n;
}

#else  /* !_WIN32: no vblank times; everything stays as before */

int present_phase_hold(uint64_t ref, void *native_window) {
    (void)ref; (void)native_window;
    return 0;
}
int64_t present_phase_correction(uint64_t period_ticks) {
    (void)period_ticks;
    return 0;
}
int present_phase_active(void) { return 0; }
void present_phase_note_done(uint64_t swap, uint64_t done, void *native_window) {
    (void)swap; (void)done; (void)native_window;
}
int present_phase_json(char *out, size_t cap) {
    return snprintf(out, cap, "\"active\":0,\"inactive_why\":2");
}

#endif
