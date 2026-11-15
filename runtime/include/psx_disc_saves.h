#ifndef PSXRECOMP_PSX_DISC_SAVES_H
#define PSXRECOMP_PSX_DISC_SAVES_H

/* Which disc each memory card save was made on, for multi-disc games.
 *
 * The card format never changes: the disc lives in a file beside the cards,
 * <memcard_dir>/discos.ini, one section per save:
 *
 *   versao=1
 *   [<card file relative to memcard_dir>|<save name>]
 *   disco=2
 *   hash=<sha-256 of the save, 64 hex>
 *   gravado=<unix seconds>
 *
 * A save is recorded only when its bytes change while the game runs, so the
 * other games' saves on a shared card never get a disc. When a card is first
 * seen, records whose save is gone or was changed outside are dropped, and a
 * save without a record that matches a recorded save byte for byte (a copy)
 * takes that record's disc and time. Pure C, no SDL: the host feeds card
 * images and asks questions. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_CARD_SAVES_MAX 15

typedef struct {
    char     name[21];   /* directory name, NUL-terminated */
    int      first;      /* first block, 0..14 */
    uint16_t blocks;     /* bit b set: block b belongs to this save */
    uint8_t  sha[32];    /* name + blocks in chain order */
} PsxCardSave;

/* The saves on a 128 KB card image (broken chains are skipped). */
int psx_card_list_saves(const uint8_t *img, PsxCardSave out[PSX_CARD_SAVES_MAX]);

/* Load the registry file (missing = empty). Forgets cards seen before. */
int psx_disc_saves_open(const char *path);

/* A card in slot 0/1 seen for the first time this session (or after a rebind):
 * prune, inherit copies, and remember its saves as they are now. A NULL key or
 * image means the slot is empty: its writes and reads are ignored until seen. */
void psx_disc_saves_card_seen(int slot, const char *card_key, const uint8_t *img);

/* The game wrote the card: saves that changed since the last look get `disc`
 * and `now`; copies inherit; deleted saves lose their record. Returns the
 * number of records changed. */
int psx_disc_saves_card_written(int slot, const uint8_t *img, int disc, int64_t now);

/* Recorded disc of the save holding `block` (0..14) on the card in `slot`,
 * 0 when that block holds no recorded save. */
int psx_disc_saves_disc_of_block(int slot, int block);

/* Disc of the newest recorded save on the cards seen, 0 when none.
 * `when` (may be NULL) gets its time. */
int psx_disc_saves_newest(int64_t *when);

/* Write the file if anything changed. 0 = written or nothing to do. */
int psx_disc_saves_flush(void);

#ifdef __cplusplus
}
#endif

#endif
