/* psx_write_atomic.h -- replace a player's file whole, never write over it.
 *
 * One writer for every file the player would lose to a cut write: memory card
 * images, savestates, their thumbnails and the save slots. */
#ifndef PSX_WRITE_ATOMIC_H
#define PSX_WRITE_ATOMIC_H

#include <stddef.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Write `data` to `path` so a crash, a full disk or a failed flush never
 * leaves a cut file: write a temporary next to it, sync it, then replace the
 * target in one step. The old file stays whole until the replacement lands.
 * The temporary carries the process id, so two processes writing the same
 * file never share (and mix) one temporary. 0 = written; -1 = the old file is
 * untouched. */
static inline int psx_write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[1024];
    FILE *f;
    int ok;
#ifdef _WIN32
    const unsigned long pid = (unsigned long)_getpid();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    if (!path || !path[0]) return -1;
    if (snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, pid) >= (int)sizeof tmp) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -1;
    ok = fwrite(data, 1, len, f) == len && fflush(f) == 0;
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

#endif /* PSX_WRITE_ATOMIC_H */
