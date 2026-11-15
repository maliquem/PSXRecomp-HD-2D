/* overlay_abi_verdict.h — pure decisions of the loader's ABI pre-flight
 * sweep, split out so they can be tested without LoadLibrary/dlopen. No
 * allocation, no I/O, no dependency on the rest of the loader. */
#ifndef PSXRECOMP_OVERLAY_ABI_VERDICT_H
#define PSXRECOMP_OVERLAY_ABI_VERDICT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = keep the cached DLL, 0 = reject it. `loaded` is nonzero when the
 * library opened; `abi` is what overlay_abi() returned (0 when the symbol is
 * missing or the library did not load). */
int psx_overlay_abi_accepts(int loaded, int abi, int runtime_tag);

/* 1 when a finished sweep may record its `.abi_<tag>.ok` marker: only when
 * nothing was rejected (rejected files stay on disk, so a marker would hide
 * their rejection on the next boot). */
int psx_overlay_abi_marker_wanted(int purged);

/* Writes "<dir>/.abi_<tag as %08x>.ok" into `out` (snprintf semantics:
 * truncated and NUL-terminated when it does not fit; no-op for cap == 0). */
void psx_overlay_abi_marker_name(char *out, size_t cap, const char *dir,
                                 int tag);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_OVERLAY_ABI_VERDICT_H */
