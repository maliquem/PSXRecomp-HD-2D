/*
 * save_slots.c -- many saves instead of one memory card (psx_save_slots.h).
 *
 * The card format (directory frames 1..15, 8 KB blocks, linked files) is the
 * PS1's own; the game's card code runs unchanged against a card built in RAM.
 * No fprintf (CLAUDE.md rule 3): failures are return codes.
 */

#include "psx_save_slots.h"
#include "memcard.h"
#include "sio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define BLOCK_SIZE   8192
#define FRAME_SIZE   128
#define CARD_BLOCKS  15
#define KEEP_MAX     8

#define ST_FREE   0xA0u
#define ST_FIRST  0x51u
#define ST_MIDDLE 0x52u
#define ST_LAST   0x53u

/* ---- lock: the page reads from the render thread ---------------------- */

#ifdef _WIN32
static SRWLOCK s_lock = SRWLOCK_INIT;
static void lock(void)   { AcquireSRWLockExclusive(&s_lock); }
static void unlock(void) { ReleaseSRWLockExclusive(&s_lock); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)   { pthread_mutex_lock(&s_lock); }
static void unlock(void) { pthread_mutex_unlock(&s_lock); }
#endif

/* ---- state -------------------------------------------------------------- */

typedef struct {
    PSXSaveSlotInfo info;
    uint8_t header[FRAME_SIZE];   /* first block's header: title, icon, clut */
    uint8_t icon[3][FRAME_SIZE];
} SlotCache;

static char       s_dir[512];
static int        s_count;
static SlotCache *s_slots;          /* s_count entries, index slot - 1 */
static char       s_keep[KEEP_MAX][21];
static int        s_keep_count;

static int        s_mount_card = -1;
static int        s_mount_slot = -1;
static uint8_t    s_rest[MEMCARD_SIZE];   /* the player's card while mounted */
static int        s_rest_changed;
static uint8_t    s_mounted[MEMCARD_SIZE];  /* the card as mounted */

/* ---- small file helpers ------------------------------------------------- */

static void make_dir(const char *dir) {
    char tmp[sizeof(s_dir)];
    size_t len, i;
    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    for (i = 1; i <= len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\' || tmp[i] == '\0') {
            char c = tmp[i];
#ifdef _WIN32
            if (i == 2 && tmp[1] == ':') continue;
#endif
            tmp[i] = '\0';
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0755);
#endif
            tmp[i] = c;
        }
    }
}

static int write_atomic(const char *path, const void *data, size_t len) {
    char tmp[sizeof(s_dir) + 64];
    FILE *f;
    int ok;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -1;
    ok = fwrite(data, 1, len, f) == len && fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) remove(tmp);
    return ok ? 0 : -1;
}

/* Whole file into a malloc'd buffer (caller frees). NULL when missing. */
static uint8_t *read_all(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = 0;
    *len_out = (size_t)n;
    return buf;
}

static void slot_path(char *out, size_t cap, int slot, const char *ext) {
    snprintf(out, cap, "%s/slot_%03d.%s", s_dir, slot, ext);
}

/* ---- card image -------------------------------------------------------- */

static uint8_t *frame_at(uint8_t *img, int block) { return img + (size_t)(block + 1) * FRAME_SIZE; }
static uint8_t *block_at(uint8_t *img, int block) { return img + (size_t)(block + 1) * BLOCK_SIZE; }

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void frame_seal(uint8_t *frame) {
    uint8_t x = 0;
    int i;
    for (i = 0; i < FRAME_SIZE - 1; i++) x ^= frame[i];
    frame[FRAME_SIZE - 1] = x;
}

static void frame_name(const uint8_t *frame, char out[21]) {
    memcpy(out, frame + 0x0A, 20);
    out[20] = '\0';
}

static int is_kept(const char *name) {
    int i;
    for (i = 0; i < s_keep_count; i++)
        if (strncmp(name, s_keep[i], strlen(s_keep[i])) == 0) return 1;
    return 0;
}

/* Blocks of the file starting at `first`, in order. Returns the count, or -1
 * for a broken chain. */
static int file_chain(uint8_t *img, int first, int chain[CARD_BLOCKS]) {
    int n = 0, b = first;
    for (;;) {
        uint8_t *f;
        uint32_t st;
        int next;
        if (b < 0 || b >= CARD_BLOCKS || n >= CARD_BLOCKS) return -1;
        f = frame_at(img, b);
        st = rd32(f) & 0xFFu;
        if (n == 0 ? st != ST_FIRST : (st != ST_MIDDLE && st != ST_LAST)) return -1;
        chain[n++] = b;
        next = f[8] | (f[9] << 8);
        if (next == 0xFFFF) return n;
        b = next;
    }
}

/* One save as .mcs bytes: its first frame (next cleared) then its blocks.
 * Returns a malloc'd buffer and its length, or NULL. */
static uint8_t *extract_file(uint8_t *img, int first, size_t *len_out) {
    int chain[CARD_BLOCKS];
    int n = file_chain(img, first, chain), i;
    uint8_t *out;
    if (n <= 0) return NULL;
    out = (uint8_t *)malloc(FRAME_SIZE + (size_t)n * BLOCK_SIZE);
    if (!out) return NULL;
    memcpy(out, frame_at(img, first), FRAME_SIZE);
    wr32(out + 4, (uint32_t)n * BLOCK_SIZE);
    out[8] = 0xFF; out[9] = 0xFF;
    frame_seal(out);
    for (i = 0; i < n; i++)
        memcpy(out + FRAME_SIZE + (size_t)i * BLOCK_SIZE, block_at(img, chain[i]), BLOCK_SIZE);
    *len_out = FRAME_SIZE + (size_t)n * BLOCK_SIZE;
    return out;
}

/* Place a .mcs save on the card under `name` (NULL = its own). 0 = ok, -1 no
 * room or a bad file. */
static int place_file(uint8_t *img, const uint8_t *mcs, size_t len, const char *name) {
    int n, free_blocks[CARD_BLOCKS], nfree = 0, b, i;
    if (len < FRAME_SIZE + BLOCK_SIZE || (len - FRAME_SIZE) % BLOCK_SIZE != 0) return -1;
    n = (int)((len - FRAME_SIZE) / BLOCK_SIZE);
    for (b = 0; b < CARD_BLOCKS; b++) {
        uint32_t st = rd32(frame_at(img, b)) & 0xFFu;
        if (st == ST_FREE || st == 0xA1u || st == 0xA2u || st == 0xA3u) free_blocks[nfree++] = b;
    }
    if (n > nfree) return -1;
    for (i = 0; i < n; i++) {
        uint8_t *f = frame_at(img, free_blocks[i]);
        int next = (i + 1 < n) ? free_blocks[i + 1] : 0xFFFF;
        if (i == 0) {
            memcpy(f, mcs, FRAME_SIZE);
            wr32(f, ST_FIRST);
            wr32(f + 4, (uint32_t)n * BLOCK_SIZE);
            if (name) {
                memset(f + 0x0A, 0, 21);
                memcpy(f + 0x0A, name, strnlen(name, 20));
            }
        } else {
            memset(f, 0, FRAME_SIZE);
            wr32(f, i + 1 < n ? ST_MIDDLE : ST_LAST);
        }
        f[8] = (uint8_t)(next & 0xFF);
        f[9] = (uint8_t)(next >> 8);
        frame_seal(f);
        memcpy(block_at(img, free_blocks[i]), mcs + FRAME_SIZE + (size_t)i * BLOCK_SIZE, BLOCK_SIZE);
    }
    return 0;
}

static void remove_file(uint8_t *img, int first) {
    int chain[CARD_BLOCKS], n = file_chain(img, first, chain), i;
    for (i = 0; i < n; i++) {
        uint8_t *f = frame_at(img, chain[i]);
        memset(f, 0, FRAME_SIZE);
        wr32(f, ST_FREE);
        f[8] = 0xFF; f[9] = 0xFF;
        frame_seal(f);
    }
}

static int find_file(uint8_t *img, const char *name) {
    int b;
    for (b = 0; b < CARD_BLOCKS; b++) {
        char nm[21];
        if ((rd32(frame_at(img, b)) & 0xFFu) != ST_FIRST) continue;
        frame_name(frame_at(img, b), nm);
        if (strcmp(nm, name) == 0) return b;
    }
    return -1;
}

/* ---- details file ------------------------------------------------------- */

/* Text of a details file, built in memory and written in one step. */
typedef struct {
    char   buf[8192];
    size_t n;
} Text;

static void text_add(Text *t, const char *fmt, ...) {
    va_list ap;
    int w;
    if (t->n >= sizeof(t->buf)) return;
    va_start(ap, fmt);
    w = vsnprintf(t->buf + t->n, sizeof(t->buf) - t->n, fmt, ap);
    va_end(ap);
    if (w > 0) t->n += ((size_t)w < sizeof(t->buf) - t->n) ? (size_t)w : sizeof(t->buf) - t->n - 1;
}

static void put_str(Text *t, const char *key, const char *v) {
    text_add(t, "%s=", key);
    for (; *v; v++) {
        if (*v == '\\') text_add(t, "\\\\");
        else if (*v == '\n') text_add(t, "\\n");
        else if (*v == '\r') continue;
        else text_add(t, "%c", *v);
    }
    text_add(t, "\n");
}

static void get_str(char *dst, size_t cap, const char *v) {
    size_t o = 0;
    for (; *v && o + 1 < cap; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            dst[o++] = (*v == 'n') ? '\n' : *v;
        } else {
            dst[o++] = *v;
        }
    }
    dst[o] = '\0';
}

static int write_info(int slot, const PSXSaveSlotInfo *in) {
    static Text t;
    char path[sizeof(s_dir) + 32];
    const PSXSaveSlotDetails *d = &in->details;
    int i;
    t.n = 0;
    text_add(&t, "format=1\n");
    put_str(&t, "name", in->name);
    text_add(&t, "saved=%lld\n", (long long)in->saved_time);
    if (d->fields & PSX_SAVE_FIELD_PLAYTIME) text_add(&t, "playtime=%lld\n", (long long)d->playtime_seconds);
    if (d->fields & PSX_SAVE_FIELD_MONEY) text_add(&t, "money=%lld\n", (long long)d->money);
    if (d->fields & PSX_SAVE_FIELD_CHAPTER) {
        text_add(&t, "chapter=%d\n", (int)d->chapter);
        put_str(&t, "chapter_label", d->chapter_label);
    }
    if (d->fields & PSX_SAVE_FIELD_LOCATION) put_str(&t, "location", d->location);
    if (d->fields & PSX_SAVE_FIELD_PARTY) {
        text_add(&t, "party=");
        for (i = 0; i < d->party_count && i < PSX_SAVE_SLOT_PARTY_MAX; i++)
            text_add(&t, "%s%d:%d", i ? "," : "", (int)d->party[i].id, (int)d->party[i].level);
        text_add(&t, "\n");
    }
    if (d->fields & PSX_SAVE_FIELD_DIFFICULTY) put_str(&t, "difficulty", d->difficulty);
    if (d->fields & PSX_SAVE_FIELD_ACHIEVEMENTS) {
        text_add(&t, "achievements=%d/%d\n", (int)d->achievements_unlocked, (int)d->achievements_total);
        text_add(&t, "achievement_mode=%s\n",
                 d->achievement_mode == PSX_SAVE_ACH_HARDCORE ? "hardcore"
                 : d->achievement_mode == PSX_SAVE_ACH_SOFTCORE ? "softcore" : "off");
    }
    for (i = 0; i < d->extra_count && i < PSX_SAVE_SLOT_EXTRA_MAX; i++) {
        char key[40];
        snprintf(key, sizeof(key), "x.%s", d->extra[i].key);
        put_str(&t, key, d->extra[i].value);
    }
    slot_path(path, sizeof(path), slot, "info");
    return write_atomic(path, t.buf, t.n);
}

static void parse_info(const char *text, PSXSaveSlotInfo *out) {
    PSXSaveSlotDetails *d = &out->details;
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        char buf[256], *eq, *v;
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = '\0';
        if (n && buf[n - 1] == '\r') buf[n - 1] = '\0';
        line = end ? end + 1 : line + strlen(line);
        eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = '\0';
        v = eq + 1;
        if (!strcmp(buf, "name")) get_str(out->name, sizeof(out->name), v);
        else if (!strcmp(buf, "saved")) out->saved_time = strtoll(v, NULL, 10);
        else if (!strcmp(buf, "playtime")) { d->playtime_seconds = strtoll(v, NULL, 10); d->fields |= PSX_SAVE_FIELD_PLAYTIME; }
        else if (!strcmp(buf, "money")) { d->money = strtoll(v, NULL, 10); d->fields |= PSX_SAVE_FIELD_MONEY; }
        else if (!strcmp(buf, "chapter")) { d->chapter = (int32_t)strtol(v, NULL, 10); d->fields |= PSX_SAVE_FIELD_CHAPTER; }
        else if (!strcmp(buf, "chapter_label")) get_str(d->chapter_label, sizeof(d->chapter_label), v);
        else if (!strcmp(buf, "location")) { get_str(d->location, sizeof(d->location), v); d->fields |= PSX_SAVE_FIELD_LOCATION; }
        else if (!strcmp(buf, "party")) {
            char *p = v;
            d->fields |= PSX_SAVE_FIELD_PARTY;
            d->party_count = 0;
            while (*p && d->party_count < PSX_SAVE_SLOT_PARTY_MAX) {
                char *colon;
                long id = strtol(p, &colon, 10);
                if (*colon != ':') break;
                d->party[d->party_count].id = (int32_t)id;
                d->party[d->party_count].level = (int32_t)strtol(colon + 1, &p, 10);
                d->party_count++;
                if (*p == ',') p++;
            }
        }
        else if (!strcmp(buf, "difficulty")) { get_str(d->difficulty, sizeof(d->difficulty), v); d->fields |= PSX_SAVE_FIELD_DIFFICULTY; }
        else if (!strcmp(buf, "achievements")) {
            char *slash;
            d->achievements_unlocked = (int32_t)strtol(v, &slash, 10);
            d->achievements_total = (*slash == '/') ? (int32_t)strtol(slash + 1, NULL, 10) : 0;
            d->fields |= PSX_SAVE_FIELD_ACHIEVEMENTS;
        }
        else if (!strcmp(buf, "achievement_mode"))
            d->achievement_mode = !strcmp(v, "hardcore") ? PSX_SAVE_ACH_HARDCORE
                                : !strcmp(v, "softcore") ? PSX_SAVE_ACH_SOFTCORE : PSX_SAVE_ACH_OFF;
        else if (!strncmp(buf, "x.", 2) && d->extra_count < PSX_SAVE_SLOT_EXTRA_MAX) {
            PSXSaveSlotExtra *x = &d->extra[d->extra_count++];
            snprintf(x->key, sizeof(x->key), "%s", buf + 2);
            get_str(x->value, sizeof(x->value), v);
        }
    }
}

/* ---- cache -------------------------------------------------------------- */

/* Fill a cache entry from a .mcs buffer (header and icon). */
static void cache_from_mcs(SlotCache *c, const uint8_t *mcs, size_t len) {
    const uint8_t *hdr = mcs + FRAME_SIZE;
    int k;
    c->info.used = 1;
    frame_name(mcs, c->info.file_name);
    c->info.blocks = (int32_t)((len - FRAME_SIZE) / BLOCK_SIZE);
    memcpy(c->header, hdr, FRAME_SIZE);
    memcpy(c->info.title_sjis, hdr + 4, 64);
    c->info.title_sjis[63] = 0;
    c->info.icon_frames = (hdr[0] == 'S' && hdr[1] == 'C' && hdr[2] >= 0x11 && hdr[2] <= 0x13) ? hdr[2] - 0x10 : 0;
    for (k = 0; k < c->info.icon_frames; k++)
        memcpy(c->icon[k], hdr + 0x80 + (size_t)k * FRAME_SIZE, FRAME_SIZE);
}

static void cache_load(int slot) {
    SlotCache *c = &s_slots[slot - 1];
    char path[sizeof(s_dir) + 32];
    size_t len = 0;
    uint8_t *mcs, *info;
    memset(c, 0, sizeof(*c));
    slot_path(path, sizeof(path), slot, "mcs");
    mcs = read_all(path, &len);
    if (!mcs) return;
    if (len >= FRAME_SIZE + BLOCK_SIZE && (len - FRAME_SIZE) % BLOCK_SIZE == 0)
        cache_from_mcs(c, mcs, len);
    free(mcs);
    if (!c->info.used) return;
    slot_path(path, sizeof(path), slot, "info");
    info = read_all(path, &len);
    if (info) {
        parse_info((const char *)info, &c->info);
        free(info);
    }
}

static int slot_ok(int slot) { return s_slots && slot >= 1 && slot <= s_count; }

/* ---- API ---------------------------------------------------------------- */

int psx_save_slots_open(int count, const char *dir) {
    SlotCache *slots;
    int i;
    if (count < 1 || count > PSX_SAVE_SLOTS_MAX) return -1;
    if (dir && dir[0]) snprintf(s_dir, sizeof(s_dir), "%s", dir);
    else if (memcard_dir()[0]) snprintf(s_dir, sizeof(s_dir), "%s/slots", memcard_dir());
    else return -1;
    make_dir(s_dir);
    slots = (SlotCache *)calloc((size_t)count, sizeof(SlotCache));
    if (!slots) return -1;
    lock();
    free(s_slots);
    s_slots = slots;
    s_count = count;
    for (i = 1; i <= count; i++) cache_load(i);
    unlock();
    return 0;
}

int psx_save_slots_count(void) { return s_count; }

int psx_save_slots_get(int slot, PSXSaveSlotInfo *out) {
    if (!out) return -1;
    lock();
    if (!slot_ok(slot)) { unlock(); return -1; }
    *out = s_slots[slot - 1].info;
    unlock();
    return 0;
}

int psx_save_slots_icon(int slot, int frame, uint8_t *rgba) {
    int i, frames;
    uint8_t clut[16][4];
    if (!rgba) return -1;
    lock();
    if (!slot_ok(slot) || !s_slots[slot - 1].info.used) { unlock(); return -1; }
    frames = s_slots[slot - 1].info.icon_frames;
    if (frame < 0 || frame >= frames) { unlock(); return -1; }
    for (i = 0; i < 16; i++) {
        const uint8_t *e = s_slots[slot - 1].header + 0x60 + i * 2;
        unsigned c = (unsigned)e[0] | ((unsigned)e[1] << 8);
        clut[i][0] = (uint8_t)(((c >> 0) & 31) * 255 / 31);
        clut[i][1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
        clut[i][2] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
        clut[i][3] = (uint8_t)(c == 0 ? 0 : 255);   /* 0x0000 draws as transparent */
    }
    for (i = 0; i < 256; i++) {
        uint8_t byte = s_slots[slot - 1].icon[frame][i >> 1];
        int idx = (i & 1) ? (byte >> 4) : (byte & 15);
        memcpy(rgba + i * 4, clut[idx], 4);
    }
    unlock();
    return frames;
}

int psx_save_slots_keep_files(const char *prefix) {
    int i;
    if (!prefix || !prefix[0]) return -1;
    for (i = 0; i < s_keep_count; i++)
        if (strncmp(s_keep[i], prefix, 20) == 0) return 0;
    if (s_keep_count >= KEEP_MAX) return -1;
    snprintf(s_keep[s_keep_count++], sizeof(s_keep[0]), "%s", prefix);
    return 0;
}

int psx_save_slots_mount(int card, int slot, const char *as_name) {
    static uint8_t img[MEMCARD_SIZE];
    int b;
    if (card < 0 || card > 1 || s_mount_card >= 0) return -1;
    if (slot != 0 && !slot_ok(slot)) return -1;
    if (sio_card_protocol_active()) return -3;
    if (memcard_export_raw(card, s_rest) != 0) return -2;

    memcard_format_image(img);
    for (b = 0; b < CARD_BLOCKS; b++) {
        char nm[21];
        size_t len = 0;
        uint8_t *mcs;
        if ((rd32(frame_at(s_rest, b)) & 0xFFu) != ST_FIRST) continue;
        frame_name(frame_at(s_rest, b), nm);
        if (!is_kept(nm)) continue;
        mcs = extract_file(s_rest, b, &len);
        if (mcs) {
            (void)place_file(img, mcs, len, NULL);
            free(mcs);
        }
    }
    if (slot > 0) {
        char path[sizeof(s_dir) + 32];
        size_t len = 0;
        uint8_t *mcs;
        int rc;
        lock();
        slot_path(path, sizeof(path), slot, "mcs");
        mcs = s_slots[slot - 1].info.used ? read_all(path, &len) : NULL;
        unlock();
        if (s_slots[slot - 1].info.used) {
            if (!mcs) return -4;
            rc = place_file(img, mcs, len, as_name && as_name[0] ? as_name : NULL);
            free(mcs);
            if (rc != 0) return -4;
        }
    }
    memcpy(s_mounted, img, MEMCARD_SIZE);
    if (memcard_mount_image(card, img) != 0) return -2;
    if (sio_memcard_reinsert(card) != 0) {
        (void)memcard_unmount_image(card, s_rest);
        return -3;
    }
    s_mount_card = card;
    s_mount_slot = slot;
    s_rest_changed = 0;
    return 0;
}

int psx_save_slots_commit(int slot, const PSXSaveSlotDetails *details) {
    static uint8_t img[MEMCARD_SIZE];
    char path[sizeof(s_dir) + 32];
    PSXSaveSlotInfo info;
    int b, chosen = -1, chosen_changed = 0;
    uint8_t *mcs;
    size_t len = 0;

    if (s_mount_card < 0 || !slot_ok(slot)) return -1;
    if (memcard_export_raw(s_mount_card, img) != 0) return -1;

    for (b = 0; b < CARD_BLOCKS; b++) {
        char nm[21];
        if ((rd32(frame_at(img, b)) & 0xFFu) != ST_FIRST) continue;
        frame_name(frame_at(img, b), nm);
        if (is_kept(nm)) {
            /* A kept file the game rewrote goes back to the player's card,
             * swapped on a copy: when the new one does not fit, the old stays. */
            static uint8_t rest[MEMCARD_SIZE];
            int old = find_file(s_rest, nm);
            uint8_t *a = extract_file(img, b, &len), *o = NULL;
            size_t olen = 0;
            if (old >= 0) o = extract_file(s_rest, old, &olen);
            if (a && (!o || olen != len || memcmp(a, o, len) != 0)) {
                memcpy(rest, s_rest, MEMCARD_SIZE);
                if (old >= 0) remove_file(rest, old);
                if (place_file(rest, a, len, NULL) == 0) {
                    memcpy(s_rest, rest, MEMCARD_SIZE);
                    s_rest_changed = 1;
                }
            }
            free(a);
            free(o);
            continue;
        }
        /* The save the game wrote: a file that is new or changed since the
         * mount; the first other file when the game rewrote nothing new. */
        {
            int was = find_file(s_mounted, nm);
            int changed = 1;
            if (was >= 0) {
                size_t l1 = 0, l2 = 0;
                uint8_t *x = extract_file(img, b, &l1), *y = extract_file(s_mounted, was, &l2);
                changed = !(x && y && l1 == l2 && memcmp(x, y, l1) == 0);
                free(x);
                free(y);
            }
            if (changed && (chosen < 0 || !chosen_changed)) { chosen = b; chosen_changed = 1; }
            else if (chosen < 0) chosen = b;
        }
    }
    if (chosen < 0) return -2;

    mcs = extract_file(img, chosen, &len);
    if (!mcs) return -2;
    slot_path(path, sizeof(path), slot, "mcs");
    if (write_atomic(path, mcs, len) != 0) { free(mcs); return -3; }

    lock();
    info = s_slots[slot - 1].info;          /* keeps the player's name */
    unlock();
    {
        SlotCache c;
        memset(&c, 0, sizeof(c));
        cache_from_mcs(&c, mcs, len);
        snprintf(c.info.name, sizeof(c.info.name), "%s", info.used ? info.name : "");
        c.info.saved_time = (int64_t)time(NULL);
        if (details) c.info.details = *details;
        free(mcs);
        if (write_info(slot, &c.info) != 0) return -3;
        lock();
        s_slots[slot - 1] = c;
        unlock();
    }
    return 0;
}

int psx_save_slots_unmount(void) {
    int card = s_mount_card;
    if (card < 0) return -1;
    if (sio_card_protocol_active()) return -3;
    if (memcard_unmount_image(card, s_rest) != 0) return -1;
    if (s_rest_changed) (void)memcard_import_raw(card, s_rest);
    (void)sio_memcard_reinsert(card);
    s_mount_card = -1;
    s_mount_slot = -1;
    s_rest_changed = 0;
    return 0;
}

int psx_save_slots_mounted(void) { return s_mount_card >= 0 ? s_mount_slot : -1; }

int psx_save_slots_rename(int slot, const char *name) {
    PSXSaveSlotInfo info;
    lock();
    if (!slot_ok(slot) || !s_slots[slot - 1].info.used) { unlock(); return -1; }
    info = s_slots[slot - 1].info;
    unlock();
    snprintf(info.name, sizeof(info.name), "%s", name ? name : "");
    if (write_info(slot, &info) != 0) return -3;
    lock();
    s_slots[slot - 1].info = info;
    unlock();
    return 0;
}

int psx_save_slots_delete(int slot) {
    char a[sizeof(s_dir) + 32], b[sizeof(s_dir) + 32];
    if (!slot_ok(slot)) return -1;
    slot_path(a, sizeof(a), slot, "mcs");
    slot_path(b, sizeof(b), slot, "info");
    /* The save first: an orphan details file is harmless and ignored. */
    if (remove(a) != 0 && s_slots[slot - 1].info.used) return -3;
    (void)remove(b);
    lock();
    memset(&s_slots[slot - 1], 0, sizeof(SlotCache));
    unlock();
    return 0;
}

int psx_save_slots_copy(int from, int to) {
    char path[sizeof(s_dir) + 32];
    PSXSaveSlotInfo info;
    size_t len = 0;
    uint8_t *mcs;
    SlotCache c;
    if (!slot_ok(from) || !slot_ok(to) || from == to) return -1;
    lock();
    info = s_slots[from - 1].info;
    unlock();
    if (!info.used) return -1;
    slot_path(path, sizeof(path), from, "mcs");
    mcs = read_all(path, &len);
    if (!mcs) return -1;
    slot_path(path, sizeof(path), to, "mcs");
    if (write_atomic(path, mcs, len) != 0) { free(mcs); return -3; }
    memset(&c, 0, sizeof(c));
    cache_from_mcs(&c, mcs, len);
    free(mcs);
    snprintf(c.info.name, sizeof(c.info.name), "%s", info.name);
    c.info.saved_time = info.saved_time;
    c.info.details = info.details;
    if (write_info(to, &c.info) != 0) return -3;
    lock();
    s_slots[to - 1] = c;
    unlock();
    return 0;
}

int psx_save_slots_import_card(const char *mcd_path, const char *prefix) {
    size_t len = 0;
    uint8_t *card;
    int b, slot = 1, imported = 0;
    (void)memcard_flush_wait(10000);    /* the player's card may be queued */
    card = read_all(mcd_path, &len);
    if (!card) return -1;
    if (len != MEMCARD_SIZE || card[0] != 'M' || card[1] != 'C') { free(card); return -1; }
    for (b = 0; b < CARD_BLOCKS; b++) {
        char nm[21], path[sizeof(s_dir) + 32];
        size_t mlen = 0;
        uint8_t *mcs;
        SlotCache c;
        if ((rd32(frame_at(card, b)) & 0xFFu) != ST_FIRST) continue;
        frame_name(frame_at(card, b), nm);
        if (prefix && strncmp(nm, prefix, strlen(prefix)) != 0) continue;
        if (is_kept(nm)) continue;
        while (slot <= s_count && s_slots[slot - 1].info.used) slot++;
        if (slot > s_count) break;
        mcs = extract_file(card, b, &mlen);
        if (!mcs) continue;
        slot_path(path, sizeof(path), slot, "mcs");
        if (write_atomic(path, mcs, mlen) != 0) { free(mcs); continue; }
        memset(&c, 0, sizeof(c));
        cache_from_mcs(&c, mcs, mlen);
        free(mcs);
        c.info.saved_time = (int64_t)time(NULL);
        if (write_info(slot, &c.info) != 0) continue;
        lock();
        s_slots[slot - 1] = c;
        unlock();
        imported++;
    }
    free(card);
    return imported;
}
