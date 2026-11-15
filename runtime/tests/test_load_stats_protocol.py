"""Run the production boot_state_stats / interp_faults TCP handlers through the C ABI.

The handlers, json_escape_string, the boot_state stats store and the native
(stub) interp_faults are cut from the real sources and linked into a tiny
harness. No server, game or log file. Missing compiler is an explicit skip.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_wtrace_dump import matching_brace

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^(?:static )?[^;{}\n]*\b' + name + r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(f"production function missing: {name}")
    return source[match.start():matching_brace(source, match.end() - 1) + 1]


REASON = 'bad"q\\b\x01'


class LoadStatsProtocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get("CC", "cc")
        if not shutil.which(cc):
            raise unittest.SkipTest("set CC to the native C compiler")
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        temp = Path(cls.temp.name)
        src = ROOT / "runtime/src"
        debug = (src / "debug_server.c").read_text(encoding="utf-8")
        boot = (src / "boot_state.c").read_text(encoding="utf-8")
        stub = (src / "stub_interpreter.c").read_text(encoding="utf-8")
        harness = '''#include <stdio.h>
#include <string.h>
#include "boot_state.h"
#include "psx_interpreter.h"
#define send_fmt(...) (printf(__VA_ARGS__), putchar('\\n'))
static BootStateLoadStats s_load_stats;
'''
        harness += function(boot, "boot_state_load_stats") + "\n"
        harness += function(boot, "boot_state_note_reject") + "\n"
        harness += function(stub, "interp_faults") + "\n"
        for name in ("json_escape_string", "handle_boot_state_stats", "handle_interp_faults"):
            harness += function(debug, name) + "\n"
        harness += '''int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        char path[301];
        memset(path, 'p', 300);
        path[300] = 0;
        boot_state_note_reject("bad\\"q\\\\b\\001", path);
    }
    handle_boot_state_stats(7, "{}");
    handle_interp_faults(8, "{}");
    return 0;
}
'''
        (temp / "harness.c").write_text(harness, encoding="utf-8")
        cls.exe = temp / ("protocol.exe" if os.name == "nt" else "protocol")
        built = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                                "-O2", "-Wno-attributes", "-Wno-unused-function",  # cpu_state.h inlines
                                "-I", str(ROOT / "runtime/include"), str(temp / "harness.c"),
                                "-o", str(cls.exe)], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stderr[-3000:])

    def query(self, *args):
        lines = subprocess.check_output([str(self.exe), *args], text=True).splitlines()
        return [json.loads(line) for line in lines]

    def test_fresh_state_is_zero(self):
        stats, faults = self.query()
        self.assertEqual((stats["id"], stats["loads"], stats["rejects"], stats["file_len"]), (7, 0, 0, 0))
        self.assertEqual((stats["total_ms"], stats["last_reject"]), (0.0, ""))
        self.assertEqual((faults["id"], faults["null_jumps"], faults["unknown_cop0"],
                          faults["unknown_cop2"], faults["last_pc"]), (8, 0, 0, 0, "0x00000000"))

    def test_reject_reason_escaped_and_truncated(self):
        stats, _ = self.query("reject")
        self.assertEqual(stats["rejects"], 1)
        self.assertEqual(stats["last_reject"], (REASON + " " + "p" * 300)[:159])


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LoadStatsProtocol)
    result = unittest.TextTestRunner().run(suite)
    raise SystemExit(77 if result.skipped else 0 if result.wasSuccessful() else 1)
