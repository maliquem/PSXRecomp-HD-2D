/*
 * test_memcard_file.c -- the player's card file on disk.
 *
 * MEM-1: a card file that exists but cannot be read (read access denied,
 * while replacing it is still allowed) must not be formatted over: the slot
 * comes up absent and the file keeps its bytes. Only a missing file is
 * created blank.
 * MEM-9: the temporary written before the swap is not a fixed "<card>.tmp"
 * shared by every process: a leftover in the way does not block the write.
 */
#include "memcard.h"
#include "psx_write_atomic.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif

static int failures;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static uint8_t card_bytes[MEMCARD_SIZE];

static int write_file(const char *path, const uint8_t *data, size_t n) {
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

static int file_equals(const char *path, const uint8_t *data, size_t n) {
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    FILE *f = fopen(path, "rb");
    size_t got;
    int same;
    if (!f || !buf) { if (f) fclose(f); free(buf); return 0; }
    got = fread(buf, 1, n + 1, f);
    fclose(f);
    same = got == n && memcmp(buf, data, n) == 0;
    free(buf);
    return same;
}

/* Deny reading the file but keep the rights a rename-over needs. */
static int deny_read(const char *path) {
#ifdef _WIN32
    PSECURITY_DESCRIPTOR sd = NULL;
    BOOL ok;
    /* Everyone: DELETE | READ_CONTROL | WRITE_DAC | SYNCHRONIZE | FILE_READ_ATTRIBUTES. */
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;0x170080;;;WD)", SDDL_REVISION_1, &sd, NULL))
        return 0;
    ok = SetFileSecurityA(path, DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd);
    return ok != 0;
#else
    return chmod(path, 0200) == 0;
#endif
}

static void allow_all(const char *path) {
#ifdef _WIN32
    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;FA;;;WD)", SDDL_REVISION_1, &sd, NULL)) {
        (void)SetFileSecurityA(path, DACL_SECURITY_INFORMATION, sd);
        LocalFree(sd);
    }
#else
    (void)chmod(path, 0644);
#endif
}

static void test_unreadable_card_not_formatted(void) {
    const char *path = "memcard_file_scratch/unreadable.mcd";
    MemcardSlotConfig slots[2] = { { NULL, 1 }, { NULL, 0 } };
    FILE *probe;

    slots[0].path = path;
    remove(path);
    check(write_file(path, card_bytes, MEMCARD_SIZE), "unreadable: seed card");
    if (!deny_read(path)) { check(0, "unreadable: deny read"); return; }
    probe = fopen(path, "rb");
    if (probe) {
        /* Running as a user the permission does not bind (root): no case. */
        fclose(probe);
        allow_all(path);
        remove(path);
        printf("SKIP  unreadable card (read permission not enforced here)\n");
        return;
    }

    memcard_init_slots("memcard_file_scratch", slots);
    allow_all(path);
    check(!memcard_is_present(0), "MEM-1: unreadable card comes up absent");
    check(file_equals(path, card_bytes, MEMCARD_SIZE),
          "MEM-1: unreadable card keeps its bytes (not formatted over)");
    remove(path);
}

static void test_missing_card_created(void) {
    const char *path = "memcard_file_scratch/missing.mcd";
    MemcardSlotConfig slots[2] = { { NULL, 1 }, { NULL, 0 } };
    uint8_t blank[MEMCARD_SIZE];

    slots[0].path = path;
    remove(path);
    memcard_init_slots("memcard_file_scratch", slots);
    check(memcard_is_present(0), "missing card is created and present");
    check(memcard_format_image(blank) == 0 && file_equals(path, blank, MEMCARD_SIZE),
          "missing card is written as a formatted image");
    remove(path);
}

static void test_fixed_tmp_name_not_shared(void) {
    const char *path = "memcard_file_scratch/shared.mcd";
    const char *fixed_tmp = "memcard_file_scratch/shared.mcd.tmp";
    uint8_t blank[MEMCARD_SIZE];

    remove(path);
    /* Another writer's temporary under the fixed name, still in the way. */
#ifdef _WIN32
    (void)_mkdir(fixed_tmp);
#else
    (void)mkdir(fixed_tmp, 0755);
#endif
    check(memcard_format_file(path) == 0,
          "MEM-9: a write does not use the fixed <card>.tmp another process holds");
    check(memcard_format_image(blank) == 0 && file_equals(path, blank, MEMCARD_SIZE),
          "MEM-9: card written whole");
#ifdef _WIN32
    (void)_rmdir(fixed_tmp);
#else
    (void)rmdir(fixed_tmp);
#endif
    remove(path);
}

static unsigned long my_pid(void) {
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    n = (fseek(f, 0, SEEK_END) == 0) ? ftell(f) : -1;
    fclose(f);
    return n;
}

static void test_atomic_write_and_rewrite(void) {
    const char *path = "memcard_file_scratch/atomic.mcd";
    char tmp[256];
    uint8_t other[MEMCARD_SIZE];
    FILE *probe;

    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, my_pid());
    for (size_t i = 0; i < MEMCARD_SIZE; i++) other[i] = (uint8_t)(i * 13u + 5u);
    remove(path);

    check(psx_write_file_atomic(path, card_bytes, MEMCARD_SIZE) == 0, "atomic: first write ok");
    check(file_size(path) == (long)MEMCARD_SIZE && MEMCARD_SIZE == 128 * 1024,
          "atomic: file is exactly 128 KiB");
    check(file_equals(path, card_bytes, MEMCARD_SIZE), "atomic: content exact");

    check(psx_write_file_atomic(path, other, MEMCARD_SIZE) == 0, "atomic: rewrite ok");
    check(file_equals(path, other, MEMCARD_SIZE), "atomic: rewrite replaced the content");
    probe = fopen(tmp, "rb");
    check(probe == NULL, "atomic: no temporary left behind");
    if (probe) { fclose(probe); remove(tmp); }
    remove(path);
}

static void test_atomic_write_failure_keeps_old(void) {
    const char *path = "memcard_file_scratch/keep.mcd";
    const char *nodir = "memcard_file_scratch/no_such_dir_xyz/card.mcd";
    char tmp[256];
    uint8_t other[MEMCARD_SIZE];

    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, my_pid());
    memset(other, 0x5A, sizeof other);
    remove(path);

    check(psx_write_file_atomic(nodir, other, MEMCARD_SIZE) == -1,
          "failure: destination in a missing folder returns -1");

    check(write_file(path, card_bytes, MEMCARD_SIZE), "failure: seed card");
    /* A directory squatting on the temporary's name makes its open fail. */
#ifdef _WIN32
    (void)_mkdir(tmp);
#else
    (void)mkdir(tmp, 0755);
#endif
    check(psx_write_file_atomic(path, other, MEMCARD_SIZE) == -1,
          "failure: temporary cannot be opened returns -1");
    check(file_equals(path, card_bytes, MEMCARD_SIZE),
          "failure: the previous card file is untouched");
#ifdef _WIN32
    (void)_rmdir(tmp);
#else
    (void)rmdir(tmp);
#endif
    remove(path);
}

/* memcard.c documents: only a missing file is created blank; a file that
 * exists but cannot be read whole leaves the slot absent with its bytes kept. */
static void test_wrong_size_card_left_alone(void) {
    const char *path = "memcard_file_scratch/short.mcd";
    MemcardSlotConfig slots[2] = { { NULL, 1 }, { NULL, 0 } };
    static const size_t sizes[2] = { 1000, 0 };

    slots[0].path = path;
    for (int k = 0; k < 2; k++) {
        remove(path);
        check(write_file(path, card_bytes, sizes[k]), "short: seed card");
        memcard_init_slots("memcard_file_scratch", slots);
        check(!memcard_is_present(0), "short: wrong-size card comes up absent");
        check(file_equals(path, card_bytes, sizes[k]),
              "short: wrong-size card keeps its bytes (not formatted over)");
    }
    remove(path);
}

/* MEM-O3: a burst of sector writes+flushes (most merge into one queued image
 * without a 128 KB copy) must land on disk byte for byte as the card holds it. */
static void test_sector_burst_lands_whole(void) {
    const char *path = "memcard_file_scratch/burst.mcd";
    MemcardSlotConfig slots[2] = { { NULL, 1 }, { NULL, 0 } };
    uint8_t sec[MEMCARD_SECTOR_SIZE], want[MEMCARD_SIZE];

    remove(path);
    slots[0].path = path;
    memcard_init_slots("memcard_file_scratch", slots);
    check(memcard_is_present(0), "burst: card present");
    for (int s = 0; s < MEMCARD_SECTORS; s++) {
        memset(sec, (int)(s * 5 + 1), sizeof(sec));
        check(memcard_write_sector(0, s, sec) == 0, "burst: write sector");
        memcard_flush(0);
    }
    check(memcard_flush_wait(10000) == 0, "burst: writer drains");
    check(memcard_export_raw(0, want) == 0, "burst: export");
    check(file_equals(path, want, MEMCARD_SIZE), "burst: file equals card image");
    remove(path);
}

int main(void) {
    for (size_t i = 0; i < MEMCARD_SIZE; i++) card_bytes[i] = (uint8_t)(i * 7u + 3u);
#ifdef _WIN32
    (void)_mkdir("memcard_file_scratch");
#else
    (void)mkdir("memcard_file_scratch", 0755);
#endif
    test_unreadable_card_not_formatted();
    test_missing_card_created();
    test_fixed_tmp_name_not_shared();
    test_atomic_write_and_rewrite();
    test_atomic_write_failure_keeps_old();
    test_wrong_size_card_left_alone();
    test_sector_burst_lands_whole();
    if (failures == 0) printf("PASS  memcard_file (unreadable card kept, missing created, private tmp)\n");
    return failures ? 1 : 0;
}
