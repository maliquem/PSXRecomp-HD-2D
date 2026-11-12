/*
 * memcard.c -- PS1 memory card image I/O
 *
 * 128KB per card, organized as 1024 sectors of 128 bytes each.
 * Stored as raw .mcd files on disk.
 *
 * Pure hardware simulation. No BIOS state, no HLE, no stubs.
 *
 * Ported from v3 with audit:
 *   - Removed all fprintf (CLAUDE.md rule #3)
 *   - No BIOS manipulation found (clean)
 *   - No fake events found (clean)
 */

#include "memcard.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define MAX_CARDS 2

typedef struct {
    uint8_t data[MEMCARD_SIZE];
    char filepath[512];
    int present;
    int dirty;
    /* A mounted image (save slots) lives in RAM only: flushes never reach
     * the bound file, which keeps the player's own card until unmount. */
    int memory_only;
} MemCard;

static MemCard cards[MAX_CARDS];
static char s_memcard_dir[512];

/* Write a whole card image to `path` so a crash or a full disk never leaves a
 * cut file: write a sibling temporary, sync it, then replace the target in
 * one step. The old file stays whole until the replacement lands. */
static int memcard_write_file_atomic(const char *path, const uint8_t *data) {
    char tmp[sizeof(((MemCard *)0)->filepath) + 8];
    FILE *f;
    size_t n;
    int ok;
    if (!path || !path[0]) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -1;
    n = fwrite(data, 1, MEMCARD_SIZE, f);
    ok = (n == MEMCARD_SIZE) && fflush(f) == 0;
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

/* ---- background writer -------------------------------------------------
 * The game writes a card one 128-byte sector at a time and each write asks
 * for a flush. A synced, atomic write of the whole 128 KB costs milliseconds,
 * so it does not run on the emulation thread: memcard_flush only copies the
 * image into a per-card pending slot, and one writer thread puts the newest
 * pending image on disk. Writes in a burst collapse into the last one. A
 * failed write stays pending and is tried again. */

typedef struct {
    uint8_t data[MEMCARD_SIZE];
    char    path[sizeof(((MemCard *)0)->filepath)];
    int     pending;
} PendingWrite;

static PendingWrite s_pending[MAX_CARDS];
static volatile int s_write_failing;   /* the last background write failed */
static int s_writer_busy;
static int s_writer_started;

#ifdef _WIN32
static SRWLOCK s_wlock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_wwake = CONDITION_VARIABLE_INIT;
static CONDITION_VARIABLE s_widle = CONDITION_VARIABLE_INIT;
static void wlock(void)   { AcquireSRWLockExclusive(&s_wlock); }
static void wunlock(void) { ReleaseSRWLockExclusive(&s_wlock); }
static void wsignal(CONDITION_VARIABLE *cv) { WakeAllConditionVariable(cv); }
/* 0 when woken, -1 on timeout. */
static int wwait(CONDITION_VARIABLE *cv, unsigned ms) {
    return SleepConditionVariableSRW(cv, &s_wlock, ms == 0xFFFFFFFFu ? INFINITE : ms, 0) ? 0 : -1;
}
static void wsleep(unsigned ms) { Sleep(ms); }
#else
static pthread_mutex_t s_wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wwake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t s_widle = PTHREAD_COND_INITIALIZER;
static void wlock(void)   { pthread_mutex_lock(&s_wlock); }
static void wunlock(void) { pthread_mutex_unlock(&s_wlock); }
static void wsignal(pthread_cond_t *cv) { pthread_cond_broadcast(cv); }
static int wwait(pthread_cond_t *cv, unsigned ms) {
    struct timespec ts;
    if (ms == 0xFFFFFFFFu) return pthread_cond_wait(cv, &s_wlock) == 0 ? 0 : -1;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, &s_wlock, &ts) == 0 ? 0 : -1;
}
static void wsleep(unsigned ms) { usleep(ms * 1000u); }
#endif

static void writer_loop(void) {
    static PendingWrite job;
    for (;;) {
        int i, card = -1, rc;
        wlock();
        for (;;) {
            for (i = 0; i < MAX_CARDS; i++)
                if (s_pending[i].pending) { card = i; break; }
            if (card >= 0) break;
            (void)wwait(&s_wwake, 0xFFFFFFFFu);
        }
        memcpy(&job, &s_pending[card], sizeof(job));
        s_pending[card].pending = 0;
        s_writer_busy = 1;
        wunlock();

        rc = memcard_write_file_atomic(job.path, job.data);

        wlock();
        s_writer_busy = 0;
        s_write_failing = rc != 0;
        if (rc != 0 && !s_pending[card].pending) {
            memcpy(&s_pending[card], &job, sizeof(job));   /* try again */
            s_pending[card].pending = 1;
        }
        wsignal(&s_widle);
        wunlock();
        if (rc != 0) wsleep(250);
    }
}

#ifdef _WIN32
static DWORD WINAPI writer_main(LPVOID arg) { (void)arg; writer_loop(); return 0; }
#else
static void *writer_main(void *arg) { (void)arg; writer_loop(); return NULL; }
#endif

/* Called with s_wlock held. */
static void writer_start_locked(void) {
    if (s_writer_started) return;
#ifdef _WIN32
    {
        HANDLE h = CreateThread(NULL, 0, writer_main, NULL, 0, NULL);
        if (!h) return;
        CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, writer_main, NULL) != 0) return;
        pthread_detach(t);
    }
#endif
    s_writer_started = 1;
}

/* Hand the card's current image to the writer. 0 = queued; -1 = no writer
 * thread could start (the caller then writes it itself). */
static int writer_enqueue(int card) {
    int started;
    wlock();
    writer_start_locked();
    started = s_writer_started;
    if (started) {
        memcpy(s_pending[card].data, cards[card].data, MEMCARD_SIZE);
        snprintf(s_pending[card].path, sizeof(s_pending[card].path), "%s", cards[card].filepath);
        s_pending[card].pending = 1;
        wsignal(&s_wwake);
    }
    wunlock();
    return started ? 0 : -1;
}

int memcard_write_failing(void) { return s_write_failing; }

int memcard_flush_wait(unsigned timeout_ms) {
    int drained;
    wlock();
    for (;;) {
        int i, busy = s_writer_busy;
        for (i = 0; i < MAX_CARDS; i++) busy |= s_pending[i].pending;
        if (!busy || !s_writer_started) { drained = !busy; break; }
        if (wwait(&s_widle, timeout_ms) != 0) {
            busy = s_writer_busy;
            for (i = 0; i < MAX_CARDS; i++) busy |= s_pending[i].pending;
            drained = !busy;
            break;
        }
    }
    wunlock();
    return drained ? 0 : -1;
}

/* XOR checksum of bytes 0x00-0x7E, stored at 0x7F. */
static uint8_t frame_checksum(const uint8_t *frame) {
    uint8_t xor_val = 0;
    for (int i = 0; i < 127; i++)
        xor_val ^= frame[i];
    return xor_val;
}

/* Format a 128KB card image to match a real PS1 blank card.
 * Layout verified against DuckStation's MemoryCardImage::Format(). */
static void memcard_format(uint8_t *data) {
    memset(data, 0xFF, MEMCARD_SIZE);

    /* Frame 0: Header ("MC" magic) */
    memset(&data[0], 0x00, 128);
    data[0] = 'M';
    data[1] = 'C';
    data[0x7F] = frame_checksum(&data[0]);

    /* Frames 1-15: Directory entries (all free) */
    for (int s = 1; s <= 15; s++) {
        int off = s * 128;
        memset(&data[off], 0x00, 128);
        data[off + 0] = 0xA0;  /* status: free/available */
        data[off + 8] = 0xFF;  /* next block pointer = none */
        data[off + 9] = 0xFF;
        data[off + 0x7F] = frame_checksum(&data[off]);
    }

    /* Frames 16-35: Broken sector list (no broken sectors) */
    for (int s = 16; s <= 35; s++) {
        int off = s * 128;
        memset(&data[off], 0x00, 128);
        data[off + 0] = 0xFF;
        data[off + 1] = 0xFF;
        data[off + 2] = 0xFF;
        data[off + 3] = 0xFF;
        data[off + 8] = 0xFF;
        data[off + 9] = 0xFF;
        data[off + 0x7F] = frame_checksum(&data[off]);
    }

    /* Frames 36-62: Broken sector replacement data + unused */
    for (int s = 36; s <= 62; s++) {
        memset(&data[s * 128], 0x00, 128);
    }

    /* Frame 63: Write test frame (copy of frame 0) */
    memcpy(&data[63 * 128], &data[0], 128);
}

/* mkdir -p: create each path component (guest sandbox is <memcard>/netplay). */
static void memcard_ensure_dir(const char* dir) {
    char tmp[512];
    size_t len;
    size_t i;
    if (!dir || !dir[0]) return;
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[--len] = '\0';
    }
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
#ifdef _WIN32
            if (i == 2 && tmp[1] == ':')
                continue;
#endif
            tmp[i] = '\0';
#ifdef _WIN32
            (void)_mkdir(tmp);
#else
            (void)mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    (void)_mkdir(tmp);
#else
    (void)mkdir(tmp, 0755);
#endif
}

void memcard_init_slots(const char* dir, const MemcardSlotConfig slots[2]) {
    /* A reboot re-reads the files: writes still queued land first. */
    (void)memcard_flush_wait(10000);
    memset(cards, 0, sizeof(cards));
    snprintf(s_memcard_dir, sizeof(s_memcard_dir), "%s", dir ? dir : "");
    memcard_ensure_dir(dir);

    for (int i = 0; i < MAX_CARDS; i++) {
        cards[i].present = 0;
        cards[i].dirty = 0;

        const int enabled = slots ? slots[i].enabled : 1;
        const char* path  = slots ? slots[i].path : NULL;
        if (!enabled) continue;  /* slot empty: no card inserted */

        if (path && path[0]) {
            snprintf(cards[i].filepath, sizeof(cards[i].filepath), "%s", path);
        } else if (dir) {
            snprintf(cards[i].filepath, sizeof(cards[i].filepath),
                     "%s/card%d.mcd", dir, i + 1);
        } else {
            continue;  /* no path and no dir: cannot resolve a file */
        }

        FILE* f = fopen(cards[i].filepath, "rb");
        if (f) {
            size_t n = fread(cards[i].data, 1, MEMCARD_SIZE, f);
            fclose(f);
            if (n == MEMCARD_SIZE) {
                cards[i].present = 1;
            }
        } else {
            memcard_format(cards[i].data);
            if (memcard_write_file_atomic(cards[i].filepath, cards[i].data) == 0) {
                cards[i].present = 1;
            }
        }
    }
}

void memcard_init(const char* dir) {
    const MemcardSlotConfig both[2] = { { NULL, 1 }, { NULL, 1 } };
    memcard_init_slots(dir, both);
}

int memcard_format_file(const char* path) {
    if (!path || !path[0]) return -1;
    uint8_t* data = (uint8_t*)malloc(MEMCARD_SIZE);
    if (!data) return -1;
    memcard_format(data);
    int ok = memcard_write_file_atomic(path, data) == 0;
    free(data);
    return ok ? 0 : -1;
}

int memcard_summary_path(const char* path, MemcardSummary* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->total_blocks = 15;
    if (!path || !path[0]) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    out->exists = 1;

    uint8_t* data = (uint8_t*)malloc(MEMCARD_SIZE);
    if (!data) { fclose(f); return 0; }
    size_t n = fread(data, 1, MEMCARD_SIZE, f);
    fclose(f);
    out->size_bytes = (long long)n;

    if (n == MEMCARD_SIZE && data[0] == 'M' && data[1] == 'C') {
        out->valid = 1;
        /* Directory frames 1..15: byte 0 is the block-allocation status.
         * 0xA0 = free; 0x51/0x52/0x53 = occupied (first/middle/last). */
        for (int s = 1; s <= 15; s++) {
            const uint8_t status = data[s * 128];
            const int used = ((status & 0xF0) == 0x50);
            out->block_used[s - 1] = (uint8_t)(used ? 1 : 0);
            if (used) out->used_blocks++;
        }
    }
    free(data);

#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(path, &st) == 0) out->mtime = (long long)st.st_mtime;
#else
    struct stat st;
    if (stat(path, &st) == 0) out->mtime = (long long)st.st_mtime;
#endif
    return 0;
}

int memcard_read_sector(int card, int sector, uint8_t* buf) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (!cards[card].present) return -1;
    if (sector < 0 || sector >= MEMCARD_SECTORS) return -1;

    memcpy(buf, cards[card].data + sector * MEMCARD_SECTOR_SIZE, MEMCARD_SECTOR_SIZE);
    return 0;
}

int memcard_write_sector(int card, int sector, const uint8_t* buf) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (!cards[card].present) return -1;
    if (sector < 0 || sector >= MEMCARD_SECTORS) return -1;

    memcpy(cards[card].data + sector * MEMCARD_SECTOR_SIZE, buf, MEMCARD_SECTOR_SIZE);
    cards[card].dirty = 1;
    return 0;
}

void memcard_flush(int card) {
    if (card < 0 || card >= MAX_CARDS) return;
    if (!cards[card].dirty) return;
    if (cards[card].memory_only) return;
    if (cards[card].filepath[0] == '\0') return;

    if (writer_enqueue(card) == 0 ||
        memcard_write_file_atomic(cards[card].filepath, cards[card].data) == 0) {
        cards[card].dirty = 0;
    }
}

void memcard_flush_all(void) {
    for (int i = 0; i < MAX_CARDS; i++) {
        memcard_flush(i);
    }
    (void)memcard_flush_wait(10000);
}

int memcard_is_present(int card) {
    if (card < 0 || card >= MAX_CARDS) return 0;
    return cards[card].present;
}

int memcard_debug_info(int card, const char **path_out,
                       uint8_t magic_out[2], int *present_out,
                       int *dirty_out) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (path_out)    *path_out    = cards[card].filepath;
    if (magic_out)   { magic_out[0] = cards[card].data[0];
                       magic_out[1] = cards[card].data[1]; }
    if (present_out) *present_out = cards[card].present;
    if (dirty_out)   *dirty_out   = cards[card].dirty;
    return 0;
}

int memcard_debug_read_buffer(int card, uint32_t offset, uint32_t len,
                              uint8_t *dst) {
    if (card < 0 || card >= MAX_CARDS) return 0;
    if (!cards[card].present) return 0;
    if (!dst || len == 0) return 0;
    if (offset >= MEMCARD_SIZE) return 0;
    uint32_t avail = (uint32_t)MEMCARD_SIZE - offset;
    if (len > avail) len = avail;
    memcpy(dst, cards[card].data + offset, len);
    return (int)len;
}

int memcard_export_raw(int card, uint8_t *dst) {
    if (card < 0 || card >= MAX_CARDS || !dst) return -1;
    if (!cards[card].present) return -1;
    memcpy(dst, cards[card].data, MEMCARD_SIZE);
    return 0;
}

int memcard_import_raw(int card, const uint8_t *src) {
    if (card < 0 || card >= MAX_CARDS || !src) return -1;
    if (cards[card].filepath[0] == '\0') return -1;
    memcpy(cards[card].data, src, MEMCARD_SIZE);
    cards[card].present = 1;
    cards[card].dirty = 1;
    memcard_flush(card);
    return 0;
}

int memcard_rebind_dir(const char *dir) {
    int i;
    if (!dir || !dir[0]) return -1;
    memcard_ensure_dir(dir);
    for (i = 0; i < MAX_CARDS; i++) {
        if (!cards[i].present && cards[i].filepath[0] == '\0')
            continue;
        snprintf(cards[i].filepath, sizeof(cards[i].filepath),
                 "%s/card%d.mcd", dir, i + 1);
        /* Force a flush of current RAM into the sandbox path when dirty or
         * when establishing the guest mirror for the first time. */
        cards[i].dirty = 1;
        memcard_flush(i);
    }
    return 0;
}

int memcard_rebind_paths(const char *path0, const char *path1) {
    if (path0 && path0[0]) {
        snprintf(cards[0].filepath, sizeof(cards[0].filepath), "%s", path0);
    }
    if (path1 && path1[0]) {
        snprintf(cards[1].filepath, sizeof(cards[1].filepath), "%s", path1);
    }
    return 0;
}

int memcard_rebind_path(int card, const char *path) {
    if (card < 0 || card >= MAX_CARDS) return -1;
    if (path && path[0]) {
        snprintf(cards[card].filepath, sizeof(cards[card].filepath), "%s", path);
        /* The first flush must not fail on a missing directory (the host's
         * netplay/ dir does not exist until a guest card is bound there). */
        {
            char dir[sizeof(cards[card].filepath)];
            size_t n;
            snprintf(dir, sizeof(dir), "%s", path);
            n = strlen(dir);
            while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\') --n;
            if (n > 1) {
                dir[n - 1] = '\0';
                memcard_ensure_dir(dir);
            }
        }
    } else {
        cards[card].filepath[0] = '\0';
        cards[card].present = 0;
        cards[card].dirty = 0;
    }
    return 0;
}

int memcard_reload_bound(void) {
    int i;
    (void)memcard_flush_wait(10000);
    for (i = 0; i < MAX_CARDS; i++) {
        FILE *f;
        if (cards[i].filepath[0] == '\0') {
            cards[i].present = 0;
            cards[i].dirty = 0;
            continue;
        }
        f = fopen(cards[i].filepath, "rb");
        if (!f) {
            cards[i].present = 0;
            cards[i].dirty = 0;
            continue;
        }
        if (fread(cards[i].data, 1, MEMCARD_SIZE, f) == MEMCARD_SIZE) {
            cards[i].present = 1;
            cards[i].dirty = 0;
        } else {
            cards[i].present = 0;
            cards[i].dirty = 0;
        }
        fclose(f);
    }
    return 0;
}

const char *memcard_dir(void) {
    return s_memcard_dir;
}

int memcard_format_image(uint8_t *dst) {
    if (!dst) return -1;
    memcard_format(dst);
    return 0;
}

int memcard_mount_image(int card, const uint8_t *image) {
    if (card < 0 || card >= MAX_CARDS || !image) return -1;
    if (!cards[card].present || cards[card].memory_only) return -1;
    /* Anything the game wrote to the player's card is on disk first. */
    memcard_flush(card);
    memcpy(cards[card].data, image, MEMCARD_SIZE);
    cards[card].memory_only = 1;
    cards[card].dirty = 0;
    return 0;
}

int memcard_unmount_image(int card, const uint8_t *restore) {
    if (card < 0 || card >= MAX_CARDS || !restore) return -1;
    if (!cards[card].memory_only) return -1;
    memcpy(cards[card].data, restore, MEMCARD_SIZE);
    cards[card].memory_only = 0;
    cards[card].dirty = 0;
    return 0;
}

int memcard_image_mounted(int card) {
    if (card < 0 || card >= MAX_CARDS) return 0;
    return cards[card].memory_only;
}
