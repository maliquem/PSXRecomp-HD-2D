/* psx_rewind.c — local snap ring + filmstrip overlay. */

#include "psx_rewind.h"

#include "boot_state.h"
#include "cdrom.h"
#include "gpu.h"
#include "host_keymap.h"
#include "host_osd.h"
#include "interrupts.h"
#include "mdec.h"
#include "psx_cycle_freeze.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "psx_netplay_rb.h"
#include "psx_scheduler.h"
#include "savestate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PSX_HAS_RBENGINE_SNAP)
#include "retcomm_rbengine/snap_ring.h"
#endif

#define RW_THUMB_W     128
#define RW_THUMB_H      96
#define RW_MAX_DEPTH   200
#define RW_DEF_DEPTH    50
#define RW_DEF_INTERVAL 15
/* Match netplay §96 FMV media snaps (default 4; MEDIA_KF uses 2). */
#define RW_DEF_FMV_INTERVAL 4
#define RW_FMV_MDEC_HYSTERESIS 8u
#define RW_PANEL_W     640
#define RW_PANEL_H     176
#define RW_SLIDE_MS    180u

#if defined(PSX_HAS_RBENGINE_SNAP)
#include "psx_font8x8.h"
#endif

/* Title block (psx_mod_set_rewind_blocked): set by the running title while
 * a mode it cannot rewind is live, cleared at every mod session start. */
static int s_title_blocked;

int psx_rewind_title_blocked(void)
{
    return s_title_blocked;
}

#if !defined(PSX_HAS_RBENGINE_SNAP)

void psx_rewind_set_title_blocked(int blocked) { s_title_blocked = blocked ? 1 : 0; }
uint32_t psx_rewind_snap_count(void) { return 0; }
void psx_rewind_set_depth(uint32_t depth) { (void)depth; }
void psx_rewind_set_interval(uint32_t interval) { (void)interval; }
void psx_rewind_set_enabled(int enabled) { (void)enabled; }
void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc)
{
    (void)bios_checksum;
    (void)entry_pc;
}
void psx_rewind_shutdown(void) {}
uint32_t psx_rewind_discard_history(void) { return 0; }
int  psx_rewind_enabled(void) { return 0; }
int  psx_rewind_is_open(void) { return 0; }
int  psx_rewind_needs_present(void) { return 0; }
void psx_rewind_note_frame(void) {}
void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc)
{
    (void)cpu;
    (void)resume_pc;
}
int  psx_rewind_toggle(void) { return 0; }
int  psx_rewind_cancel(void) { return 0; }
int  psx_rewind_accept(void) { return 0; }
void psx_rewind_move(int delta) { (void)delta; }
void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms)
{
    (void)left_down; (void)right_down; (void)accept_down;
    (void)cancel_down; (void)now_ms;
}
void psx_rewind_present_tick(uint32_t now_ms) { (void)now_ms; }
int  psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    return 0;
}
float psx_rewind_slide(void) { return 0.f; }

#else /* PSX_HAS_RBENGINE_SNAP */

typedef struct RwThumb {
    uint32_t tick;
    uint32_t px[RW_THUMB_W * RW_THUMB_H];
} RwThumb;

static int s_enabled = -1;
static uint32_t s_bios;
static uint32_t s_entry;
static uint32_t s_interval = RW_DEF_INTERVAL;
static uint32_t s_fmv_interval = RW_DEF_FMV_INTERVAL;
static uint32_t s_depth = RW_DEF_DEPTH;
static int s_depth_pref = -1; /* -1 = unset; else from settings.toml / launcher */
static int s_interval_pref = -1;
static int s_enabled_pref = -1; /* -1 = unset; else from settings.toml / launcher */
static uint32_t s_frame;
static uint32_t s_last_capture_frame = 0xffffffffu;
static int s_capture_due;
static int s_configured;

static RbeSnapRing *s_ring;
/* s_thumbs points into s_thumb_pool; the list reorders pointers, never the
 * 48 KB thumbnails (EST-O2). s_list_cap is what configure allocated. */
static RwThumb *s_thumb_pool;
static RwThumb **s_thumbs;
static uint32_t s_list_cap;
static uint32_t *s_ticks;
static uint32_t s_count;
static int s_sel;

static int s_open;
static float s_slide;
static uint32_t s_anim_t0;
static int s_anim_dir; /* +1 opening, -1 closing, 0 idle */
static uint32_t s_panel[RW_PANEL_W * RW_PANEL_H];
static int s_panel_dirty = 1;

static int s_load_pending;
static uint32_t s_load_tick;

static int s_left_was, s_right_was, s_acc_was, s_can_was;
static uint32_t s_rep_next;
static int s_rep_dir;

static uint32_t env_u32(const char *name, uint32_t def, uint32_t lo, uint32_t hi)
{
    const char *e = getenv(name);
    unsigned v;
    if (!e || !e[0])
        return def;
    v = (unsigned)atoi(e);
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}

/* UI offers 50/100/150/200; nearest match keeps hand-edited toml values sane. */
static uint32_t normalize_rewind_depth(uint32_t v)
{
    static const uint32_t opts[4] = {50u, 100u, 150u, 200u};
    uint32_t best = opts[0];
    uint32_t best_d = v > best ? v - best : best - v;
    unsigned i;
    for (i = 1; i < 4; i++) {
        uint32_t d = v > opts[i] ? v - opts[i] : opts[i] - v;
        if (d < best_d) {
            best_d = d;
            best = opts[i];
        }
    }
    return best;
}

/* UI offers 1/4/8/12/15. */
static uint32_t normalize_rewind_interval(uint32_t v)
{
    static const uint32_t opts[5] = {1u, 4u, 8u, 12u, 15u};
    uint32_t best = opts[0];
    uint32_t best_d = v > best ? v - best : best - v;
    unsigned i;
    for (i = 1; i < 5; i++) {
        uint32_t d = v > opts[i] ? v - opts[i] : opts[i] - v;
        if (d < best_d) {
            best_d = d;
            best = opts[i];
        }
    }
    return best;
}

void psx_rewind_set_depth(uint32_t depth)
{
    s_depth_pref = (int)normalize_rewind_depth(depth);
    /* Apply immediately if prefs are already resolved (reconfigure path). */
    if (s_enabled >= 0)
        s_depth = (uint32_t)s_depth_pref;
}

void psx_rewind_set_interval(uint32_t interval)
{
    s_interval_pref = (int)normalize_rewind_interval(interval);
    if (s_enabled >= 0)
        s_interval = (uint32_t)s_interval_pref;
}

void psx_rewind_set_enabled(int enabled)
{
    const char *e = getenv("PSX_REWIND");
    s_enabled_pref = enabled ? 1 : 0;
    /* PSX_REWIND is the operator's override and outranks the UI, so once it
     * has spoken the preference only records intent. Otherwise apply now; the
     * caller re-arms (configure) or tears down (shutdown) to match. */
    if (e && e[0])
        return;
    if (s_enabled >= 0)
        s_enabled = s_enabled_pref;
}

static int rewind_wanted(void)
{
    const char *e;
    if (s_enabled < 0) {
        /* Off unless asked for. The ring holds up to RW_MAX_DEPTH machine
         * snapshots (2 MB RAM + 1 MB VRAM + 512 KB SPU RAM each) and captures
         * one every s_interval frames, which is real cost on a low-end host
         * for a feature most sessions never open. Order: env wins, then the
         * settings.toml / launcher preference, then off. */
        e = getenv("PSX_REWIND");
        if (e && e[0])
            s_enabled = (e[0] == '0') ? 0 : 1;
        else if (s_enabled_pref >= 0)
            s_enabled = s_enabled_pref;
        else
            s_enabled = 0;
        {
            uint32_t iv_def = s_interval_pref > 0 ? (uint32_t)s_interval_pref
                                                  : RW_DEF_INTERVAL;
            s_interval = normalize_rewind_interval(
                env_u32("PSX_REWIND_INTERVAL", iv_def, 1u, 60u));
        }
        s_fmv_interval =
            env_u32("PSX_REWIND_FMV_INTERVAL", RW_DEF_FMV_INTERVAL, 1u, 16u);
        {
            uint32_t depth_def = s_depth_pref > 0 ? (uint32_t)s_depth_pref
                                                  : RW_DEF_DEPTH;
            s_depth = normalize_rewind_depth(
                env_u32("PSX_REWIND_DEPTH", depth_def, 4u, RW_MAX_DEPTH));
        }
    }
    /* Multi-disc titles swap discs under the player (save loads, states);
     * rewind across a swap is not proven yet, so it stays off there. */
    if (cdrom_disc_roster_count() > 1)
        return 0;
    return s_enabled;
}

/* Same heuristic as netplay rb_fmv_media_active — denser snaps while media runs. */
static int rewind_fmv_media_active(void)
{
    if (gpu_display_is_depth24())
        return 1;
    if (mdec_recently_active(RW_FMV_MDEC_HYSTERESIS))
        return 1;
    if (cdrom_xa_stream_active())
        return 1;
    return 0;
}

static uint32_t rewind_capture_interval(void)
{
    if (!rewind_fmv_media_active())
        return s_interval;
    /* FMV densifies toward s_fmv_interval but never sparser than the user pick. */
    return s_interval < s_fmv_interval ? s_interval : s_fmv_interval;
}

static int resume_pc_ok(uint32_t pc)
{
    return pc != 0u && (pc & 3u) == 0u && psx_is_dispatchable(pc) &&
           pc != 0x80000080u && pc != 0xbfc00180u && pc != 0x80000000u;
}

/* Mid-FMV / present-edge IRQ paths often pass hint=0 — same latch walk as
 * savestate_resolve_resume_pc / selfcheck sc_pick_snap_pc. */
static uint32_t rewind_resolve_resume_pc(const CPUState *cpu, uint32_t hint)
{
    uint32_t cands[7];
    int n = 0;
    int i;

    if (psx_irq_resume_context_snapshot_site() != 0)
        cands[n++] = psx_irq_resume_context_snapshot_pc();
    cands[n++] = hint;
    cands[n++] = cpu ? cpu->pc : 0u;
    cands[n++] = psx_compiled_irq_resume_pc();
    cands[n++] = psx_last_irq_check_pc();
    cands[n++] = psx_netplay_rb_sticky_bb_pc();
    cands[n++] = cpu ? cpu->gpr[31] : 0u;

    for (i = 0; i < n; ++i) {
        if (resume_pc_ok(cands[i]))
            return cands[i];
    }
    return 0u;
}

static void capture_thumb(uint32_t *dst)
{
    GpuDisplayInfo di;
    uint32_t dw, dh, x, y;
    gpu_get_display_info(&di);
    dw = di.width ? di.width : 320u;
    dh = di.height ? di.height : 240u;
    if (di.disabled) {
        memset(dst, 0, (size_t)RW_THUMB_W * RW_THUMB_H * sizeof(uint32_t));
        return;
    }
    for (y = 0; y < RW_THUMB_H; y++) {
        uint32_t sy = y * dh / RW_THUMB_H;
        for (x = 0; x < RW_THUMB_W; x++) {
            uint32_t sx = x * dw / RW_THUMB_W;
            dst[y * RW_THUMB_W + x] =
                gpu_display_pixel_argb(&di, sx, sy) | 0xFF000000u;
        }
    }
}

static void list_drop_after(uint32_t tick)
{
    uint32_t i, n = 0;
    for (i = 0; i < s_count; i++) {
        if (s_ticks[i] <= tick) {
            if (n != i) {
                RwThumb *t = s_thumbs[n];   /* swap: no pool slot is lost */
                s_ticks[n] = s_ticks[i];
                s_thumbs[n] = s_thumbs[i];
                s_thumbs[i] = t;
            }
            n++;
        }
    }
    s_count = n;
    if (s_sel >= (int)s_count)
        s_sel = s_count ? (int)s_count - 1 : 0;
    s_panel_dirty = 1;
}

/* Claims the list slot for tick and returns it for the caller to fill, so the
 * thumbnail is rendered in place instead of through a 48 KB stack copy. */
static RwThumb *list_push_slot(uint32_t tick)
{
    uint32_t i;
    for (i = 0; i < s_count; i++) {
        if (s_ticks[i] == tick) {
            s_thumbs[i]->tick = tick;
            s_panel_dirty = 1;
            return s_thumbs[i];
        }
    }
    /* s_depth can change live (settings) without a reconfigure; never index
     * past what configure allocated. */
    if (s_count >= s_depth || s_count >= s_list_cap) {
        RwThumb *oldest = s_thumbs[0];
        memmove(s_ticks, s_ticks + 1, (s_count - 1u) * sizeof(s_ticks[0]));
        memmove(s_thumbs, s_thumbs + 1, (s_count - 1u) * sizeof(s_thumbs[0]));
        s_thumbs[s_count - 1u] = oldest;
        s_count--;
        if (s_sel > 0)
            s_sel--;
    }
    s_ticks[s_count] = tick;
    s_thumbs[s_count]->tick = tick;
    s_count++;
    s_sel = (int)s_count - 1;
    s_panel_dirty = 1;
    return s_thumbs[s_count - 1u];
}

void psx_rewind_configure(uint32_t bios_checksum, uint32_t entry_pc)
{
    if (!rewind_wanted())
        return;
    psx_rewind_shutdown();
    s_bios = bios_checksum;
    s_entry = entry_pc;
    s_ring = rbe_snap_ring_create(s_depth);
    s_thumb_pool = (RwThumb *)calloc(s_depth, sizeof(RwThumb));
    s_thumbs = (RwThumb **)calloc(s_depth, sizeof(RwThumb *));
    s_ticks = (uint32_t *)calloc(s_depth, sizeof(uint32_t));
    if (!s_ring || !s_thumb_pool || !s_thumbs || !s_ticks) {
        psx_rewind_shutdown();
        fprintf(stderr, "psxrecomp: rewind alloc failed — disabled\n");
        s_enabled = 0;
        return;
    }
    for (uint32_t i = 0; i < s_depth; i++)
        s_thumbs[i] = &s_thumb_pool[i];
    s_list_cap = s_depth;
    s_configured = 1;
    s_frame = 0;
    s_last_capture_frame = 0xffffffffu;
    s_count = 0;
    s_sel = 0;
    fprintf(stderr,
            "psxrecomp: rewind on (interval=%u fmv=%u depth=%u ~%.1fs) — "
            "F8 / View / L3\n",
            (unsigned)s_interval, (unsigned)s_fmv_interval, (unsigned)s_depth,
            (double)s_interval * (double)s_depth / 60.0);
}

void psx_rewind_shutdown(void)
{
    if (s_ring)
        rbe_snap_ring_destroy(s_ring);
    free(s_thumb_pool);
    free(s_thumbs);
    free(s_ticks);
    s_ring = NULL;
    s_thumb_pool = NULL;
    s_thumbs = NULL;
    s_list_cap = 0;
    s_ticks = NULL;
    s_count = 0;
    s_configured = 0;
    s_open = 0;
    s_slide = 0.f;
    s_anim_dir = 0;
    s_load_pending = 0;
    s_capture_due = 0;
}

uint32_t psx_rewind_discard_history(void)
{
    uint32_t dropped = s_count;
    if (!s_configured || !s_ring) return 0;
    rbe_snap_ring_clear(s_ring);
    s_count = 0;
    s_sel = 0;
    s_load_pending = 0;
    s_last_capture_frame = 0xffffffffu;
    s_panel_dirty = 1;
    return dropped;
}

int psx_rewind_enabled(void)
{
    return rewind_wanted() && s_configured && s_ring != NULL;
}

int psx_rewind_is_open(void)
{
    return s_open;
}

uint32_t psx_rewind_snap_count(void)
{
    return s_count;
}

int psx_rewind_needs_present(void)
{
    return s_open || s_slide > 0.01f || s_anim_dir != 0;
}

void psx_rewind_note_frame(void)
{
    uint32_t iv;
    if (!psx_rewind_enabled() || s_open || psx_netplay_active() ||
        s_title_blocked)
        return;
    s_frame++;
    iv = rewind_capture_interval();
    if (s_last_capture_frame == 0xffffffffu ||
        s_frame - s_last_capture_frame >= iv)
        s_capture_due = 1;
}

static int do_capture(CPUState *cpu, uint32_t resume_pc)
{
    uint8_t *blob = NULL;
    size_t len = 0;
    CPUState snap;
    uint32_t tick = s_frame;
    uint32_t pc;

    if (!cpu || !s_ring)
        return 0;
    pc = rewind_resolve_resume_pc(cpu, resume_pc);
    if (!psx_irq_resume_context_snapshot_safe_at(pc) || !resume_pc_ok(pc))
        return 0;
    snap = *cpu;
    snap.pc = pc;
    if (!boot_state_save_buffer_raw(&snap, s_bios, s_entry, &blob, &len) ||
        !blob || !len)
        return 0;
    if (!rbe_snap_ring_store(s_ring, tick, blob, len)) {
        free(blob);
        return 0;
    }
    capture_thumb(list_push_slot(tick)->px);
    s_panel_dirty = 1; /* the pixels land after the slot was listed */
    s_last_capture_frame = s_frame;
    s_capture_due = 0;
    return 1;
}

/* Same continuation gates as savestate_poll: a load longjmps to the
 * scheduler, so it must not unwind a mod callback (psx_mod_call_guest
 * restores registers after it) or a host call that restores registers and
 * clocks (frozen guest time would leak). */
static int rewind_load_blocked(void)
{
    extern int psx_mod_function_entry_active(void);
    return psx_mod_function_entry_active() || psx_snapshot_host_call_depth() ||
           g_psx_guest_time_frozen;
}

static int do_load(CPUState *cpu, uint32_t tick)
{
    size_t size = 0;
    const uint8_t *data;
    uint8_t *rollback = NULL;
    size_t rollback_len = 0;

    if (!cpu || !s_ring)
        return 0;
    if (!psx_hle_scheduler_enabled()) {
        fprintf(stderr, "psxrecomp: rewind load needs HLE scheduler\n");
        return 0;
    }
    data = rbe_snap_ring_peek(s_ring, tick, &size);
    if (!data || !size)
        return 0;
    /* The resume-PC gate can only judge a snap AFTER it is applied
     * (dispatchability depends on the restored RAM), so hold the pre-load
     * machine and put it back on refusal, as savestate_poll does: a refused
     * rewind never leaves a foreign machine running on this host stack. */
    if (!boot_state_save_buffer_raw(cpu, s_bios, s_entry, &rollback,
                                    &rollback_len))
        return 0;
    if (!boot_state_load_buffer(data, size, s_bios, s_entry, cpu)) {
        free(rollback); /* refuses before mutating anything */
        return 0;
    }
    if (!resume_pc_ok(cpu->pc)) {
        /* Our own just-written state; failing to restore it is fatal. */
        if (!boot_state_load_buffer(rollback, rollback_len, s_bios, s_entry,
                                    cpu))
            abort();
        free(rollback);
        /* Host clock/IRQ anchors were moved by the refused apply. */
        psx_cycles_resync_after_restore(cpu);
        interrupts_resync_after_restore();
        return 0;
    }
    free(rollback);
    rbe_snap_ring_drop_after(s_ring, tick);
    list_drop_after(tick);
    s_frame = tick;
    s_last_capture_frame = tick;
    psx_cycles_resync_after_restore(cpu);
    interrupts_resync_after_restore();
    cdrom_accelerate_after_savestate();
    psx_frontend_on_savestate_loaded();
    host_osd_push("Rewind loaded", 1200);
    psx_scheduler_resume_at(cpu->pc);
    return 1; /* not reached on success */
}

void psx_rewind_poll(CPUState *cpu, uint32_t resume_pc)
{
    if (!psx_rewind_enabled())
        return;
    if (s_load_pending) {
        uint32_t tick = s_load_tick;
        if (rewind_load_blocked())
            return; /* stays staged; a later poll outside the frame loads it */
        s_load_pending = 0;
        s_open = 0;
        s_anim_dir = -1;
        (void)do_load(cpu, tick);
        return;
    }
    if (s_capture_due && !s_open && !psx_netplay_active() && !s_title_blocked)
        (void)do_capture(cpu, resume_pc);
}

int psx_rewind_toggle(void)
{
    if (!psx_rewind_enabled()) {
        /* Rewind is off by default, so this is now the common case: say why
         * rather than leaving F8 / Select+R3 looking broken. Silent only when
         * the feature was compiled out, where there is nothing to turn on. */
        host_osd_push("Rewind is off — enable it in Settings", 1800);
        return 0;
    }
    if (psx_netplay_active()) {
        host_osd_push("Rewind off during netplay", 1500);
        return 0;
    }
    if (s_title_blocked && !s_open) {
        host_osd_push("Rewind is off in this mode", 1500);
        return 0;
    }
    if (s_open) {
        s_open = 0;
        s_anim_dir = -1;
        s_anim_t0 = 0u;
        return 1;
    }
    s_open = 1;
    s_anim_dir = 1;
    s_anim_t0 = 0u;
    /* Swallow currently-held Back/L3/A so open doesn't instantly cancel. */
    s_left_was = s_right_was = s_acc_was = s_can_was = 1;
    s_rep_dir = 0;
    if (s_count)
        s_sel = (int)s_count - 1;
    s_panel_dirty = 1;
    host_osd_push(s_count ? "Rewind" : "Rewind (no snaps yet)", 1200);
    return 1;
}

void psx_rewind_set_title_blocked(int blocked)
{
    s_title_blocked = blocked ? 1 : 0;
    if (!s_title_blocked)
        return;
    /* Captures stop here; one already scheduled is dropped so nothing from
     * the blocked mode lands in history. An open panel closes unloaded. */
    s_capture_due = 0;
    (void)psx_rewind_cancel();
}

int psx_rewind_cancel(void)
{
    if (!s_open)
        return 0;
    s_open = 0;
    s_anim_dir = -1;
    s_anim_t0 = 0u;
    return 1;
}

int psx_rewind_accept(void)
{
    if (!s_open || !s_count)
        return 0;
    s_load_tick = s_ticks[s_sel];
    s_load_pending = 1;
    s_open = 0;
    s_anim_dir = -1;
    s_anim_t0 = 0u;
    return 1;
}

void psx_rewind_move(int delta)
{
    int n;
    if (!s_open || !s_count || delta == 0)
        return;
    n = (int)s_count;
    s_sel += delta;
    if (s_sel < 0)
        s_sel = 0;
    if (s_sel >= n)
        s_sel = n - 1;
    s_panel_dirty = 1;
}

void psx_rewind_nav_held(int left_down, int right_down, int accept_down,
                         int cancel_down, uint32_t now_ms)
{
    int left_edge, right_edge, acc_edge, can_edge;
    int dir;

    if (!s_open)
        return;

    left_edge = left_down && !s_left_was;
    right_edge = right_down && !s_right_was;
    acc_edge = accept_down && !s_acc_was;
    can_edge = cancel_down && !s_can_was;
    s_left_was = left_down;
    s_right_was = right_down;
    s_acc_was = accept_down;
    s_can_was = cancel_down;

    if (can_edge) {
        psx_rewind_cancel();
        return;
    }
    if (acc_edge) {
        psx_rewind_accept();
        return;
    }
    if (left_edge) {
        psx_rewind_move(-1);
        s_rep_dir = -1;
        s_rep_next = now_ms + 280u;
    } else if (right_edge) {
        psx_rewind_move(+1);
        s_rep_dir = +1;
        s_rep_next = now_ms + 280u;
    } else {
        dir = left_down ? -1 : (right_down ? +1 : 0);
        if (dir && dir == s_rep_dir && now_ms >= s_rep_next) {
            psx_rewind_move(dir);
            s_rep_next = now_ms + 70u;
        } else if (!dir) {
            s_rep_dir = 0;
        }
    }
}

void psx_rewind_present_tick(uint32_t now_ms)
{
    float t;
    if (s_anim_dir == 0)
        return;
    if (s_anim_t0 == 0u)
        s_anim_t0 = now_ms ? now_ms : 1u;
    t = (float)(now_ms - s_anim_t0) / (float)RW_SLIDE_MS;
    if (t < 0.f)
        t = 0.f;
    if (t > 1.f)
        t = 1.f;
    /* ease-out */
    t = 1.f - (1.f - t) * (1.f - t);
    if (s_anim_dir > 0)
        s_slide = t;
    else
        s_slide = 1.f - t;
    if ((s_anim_dir > 0 && s_slide >= 0.999f) ||
        (s_anim_dir < 0 && s_slide <= 0.001f)) {
        s_slide = s_anim_dir > 0 ? 1.f : 0.f;
        s_anim_dir = 0;
        s_anim_t0 = 0u;
    }
}

float psx_rewind_slide(void)
{
    return s_slide;
}

static void blit_thumb(uint32_t *dst, int dw, int x0, int y0, int tw, int th,
                       const uint32_t *src, int sel)
{
    int x, y, dx, dy;
    uint32_t border = sel ? 0xFFFFD24Du : 0xFF3A3A3Au;
    int bw = sel ? 3 : 1;
    for (y = -bw; y < th + bw; y++) {
        dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = -bw; x < tw + bw; x++) {
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            if (x < 0 || y < 0 || x >= tw || y >= th)
                dst[dy * dw + dx] = border;
        }
    }
    for (y = 0; y < th; y++) {
        dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = 0; x < tw; x++) {
            int sx = x * RW_THUMB_W / tw;
            int sy = y * RW_THUMB_H / th;
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            dst[dy * dw + dx] = src[sy * RW_THUMB_W + sx];
        }
    }
}

static void glyph_fill_rect(uint32_t *dst, int dw, int x0, int y0,
                            int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > dw) w = dw - x0;
    if (y0 + h > RW_PANEL_H) h = RW_PANEL_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * dw + x] = col;
}

static void glyph_fill_disc(uint32_t *dst, int dw, int cx, int cy,
                            int r, uint32_t col)
{
    int x, y;
    const int rr = r * r;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y > rr) continue;
            glyph_fill_rect(dst, dw, cx + x, cy + y, 1, 1, col);
        }
    }
}

static void glyph_line(uint32_t *dst, int dw, int x0, int y0, int x1, int y1,
                       int thickness, uint32_t col)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int inset = thickness / 2;

    for (;;) {
        glyph_fill_rect(dst, dw, x0 - inset, y0 - inset, thickness, thickness,
                        col);
        if (x0 == x1 && y0 == y1)
            break;
        {
            int e2 = err * 2;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void draw_psx_button(uint32_t *dst, int dw, int x, int y, char kind)
{
    switch (kind) {
    case 'x':
        glyph_line(dst, dw, x + 4, y + 4, x + 14, y + 14, 2, 0xFF5FA8FFu);
        glyph_line(dst, dw, x + 14, y + 4, x + 4, y + 14, 2, 0xFF5FA8FFu);
        break;
    case 'o':
        glyph_fill_disc(dst, dw, x + 9, y + 9, 8, 0xFFFF6B6Bu);
        glyph_fill_disc(dst, dw, x + 9, y + 9, 5, 0xE0101218u);
        break;
    default:
        glyph_fill_rect(dst, dw, x + 7, y + 2, 5, 15, 0xFFD7DCE6u);
        glyph_fill_rect(dst, dw, x + 2, y + 7, 15, 5, 0xFFD7DCE6u);
        break;
    }
}

static void draw_char(uint32_t *dst, int dw, int x0, int y0, char c, uint32_t col)
{
    const uint8_t *g;
    int x, y;
    if (c < 32 || c > 90)
        c = '?';
    g = FONT8[(int)c - 32];
    for (y = 0; y < 8; y++) {
        uint8_t row = g[y];
        int dy = y0 + y;
        if (dy < 0 || dy >= RW_PANEL_H)
            continue;
        for (x = 0; x < 8; x++) {
            int dx;
            if ((row & (1u << x)) == 0)
                continue;
            dx = x0 + x;
            if (dx < 0 || dx >= dw)
                continue;
            dst[dy * dw + dx] = col;
        }
    }
}

static void draw_text(uint32_t *dst, int dw, int x0, int y0, const char *s,
                      uint32_t col)
{
    int x = x0;
    if (!s)
        return;
    while (*s) {
        char c = *s++;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        draw_char(dst, dw, x, y0, c, col);
        x += 8;
    }
}

static void rasterize_panel(void)
{
    uint32_t *d = s_panel;
    int i, n, card_w, card_h, gap, x, y, sel_w, sel_h;
    int fy;
    int center_x, origin;
    char buf[96];
    char hk[32];

    for (i = 0; i < RW_PANEL_W * RW_PANEL_H; i++)
        d[i] = 0xE0101218u;

    draw_text(d, RW_PANEL_W, 12, 8, "REWIND", 0xFFFFD24Du);
    if (s_count) {
        snprintf(buf, sizeof(buf), "%d / %u   F%u",
                 s_sel + 1, (unsigned)s_count,
                 (unsigned)s_ticks[s_sel]);
        draw_text(d, RW_PANEL_W, 120, 8, buf, 0xFFE8E8E8u);
    } else {
        draw_text(d, RW_PANEL_W, 120, 8, "NO SNAPSHOTS YET", 0xFF888888u);
    }
    host_keymap_label(HOST_KEYMAP_REWIND, hk, sizeof(hk));
    fy = RW_PANEL_H - 22;
    draw_psx_button(d, RW_PANEL_W, 12, fy, 'd');
    draw_text(d, RW_PANEL_W, 36, fy + 5, "SEEK", 0xFFE2E5EBu);
    draw_psx_button(d, RW_PANEL_W, 116, fy, 'x');
    draw_text(d, RW_PANEL_W, 140, fy + 5, "LOAD", 0xFFE2E5EBu);
    draw_psx_button(d, RW_PANEL_W, 216, fy, 'o');
    draw_text(d, RW_PANEL_W, 240, fy + 5, "CLOSE", 0xFFE2E5EBu);
    snprintf(buf, sizeof(buf), "%s MENU", hk[0] ? hk : "F8");
    draw_text(d, RW_PANEL_W, 512, fy + 5, buf, 0xFFAAAAAAu);

    n = (int)s_count;
    if (n <= 0) {
        s_panel_dirty = 0;
        return;
    }

    card_w = 96;
    card_h = 72;
    sel_w = 120;
    sel_h = 90;
    gap = 10;
    center_x = RW_PANEL_W / 2;
    origin = center_x - (s_sel * (card_w + gap)) - sel_w / 2;

    y = 28;
    for (i = 0; i < n; i++) {
        int w = (i == s_sel) ? sel_w : card_w;
        int h = (i == s_sel) ? sel_h : card_h;
        int yy = (i == s_sel) ? (y - 8) : y;
        x = origin + i * (card_w + gap);
        if (i > s_sel)
            x += (sel_w - card_w);
        if (x + w < 0 || x >= RW_PANEL_W)
            continue;
        blit_thumb(d, RW_PANEL_W, x, yy, w, h, s_thumbs[i]->px, i == s_sel);
    }
    s_panel_dirty = 0;
}

int psx_rewind_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!psx_rewind_needs_present() || s_slide <= 0.01f) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    if (s_panel_dirty)
        rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = RW_PANEL_W;
    if (h) *h = RW_PANEL_H;
    return 1;
}

#endif /* PSX_HAS_RBENGINE_SNAP */
