#ifndef PSX_GAME_CORE_H
#define PSX_GAME_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

/* [runtime] game_core from game.toml (PSX_GAME_CORE=0/1 overrides it). Call
 * before psx_game_core_reserve. */
void psx_game_core_configure(int on);

/* One physical core for the emulation thread (game_core.c). Call on the
 * emulation thread. Returns the picked core's index, or -1 when nothing was
 * changed (off, no SMT, fewer than 4 cores, or an API refused). */
int psx_game_core_reserve(void);

/* The core picked by psx_game_core_reserve, -1 if none. */
int psx_game_core_index(void);

#ifdef __cplusplus
}
#endif

#endif
