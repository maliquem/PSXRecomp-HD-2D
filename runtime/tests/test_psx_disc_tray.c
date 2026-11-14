/* psx_disc_tray_test: next-disc choice, RAM request rules and the automatic
 * request episode (psx_disc_tray.h), against a fake drive. */
#include "psx_disc_tray.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

/* Fake drive and host: what psx_disc_tray.c links against. */
uint64_t psx_cycle_count;
static int s_count, s_selected, s_netplay, s_selects, s_reinserts, s_open_ok = 1;
int cdrom_disc_roster_count(void) { return s_count; }
int cdrom_disc_selected(void) { return s_selected; }
const char *cdrom_disc_roster_path(int i) { return i >= 1 && i <= s_count ? "disc.cue" : NULL; }
int cdrom_disc_select(int i) { ++s_selects; s_selected = s_open_ok ? i : 0; return s_open_ok; }
void debug_force_cd_reinsert(void) { ++s_reinserts; }
int psx_netplay_active(void) { return s_netplay; }
uint32_t psx_rewind_discard_history(void) { return 0; }
void psx_savestate_menu_note_slots_changed(void) {}

static void drive(int count, int selected) {
    s_count = count; s_selected = selected; s_selects = s_reinserts = 0;
    s_netplay = 0; s_open_ok = 1;
}

int main(void) {
    /* Next disc: wraps after the last, 0 for a single-disc title. */
    drive(0, 0);  CHECK(psx_disc_tray_next_disc() == 0);
    drive(1, 1);  CHECK(psx_disc_tray_next_disc() == 0);
    drive(2, 1);  CHECK(psx_disc_tray_next_disc() == 2);
    drive(2, 2);  CHECK(psx_disc_tray_next_disc() == 1);
    drive(3, 2);  CHECK(psx_disc_tray_next_disc() == 3);
    drive(3, 0);  CHECK(psx_disc_tray_next_disc() == 1);  /* empty drive */

    /* Hotkey path: single disc reinserts, multi-disc changes. */
    drive(1, 1);
    CHECK(psx_disc_tray_cycle(psx_disc_tray_next_disc(), PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT) == PSX_DISC_TRAY_OK);
    CHECK(s_reinserts == 1 && s_selects == 0);
    drive(2, 2);
    CHECK(psx_disc_tray_cycle(psx_disc_tray_next_disc(), PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT) == PSX_DISC_TRAY_OK);
    CHECK(s_selected == 1 && s_selects == 1);
    drive(2, 1); s_netplay = 1;
    CHECK(psx_disc_tray_cycle(2, PSX_DISC_TRAY_ORIGIN_HOTKEY_REINSERT) == PSX_DISC_TRAY_ERR_NETPLAY);
    CHECK(s_selected == 1 && s_selects == 0);

    /* Request rules: masked little-endian compare, address mirrors masked. */
    uint8_t ram[64];
    memset(ram, 0, sizeof ram);
    const PsxDiscRequestRule rules[] = {
        {2, 0x80000010u, 2, 0xFFFFu, 0x1234u},
        {1, 0x00000020u, 1, 0x0Fu,   0x01u},
    };
    psx_disc_tray_set_request_rules(rules, 2);
    CHECK(psx_disc_tray_request_rule_count() == 2);
    CHECK(psx_disc_tray_requested_disc(ram, 63) == 0);
    CHECK(psx_disc_tray_requested_disc(NULL, 63) == 0);
    ram[0x10] = 0x34; ram[0x11] = 0x12;
    CHECK(psx_disc_tray_requested_disc(ram, 63) == 2);
    ram[0x10] = 0x35;
    CHECK(psx_disc_tray_requested_disc(ram, 63) == 0);
    ram[0x20] = 0xF1;                       /* high nibble masked off */
    CHECK(psx_disc_tray_requested_disc(ram, 63) == 1);
    psx_disc_tray_set_request_rules(NULL, 0);
    CHECK(psx_disc_tray_request_rule_count() == 0 && psx_disc_tray_requested_disc(ram, 63) == 0);

    /* Auto episode: one attempt per request, none for the mounted disc,
     * a new attempt only after the request clears. */
    drive(2, 1);
    psx_disc_tray_reset_session();
    CHECK(psx_disc_tray_auto_poll(0, 0) == -1);
    CHECK(psx_disc_tray_auto_poll(2, 1) == PSX_DISC_TRAY_OK && s_selected == 2 && s_selects == 1);
    CHECK(psx_disc_tray_auto_poll(2, 1) == -1 && s_selects == 1);
    CHECK(psx_disc_tray_auto_poll(0, 0) == -1);
    CHECK(psx_disc_tray_auto_poll(2, 1) == -1 && s_selects == 1);  /* already in */
    CHECK(psx_disc_tray_auto_poll(1, 1) == PSX_DISC_TRAY_OK && s_selected == 1);
    /* Netplay refuses, and the episode is not retried every frame. */
    drive(2, 1); s_netplay = 1;
    psx_disc_tray_reset_session();
    CHECK(psx_disc_tray_auto_poll(2, 1) == PSX_DISC_TRAY_ERR_NETPLAY);
    CHECK(psx_disc_tray_auto_poll(2, 1) == -1 && s_selects == 0);

    /* Every attempt is in the ring. */
    const PsxDiscTrayEvent *log = NULL;
    CHECK(psx_disc_tray_log(&log) > 0 && log != NULL);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    puts("psx_disc_tray_test: ok");
    return 0;
}
