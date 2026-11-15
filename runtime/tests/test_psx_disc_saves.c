/* psx_disc_saves_test: card directory reading and the disc registry
 * (psx_disc_saves.h): record on write, other games untouched, copies inherit,
 * deletes and outside changes prune, the file round-trips. */
#include "psx_disc_saves.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

#define CARD (128 * 1024)
static uint8_t card0[CARD], card1[CARD];

static void format(uint8_t *img)
{
    memset(img, 0, CARD);
    img[0] = 'M'; img[1] = 'C';
    for (int b = 0; b < 15; b++) {
        uint8_t *f = img + (b + 1) * 128;
        f[0] = 0xA0; f[8] = 0xFF; f[9] = 0xFF;
    }
}

/* A save of `n` blocks starting at `first` (blocks first..first+n-1). */
static void put(uint8_t *img, int first, int n, const char *name, uint8_t fill)
{
    for (int i = 0; i < n; i++) {
        const int b = first + i;
        uint8_t *f = img + (b + 1) * 128;
        f[0] = i == 0 ? 0x51 : (i == n - 1 ? 0x53 : 0x52);
        if (n == 1) f[0] = 0x51;
        const int next = i == n - 1 ? 0xFFFF : b + 1;
        f[8] = (uint8_t)next; f[9] = (uint8_t)(next >> 8);
        if (i == 0) { memset(f + 0x0A, 0, 20); memcpy(f + 0x0A, name, strlen(name)); }
        memset(img + (size_t)(b + 1) * 8192, fill, 8192);
    }
}

static void erase(uint8_t *img, int first, int n)
{
    for (int i = 0; i < n; i++) img[(first + i + 1) * 128] = 0xA0;
}

int main(void)
{
    const char *path = "discos_test.ini";
    remove(path);

    format(card0);
    put(card0, 0, 2, "BASLUS-01234GAMEA", 0x11);   /* this game */
    put(card0, 2, 1, "BASLUS-09999OTHER", 0x22);   /* another game */
    PsxCardSave list[15];
    CHECK(psx_card_list_saves(card0, list) == 2);
    CHECK(!strcmp(list[0].name, "BASLUS-01234GAMEA") && list[0].blocks == 0x3);
    CHECK(list[1].first == 2 && list[1].blocks == 0x4);

    /* Broken chain: skipped. */
    {
        static uint8_t bad[CARD];
        format(bad);
        put(bad, 0, 2, "X", 1);
        bad[2 * 128] = 0xA0;   /* second link free */
        CHECK(psx_card_list_saves(bad, list) == 0);
    }

    CHECK(psx_disc_saves_open(path) == 0);
    psx_disc_saves_card_seen(0, "card1.mcd", card0);
    CHECK(psx_disc_saves_newest(NULL) == 0);       /* nothing recorded yet */

    /* The game saves on disc 2: only its save gets a record. */
    put(card0, 0, 2, "BASLUS-01234GAMEA", 0x12);
    CHECK(psx_disc_saves_card_written(0, card0, 2, 1000) == 1);
    CHECK(psx_disc_saves_disc_of_block(0, 0) == 2);
    CHECK(psx_disc_saves_disc_of_block(0, 1) == 2);
    CHECK(psx_disc_saves_disc_of_block(0, 2) == 0);
    CHECK(psx_disc_saves_disc_of_block(0, 5) == 0);
    int64_t when = 0;
    CHECK(psx_disc_saves_newest(&when) == 2 && when == 1000);

    /* Nothing changed: no new record. */
    CHECK(psx_disc_saves_card_written(0, card0, 1, 1100) == 0);

    /* A new save on disc 1, later. */
    put(card0, 3, 1, "BASLUS-01234GAMEB", 0x33);
    CHECK(psx_disc_saves_card_written(0, card0, 1, 2000) == 1);
    CHECK(psx_disc_saves_disc_of_block(0, 3) == 1);
    CHECK(psx_disc_saves_newest(&when) == 1 && when == 2000);

    /* Copy to the other card (same bytes): inherits disc and time. */
    format(card1);
    psx_disc_saves_card_seen(1, "card2.mcd", card1);
    put(card1, 0, 2, "BASLUS-01234GAMEA", 0x12);
    CHECK(psx_disc_saves_card_written(1, card1, 1, 3000) == 1);
    CHECK(psx_disc_saves_disc_of_block(1, 0) == 2);
    CHECK(psx_disc_saves_newest(&when) == 1 && when == 2000);

    /* Delete GAMEB: its record goes; newest falls back to disc 2. */
    erase(card0, 3, 1);
    CHECK(psx_disc_saves_card_written(0, card0, 1, 4000) == 1);
    CHECK(psx_disc_saves_disc_of_block(0, 3) == 0);
    CHECK(psx_disc_saves_newest(&when) == 2 && when == 1000);

    /* Round trip through the file. */
    CHECK(psx_disc_saves_flush() == 0);
    CHECK(psx_disc_saves_open(path) == 0);
    CHECK(psx_disc_saves_newest(&when) == 2 && when == 1000);   /* no card seen: all records */
    psx_disc_saves_card_seen(0, "card1.mcd", card0);
    CHECK(psx_disc_saves_disc_of_block(0, 0) == 2);

    /* Changed outside (another emulator): record dropped on sight. */
    CHECK(psx_disc_saves_open(path) == 0);
    put(card0, 0, 2, "BASLUS-01234GAMEA", 0x77);
    psx_disc_saves_card_seen(0, "card1.mcd", card0);
    CHECK(psx_disc_saves_disc_of_block(0, 0) == 0);

    /* Copied outside while closed: the unrecorded save inherits on sight. */
    CHECK(psx_disc_saves_open(path) == 0);
    format(card0);
    put(card0, 5, 2, "BASLUS-01234GAMEC", 0x12);   /* bytes differ: name is hashed */
    put(card0, 8, 2, "BASLUS-01234GAMEA", 0x12);   /* same name and bytes as card2's */
    psx_disc_saves_card_seen(0, "card9.mcd", card0);
    CHECK(psx_disc_saves_disc_of_block(0, 5) == 0);
    CHECK(psx_disc_saves_disc_of_block(0, 8) == 2);

    /* Slot emptied: its blocks answer nothing, its writes are ignored. */
    psx_disc_saves_card_seen(0, NULL, NULL);
    CHECK(psx_disc_saves_disc_of_block(0, 8) == 0);
    CHECK(psx_disc_saves_card_written(0, card0, 2, 9) == 0);

    /* Write on a card never seen: ignored. */
    CHECK(psx_disc_saves_open(path) == 0);
    CHECK(psx_disc_saves_card_written(1, card1, 2, 9) == 0);

    remove(path);
    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("psx_disc_saves_test ok\n");
    return 0;
}
