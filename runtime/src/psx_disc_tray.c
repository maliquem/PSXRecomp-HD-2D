/* psx_disc_tray.c - the one host entry point for a CD lid cycle.
 *
 * See psx_disc_tray.h for the contract. The guest-visible part of the cycle
 * (stop transfers, ShellOpen, hide media, swap image, timed close) is owned by
 * cdrom.c; this file owns the host policy around it, so the in-game disc menu,
 * the reinsert hotkey and the debug commands cannot drift apart. */

#include "psx_disc_tray.h"

#include "cdrom.h"
#include "psx_netplay.h"
#include "psx_savestate_menu.h"

#include <stddef.h>
#include <stdint.h>

/* psx_cycles.h and psx_rewind.h pull cpu_state.h and the cycle scheduler,
 * which would drag the whole core into the unit test; these two are all this
 * file uses of them. */
extern uint64_t psx_cycle_count;
uint32_t psx_rewind_discard_history(void);

static PsxDiscTrayHostHook s_hook;
static void *s_hook_ctx;
static int s_episode_disc, s_attempted;

void psx_disc_tray_reset_session(void)
{
    s_episode_disc = s_attempted = 0;
}

static PsxDiscTrayEvent s_log[PSX_DISC_TRAY_LOG_CAP];
static uint64_t s_log_total;

void psx_disc_tray_set_host_hook(PsxDiscTrayHostHook hook, void *ctx)
{
    s_hook = hook;
    s_hook_ctx = hook ? ctx : NULL;
}

int psx_disc_tray_disc_count(void)
{
    const int n = cdrom_disc_roster_count();
    return n > 1 ? n : 0;
}

int psx_disc_tray_mounted_disc(void)
{
    return psx_disc_tray_disc_count() ? cdrom_disc_selected() : 0;
}

const char *psx_disc_tray_disc_path(int disc_1based)
{
    if (!psx_disc_tray_disc_count())
        return NULL;
    return cdrom_disc_roster_path(disc_1based);
}

static void log_event(int origin, int requested, int from_disc, int to_disc,
                      int result, int rewind_dropped)
{
    PsxDiscTrayEvent *e = &s_log[s_log_total % PSX_DISC_TRAY_LOG_CAP];
    e->seq = s_log_total;
    e->cycle = psx_cycle_count;
    e->origin = origin;
    e->requested = requested;
    e->from_disc = from_disc;
    e->to_disc = to_disc;
    e->result = result;
    e->rewind_dropped = rewind_dropped;
    s_log_total++;
}

int psx_disc_tray_cycle(int disc_1based, int origin)
{
    const int count = psx_disc_tray_disc_count();
    const int from = count ? cdrom_disc_selected() : 0;
    int result = PSX_DISC_TRAY_OK;
    int to = from;
    int rewind_dropped = 0;

    if (disc_1based != 0 && count == 0) {
        result = PSX_DISC_TRAY_ERR_NO_ROSTER;
    } else if (disc_1based < 0 || disc_1based > count) {
        result = PSX_DISC_TRAY_ERR_RANGE;
    } else if (psx_netplay_active()) {
        /* A lid cycle is local input the peers never see: the opening peer's
         * guest takes a ShellOpen error the others do not. */
        result = PSX_DISC_TRAY_ERR_NETPLAY;
    } else if (disc_1based == 0) {
        /* Same media back in: nothing the rewind ring or the host tracks
         * changes, so this is the plain lid cycle. */
        debug_force_cd_reinsert();
    } else {
        const int changed = disc_1based != from;
        const int ok = cdrom_disc_select(disc_1based);
        to = cdrom_disc_selected();
        if (!ok)
            result = PSX_DISC_TRAY_ERR_OPEN_FAILED;
        /* A changed disc makes rewind snaps describe media no longer mounted.
         * A failed open changes nothing (the current disc stays, lid closed),
         * and re-selecting the mounted disc reopens the same file and behaves
         * as a reinsert: both keep the history. */
        if (ok && changed)
            rewind_dropped = (int)psx_rewind_discard_history();
        if (ok) {
            /* cdrom_disc_select re-scoped savestates to the new disc; the slot
             * browser caches thumbnails from the old scope. */
            psx_savestate_menu_note_slots_changed();
            if (changed && s_hook)
                s_hook(s_hook_ctx, disc_1based,
                       cdrom_disc_roster_path(disc_1based));
        }
    }

    log_event(origin, disc_1based, from, to, result, rewind_dropped);
    return result;
}

int psx_disc_tray_next_disc(void)
{
    const int count = psx_disc_tray_disc_count();
    if (!count) return 0;
    const int cur = cdrom_disc_selected();
    /* Nothing (or an unreadable image) mounted: start the set over at 1. */
    return (cur >= 1 && cur < count) ? cur + 1 : 1;
}

static PsxDiscRequestRule s_rules[PSX_DISC_TRAY_MAX_REQUEST_RULES];
static int s_rule_count;

void psx_disc_tray_set_request_rules(const PsxDiscRequestRule *rules, int count)
{
    s_rule_count = 0;
    if (!rules || count <= 0) return;
    if (count > PSX_DISC_TRAY_MAX_REQUEST_RULES)
        count = PSX_DISC_TRAY_MAX_REQUEST_RULES;
    for (int i = 0; i < count; i++)
        s_rules[s_rule_count++] = rules[i];
}

int psx_disc_tray_request_rule_count(void) { return s_rule_count; }

int psx_disc_tray_requested_disc(const uint8_t *ram, uint32_t ram_mask)
{
    if (!ram) return 0;
    for (int i = 0; i < s_rule_count; i++) {
        const PsxDiscRequestRule *r = &s_rules[i];
        uint32_t v = 0;
        for (uint32_t b = 0; b < r->size && b < 4; b++)
            v |= (uint32_t)ram[(r->addr + b) & ram_mask] << (8 * b);
        if ((v & r->mask) == r->value) return r->disc;
    }
    return 0;
}

int psx_disc_tray_auto_poll(int requested, int ready)
{
    if (requested != s_episode_disc) {
        s_episode_disc = requested;
        s_attempted = 0;
    }
    if (requested <= 0 || !ready || s_attempted) return -1;
    if (requested == psx_disc_tray_mounted_disc()) return -1;
    s_attempted = 1;
    return psx_disc_tray_cycle(requested, PSX_DISC_TRAY_ORIGIN_AUTO);
}

const char *psx_disc_tray_result_name(int result)
{
    switch (result) {
    case PSX_DISC_TRAY_OK:              return "ok";
    case PSX_DISC_TRAY_ERR_NO_ROSTER:   return "no_roster";
    case PSX_DISC_TRAY_ERR_RANGE:       return "out_of_range";
    case PSX_DISC_TRAY_ERR_NETPLAY:     return "netplay";
    case PSX_DISC_TRAY_ERR_OPEN_FAILED: return "open_failed";
    default:                            return "unknown";
    }
}

const char *psx_disc_tray_origin_name(int origin)
{
    switch (origin) {
    case PSX_DISC_TRAY_ORIGIN_MENU:            return "menu";
    case PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT: return "hotkey_reinsert";
    case PSX_DISC_TRAY_ORIGIN_DEBUG_SELECT:    return "debug_select";
    case PSX_DISC_TRAY_ORIGIN_DEBUG_REINSERT:  return "debug_reinsert";
    case PSX_DISC_TRAY_ORIGIN_DEBUG_MENU:      return "debug_menu";
    case PSX_DISC_TRAY_ORIGIN_AUTO:            return "auto";
    default:                                   return "unknown";
    }
}

uint64_t psx_disc_tray_log(const PsxDiscTrayEvent **entries)
{
    if (entries)
        *entries = s_log;
    return s_log_total;
}
