import base64
import binascii
import json
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import overlay_keep as ok


def cap(load, data, roots, executed=()):
    h = lambda xs: ["0x%08X" % p for p in xs]
    return {"schema": "psxrecomp overlay capture v3", "load_addr": "0x%08X" % load,
            "bytes_b64": base64.b64encode(data).decode(), "executed_pcs": h(list(roots) + list(executed)),
            "size": len(data), "dispatch_entry_pcs": h(roots), "dispatch_entry_segments": {}}


def crc(data):
    return binascii.crc32(data) & 0xFFFFFFFF


class OverlayKeepTest(unittest.TestCase):
    def test_mark_prune_compile_list(self):
        a, b, c, base = b"A" * 64, b"B" * 32 + b"F" * 32, b"C" * 64, b"M" * 64
        b2 = b"X" * 32 + b"F" * 32          # second image holding the same fragment function
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            main = cap(0x80100000, base, [0x80100000])
            (t / "overlay_captures.json").write_text(json.dumps([main]))
            d = t / "overlay_captures.json.d"
            d.mkdir()
            ra = cap(0x80180000, a, [0x80180000])
            ra_big = cap(0x80180000, a, [0x80180000], executed=[0x80180010, 0x80180020])
            rb = cap(0x80190000, b, [0x80190020])
            rb2 = cap(0x80190000, b2, [0x80190020], executed=[0x80190024])
            (d / "1.json").write_text(json.dumps([ra, rb]))
            (d / "2.json").write_text(json.dumps([cap(0x801A0000, c, [0x801A0000])]))
            (d / "3.json").write_text(json.dumps([rb2, ra_big]))
            inc = t / "overlay_toolchain" / "include"
            inc.mkdir(parents=True)
            (inc / "overlay_codegen_hash.h").write_text("#define PSX_OVERLAY_CODEGEN_HASH 0x46bb7ed0u\n")
            cg = t / "cache" / "G" / "gcc" / "win-x64" / "cg18_46bb7ed0_gc1_f2"
            old = t / "cache" / "G" / "gcc" / "win-x64" / "cg18_11111111_gc1_f2"
            cg.mkdir(parents=True)
            old.mkdir(parents=True)
            (old / "x.dll").write_text("")
            region = "00180000_%08X" % crc(a)
            frag = "00190000_DEADBEEF"
            hosted = "00190000_CAFEF00D"
            other = "001A0000_%08X" % crc(c)
            for n in (region, frag, hosted, other):
                (cg / (n + ".dll")).write_text("")
                (cg / (n + ".dll.pair-lock")).write_text("")
            # region: two functions, the second rooted in no record (only its manifest has it)
            (cg / (region + ".ranges")).write_text("F 80180000 %08X\nR 80180000 10\nF 80180010 %08X\nR 80180010 10\n"
                                                   % (crc(b"A" * 16), crc(b"A" * 16)))
            (cg / (frag + ".ranges")).write_text("F 80190020 %08X\nR 80190020 20\n" % crc(b"F" * 32))
            # hosted shard: 80190030 is an alias inside its host 80190020; host 80190038 kept no alias
            (cg / (hosted + ".ranges")).write_text(
                "# psxrecomp overlay provenance hosted-v1\nF 80190020 %08X\nR 80190020 20\n"
                "F 80190030 %08X\nR 80190020 20\nF 80190038 %08X\nR 80190038 8\n"
                % (crc(b"F" * 32), crc(b"F" * 32), crc(b"F" * 8)))
            (cg / (other.split("_")[1] + "_patched.c")).write_text("")
            (cg / "notes.txt").write_text("")
            mods = t / "mods.txt"
            mods.write_text("%s\n%s\n# fim de cena\n%s\n" % (cg / (region + ".dll"), cg / (frag + ".dll"),
                                                             cg / (hosted + ".dll")))

            ok.cmd_mark(Namespace(build=t, loaded=[mods], out=t / "keep.json"))
            keep = json.loads((t / "keep.json").read_text())
            imgs = sorted(["00190000_%08X" % crc(b), "00190000_%08X" % crc(b2)])   # both hold the fragment
            self.assertEqual(keep["entries"], {region: ["00180000", "00180010"]})
            self.assertEqual(keep["fragments"], {
                frag: {"images": imgs, "provenance": "", "entries": ["00190020"]},
                hosted: {"images": imgs, "provenance": "hosted-v1", "entries": ["00190020", "00190030", "00190038"],
                         "hosts": {"00190030": ["00190020", "%08X" % crc(b"F" * 32), "20"],
                                   "0019003C": ["00190038", "%08X" % crc(b"F" * 8), "8"]}}})   # host+4
            want = {ok.record_id(x) for x in (main, ra, rb, rb2)}       # the smaller record of image a
            self.assertEqual(set(keep["records"]), want)
            self.assertEqual(sorted(keep["dlls"]), sorted([region, frag, hosted]))

            ok.cmd_prune(Namespace(build=t, keep=t / "keep.json", apply=False, drop_other_hashes=True))
            self.assertTrue((cg / (other + ".dll")).exists())   # dry run deletes nothing

            ok.cmd_prune(Namespace(build=t, keep=t / "keep.json", apply=True, drop_other_hashes=True))
            left = sorted(p.name for p in cg.iterdir())
            self.assertEqual(left, sorted([n + x for n in (region, frag, hosted)
                                           for x in (".dll", ".dll.pair-lock", ".ranges")] + ["notes.txt"]))
            self.assertFalse(old.exists())
            self.assertFalse((d / "2.json").exists())
            self.assertTrue((t / "overlay_captures.json").exists())
            kept = {ok.record_id(x) for f in d.glob("*.json") for x in ok.read_caps(f)}
            self.assertEqual(kept, want - {ok.record_id(main)})

            recs, _, entries, _ = ok.load_keep(t / "keep.json")
            regions, by_img = ok.protected_captures(t, recs, entries, lambda data, load, pc: pc == 0x80180000)
            self.assertEqual(len(regions), 1)
            self.assertEqual(sorted(by_img), sorted(ok.image_key(x)[0] for x in (main, ra, rb, rb2)))
            syn = regions[0]                  # only the listed entries are roots, none of ra's own extras
            self.assertEqual(syn["function_entry_pcs"], ["0x80180000"])
            self.assertEqual(syn["dispatch_entry_pcs"], ["0x80180010"])
            self.assertEqual(syn["seeds"], ["0x80180010"])
            self.assertEqual(syn["dispatch_entry_segments"], {"kseg0": ["0x80180010"]})
            self.assertEqual(syn["executed_pcs"], ra["executed_pcs"])


if __name__ == "__main__":
    unittest.main()
