# Save slots

Many saves instead of one 15-block memory card. API: [`runtime/include/psx_save_slots.h`](../runtime/include/psx_save_slots.h).
Test: `runtime/tests/test_save_slots.c` (`save_slots_test`).

## The idea

The game keeps using a memory card. When the player saves or loads, the title's plugin mounts a card built in RAM that
holds only what the game should see at that moment:

- **loading slot N:** the save in slot N, under the name the game reads;
- **saving over slot N:** the save in slot N, so the game's "overwrite?" path runs as usual;
- **saving to an empty slot:** a card with nothing on it.

The game's own card code (BIOS, SIO, its save routine) runs unchanged. To the game, the player swapped cards: the mount
reports the card as freshly inserted (the FLAG "new card" bit), as the hardware does after a real swap. Afterwards the
plugin commits the file the game wrote into the chosen slot and unmounts, which puts the player's card back.

The player's own card file is never written while a slot card is mounted. Files named by `psx_save_slots_keep_files`
(a title's settings file, for example) are on every mounted card, and changes to them go back to the player's card.

## Files

Under `<memcard_dir>/slots/` (`[runtime] memcard_dir`, `saves` in our titles):

| File | What |
|---|---|
| `slot_NNN.mcs` | the save: its first directory frame and its 8 KB blocks, the single-save format card tools (MemcardRex and others) open |
| `slot_NNN.info` | details kept beside the save, UTF-8 `key=value` lines |

Both are written to a temporary file, synced and then put in place in one step, so a crash or a full disk never leaves a
cut save. The game's save holds only what the game itself keeps; everything a page shows beyond that lives in the
`.info` file:

```
format=1
name=Before the desert        player's own name for the save (optional)
saved=1762889000               unix time of the last commit
playtime=12300                 seconds
money=4520
chapter=2
chapter_label=Chapter 2
location=Sand Flats
party=1:12,2:11                character id:level, in party order (up to 8)
difficulty=Normal
achievements=7/96
achievement_mode=hardcore      off | softcore | hardcore
x.anything=value               free fields a title adds (up to 16)
```

A title fills `PSXSaveSlotDetails` at commit time from the game's RAM and its own plugins (difficulty, achievements),
setting `fields` for what it filled. A commit with `details = NULL` stores no details. The player's name for a slot
survives commits; `psx_save_slots_rename` changes it.

## A title's page

The page belongs to the title and follows the game's own UI. The convention for our RPGs:

- **100 slots in 10 pages of 10.** `psx_save_slots_open(100, NULL)` at boot.
- Each slot shows: the player's name for it (or the game's own title for the save), the date, play time, money,
  chapter, the party (a fixed row of places for every member the game can have: portrait and level for those present,
  empty places for those not yet in the party), difficulty, and achievements (unlocked/total, softcore or hardcore).
- Actions: save, load, rename (optional name), copy and delete.
- The game's icon for a save is available as RGBA through `psx_save_slots_icon`.

Portraits are drawn by the page from the game's own art at display time; the `.info` file keeps only character ids and
levels.

The page opens over the running game, so its music goes on: call `psx_mod_title_page_run_game(1)` right before
`psx_mod_open_title_page`. The game runs with every pad released, its picture shows wherever the page's canvas is
transparent, and the page takes the host's buttons and keys each frame. Open it where the game idles with no input (its
own menu). A frozen game cannot keep its music: most RPG field music is sequenced by the game's own driver, so a copy
of the SPU only sustains the notes already sounding.

## Flow in a plugin

```c
/* Boot */
psx_save_slots_open(100, NULL);
psx_save_slots_keep_files("BISLPS-01000-FM2-S");   /* only titles with such a file */

/* Player chose slot n on the page to SAVE */
psx_save_slots_mount(0, n, game_name_for_place_0);  /* n empty: an empty card */
/* ...let the game's save routine write its place 0 (its menu, or a guest call)... */
PSXSaveSlotDetails d = { 0 };
/* fill d from RAM */
psx_save_slots_commit(n, &d);
psx_save_slots_unmount();

/* Player chose slot n to LOAD */
psx_save_slots_mount(0, n, game_name_for_place_0);
/* ...the game's load routine reads place 0... */
psx_save_slots_unmount();
```

`psx_save_slots_mount` and `psx_save_slots_unmount` return -3 while a card exchange is in flight; try again on a later
frame. Mount before the game opens its card screen, never in the middle of a read, so the game reads the new directory.

`psx_save_slots_import_card` puts the saves of an old `.mcd` card into the first free slots.

## Card writes

The card's own file is also written atomically now (temporary file, sync, replace), on a writer thread: the game's
write of each sector only copies the image for the writer, so a save no longer stalls the emulation thread, and a burst
of sector writes collapses into one file write. `memcard_flush_all` (at exit) and `memcard_flush_wait` wait for the
writes to land.

## Later

- **Cloud folders (launchers):** let the player put the save folder in OneDrive, Dropbox or Google Drive when one is
  installed, and find it again on another computer. Planned, not built.
