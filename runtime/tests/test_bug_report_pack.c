/* The bug report's ZIP and sealed box: an archive any reader takes, a box that
 * opens with the right key only and refuses any changed byte. */
#include "../src/bug_report_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int checks, failures;
#define CHECK(c)                                                  \
    do {                                                          \
        checks++;                                                 \
        if (!(c)) {                                               \
            failures++;                                           \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);  \
        }                                                         \
    } while (0)

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

/* Walks the central directory the way an unzip does. */
static void test_zip(void) {
    BrZip z;
    br_zip_init(&z);
    static const char a[] = "first member", b[] = "second";
    br_zip_add(&z, "report.json", a, sizeof a - 1);
    br_zip_add(&z, "config/settings.toml", b, sizeof b - 1);
    br_zip_add(&z, "empty.bin", "", 0);
    CHECK(br_zip_finish(&z));
    CHECK(z.count == 3);
    const uint8_t *end = z.buf + z.len - 22;
    CHECK(rd32(end) == 0x06054B50u);
    CHECK(rd16(end + 10) == 3);
    const uint8_t *e = z.buf + rd32(end + 16);
    static const char *const names[3] = { "report.json", "config/settings.toml", "empty.bin" };
    static const char *const data[3] = { a, b, "" };
    for (int i = 0; i < 3; i++) {
        CHECK(rd32(e) == 0x02014B50u);
        const uint32_t size = rd32(e + 24), name_n = rd16(e + 28), at = rd32(e + 42);
        CHECK(rd16(e + 10) == 0);                 /* stored */
        CHECK(rd16(e + 8) & (1u << 11));          /* UTF-8 names */
        CHECK(name_n == strlen(names[i]) && !memcmp(e + 46, names[i], name_n));
        const uint8_t *l = z.buf + at;
        CHECK(rd32(l) == 0x04034B50u);
        const uint8_t *body = l + 30 + rd16(l + 26) + rd16(l + 28);
        CHECK(size == strlen(data[i]) && !memcmp(body, data[i], size));
        CHECK(rd32(e + 16) == (uint32_t)crc32(0L, body, size));
        e += 46 + name_n + rd16(e + 30) + rd16(e + 32);
    }
    br_zip_free(&z);

    BrZip full;
    br_zip_init(&full);
    for (int i = 0; i < BR_ZIP_ENTRIES_MAX + 1; i++) br_zip_add(&full, "x", "y", 1);
    CHECK(full.failed);
    CHECK(!br_zip_finish(&full));
    br_zip_free(&full);
}

static void test_seal(void) {
    uint8_t priv[32], pub[32], other_priv[32], other_pub[32];
    CHECK(br_keypair(priv, pub));
    CHECK(br_keypair(other_priv, other_pub));
    CHECK(memcmp(priv, other_priv, 32) != 0);

    static const char text[] = "the save state of a player";
    const size_t n = sizeof text;
    uint8_t *box = (uint8_t *)malloc(n + BR_SEAL_OVERHEAD), *back = (uint8_t *)malloc(n), *box2 = (uint8_t *)malloc(n + BR_SEAL_OVERHEAD);
    CHECK(br_seal(pub, text, n, box));
    CHECK(br_seal(pub, text, n, box2));
    CHECK(memcmp(box, box2, n + BR_SEAL_OVERHEAD) != 0);      /* a new one-time key each time */
    CHECK(memcmp(box + BR_SEAL_OVERHEAD, text, n) != 0);
    CHECK(br_unseal(priv, box, n + BR_SEAL_OVERHEAD, back));
    CHECK(!memcmp(back, text, n));
    CHECK(!br_unseal(other_priv, box, n + BR_SEAL_OVERHEAD, back));

    /* every byte of the box counts: the header too */
    for (size_t i = 0; i < n + BR_SEAL_OVERHEAD; i++) {
        box[i] ^= 0x40;
        CHECK(!br_unseal(priv, box, n + BR_SEAL_OVERHEAD, back));
        box[i] ^= 0x40;
    }
    CHECK(br_unseal(priv, box, n + BR_SEAL_OVERHEAD, back));
    CHECK(!br_unseal(priv, box, BR_SEAL_OVERHEAD - 1, back));

    /* an empty text seals too */
    CHECK(br_seal(pub, "", 0, box));
    CHECK(br_unseal(priv, box, BR_SEAL_OVERHEAD, back));
    free(box);
    free(box2);
    free(back);
}

int main(void) {
    test_zip();
    test_seal();
    printf("BUG_REPORT_PACK checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
