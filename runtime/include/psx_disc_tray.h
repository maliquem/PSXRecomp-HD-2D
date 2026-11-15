#ifndef PSX_DISC_TRAY_H
#define PSX_DISC_TRAY_H

/* psx_disc_tray - the ONE host entry point for opening the CD lid.
 *
 * Every way a player (or a script) can open the lid goes through
 * psx_disc_tray_cycle(): the in-game disc menu, the CD-reinsert hotkey, and
 * the debug-server commands disc_select / cd_reinsert. It exists so that the
 * rules a lid cycle has to obey live in one place instead of being re-derived
 * at each caller:
 *
 *   - the guest sees the faithful sequence: lid opens (ShellOpen, media gone),
 *     the image is replaced while the lid is open, the lid closes on a timer
 *     (cdrom_disc_select / debug_force_cd_reinsert in cdrom.c);
 *   - netplay refuses it, because a lid cycle on one peer is a desync;
 *   - a change of image drops the rewind history, whose snapshots describe a
 *     guest reading the old disc (savestates are already disc-scoped by
 *     cdrom_disc_select);
 *   - the host is told, so the live disc is persisted as the next launch's
 *     disc and the launcher / rematch path mount what is really in the drive;
 *   - every attempt lands in an always-on ring, queryable over the debug
 *     server, including refusals.
 *
 * Ported from upstream #652 without its mod-plugin request provider: here a
 * title's disc requests come from game.toml [[game.disc_requests]] RAM rules
 * (psx_disc_tray_set_request_rules), so no plugin or codegen change is needed.
 *
 * The file has no SDL, debug-server or C++ dependency, so it is part of every
 * build (Release included) and is unit-tested directly. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PsxDiscTrayOrigin {
    PSX_DISC_TRAY_ORIGIN_MENU = 1,          /* in-game disc menu */
    PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT,   /* ChangeDisc hotkey (default Ctrl+C) */
    PSX_DISC_TRAY_ORIGIN_DEBUG_SELECT,      /* debug server disc_select */
    PSX_DISC_TRAY_ORIGIN_DEBUG_REINSERT,    /* debug server cd_reinsert */
    PSX_DISC_TRAY_ORIGIN_DEBUG_MENU,        /* debug server disc_menu accept */
    PSX_DISC_TRAY_ORIGIN_AUTO               /* verified title request */
} PsxDiscTrayOrigin;

typedef enum PsxDiscTrayResult {
    PSX_DISC_TRAY_OK = 0,
    PSX_DISC_TRAY_ERR_NO_ROSTER,   /* disc change asked of a single-disc title */
    PSX_DISC_TRAY_ERR_RANGE,       /* index outside the roster */
    PSX_DISC_TRAY_ERR_NETPLAY,     /* refused: lid cycles desync peers */
    PSX_DISC_TRAY_ERR_OPEN_FAILED  /* image would not open; current disc kept, lid untouched */
} PsxDiscTrayResult;

/* Open the lid, optionally change the disc, and let the timed close run.
 *
 * disc_1based == 0: reinsert the media already mounted (same image).
 * disc_1based >= 1: mount that roster entry while the lid is open. Choosing
 *                   the disc already mounted still cycles the lid, which is
 *                   what pressing eject on a console does.
 *
 * Returns a PsxDiscTrayResult. Only PSX_DISC_TRAY_OK opened the lid; on
 * PSX_DISC_TRAY_ERR_OPEN_FAILED the new image would not open and the mounted
 * disc stays in the closed drive. */
int psx_disc_tray_cycle(int disc_1based, int origin);
/* Main-thread VBlank policy. requested=0 ends a request episode. Readiness
 * may change without rearming it. Returns -1 when no attempt was made, else
 * PsxDiscTrayResult. Same mounted disc never cycles automatically. */
int psx_disc_tray_auto_poll(int requested, int ready);
/* The disc the ChangeDisc hotkey inserts: the roster entry after the mounted
 * one, wrapping from the last back to the first. 0 for a single-disc title
 * (the hotkey then reinserts the same media). */
int psx_disc_tray_next_disc(void);

/* A title's "the game is asking for disc N" condition, from game.toml
 * [[game.disc_requests]]: guest RAM at addr, read as a little-endian value of
 * `size` bytes (1, 2 or 4) and masked, equals value. Read-only: it never
 * touches the guest, so it changes neither codegen nor timing. */
typedef struct PsxDiscRequestRule {
    int      disc;   /* 1-based roster entry asked for */
    uint32_t addr;   /* guest address; KUSEG/KSEG0/KSEG1 all name the same RAM */
    uint32_t size;   /* 1, 2 or 4 */
    uint32_t mask;
    uint32_t value;
} PsxDiscRequestRule;
#define PSX_DISC_TRAY_MAX_REQUEST_RULES 8
/* Replace the rule set (copied; count clamped to the max, NULL/0 clears). */
void psx_disc_tray_set_request_rules(const PsxDiscRequestRule *rules, int count);
int  psx_disc_tray_request_rule_count(void);
/* The disc the first matching rule asks for, else 0. ram is the guest's main
 * RAM and ram_mask its address mask (size - 1). */
int  psx_disc_tray_requested_disc(const uint8_t *ram, uint32_t ram_mask);

/* Begin a new emulation session. Clear request suppression, preserving the
 * process-lifetime diagnostic ring. Call with emulation stopped. */
void psx_disc_tray_reset_session(void);

/* Host notification after a successful DISC CHANGE (not after a same-media
 * reinsert). path is the roster entry now mounted. Called on the emu thread,
 * from inside psx_disc_tray_cycle. NULL clears it. */
typedef void (*PsxDiscTrayHostHook)(void *ctx, int disc_1based,
                                    const char *path);
void psx_disc_tray_set_host_hook(PsxDiscTrayHostHook hook, void *ctx);

/* Roster view for UI: count is 0 for a single-disc title. */
int psx_disc_tray_disc_count(void);
int psx_disc_tray_mounted_disc(void);         /* 1-based; 0 = none */
const char *psx_disc_tray_disc_path(int disc_1based);

const char *psx_disc_tray_result_name(int result);
const char *psx_disc_tray_origin_name(int origin);

/* Always-on ring of every lid-cycle attempt, refusals included. */
#define PSX_DISC_TRAY_LOG_CAP 32
typedef struct PsxDiscTrayEvent {
    uint64_t seq;        /* 0-based, monotonic across the process */
    uint64_t cycle;      /* guest cycle count at the request */
    int      origin;     /* PsxDiscTrayOrigin */
    int      requested;  /* disc_1based argument (0 = reinsert) */
    int      from_disc;  /* mounted roster entry before (0 = none/single) */
    int      to_disc;    /* mounted roster entry after */
    int      result;     /* PsxDiscTrayResult */
    int      rewind_dropped;
} PsxDiscTrayEvent;

/* Total events ever recorded; *entries points at the ring (index seq % CAP). */
uint64_t psx_disc_tray_log(const PsxDiscTrayEvent **entries);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DISC_TRAY_H */
