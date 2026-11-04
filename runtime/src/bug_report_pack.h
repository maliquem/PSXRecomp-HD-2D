#ifndef PSX_BUG_REPORT_PACK_H
#define PSX_BUG_REPORT_PACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a bug report sends (docs/BUG_REPORT.md): a stored ZIP in memory and a
 * sealed box only the holder of the private key opens. */

typedef struct BrZipEntry {
    char name[96];
    uint32_t crc, size, offset;
} BrZipEntry;

#define BR_ZIP_ENTRIES_MAX 32

typedef struct BrZip {
    uint8_t *buf;
    size_t len, cap;
    BrZipEntry entry[BR_ZIP_ENTRIES_MAX];
    int count, failed;
} BrZip;

void br_zip_init(BrZip *z);
/* Nothing is compressed: every member is a JPEG, a PNG, a save state or a
 * sealed box already. Names are UTF-8, with '/' between folders. */
void br_zip_add(BrZip *z, const char *name, const void *data, size_t size);
/* Returns 0 when the file cannot be read (the archive stays good). */
int  br_zip_add_file(BrZip *z, const char *name, const char *path);
/* Writes the central directory. 1: z->buf / z->len hold the archive. */
int  br_zip_finish(BrZip *z);
void br_zip_free(BrZip *z);

/* "BRS1", the sender's one-time public key, nonce, tag, then the text:
 * X25519 to the recipient, BLAKE2b of the shared secret and both public keys,
 * XChaCha20-Poly1305 (Monocypher). */
#define BR_SEAL_OVERHEAD (4u + 32u + 24u + 16u)
/* out holds size + BR_SEAL_OVERHEAD bytes. 0 when no random bytes were had. */
int  br_seal(const uint8_t recipient_public[32], const void *plain, size_t size, uint8_t *out);
/* out holds size - BR_SEAL_OVERHEAD bytes. 0 when the box is not for this key
 * or was changed. */
int  br_unseal(const uint8_t recipient_private[32], const uint8_t *sealed, size_t size, uint8_t *out);
/* A new key pair. 0 when no random bytes were had. */
int  br_keypair(uint8_t private_key[32], uint8_t public_key[32]);

/* Bytes from the system's generator. */
int  br_random(void *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
