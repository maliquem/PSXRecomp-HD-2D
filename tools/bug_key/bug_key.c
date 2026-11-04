/* bug_key: the developer's side of the sealed box in a player's bug report
 * (docs/BUG_REPORT.md).
 *
 *   bug_key keygen <private key file>      writes a new key, prints the public one
 *   bug_key public <private key file>      prints the public one
 *   bug_key open <private key file> <private.bin> <out.zip>
 *
 * The private key opens every report of the title: it stays out of every
 * repository. The public one goes in the title's plugin
 * (psx_mod_set_bug_report_send). Build:
 *   clang -O2 tools/bug_key/bug_key.c runtime/src/bug_report_pack.c \
 *         runtime/third_party/monocypher/monocypher.c -lz -o bug_key.exe */
#include "../../runtime/src/bug_report_pack.h"
#include "../../runtime/third_party/monocypher/monocypher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
    fputs("bug_key keygen KEY | public KEY | open KEY private.bin out.zip\n", stderr);
    return 2;
}

static void print_public(const uint8_t pub[32]) {
    for (int i = 0; i < 32; i++) printf("%02x", pub[i]);
    putchar('\n');
}

static int read_key(const char *path, uint8_t key[32]) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    const int ok = fread(key, 1, 32, f) == 32 && fgetc(f) == EOF;
    fclose(f);
    if (!ok) fprintf(stderr, "%s is not a key\n", path);
    return ok;
}

static uint8_t *read_all(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = n >= 0 ? (uint8_t *)malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        p = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    uint8_t key[32], pub[32];
    int code = 1;
    if (!strcmp(argv[1], "keygen") && argc == 3) {
        FILE *probe = fopen(argv[2], "rb");
        if (probe) {
            fclose(probe);
            fprintf(stderr, "%s exists: a new key would lock every older report\n", argv[2]);
            return 1;
        }
        if (br_keypair(key, pub)) {
            FILE *f = fopen(argv[2], "wb");
            if (f && fwrite(key, 1, 32, f) == 32 && fclose(f) == 0) {
                print_public(pub);
                code = 0;
            }
        }
    } else if (!strcmp(argv[1], "public") && argc == 3) {
        if (read_key(argv[2], key)) {
            crypto_x25519_public_key(pub, key);
            print_public(pub);
            code = 0;
        }
    } else if (!strcmp(argv[1], "open") && argc == 5) {
        size_t n = 0;
        uint8_t *box = read_key(argv[2], key) ? read_all(argv[3], &n) : NULL;
        if (box && n >= BR_SEAL_OVERHEAD) {
            uint8_t *plain = (uint8_t *)malloc(n - BR_SEAL_OVERHEAD + 1);
            if (plain && br_unseal(key, box, n, plain)) {
                FILE *f = fopen(argv[4], "wb");
                if (f && fwrite(plain, 1, n - BR_SEAL_OVERHEAD, f) == n - BR_SEAL_OVERHEAD && fclose(f) == 0) code = 0;
            } else {
                fputs("not for this key, or changed\n", stderr);
            }
            free(plain);
        }
        free(box);
    } else {
        return usage();
    }
    crypto_wipe(key, sizeof key);
    return code;
}
