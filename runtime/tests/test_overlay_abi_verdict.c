/* Pure-decision test for the loader's ABI pre-flight sweep: which cached DLLs
 * are kept, when the `.abi_<tag>.ok` marker may be written, and its name.
 * Links only src/overlay_abi_verdict.c. */
#include "overlay_abi_verdict.h"

#include <stdio.h>
#include <string.h>

static int s_fails = 0;

static void expect(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_fails++;
    }
}

int main(void) {
    const int tag = 0x00040506; /* flavor in the high half, version in the low */

    /* Verdict: only a loaded library reporting exactly the runtime tag stays. */
    expect(psx_overlay_abi_accepts(1, tag, tag), "matching abi is kept");
    expect(!psx_overlay_abi_accepts(1, tag - 1, tag), "older abi is rejected");
    expect(!psx_overlay_abi_accepts(1, 0x00000506, tag),
           "same version, other flavor is rejected");
    expect(!psx_overlay_abi_accepts(1, 0, tag),
           "missing overlay_abi (0) is rejected");
    expect(!psx_overlay_abi_accepts(0, 0, tag), "unloadable library is rejected");
    expect(!psx_overlay_abi_accepts(0, tag, tag),
           "not loaded is rejected even if abi looks equal");
    expect(psx_overlay_abi_accepts(1, 0, 0), "tag 0 vs abi 0 compares equal");

    /* Marker: written only when the sweep rejected nothing. */
    expect(psx_overlay_abi_marker_wanted(0), "clean sweep writes the marker");
    expect(!psx_overlay_abi_marker_wanted(1), "one rejection blocks the marker");
    expect(!psx_overlay_abi_marker_wanted(250), "many rejections block the marker");

    /* Marker name. */
    char name[64];
    psx_overlay_abi_marker_name(name, sizeof name, "cache/gcc/x64/cg1", tag);
    expect(strcmp(name, "cache/gcc/x64/cg1/.abi_00040506.ok") == 0,
           "marker name uses %08x of the combined tag");
    psx_overlay_abi_marker_name(name, sizeof name, "d", -1);
    expect(strcmp(name, "d/.abi_ffffffff.ok") == 0, "negative tag prints as unsigned");

    /* snprintf semantics: truncation stays NUL-terminated; cap 0 is a no-op. */
    char small[8];
    memset(small, 'x', sizeof small);
    psx_overlay_abi_marker_name(small, sizeof small, "long/dir", tag);
    expect(small[sizeof small - 1] == '\0' && strlen(small) == sizeof small - 1,
           "truncated marker name is NUL-terminated");
    char untouched[2] = {'q', 'q'};
    psx_overlay_abi_marker_name(untouched, 0, "d", tag);
    expect(untouched[0] == 'q' && untouched[1] == 'q', "cap 0 writes nothing");
    psx_overlay_abi_marker_name(NULL, 8, "d", tag); /* must not crash */

    /* OVL-2, current behavior, not fixed here: a rejected file is never
     * deleted by the sweep and blocks the marker, so every boot/rescan repeats
     * the full sweep of that directory. */
    printf("KNOWN: OVL-2 a rejected DLL stays on disk and blocks the "
           "marker (marker_wanted(1)=%d), so the sweep repeats each boot\n",
           psx_overlay_abi_marker_wanted(1));

    if (s_fails) {
        fprintf(stderr, "%d check(s) failed\n", s_fails);
        return 1;
    }
    printf("overlay_abi_verdict_test: all checks passed\n");
    return 0;
}
