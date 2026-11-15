/*
 * test_memcard_lock.c -- MEM-9: two processes never share one card file.
 *
 * A card whose lock file is held elsewhere comes up absent and its file keeps
 * its bytes; once free it mounts and holds the lock itself.
 */
#include "memcard.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static int failures;
static void check(int cond, const char *label) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", label); failures++; }
}

/* Take the same lock another process would hold; NULL-ish (0) if busy. */
#ifdef _WIN32
typedef HANDLE LockH;
static LockH take(const char *lp) {
    HANDLE h = CreateFileA(lp, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : h;
}
static void drop(LockH h) { CloseHandle(h); }
#else
typedef long LockH;
static LockH take(const char *lp) {
    int fd = open(lp, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { close(fd); return 0; }
    return (LockH)fd + 1;
}
static void drop(LockH h) { close((int)(h - 1)); }
#endif

int main(void) {
    const char *card = "memcard_lock_scratch/card.mcd";
    const char *lock = "memcard_lock_scratch/card.mcd.lock";
    MemcardSlotConfig slots[2] = { { card, 1 }, { NULL, 0 } };
    uint8_t buf[MEMCARD_SIZE];
    LockH h;
    FILE *f;

#ifdef _WIN32
    _mkdir("memcard_lock_scratch");
#else
    mkdir("memcard_lock_scratch", 0755);
#endif
    memset(buf, 0x5A, sizeof buf);
    f = fopen(card, "wb");
    check(f && fwrite(buf, 1, sizeof buf, f) == sizeof buf, "seed card");
    if (f) fclose(f);

    h = take(lock);
    check(h != 0, "test takes the lock first");
    memcard_init_slots("memcard_lock_scratch", slots);
    check(!memcard_is_present(0), "locked elsewhere: slot absent");
    f = fopen(card, "rb");
    check(f && fread(buf, 1, 1, f) == 1 && buf[0] == 0x5A, "locked elsewhere: file untouched");
    if (f) fclose(f);
    if (h) drop(h);

    memcard_init_slots("memcard_lock_scratch", slots);
    check(memcard_is_present(0), "free: slot mounts");
    h = take(lock);
    check(h == 0, "mounted card holds the lock");
    if (h) drop(h);

    memcard_init_slots("memcard_lock_scratch", slots);
    check(memcard_is_present(0), "re-init in one process releases its own lock");

    if (failures) return 1;
    puts("memcard lock OK");
    return 0;
}
