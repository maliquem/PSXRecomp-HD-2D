#ifndef PSXRECOMP_PSX_SAVE_SLOTS_H
#define PSXRECOMP_PSX_SAVE_SLOTS_H

/* Save slots: many saves instead of one 15-block memory card.
 *
 * Each save is kept in its own file, outside any card. When the game saves or
 * loads, a title's plugin mounts a card built in RAM that holds only what the
 * game should see at that moment (the chosen save, or nothing for a new one),
 * lets the game's own card code run, then commits the file the game wrote
 * into the chosen slot. The game talks to a normal card through the SIO the
 * whole time; to it, the player swapped cards. The player's card file is
 * never touched while a slot image is mounted.
 *
 * Files, under <memcard_dir>/slots/:
 *   slot_NNN.mcs   the save itself: its first directory frame and its blocks
 *                  (the single-save format card tools open)
 *   slot_NNN.info  details kept beside the save (UTF-8 key=value lines):
 *                  the player's name for it and what the title's page shows
 *                  (play time, money, chapter, party, difficulty,
 *                  achievements, and free extra fields)
 * Both are replaced in one step, so a crash never leaves a cut save.
 *
 * The page itself (pages of slots, the look of the game) belongs to each
 * title; this module only stores, mounts and reads. Slots are numbered from
 * 1. Calls come from the emulation thread except the readers
 * (psx_save_slots_get, psx_save_slots_icon), which any thread may call. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_SAVE_SLOTS_MAX       999
#define PSX_SAVE_SLOT_PARTY_MAX  8
#define PSX_SAVE_SLOT_EXTRA_MAX  16

/* Which details a title filled in (PSXSaveSlotDetails.fields). */
enum {
    PSX_SAVE_FIELD_PLAYTIME     = 1u << 0,
    PSX_SAVE_FIELD_MONEY        = 1u << 1,
    PSX_SAVE_FIELD_CHAPTER      = 1u << 2,
    PSX_SAVE_FIELD_LOCATION     = 1u << 3,
    PSX_SAVE_FIELD_PARTY        = 1u << 4,
    PSX_SAVE_FIELD_DIFFICULTY   = 1u << 5,
    PSX_SAVE_FIELD_ACHIEVEMENTS = 1u << 6
};

/* Achievement mode the save was made in. */
enum {
    PSX_SAVE_ACH_OFF      = 0,
    PSX_SAVE_ACH_SOFTCORE = 1,
    PSX_SAVE_ACH_HARDCORE = 2
};

typedef struct {
    int32_t id;     /* title's own character id */
    int32_t level;
} PSXSaveSlotMember;

typedef struct {
    char key[32];   /* letters, digits, '_' and '.' */
    char value[96];
} PSXSaveSlotExtra;

/* What a title keeps beside a save. Strings are UTF-8. */
typedef struct {
    uint32_t fields;                 /* PSX_SAVE_FIELD_* set by the title */
    int64_t  playtime_seconds;
    int64_t  money;
    int32_t  chapter;
    char     chapter_label[64];
    char     location[64];
    int32_t  party_count;
    PSXSaveSlotMember party[PSX_SAVE_SLOT_PARTY_MAX];
    char     difficulty[32];
    int32_t  achievements_unlocked;
    int32_t  achievements_total;
    int32_t  achievement_mode;       /* PSX_SAVE_ACH_* */
    int32_t  extra_count;
    PSXSaveSlotExtra extra[PSX_SAVE_SLOT_EXTRA_MAX];
} PSXSaveSlotDetails;

typedef struct {
    int      used;                   /* 0 = empty slot, the rest is zero */
    char     name[64];               /* the player's name for it, "" = none */
    int64_t  saved_time;             /* unix seconds of the last commit */
    char     file_name[21];          /* the save's name on the card */
    int32_t  blocks;                 /* 8 KB blocks it takes */
    uint8_t  title_sjis[64];         /* the game's own title for it (Shift-JIS) */
    int32_t  icon_frames;            /* 0..3 frames of the 16x16 icon */
    PSXSaveSlotDetails details;
} PSXSaveSlotInfo;

/* Open the slot store for `count` slots (1..PSX_SAVE_SLOTS_MAX) in
 * <memcard_dir>/slots (or `dir` when not NULL) and read what is there.
 * Called again, it re-reads. 0 = ok, -1 = bad count or no directory. */
int psx_save_slots_open(int count, const char *dir);
int psx_save_slots_count(void);

/* Copy slot `slot`'s summary into out. 0 = ok, -1 = bad slot. */
int psx_save_slots_get(int slot, PSXSaveSlotInfo *out);

/* Decode frame `frame` of the save's icon into 16x16 RGBA (1024 bytes),
 * transparent where the game's palette says so. Returns the number of icon
 * frames, or -1 for an empty slot or a bad frame. */
int psx_save_slots_icon(int slot, int frame, uint8_t *rgba);

/* Files whose card name starts with `prefix` stay on every mounted card and
 * are written back to the player's card (a title's settings file, say).
 * Up to 8 prefixes. 0 = ok. */
int psx_save_slots_keep_files(const char *prefix);

/* Mount on card port `card` (0 or 1) a RAM card with the kept files and the
 * save in `slot` (0 = no save: a card ready for a new one). With `as_name`,
 * the save goes on the card under that name (the name the game looks for at
 * the place it will read). The card reports itself as freshly inserted.
 * 0 = ok; -1 bad argument or another mount open; -2 the card is absent;
 * -3 a card exchange is in flight (try on a later frame); -4 the save does
 * not fit or cannot be read. */
int psx_save_slots_mount(int card, int slot, const char *as_name);

/* Store into `slot` the save the game wrote on the mounted card, with
 * `details` (may be NULL), and record the time. The player's name for the
 * slot is kept. Kept files the game changed go back to the player's card at
 * unmount. 0 = ok; -1 nothing mounted or bad slot; -2 the game wrote no
 * save; -3 the files could not be written. */
int psx_save_slots_commit(int slot, const PSXSaveSlotDetails *details);

/* Put the player's card back on the port (reported as freshly inserted).
 * 0 = ok, -1 nothing mounted, -3 a card exchange is in flight. */
int psx_save_slots_unmount(void);

/* The slot whose card is mounted (0 for the empty card), or -1. */
int psx_save_slots_mounted(void);

/* Name a save ("" or NULL clears it). 0 = ok, -1 bad or empty slot. */
int psx_save_slots_rename(int slot, const char *name);
/* Remove a save. 0 = ok, -1 bad slot, -3 the files could not be removed. */
int psx_save_slots_delete(int slot);
/* Copy a save with its details and name. 0 = ok, -1 bad or empty source,
 * -3 write failure. */
int psx_save_slots_copy(int from, int to);

/* Put each save of a .mcd card whose name starts with `prefix` (NULL = all
 * saves) into the first free slots. Returns how many were imported, or -1
 * when the card cannot be read. */
int psx_save_slots_import_card(const char *mcd_path, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PSX_SAVE_SLOTS_H */
