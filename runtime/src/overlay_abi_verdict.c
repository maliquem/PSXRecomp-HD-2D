/* overlay_abi_verdict.c — see header. */
#include "overlay_abi_verdict.h"

#include <stdio.h>

int psx_overlay_abi_accepts(int loaded, int abi, int runtime_tag) {
    return loaded && abi == runtime_tag;
}

int psx_overlay_abi_marker_wanted(int purged) {
    return purged == 0;
}

void psx_overlay_abi_marker_name(char *out, size_t cap, const char *dir,
                                 int tag) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s/.abi_%08x.ok", dir ? dir : "", (unsigned)tag);
}
