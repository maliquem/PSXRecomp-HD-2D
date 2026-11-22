/* host_sampler_stop at exit (Windows): exit() terminates the sampler thread
 * after the atexit handlers run. A sampler terminated between SuspendThread
 * and ResumeThread leaves the exiting thread suspended and the process never
 * ends, so the sampler must be stopped before that.
 *
 * Each child starts the sampler on its main thread, waits for samples and
 * calls exit(0). A handler registered before host_sampler_start runs after
 * host_sampler_stop (atexit is LIFO) and fails the child if a sample still
 * lands; the parent fails on a child that does not end. Off Windows: skipped. */
#include "host_sampler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_psx_render_pass_active;
uint64_t s_frame_count;

#ifndef _WIN32
int main(void) {
    puts("SKIP: the exit hang is a Windows sampler fault (SuspendThread)");
    return 77;
}
#else
#include <windows.h>

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void sampler_must_be_stopped(void) {
    const uint64_t seq = host_sampler_seq();
    const double t0 = now_ms();
    while (now_ms() - t0 < 20.0) {}
    if (host_sampler_seq() != seq) _Exit(3);
}

static int child(void) {
    atexit(sampler_must_be_stopped);
    host_sampler_start();
    if (!host_sampler_supported()) return 4;
    const double t0 = now_ms();
    while (host_sampler_seq() < 5)
        if (now_ms() - t0 > 2000.0) return 2;
    exit(0);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return child();
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", argv[0]);
    for (int i = 0; i < 20; i++) {
        STARTUPINFOA si = {sizeof si};
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("FAIL: CreateProcess %lu\n", GetLastError());
            return 1;
        }
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 10000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            printf("FAIL: child %d did not end after exit()\n", i);
            return 1;
        }
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code != 0) {
            printf("FAIL: child %d exit code %lu (3 = sampled after stop)\n", i, code);
            return 1;
        }
    }
    puts("PASS: the sampler stops before exit and the process ends");
    return 0;
}
#endif
