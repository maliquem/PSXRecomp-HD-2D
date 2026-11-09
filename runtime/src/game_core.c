/* game_core.c - one physical core for the emulation thread (Windows only).
 *
 * In 3D scenes the emulation thread is the bottleneck, and what it runs short
 * of is the host's instruction fetch and decode (BoF4 boat at 8x: 3.0
 * instructions per cycle, 40% of fetches missing the op cache). Another thread
 * of this process on the same physical core, the SMT sibling, takes part of
 * that. Measured on the boat, all in one process: threads free 12.35x; the
 * emulation thread held on one CPU 12.37x; the other threads also off that CPU
 * 12.40x; the other threads off the whole physical core 12.84x.
 *
 * CPU Sets rather than affinity masks. The process default (every core but the
 * picked one) covers each thread without a selection of its own, so it also
 * covers the threads SDL, the audio and GPU drivers and the runtime create
 * later; threads that already exist get it as their own selection. The
 * emulation thread selects the picked core's logical processors. Only with SMT
 * and at least 4 cores. The picked core is the one the thread is on, unless
 * that core is slower (hybrid CPU) or holds logical processor 0, where most
 * device interrupts land. The autocompile core hold (autocompile.c) picks the
 * core the thread is on, so the two agree.
 *
 * Measured in the three titles with the render thread on, sdl_gpu, exes
 * without PGO: +6% to +12% at max speed; BoF4 golden identical (8 scenes).
 *
 * [runtime] game_core = true turns it on (off by default); PSX_GAME_CORE=0/1
 * overrides it for one run. The tooling hold PSX_GAME_THREAD_CPU wins over
 * both. It moves only where threads run: no guest state, no generator hash. */
#include "game_core.h"

#include <stdlib.h>

static int s_core = -1;
static int s_config = 0;

void psx_game_core_configure(int on) { s_config = on ? 1 : 0; }

int psx_game_core_index(void) { return s_core; }

static int game_core_wanted(void) {
    const char *env = getenv("PSX_GAME_CORE");
    if (env && env[0]) return env[0] == '1';
    return s_config;
}

#ifndef _WIN32
int psx_game_core_reserve(void) { (void)game_core_wanted; return -1; }
#else
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

/* SYSTEM_CPU_SET_INFORMATION, spelled out: the SDK only declares it for
 * Windows 10 targets. Entries are walked by Size. */
typedef struct {
    DWORD Size;
    DWORD Type;                  /* 0 = CpuSetInformation */
    DWORD Id;
    WORD Group;
    BYTE LogicalProcessorIndex;
    BYTE CoreIndex;
    BYTE LastLevelCacheIndex;
    BYTE NumaNodeIndex;
    BYTE EfficiencyClass;
    BYTE AllFlags;
    DWORD Reserved;
    DWORD64 AllocationTag;
} GcCpuSet;

typedef BOOL(WINAPI *GcGetInfo)(void *, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL(WINAPI *GcSetSets)(HANDLE, const ULONG *, ULONG);

enum { GC_MAX = 256 };

int psx_game_core_reserve(void) {
    if (!game_core_wanted() || getenv("PSX_GAME_THREAD_CPU") || s_core >= 0) return -1;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    GcGetInfo get_info = k32 ? (GcGetInfo)(void *)GetProcAddress(k32, "GetSystemCpuSetInformation") : NULL;
    GcSetSets set_process = k32 ? (GcSetSets)(void *)GetProcAddress(k32, "SetProcessDefaultCpuSets") : NULL;
    GcSetSets set_thread = k32 ? (GcSetSets)(void *)GetProcAddress(k32, "SetThreadSelectedCpuSets") : NULL;
    if (!get_info || !set_process || !set_thread) return -1;

    ULONG length = 0;
    (void)get_info(NULL, 0, &length, GetCurrentProcess(), 0);
    if (!length) return -1;
    unsigned char *buffer = (unsigned char *)malloc(length);
    if (!buffer) return -1;
    if (!get_info(buffer, length, &length, GetCurrentProcess(), 0)) { free(buffer); return -1; }

    ULONG id[GC_MAX];
    int group[GC_MAX], lp[GC_MAX], core[GC_MAX], cls[GC_MAX], n = 0;
    for (ULONG at = 0; at + sizeof(DWORD) * 2 <= length && n < GC_MAX;) {
        const GcCpuSet *s = (const GcCpuSet *)(buffer + at);
        if (!s->Size) break;
        if (s->Type == 0 && s->Size >= sizeof(GcCpuSet)) {
            id[n] = s->Id;
            group[n] = s->Group;
            lp[n] = s->LogicalProcessorIndex;
            core[n] = s->CoreIndex;
            cls[n] = s->EfficiencyClass;
            n++;
        }
        at += s->Size;
    }
    free(buffer);

    PROCESSOR_NUMBER here;
    GetCurrentProcessorNumberEx(&here);
    /* Cores of the current group: count them, see SMT, find the fastest class. */
    int cores = 0, smt = 0, fastest = 0, current = -1;
    for (int i = 0; i < n; i++) {
        if (group[i] != here.Group) continue;
        int siblings = 0, first = 1;
        for (int j = 0; j < n; j++) {
            if (group[j] != here.Group || core[j] != core[i]) continue;
            siblings++;
            if (j < i) first = 0;
        }
        if (first) cores++;
        if (siblings > 1) smt = 1;
        if (cls[i] > fastest) fastest = cls[i];
        if (lp[i] == here.Number) current = core[i];
    }
    if (!smt || cores < 4) return -1;
    int lp0_core = -1;
    for (int i = 0; i < n; i++)
        if (group[i] == here.Group && lp[i] == 0) lp0_core = core[i];
    int pick = -1;
    for (int i = 0; i < n && pick < 0; i++)
        if (group[i] == here.Group && core[i] == current && cls[i] == fastest && core[i] != lp0_core)
            pick = core[i];
    for (int i = 0; i < n && pick < 0; i++)
        if (group[i] == here.Group && cls[i] == fastest && core[i] != lp0_core) pick = core[i];
    if (pick < 0) return -1;

    ULONG game[GC_MAX], rest[GC_MAX];
    ULONG n_game = 0, n_rest = 0;
    for (int i = 0; i < n; i++) {
        if (group[i] == here.Group && core[i] == pick) game[n_game++] = id[i];
        else rest[n_rest++] = id[i];
    }
    if (!n_game || !n_rest) return -1;

    /* The emulation thread first: if it refuses, nothing has changed. */
    if (!set_thread(GetCurrentThread(), game, n_game)) return -1;
    if (!set_process(GetCurrentProcess(), rest, n_rest)) {
        (void)set_thread(GetCurrentThread(), NULL, 0);
        return -1;
    }
    /* Threads that already exist: the same set as their own selection. */
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        memset(&te, 0, sizeof te);
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE t = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!t) continue;
            (void)set_thread(t, rest, n_rest);
            CloseHandle(t);
        }
        CloseHandle(snap);
    }
    s_core = pick;
    return pick;
}
#endif
