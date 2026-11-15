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
    if (failures == 0) printf("PASS  memcard_file (unreadable card kept, missing created, private tmp)\n");
    return failures ? 1 : 0;
}
