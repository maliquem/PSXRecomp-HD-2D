/*
 * test_save_slots.c -- save slots against the real SIO card protocol.
 *
 * The "game" here is the test driving sio.c byte by byte, the way the BIOS
 * card driver does: it reads the directory of a mounted card, sees the "new
 * card" FLAG, writes a changed save, and the slot store keeps it. Also pins
 * the player's card file staying untouched while a slot card is mounted,
 * kept files flowing back, details and names round-tripping, import, copy,
 * delete and the icon decode.
 */

#include "sio.h"
#include "memcard.h"
#include "psx_save_slots.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* ---- Stubs for sio.c's external dependencies (as test_sio_card_protocol) */
uint32_t i_stat = 0;
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }
uint64_t psx_get_cycle_count(void) { static uint64_t t; return t += 64; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t addr, uint32_t value, uint8_t width) {
    (void)addr; (void)value; (void)width;
}
void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat, uint8_t active_device,
                            uint8_t selected_slot, uint16_t mc_state,
                            uint8_t mc_cmd, uint16_t mc_sector,
                            uint8_t mc_data_idx, uint32_t func_addr) {
    (void)kind; (void)tx; (void)rx; (void)ctrl; (void)stat;
    (void)active_device; (void)selected_slot; (void)mc_state;
    (void)mc_cmd; (void)mc_sector; (void)mc_data_idx; (void)func_addr;
}
void card_read_summary_record(uint8_t slot, uint8_t cmd, uint16_t sector,
                              uint8_t end, uint8_t checksum,
                              uint8_t data0, uint8_t data1,
                              uint32_t func_addr) {
    (void)slot; (void)cmd; (void)sector; (void)end; (void)checksum;
    (void)data0; (void)data1; (void)func_addr;
}
void card_data_writes_arm(uint8_t value, uint16_t mc_state,
                          uint8_t mc_data_idx, uint8_t slot) {
    (void)value; (void)mc_state; (void)mc_data_idx; (void)slot;
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }

/* ---- harness ------------------------------------------------------------ */

static int g_fail, g_checks;
#define EXPECT(label, cond)                                                   \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, label); } \
    } while (0)

#define SIO_TX_DATA 0x1F801040
#define SIO_RX_DATA 0x1F801040
#define SIO_CTRL    0x1F80104A
#define CTRL_TX_EN      (1 << 0)
#define CTRL_SELECT     (1 << 1)
#define CTRL_ACK        (1 << 4)
#define CTRL_ACK_IRQ_EN (1 << 12)

static uint8_t xchg(uint8_t tx) {
    uint16_t ctrl = CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQ_EN;
    uint8_t rx;
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_advance(1088);
    rx = (uint8_t)sio_read(SIO_RX_DATA);
    sio_advance(170);
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    i_stat &= ~0x80u;
    return rx;
}

static void deselect(void) {
    sio_write(SIO_CTRL, CTRL_TX_EN | CTRL_ACK_IRQ_EN);
    sio_tick(2000);
}

/* A whole sector read; returns the FLAG reply, data in out. */
static uint8_t game_read(uint16_t sector, uint8_t out[128], int *ok) {
    uint8_t flag;
    int i;
    xchg(0x81);
    flag = xchg(0x52);
    xchg(0); xchg(0);
    xchg((uint8_t)(sector >> 8)); xchg((uint8_t)sector);
    xchg(0); xchg(0); xchg(0); xchg(0);
    for (i = 0; i < 128; i++) out[i] = xchg(0);
    xchg(0);
    *ok = (xchg(0) == 0x47);
    deselect();
    return flag;
}

static int game_write(uint16_t sector, const uint8_t data[128]) {
    uint8_t chk = (uint8_t)((sector >> 8) ^ (sector & 0xFF));
    int i, end;
    for (i = 0; i < 128; i++) chk ^= data[i];
    xchg(0x81); xchg(0x57); xchg(0); xchg(0);
    xchg((uint8_t)(sector >> 8)); xchg((uint8_t)sector);
    for (i = 0; i < 128; i++) xchg(data[i]);
    xchg(chk); xchg(0); xchg(0);
    end = xchg(0);
    deselect();
    return end == 0x47;
}

/* ---- card building ------------------------------------------------------ */

static void seal(uint8_t *f) {
    uint8_t x = 0;
    int i;
    for (i = 0; i < 127; i++) x ^= f[i];
    f[127] = x;
}

/* Put a file of `nblocks` at blocks first.. on img, data filled with `fill`
 * and a save header (title, 1-frame icon) in its first block. */
static void add_file(uint8_t *img, int first, int nblocks, const char *name, uint8_t fill) {
    int i;
    for (i = 0; i < nblocks; i++) {
        int b = first + i;
        uint8_t *f = img + (b + 1) * 128;
        uint8_t *blk = img + (b + 1) * 8192;
        int next = (i + 1 < nblocks) ? b + 1 : 0xFFFF;
        memset(f, 0, 128);
        f[0] = (uint8_t)(i == 0 ? 0x51 : (i + 1 < nblocks ? 0x52 : 0x53));
        if (i == 0) {
            uint32_t size = (uint32_t)nblocks * 8192;
            memcpy(f + 4, &size, 4);
            memcpy(f + 0x0A, name, strlen(name));
        }
        f[8] = (uint8_t)next;
        f[9] = (uint8_t)(next >> 8);
        seal(f);
        memset(blk, fill, 8192);
        if (i == 0) {
            blk[0] = 'S'; blk[1] = 'C'; blk[2] = 0x11; blk[3] = 1;
            memcpy(blk + 4, "TEST SAVE", 9);
            memset(blk + 4 + 9, 0, 55);
            memset(blk + 0x60, 0, 32);
            blk[0x62] = 0x1F; blk[0x63] = 0x00;          /* clut[1] = red */
            memset(blk + 0x80, 0x11, 128);               /* icon: all clut[1] */
        }
    }
}

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static int drained(void) { return memcard_flush_wait(5000) == 0; }

static uint8_t card0[MEMCARD_SIZE], img[MEMCARD_SIZE], disk[MEMCARD_SIZE];

int main(void) {
    PSXSaveSlotInfo info;
    PSXSaveSlotDetails d;
    uint8_t sec[128];
    size_t len = 0;
    int ok, rc;

    MKDIR("slots_test");
    remove("slots_test/card1.mcd");
    sio_init();
    memcard_init("slots_test");
    EXPECT("player card created", memcard_is_present(0));

    /* The player's card: a kept settings file and a 2-block game save. */
    memcard_format_image(card0);
    add_file(card0, 0, 1, "BISLPS-01000-FM2-S", 0x5A);
    add_file(card0, 1, 2, "BASLUS-01324BOF400US", 0xC3);
    EXPECT("import card", memcard_import_raw(0, card0) == 0);
    EXPECT("card file written atomically",
           drained() && read_file("slots_test/card1.mcd", disk, sizeof(disk), &len) == 0 &&
           len == MEMCARD_SIZE && memcmp(disk, card0, MEMCARD_SIZE) == 0);

    /* Store with 100 slots; the kept file never becomes a slot. */
    EXPECT("open", psx_save_slots_open(100, "slots_test/slots") == 0);
    EXPECT("count", psx_save_slots_count() == 100);
    psx_save_slots_keep_files("BISLPS-01000-FM2-S");
    EXPECT("import one save", psx_save_slots_import_card("slots_test/card1.mcd", NULL) == 1);
    EXPECT("slot 1 used", psx_save_slots_get(1, &info) == 0 && info.used && info.blocks == 2);
    EXPECT("slot 1 name on card", strcmp(info.file_name, "BASLUS-01324BOF400US") == 0);
    EXPECT("slot 1 title", memcmp(info.title_sjis, "TEST SAVE", 9) == 0);
    EXPECT("slot 2 empty", psx_save_slots_get(2, &info) == 0 && !info.used);
    {
        uint8_t rgba[1024];
        EXPECT("icon frames", psx_save_slots_icon(1, 0, rgba) == 1);
        EXPECT("icon red", rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
        EXPECT("empty slot has no icon", psx_save_slots_icon(2, 0, rgba) == -1);
    }

    /* Load: mount slot 1 under the name the game reads at its first place. */
    EXPECT("mount slot 1", psx_save_slots_mount(0, 1, "BASLUS-01324BOF400US") == 0);
    EXPECT("mounted", psx_save_slots_mounted() == 1);
    EXPECT("second mount refused", psx_save_slots_mount(0, 2, NULL) == -1);
    {
        uint8_t flag = game_read(0, sec, &ok);
        EXPECT("new card flag after mount", flag == 0x08 && ok && sec[0] == 'M' && sec[1] == 'C');
        game_read(1, sec, &ok);
        EXPECT("kept file at block 0", sec[0] == 0x51 && memcmp(sec + 0x0A, "BISLPS-01000-FM2-S", 18) == 0);
        game_read(2, sec, &ok);
        EXPECT("save at block 1", sec[0] == 0x51 && memcmp(sec + 0x0A, "BASLUS-01324BOF400US", 20) == 0);
        game_read(64 * 2, sec, &ok);
        EXPECT("save data", sec[0] == 'S' && sec[1] == 'C');
    }

    /* The game saves: changes a byte of the save and of the kept file. */
    memset(sec, 0xEE, 128);
    EXPECT("game writes save", game_write(64 * 3 + 5, sec) == 1);
    memset(sec, 0x77, 128);
    EXPECT("game writes kept", game_write(64 * 1 + 9, sec) == 1);
    EXPECT("player card untouched while mounted",
           drained() && read_file("slots_test/card1.mcd", disk, sizeof(disk), &len) == 0 &&
           memcmp(disk, card0, MEMCARD_SIZE) == 0);

    memset(&d, 0, sizeof(d));
    d.fields = PSX_SAVE_FIELD_PLAYTIME | PSX_SAVE_FIELD_MONEY | PSX_SAVE_FIELD_CHAPTER |
               PSX_SAVE_FIELD_PARTY | PSX_SAVE_FIELD_DIFFICULTY | PSX_SAVE_FIELD_ACHIEVEMENTS;
    d.playtime_seconds = 3 * 3600 + 25 * 60;
    d.money = 4520;
    d.chapter = 2;
    snprintf(d.chapter_label, sizeof(d.chapter_label), "Chapter 2: Sand\nand wind");
    d.party_count = 2;
    d.party[0].id = 1; d.party[0].level = 12;
    d.party[1].id = 2; d.party[1].level = 11;
    snprintf(d.difficulty, sizeof(d.difficulty), "Normal");
    d.achievements_unlocked = 7; d.achievements_total = 96;
    d.achievement_mode = PSX_SAVE_ACH_HARDCORE;
    d.extra_count = 1;
    snprintf(d.extra[0].key, sizeof(d.extra[0].key), "location");
    snprintf(d.extra[0].value, sizeof(d.extra[0].value), "Path\\to\\desert");
    EXPECT("commit to slot 5", psx_save_slots_commit(5, &d) == 0);
    EXPECT("unmount", psx_save_slots_unmount() == 0);
    EXPECT("nothing mounted", psx_save_slots_mounted() == -1);
    {
        uint8_t flag = game_read(0, sec, &ok);
        EXPECT("new card flag after unmount", flag == 0x08 && ok);
    }

    /* Kept file went back to the player's card; the save stayed in the store. */
    EXPECT("player card file read", drained() && read_file("slots_test/card1.mcd", disk, sizeof(disk), &len) == 0);
    EXPECT("kept change on player card", disk[(0 + 1) * 8192 + 9 * 128] == 0x77);
    EXPECT("player save unchanged", disk[(1 + 1) * 8192 + 64 * 128 + 5 * 128] == 0xC3);

    /* Reopen the store from disk: everything round-trips. */
    EXPECT("reopen", psx_save_slots_open(100, "slots_test/slots") == 0);
    EXPECT("slot 5 used", psx_save_slots_get(5, &info) == 0 && info.used && info.blocks == 2);
    EXPECT("slot 5 saved time", info.saved_time > 0);
    EXPECT("details", info.details.playtime_seconds == 12300 && info.details.money == 4520 &&
                      info.details.chapter == 2 && info.details.party_count == 2 &&
                      info.details.party[1].id == 2 && info.details.party[1].level == 11 &&
                      info.details.achievements_unlocked == 7 && info.details.achievements_total == 96 &&
                      info.details.achievement_mode == PSX_SAVE_ACH_HARDCORE &&
                      strcmp(info.details.difficulty, "Normal") == 0);
    EXPECT("label with newline", strcmp(info.details.chapter_label, "Chapter 2: Sand\nand wind") == 0);
    EXPECT("extra with backslashes", info.details.extra_count == 1 &&
                      strcmp(info.details.extra[0].value, "Path\\to\\desert") == 0);
    {
        FILE *f = fopen("slots_test/slots/slot_005.mcs", "rb");
        uint8_t buf[128 + 2 * 8192];
        size_t n = f ? fread(buf, 1, sizeof(buf), f) : 0;
        if (f) fclose(f);
        EXPECT("slot 5 has the written byte", n == sizeof(buf) && buf[128 + 8192 + 5 * 128] == 0xEE);
        EXPECT("slot 5 kept file not in it", memcmp(buf + 0x0A, "BASLUS-01324BOF400US", 20) == 0);
    }

    /* Names: set, keep across a commit, clear. */
    EXPECT("rename", psx_save_slots_rename(5, "Before the desert") == 0);
    EXPECT("rename empty slot refused", psx_save_slots_rename(9, "x") == -1);
    EXPECT("mount slot 5 to save over it", psx_save_slots_mount(0, 5, NULL) == 0);
    memset(sec, 0xAB, 128);
    EXPECT("game rewrites", game_write(64 * 2 + 3, sec) == 1);
    EXPECT("commit keeps the name", psx_save_slots_commit(5, NULL) == 0);
    EXPECT("unmount 2", psx_save_slots_unmount() == 0);
    EXPECT("name kept", psx_save_slots_get(5, &info) == 0 && strcmp(info.name, "Before the desert") == 0);
    EXPECT("details replaced by NULL", info.details.fields == 0);

    /* A new save on an empty card: the game writes its own directory entry. */
    EXPECT("mount empty", psx_save_slots_mount(0, 0, NULL) == 0);
    game_read(2, sec, &ok);
    EXPECT("empty card: block 1 free", sec[0] == 0xA0);
    memset(img, 0, sizeof(img));
    add_file(img, 1, 1, "BASLUS-01324BOF401US", 0x42);
    EXPECT("game writes frame", game_write(2, img + 2 * 128) == 1);
    {
        int s;
        for (s = 0; s < 64; s++) game_write((uint16_t)(64 * 2 + s), img + 2 * 8192 + s * 128);
    }
    EXPECT("commit new to slot 7", psx_save_slots_commit(7, NULL) == 0);
    EXPECT("unmount 3", psx_save_slots_unmount() == 0);
    EXPECT("slot 7 has the new file", psx_save_slots_get(7, &info) == 0 && info.used &&
           strcmp(info.file_name, "BASLUS-01324BOF401US") == 0 && info.blocks == 1);

    /* Nothing written: commit says so. */
    EXPECT("mount empty again", psx_save_slots_mount(0, 0, NULL) == 0);
    EXPECT("commit with no save", psx_save_slots_commit(8, NULL) == -2);
    EXPECT("unmount 4", psx_save_slots_unmount() == 0);

    /* Copy and delete. */
    EXPECT("copy 5 to 10", psx_save_slots_copy(5, 10) == 0);
    EXPECT("copy has the name", psx_save_slots_get(10, &info) == 0 && strcmp(info.name, "Before the desert") == 0);
    EXPECT("delete 10", psx_save_slots_delete(10) == 0);
    EXPECT("10 empty", psx_save_slots_get(10, &info) == 0 && !info.used);
    EXPECT("delete empty ok", psx_save_slots_delete(11) == 0);
    rc = psx_save_slots_get(0, &info);
    EXPECT("slot 0 invalid", rc == -1);
    rc = psx_save_slots_get(101, &info);
    EXPECT("slot 101 invalid", rc == -1);

    /* A kept file that grows does not fit back on a full player's card: the
     * old one stays there. */
    memcard_format_image(card0);
    add_file(card0, 0, 1, "BISLPS-01000-FM2-S", 0x5A);
    add_file(card0, 1, 14, "BASLUS-01324BOF402US", 0x10);
    EXPECT("full card imported", memcard_import_raw(0, card0) == 0 && drained());
    EXPECT("mount for growth", psx_save_slots_mount(0, 0, NULL) == 0);
    memset(img, 0, sizeof(img));
    add_file(img, 0, 2, "BISLPS-01000-FM2-S", 0x66);
    EXPECT("game grows kept file", game_write(1, img + 1 * 128) == 1 &&
                                   game_write(2, img + 2 * 128) == 1);
    EXPECT("nothing else saved", psx_save_slots_commit(8, NULL) == -2);
    EXPECT("unmount 5", psx_save_slots_unmount() == 0);
    game_read(1, sec, &ok);
    EXPECT("old kept file still on the card",
           ok && sec[0] == 0x51 && memcmp(sec + 0x0A, "BISLPS-01000-FM2-S", 18) == 0);

    printf("save slots: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
