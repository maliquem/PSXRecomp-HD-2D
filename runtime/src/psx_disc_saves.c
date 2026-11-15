/* psx_disc_saves.c - which disc each memory card save was made on.
 * See psx_disc_saves.h for the file format and the rules. */

#include "psx_disc_saves.h"

#include "psx_sha256.h"
#include "psx_write_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE  8192
#define FRAME_SIZE  128
#define CARD_KEY_MAX 256
#define RECORDS_MAX  512

#define ST_FIRST  0x51u
#define ST_MIDDLE 0x52u
#define ST_LAST   0x53u

typedef struct {
    char    card[CARD_KEY_MAX];
    char    name[21];
    int     disc;
    uint8_t sha[32];
    int64_t when;
} Record;

typedef struct {
    int         valid;
    char        key[CARD_KEY_MAX];
    int         count;
    PsxCardSave saves[PSX_CARD_SAVES_MAX];
} Snapshot;

static Record   s_rec[RECORDS_MAX];
static int      s_rec_count;
static Snapshot s_snap[2];
static char     s_path[1024];
static int      s_dirty;

/* ---- card directory ---------------------------------------------------- */

int psx_card_list_saves(const uint8_t *img, PsxCardSave out[PSX_CARD_SAVES_MAX])
{
    int n = 0;
    if (!img || !out) return 0;
    for (int first = 0; first < PSX_CARD_SAVES_MAX; first++) {
        const uint8_t *f = img + (size_t)(first + 1) * FRAME_SIZE;
        if (f[0] != ST_FIRST) continue;
        int chain[PSX_CARD_SAVES_MAX], len = 0, b = first, ok = 0;
        uint16_t mask = 0;
        for (;;) {
            if (b < 0 || b >= PSX_CARD_SAVES_MAX || len >= PSX_CARD_SAVES_MAX || (mask >> b) & 1u)
                break;
            const uint8_t *fb = img + (size_t)(b + 1) * FRAME_SIZE;
            const uint8_t st = fb[0];
            if (len == 0 ? st != ST_FIRST : (st != ST_MIDDLE && st != ST_LAST))
                break;
            chain[len++] = b;
            mask |= (uint16_t)(1u << b);
            const int next = fb[8] | (fb[9] << 8);
            if (next == 0xFFFF) { ok = 1; break; }
            b = next;
        }
        if (!ok) continue;
        PsxCardSave *s = &out[n++];
        memcpy(s->name, f + 0x0A, 20);
        s->name[20] = '\0';
        s->first = first;
        s->blocks = mask;
        psx_sha256_ctx ctx;
        psx_sha256_init(&ctx);
        psx_sha256_update(&ctx, (const uint8_t *)s->name, 20);
        for (int i = 0; i < len; i++)
            psx_sha256_update(&ctx, img + (size_t)(chain[i] + 1) * BLOCK_SIZE, BLOCK_SIZE);
        psx_sha256_final(&ctx, s->sha);
    }
    return n;
}

/* ---- records ----------------------------------------------------------- */

static Record *rec_find(const char *card, const char *name)
{
    for (int i = 0; i < s_rec_count; i++)
        if (!strcmp(s_rec[i].card, card) && !strcmp(s_rec[i].name, name))
            return &s_rec[i];
    return NULL;
}

static const Record *rec_by_sha(const uint8_t sha[32])
{
    for (int i = 0; i < s_rec_count; i++)
        if (!memcmp(s_rec[i].sha, sha, 32)) return &s_rec[i];
    return NULL;
}

static void rec_remove(Record *r)
{
    *r = s_rec[--s_rec_count];
    s_dirty = 1;
}

static void rec_put(const char *card, const PsxCardSave *s, int disc, int64_t when)
{
    Record *r = rec_find(card, s->name);
    if (!r) {
        if (s_rec_count >= RECORDS_MAX) return;
        r = &s_rec[s_rec_count++];
        snprintf(r->card, sizeof r->card, "%s", card);
        memcpy(r->name, s->name, sizeof r->name);
    }
    r->disc = disc;
    r->when = when;
    memcpy(r->sha, s->sha, 32);
    s_dirty = 1;
}

static const PsxCardSave *save_named(const PsxCardSave *list, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].name, name)) return &list[i];
    return NULL;
}

/* Records of `card` whose save is not in `list` (or, with `strict`, whose
 * bytes differ) go away. */
static void prune(const char *card, const PsxCardSave *list, int n, int strict)
{
    for (int i = s_rec_count - 1; i >= 0; i--) {
        if (strcmp(s_rec[i].card, card)) continue;
        const PsxCardSave *s = save_named(list, n, s_rec[i].name);
        if (!s || (strict && memcmp(s->sha, s_rec[i].sha, 32)))
            rec_remove(&s_rec[i]);
    }
}

void psx_disc_saves_card_seen(int slot, const char *card_key, const uint8_t *img)
{
    if (slot < 0 || slot > 1) return;
    Snapshot *sn = &s_snap[slot];
    if (!card_key || !img) { sn->valid = 0; return; }   /* slot empty now */
    snprintf(sn->key, sizeof sn->key, "%s", card_key);
    sn->count = psx_card_list_saves(img, sn->saves);
    sn->valid = 1;
    prune(sn->key, sn->saves, sn->count, 1);
    for (int i = 0; i < sn->count; i++) {
        if (rec_find(sn->key, sn->saves[i].name)) continue;
        const Record *src = rec_by_sha(sn->saves[i].sha);
        if (src) rec_put(sn->key, &sn->saves[i], src->disc, src->when);
    }
}

int psx_disc_saves_card_written(int slot, const uint8_t *img, int disc, int64_t now)
{
    if (slot < 0 || slot > 1 || !img || !s_snap[slot].valid) return 0;
    Snapshot *sn = &s_snap[slot];
    PsxCardSave now_saves[PSX_CARD_SAVES_MAX];
    const int n = psx_card_list_saves(img, now_saves);
    const int before = s_rec_count;
    int changed = 0;

    prune(sn->key, now_saves, n, 0);
    changed += before - s_rec_count;
    for (int i = 0; i < n; i++) {
        const PsxCardSave *old = save_named(sn->saves, sn->count, now_saves[i].name);
        if (old && !memcmp(old->sha, now_saves[i].sha, 32)) continue;
        const Record *src = rec_by_sha(now_saves[i].sha);
        if (src)   /* a copy: same bytes as a save already recorded */
            rec_put(sn->key, &now_saves[i], src->disc, src->when);
        else if (disc > 0)
            rec_put(sn->key, &now_saves[i], disc, now);
        else
            continue;
        changed++;
    }
    memcpy(sn->saves, now_saves, sizeof now_saves);
    sn->count = n;
    return changed;
}

int psx_disc_saves_disc_of_block(int slot, int block)
{
    if (slot < 0 || slot > 1 || block < 0 || block >= PSX_CARD_SAVES_MAX) return 0;
    const Snapshot *sn = &s_snap[slot];
    if (!sn->valid) return 0;
    for (int i = 0; i < sn->count; i++) {
        if (!((sn->saves[i].blocks >> block) & 1u)) continue;
        const Record *r = rec_find(sn->key, sn->saves[i].name);
        return r ? r->disc : 0;
    }
    return 0;
}

int psx_disc_saves_newest(int64_t *when)
{
    const int any_seen = s_snap[0].valid || s_snap[1].valid;
    const Record *best = NULL;
    for (int i = 0; i < s_rec_count; i++) {
        const Record *r = &s_rec[i];
        if (any_seen && !(s_snap[0].valid && !strcmp(r->card, s_snap[0].key)) &&
            !(s_snap[1].valid && !strcmp(r->card, s_snap[1].key)))
            continue;
        if (!best || r->when > best->when) best = r;
    }
    if (when) *when = best ? best->when : 0;
    return best ? best->disc : 0;
}

/* ---- file -------------------------------------------------------------- */

static int hex_sha(const char *s, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int psx_disc_saves_open(const char *path)
{
    s_rec_count = 0;
    s_dirty = 0;
    memset(s_snap, 0, sizeof s_snap);
    snprintf(s_path, sizeof s_path, "%s", path ? path : "");
    FILE *f = s_path[0] ? fopen(s_path, "rb") : NULL;
    if (!f) return 0;
    char line[512];
    Record *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '[') {
            char *bar = strrchr(line, '|');
            char *end = strrchr(line, ']');
            cur = NULL;
            if (!bar || !end || end < bar || s_rec_count >= RECORDS_MAX) continue;
            *bar = *end = '\0';
            if (strlen(line + 1) >= CARD_KEY_MAX || strlen(bar + 1) > 20) continue;
            cur = &s_rec[s_rec_count++];
            memset(cur, 0, sizeof *cur);
            strcpy(cur->card, line + 1);
            strcpy(cur->name, bar + 1);
        } else if (cur && !strncmp(line, "disco=", 6)) {
            cur->disc = atoi(line + 6);
        } else if (cur && !strncmp(line, "hash=", 5)) {
            if (strlen(line + 5) != 64 || hex_sha(line + 5, cur->sha)) cur->disc = 0;
        } else if (cur && !strncmp(line, "gravado=", 8)) {
            cur->when = strtoll(line + 8, NULL, 10);
        }
    }
    fclose(f);
    /* A record without a disc or hash is no record. */
    for (int i = s_rec_count - 1; i >= 0; i--)
        if (s_rec[i].disc <= 0) s_rec[i] = s_rec[--s_rec_count];
    return 0;
}

int psx_disc_saves_flush(void)
{
    if (!s_dirty || !s_path[0]) return 0;
    const size_t cap = 16 + (size_t)s_rec_count * (CARD_KEY_MAX + 160);
    char *buf = (char *)malloc(cap);
    if (!buf) return -1;
    size_t len = (size_t)snprintf(buf, cap, "versao=1\n");
    for (int i = 0; i < s_rec_count; i++) {
        const Record *r = &s_rec[i];
        char hex[65];
        for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", r->sha[k]);
        len += (size_t)snprintf(buf + len, cap - len,
                                "\n[%s|%s]\ndisco=%d\nhash=%s\ngravado=%lld\n",
                                r->card, r->name, r->disc, hex, (long long)r->when);
    }
    const int rc = psx_write_file_atomic(s_path, buf, len);
    free(buf);
    if (rc == 0) s_dirty = 0;
    return rc;
}
