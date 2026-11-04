/* bug_report_pack.c - the ZIP and the sealed box of a sent bug report
 * (bug_report_pack.h, docs/BUG_REPORT.md). Nothing here knows the game. */

#ifdef _WIN32
#define _CRT_RAND_S
#endif

#include "bug_report_pack.h"

#include "../third_party/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#ifndef _WIN32
#include <sys/random.h>
#endif

/* ---- random ---------------------------------------------------------------- */

int br_random(void *out, size_t size) {
    uint8_t *p = (uint8_t *)out;
#ifdef _WIN32
    while (size) {
        unsigned v;
        if (rand_s(&v) != 0) return 0;
        const size_t n = size < sizeof v ? size : sizeof v;
        memcpy(p, &v, n);
        p += n;
        size -= n;
    }
    return 1;
#else
    while (size) {
        const size_t n = size < 256 ? size : 256;   /* getentropy's limit */
        if (getentropy(p, n) != 0) return 0;
        p += n;
        size -= n;
    }
    return 1;
#endif
}

/* ---- ZIP ------------------------------------------------------------------- */

void br_zip_init(BrZip *z) { memset(z, 0, sizeof *z); }

void br_zip_free(BrZip *z) {
    free(z->buf);
    memset(z, 0, sizeof *z);
}

static void zip_put(BrZip *z, const void *data, size_t n) {
    if (z->failed) return;
    if (z->len + n > z->cap) {
        size_t cap = z->cap ? z->cap : 1u << 20;
        while (cap < z->len + n) cap *= 2;
        uint8_t *b = (uint8_t *)realloc(z->buf, cap);
        if (!b) {
            z->failed = 1;
            return;
        }
        z->buf = b;
        z->cap = cap;
    }
    if (n) memcpy(z->buf + z->len, data, n);
    z->len += n;
}
static void zip_u16(BrZip *z, uint32_t v) {
    const uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    zip_put(z, b, 2);
}
static void zip_u32(BrZip *z, uint32_t v) {
    const uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    zip_put(z, b, 4);
}

enum { ZIP_VERSION = 20, ZIP_UTF8_NAMES = 1u << 11, ZIP_DOS_DATE = (1u << 5) | 1u };   /* 1980-01-01: no clock inside */

/* The fields a local header and a central entry share, from the version on. */
static void zip_fields(BrZip *z, const BrZipEntry *e) {
    zip_u16(z, ZIP_VERSION);
    zip_u16(z, ZIP_UTF8_NAMES);
    zip_u16(z, 0);              /* stored */
    zip_u16(z, 0);              /* time */
    zip_u16(z, ZIP_DOS_DATE);
    zip_u32(z, e->crc);
    zip_u32(z, e->size);
    zip_u32(z, e->size);
    zip_u16(z, (uint32_t)strlen(e->name));
    zip_u16(z, 0);              /* no extra field */
}

void br_zip_add(BrZip *z, const char *name, const void *data, size_t size) {
    if (z->failed) return;
    if (z->count >= BR_ZIP_ENTRIES_MAX || strlen(name) >= sizeof z->entry[0].name || size > 0x7FFFFFFFu) {
        z->failed = 1;
        return;
    }
    BrZipEntry *e = &z->entry[z->count++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = (uint32_t)crc32(0L, (const Bytef *)data, (uInt)size);
    e->size = (uint32_t)size;
    e->offset = (uint32_t)z->len;
    zip_u32(z, 0x04034B50u);
    zip_fields(z, e);
    zip_put(z, e->name, strlen(e->name));
    zip_put(z, data, size);
}

int br_zip_add_file(BrZip *z, const char *name, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = size >= 0 ? (uint8_t *)malloc(size ? (size_t)size : 1) : NULL;
    const int got = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (got) br_zip_add(z, name, data, (size_t)size);
    free(data);
    return got;
}

int br_zip_finish(BrZip *z) {
    const uint32_t start = (uint32_t)z->len;
    for (int i = 0; i < z->count; i++) {
        const BrZipEntry *e = &z->entry[i];
        zip_u32(z, 0x02014B50u);
        zip_u16(z, ZIP_VERSION);
        zip_fields(z, e);
        zip_u16(z, 0);          /* no comment */
        zip_u16(z, 0);          /* disk */
        zip_u16(z, 0);          /* internal attributes */
        zip_u32(z, 0);          /* external attributes */
        zip_u32(z, e->offset);
        zip_put(z, e->name, strlen(e->name));
    }
    const uint32_t dir = (uint32_t)z->len - start;
    zip_u32(z, 0x06054B50u);
    zip_u16(z, 0);
    zip_u16(z, 0);
    zip_u16(z, (uint32_t)z->count);
    zip_u16(z, (uint32_t)z->count);
    zip_u32(z, dir);
    zip_u32(z, start);
    zip_u16(z, 0);
    return !z->failed;
}

/* ---- the sealed box -------------------------------------------------------- */

static const uint8_t k_magic[4] = { 'B', 'R', 'S', '1' };

int br_keypair(uint8_t private_key[32], uint8_t public_key[32]) {
    if (!br_random(private_key, 32)) return 0;
    crypto_x25519_public_key(public_key, private_key);
    return 1;
}

static void seal_key(uint8_t key[32], const uint8_t shared[32], const uint8_t sender[32],
                     const uint8_t recipient[32]) {
    uint8_t all[96];
    memcpy(all, shared, 32);
    memcpy(all + 32, sender, 32);
    memcpy(all + 64, recipient, 32);
    crypto_blake2b(key, 32, all, sizeof all);
    crypto_wipe(all, sizeof all);
}

int br_seal(const uint8_t recipient_public[32], const void *plain, size_t size, uint8_t *out) {
    uint8_t secret[32], sender[32], shared[32], key[32], nonce[24];
    if (!br_keypair(secret, sender) || !br_random(nonce, sizeof nonce)) return 0;
    crypto_x25519(shared, secret, recipient_public);
    seal_key(key, shared, sender, recipient_public);
    memcpy(out, k_magic, 4);
    memcpy(out + 4, sender, 32);
    memcpy(out + 36, nonce, 24);
    crypto_aead_lock(out + BR_SEAL_OVERHEAD, out + 60, key, nonce, out, 36, (const uint8_t *)plain, size);
    crypto_wipe(secret, sizeof secret);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(key, sizeof key);
    return 1;
}

int br_unseal(const uint8_t recipient_private[32], const uint8_t *sealed, size_t size, uint8_t *out) {
    uint8_t recipient[32], shared[32], key[32];
    if (size < BR_SEAL_OVERHEAD || memcmp(sealed, k_magic, 4) != 0) return 0;
    crypto_x25519_public_key(recipient, recipient_private);
    crypto_x25519(shared, recipient_private, sealed + 4);
    seal_key(key, shared, sealed + 4, recipient);
    const int bad = crypto_aead_unlock(out, sealed + 60, key, sealed + 36, sealed, 36,
                                       sealed + BR_SEAL_OVERHEAD, size - BR_SEAL_OVERHEAD);
    crypto_wipe(shared, sizeof shared);
    crypto_wipe(key, sizeof key);
    return bad == 0;
}
