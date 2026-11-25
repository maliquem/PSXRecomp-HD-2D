#!/usr/bin/env python3
"""Keep the development overlay cache down to what the golden check uses.

A dev cache collects every capture of weeks of testing (BoF4: 12k captures, 4k DLLs) and a
codegen-hash change recompiles all of it (hours). The player never needs that: the setup starts
from the game's base list and the rest compiles on demand. This tool keeps only the protected set:

  mark     protected list from the DLLs the golden scenes loaded (one path per line, '#' lines
           ignored: the process module list of each scene): every function entry of each region
           and fragment DLL, one small record of each image behind them, and every record of the
           main overlay_captures.json (the base list, never dropped).
  prune    show (default) or delete (--apply) everything else: records in
           overlay_captures.json.d/ and the DLL files of the current-hash cache dirs.
           --drop-other-hashes also deletes the cache dirs of other codegen hashes.
  compile  rebuild exactly the listed DLLs (after a codegen-hash change, or into a test copy to
           prove the list): the regions with the game's own compile line, each from one record
           whose only roots are the listed entries; then each fragment with compile_overlays'
           fragment builder. Run prune --apply after it: extra DLLs change which one the runtime
           takes.

Why exact DLLs: the runtime takes, among the DLLs whose functions match the live bytes, the one
with the fewest functions, and another split of the same code changes the cycle count. A fresh
compile does not repeat the dev cache: compile_overlays walks functions only from roots, unions the
demands of every capture of an image, and the cache grew run by run. So the list carries each
DLL's entries; the region DLL's name is its image's crc, a fragment's the crc of its function
identities, so a rebuilt DLL with the listed name is the same split. Keeping whole capture images
instead (thousands of records) rebuilds ten times the DLLs the golden opens.
The list holds addresses and CRCs only (no game bytes), so it is versioned in the game's repository:

  {"schema": "psxrecomp overlay keep v5",
   "records":   ["0016E000_0B1C2D3E:5A1F03C2", ...],  image (phys load address + crc32 of the
                                                       bytes) : crc32 of the record's pc lists
   "dlls":      ["0018E000_CE5CB768", "seg-kuseg/00002000_2DFF477B", ...],   what the golden opened
   "entries":   {"<seg/>PHYS_CRC": ["0018E010", ...]},   region DLL function entries (phys)
   "fragments": {"<seg/>PHYS_KEY": {"images": [...],     images holding all its functions (one of
                                                          them built it: a shorter capture ends a
                                                          function early, so each is tried)
                                    "provenance": "orphan-v1" | "hosted-v1" | "",
                                    "entries": ["0018F2A4", ...],
                                    "hosts": {target: [host, crc, len]}}}   hosted-v1 only

A DLL is <phys>_<key>; its .ranges manifest lists each function's entry, code crc and ranges.

    python overlay_keep.py mark    --build <dir> --loaded mods.txt [...] --out keep.json
    python overlay_keep.py prune   --build <dir> --keep keep.json [--apply] [--drop-other-hashes]
    python overlay_keep.py compile --build <dir> --keep keep.json [--jobs N] [--flavor 2]

<dir> is the folder of the exe: overlay_captures.json(.d), cache/, overlay_toolchain/, game.toml.
"""
import argparse, base64, binascii, json, os, re, shutil, subprocess, sys
from pathlib import Path

SCHEMA = "psxrecomp overlay keep v5"
PHYS = 0x1FFFFFFF
DLL_RE = re.compile(r"^([0-9A-F]{8})_([0-9A-F]{8})\.(dll|so)$")
PC_FIELDS = ("executed_pcs", "dispatch_entry_pcs", "function_entry_pcs", "seeds")
# compile_overlays walks functions only from these; an executed pc alone is excluded (OBSERVED_PC_ONLY)
ROOT_FIELDS = ("dispatch_entry_pcs", "function_entry_pcs", "seeds", "static_discovery_entry_pcs")


# ------------------------------------------------------------------ inputs
def capture_files(build):
    return Path(build) / "overlay_captures.json", sorted(Path(build).glob("overlay_captures.json.d/*.json"))


def read_caps(path):
    d = json.loads(Path(path).read_text(encoding="utf-8"))
    return d if isinstance(d, list) else [d]


def image_key(cap):
    data = base64.b64decode(cap["bytes_b64"])
    return "%08X_%08X" % (int(cap["load_addr"], 16) & PHYS, binascii.crc32(data) & 0xFFFFFFFF), data


def record_id(cap):
    pcs = json.dumps([cap.get(f, []) for f in PC_FIELDS]).encode()
    return "%s:%08X" % (image_key(cap)[0], binascii.crc32(pcs) & 0xFFFFFFFF)


def record_pcs(cap):
    return {int(x, 16) & PHYS for f in PC_FIELDS for x in cap.get(f, [])}


def load_records(build):
    """images {key: (load address, bytes)}, records {id: (image key, pcs)}, ids of the main list."""
    main, hist = capture_files(build)
    imgs, recs, base = {}, {}, set()
    for f in [main] + hist:
        if not f.exists():
            continue
        for cap in read_caps(f):
            k, data = image_key(cap)
            imgs.setdefault(k, (int(cap["load_addr"], 16), data))
            rid = record_id(cap)
            recs.setdefault(rid, (k, record_pcs(cap)))
            if f == main:
                base.add(rid)
    return imgs, recs, base


def codegen_hash(build):
    h = Path(build) / "overlay_toolchain" / "include" / "overlay_codegen_hash.h"
    return int(re.search(r"PSX_OVERLAY_CODEGEN_HASH\s+0x([0-9a-fA-F]+)", h.read_text()).group(1), 16)


def cache_dirs(build):
    """[(hash, dir)] of every cache/<game>/<compiler>/<abi>/cg<ver>_<hash>_<config>_f<flavor>."""
    out = []
    for d in Path(build).glob("cache/*/*/*/cg*_*"):
        m = re.match(r"cg\d+_([0-9a-f]{8})_", d.name)
        if d.is_dir() and m:
            out.append((int(m.group(1), 16), d))
    return out


def dll_name(path):
    """'seg-kuseg/00002000_2DFF477B' (KSEG0 DLLs sit at the top of the cache dir)."""
    p = Path(path.replace("\\", "/"))
    stem = p.name.split(".")[0]
    return (p.parent.name + "/" + stem) if p.parent.name.startswith("seg-") else stem


def read_ranges(path):
    """[(entry, function crc, [(addr, length)])] of a .ranges manifest."""
    funcs = []
    for ln in Path(path).read_text().splitlines():
        p = ln.split()
        if p and p[0] == "F":
            funcs.append((int(p[1], 16) & PHYS, int(p[2], 16), []))
        elif p and p[0] == "R" and funcs:
            funcs[-1][2].append((int(p[1], 16), int(p[2], 16)))
    return funcs


def func_matches(crc, ranges, load, data):
    base, c = load & PHYS, 0
    for addr, n in ranges:
        off = (addr & PHYS) - base
        if off < 0 or off + n > len(data):
            return False
        c = binascii.crc32(data[off:off + n], c)
    return (c & 0xFFFFFFFF) == crc


# ------------------------------------------------------------------ mark
SEGS = {"": "kseg0", "seg-kuseg/": "kuseg", "seg-kseg1/": "kseg1"}
SEG_BASE = {"kseg0": 0x80000000, "kuseg": 0x00000000, "kseg1": 0xA0000000}


def cmd_mark(a):
    imgs, recs, base = load_records(a.build)
    loaded = {}
    for f in a.loaded:
        for ln in Path(f).read_text(encoding="utf-8").splitlines():
            ln = ln.strip()
            if ln and not ln.startswith("#") and DLL_RE.match(Path(ln.replace("\\", "/")).name):
                loaded[dll_name(ln)] = ln
    by_img = {}
    for rid, (k, _) in recs.items():
        by_img.setdefault(k, []).append(rid)
    # entries["<seg/>PHYS_CRC"]: all F entries of a region DLL (named by its image): as roots they
    # fix every function's bounds, so the shard comes out the same.
    # fragments["<seg/>PHYS_KEY"]: a fragment DLL's image, provenance and F entries. Its name is the
    # crc of its function identities, which a fresh compile seldom repeats (compile_overlays unions
    # the demands of every capture of an image, and the dev cache grew run by run), so it is rebuilt
    # from exactly these entries. The runtime takes the smallest DLL that holds a function, and
    # another split of the same code changes the cycle count.
    entries, fragments, orphans = {}, {}, []
    for name, path in sorted(loaded.items()):
        seg = name[:name.index("/") + 1] if "/" in name else ""
        stem = name[len(seg):]
        rp = os.path.splitext(path)[0] + ".ranges"
        funcs = read_ranges(rp) if os.path.exists(rp) else []
        if not funcs:
            orphans.append(name)
        elif stem in by_img:
            entries[seg + stem] = ["%08X" % e for e in sorted({e for e, _, _ in funcs})]
        else:
            src = sorted(k for k in by_img if k.startswith(stem[:9])
                         and all(func_matches(c, r, *imgs[k]) for _, c, r in funcs))
            if not src:
                orphans.append(name)
                continue
            prov = next((ln.split()[-1] for ln in Path(rp).read_text().splitlines()
                         if ln.startswith("# psxrecomp overlay provenance ")), "")
            fragments[name] = {"images": src, "provenance": prov,
                               "entries": ["%08X" % e for e in sorted({e for e, _, _ in funcs})]}
            if prov == "hosted-v1":
                # a hosted shard is built from its alias targets (an F whose range starts at another
                # F, its host) plus the host's identity, not from dispatch roots
                ident = {e: ["%08X" % e, "%08X" % c, "%X" % r[0][1]] for e, c, r in funcs
                         if len(r) == 1 and r[0][0] & PHYS == e}
                hosts = {"%08X" % e: ident[r[0][0] & PHYS] for e, c, r in funcs
                         if len(r) == 1 and r[0][0] & PHYS != e and r[0][0] & PHYS in ident}
                # every other F of a hosted shard was a host whose target failed the block-leader
                # proof (no alias kept); host+4 is not a block leader, so it fails the same way
                served = {h[0] for h in hosts.values()}
                hosts.update({"%08X" % (e + 4): v for e, v in ident.items() if v[0] not in served})
                fragments[name]["hosts"] = hosts
    keep = set(base)
    for k in {k.split("/")[-1] for k in entries} | {i for f in fragments.values() for i in f["images"]}:
        if not any(r in base for r in by_img[k]):
            keep.add(min(by_img[k], key=lambda r: (len(recs[r][1]), r)))   # fewest extra fragments
    out = {"schema": SCHEMA, "records": sorted(keep), "dlls": sorted(loaded),
           "entries": entries, "fragments": fragments}
    Path(a.out).write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")
    print("DLLs abertas pela dourada: %d (%d regiões, %d fragmentos); registros protegidos: %d "
          "(lista base %d) de %d" % (len(loaded), len(entries), len(fragments), len(keep), len(base), len(recs)))
    if orphans:
        # no capture behind it: compile cannot rebuild it, the golden proof will show
        print("sem captura de origem (%d): %s" % (len(orphans), " ".join(orphans)))
    return 0


# ------------------------------------------------------------------ prune
def load_keep(path):
    k = json.loads(Path(path).read_text(encoding="utf-8"))
    if k.get("schema") != SCHEMA:
        raise SystemExit("%s: schema %r, esperado %r" % (path, k.get("schema"), SCHEMA))
    return set(k["records"]), set(k["dlls"]), k["entries"], k["fragments"]


def file_key(name):
    """('dll', '<phys>_<key>') for .dll/.so/.ranges/.dll.pair-lock, ('c', '<key>') for
    <key>_patched.c / <key>_fragment_patched.c, None for anything else (left alone)."""
    m = re.match(r"^([0-9A-F]{8}_[0-9A-F]{8})\.(dll|so|ranges)(\.pair-lock)?$", name)
    if m:
        return ("dll", m.group(1))
    m = re.match(r"^([0-9A-F]{8})_(fragment_)?patched\.c$", name)
    return ("c", m.group(1)) if m else None


def cmd_prune(a):
    keep_recs, keep_dlls, _, _ = load_keep(a.keep)
    main, hist = capture_files(a.build)              # the main list is never touched
    have = {record_id(c) for f in ([main] if main.exists() else []) + hist for c in read_caps(f)}
    if keep_recs - have:     # without them a new hash could not rebuild the protected DLLs
        raise SystemExit("%d registros da lista não estão nas capturas de %s (ex.: %s); repor antes de podar"
                         % (len(keep_recs - have), a.build, min(keep_recs - have)))
    caps_out = files_gone = cap_bytes = caps_left = 0
    rewrites = []
    for f in hist:
        caps = read_caps(f)
        kept = [c for c in caps if record_id(c) in keep_recs]
        caps_left += len(kept)
        if len(kept) < len(caps):
            caps_out += len(caps) - len(kept)
            if not kept:
                files_gone += 1
                cap_bytes += f.stat().st_size
            rewrites.append((f, kept))
    h = codegen_hash(a.build)
    keys_c = {n.split("/")[-1].split("_")[1] for n in keep_dlls}
    removes, drop_dirs, nbytes = [], [], 0
    for hh, d in cache_dirs(a.build):
        if hh != h:
            if a.drop_other_hashes:
                drop_dirs.append(d)
                nbytes += sum(p.stat().st_size for p in d.rglob("*") if p.is_file())
            continue
        for p in d.rglob("*"):
            fk = file_key(p.name) if p.is_file() else None
            if fk is None:
                continue
            seg = p.parent.name + "/" if p.parent.name.startswith("seg-") else ""
            if (fk[1] not in keys_c) if fk[0] == "c" else (seg + fk[1] not in keep_dlls):
                removes.append(p)
                nbytes += p.stat().st_size
    ndll = sum(1 for p in removes if p.suffix in (".dll", ".so"))
    print("capturas do histórico: saem %d registros (%d arquivos .d apagados, %.0f MB; %d reescritos); "
          "ficam %d" % (caps_out, files_gone, cap_bytes / 2**20, len(rewrites) - files_gone, caps_left))
    print("cache %08x: saem %d arquivos (%d DLLs); pastas de outros hashes: %d; total %.0f MB"
          % (h, len(removes), ndll, len(drop_dirs), nbytes / 2**20))
    for d in drop_dirs:
        print("  sai a pasta %s" % d)
    if not a.apply:
        print("(só mostrado: --apply apaga)")
        return 0
    for f, kept in rewrites:
        if kept:
            tmp = f.with_name(f.name + ".tmp")
            tmp.write_text(json.dumps(kept, indent=2), encoding="utf-8")
            os.replace(tmp, f)
        else:
            f.unlink()
    for p in removes:
        p.unlink()
    for d in drop_dirs:
        shutil.rmtree(d)
    print("apagado.")
    return 0


# ------------------------------------------------------------------ compile
def protected_captures(build, keep_recs, entries, callable_at):
    """(regions, by_img). by_img: one record per image of the main list and the protected history
    records. regions: one per entries key, a copy of a record of that image whose only roots are
    the listed F entries, split as compile_overlays splits a prior manifest's (callable_at(data,
    load, pc): function entry, else dispatch entry), in that segment."""
    main, hist = capture_files(build)
    caps = read_caps(main) if main.exists() else []
    seen = {record_id(c) for c in caps}
    for f in hist:
        for c in read_caps(f):
            rid = record_id(c)
            if rid in keep_recs and rid not in seen:
                seen.add(rid)
                caps.append(c)
    if keep_recs - seen:
        raise SystemExit("%d registros da lista não estão nas capturas de %s (ex.: %s)"
                         % (len(keep_recs - seen), build, min(keep_recs - seen)))
    by_img = {}
    for c in caps:
        by_img.setdefault(image_key(c)[0], c)
    regions = []
    for key, ents in sorted(entries.items()):
        seg = SEGS[key[:key.index("/") + 1] if "/" in key else ""]
        src = by_img[key.split("/")[-1]]
        data = base64.b64decode(src["bytes_b64"])
        load = (int(src["load_addr"], 16) & PHYS) | 0x80000000
        fe, de = set(), set()
        for e in (int(x, 16) for x in ents):
            (fe if callable_at(data, load, e | 0x80000000) else de).add(e)
        c = {k: v for k, v in src.items() if k not in ROOT_FIELDS}
        c["function_entry_pcs"] = ["0x%08X" % (x | 0x80000000) for x in sorted(fe)]
        c["dispatch_entry_pcs"] = ["0x%08X" % (x | 0x80000000) for x in sorted(de)]
        c["seeds"] = list(c["dispatch_entry_pcs"])
        c["dispatch_entry_segments"] = {seg: ["0x%08X" % (x | SEG_BASE[seg]) for x in sorted(de)]}
        regions.append(c)
    return regions, by_img


def build_fragments(co, b, fragments, by_img, flavor, jobs):
    """Build each listed fragment DLL from its own entries, with compile_overlays' fragment
    builder (one dispatch root per entry, the DLL's provenance). Returns the names that came out
    under another name or failed."""
    import tomllib
    from concurrent.futures import ThreadPoolExecutor
    os.environ["PSX_OVERLAY_ARCH_ABI"] = "win-x64"     # cache_arch_abi() reads it
    tk = b / "overlay_toolchain"
    args = argparse.Namespace(
        recompiler=str(tk / "psxrecomp-game.exe"), game_toml=str(b / "game.toml"), project_root=str(tk),
        runtime_include=str(tk / "include"), flavor=flavor, cps=True, compiler="gcc", tcc="tcc",
        gcc=(tk / "compiler.txt").read_text(encoding="utf-8").strip(), force=False)
    co.load_dispatch_preamble(args.runtime_include)    # main() does this before any shard build
    toml = tomllib.loads((b / "game.toml").read_text(encoding="utf-8-sig"))
    cache =os.path.join(str(b / "cache"), toml.get("game", {}).get("id", "UNKNOWN"), "gcc",
                         co.cache_arch_abi(), co.cache_tag(args.runtime_include, args.recompiler,
                                                           args.game_toml, flavor))
    env = dict(os.environ, PSX_CPS="1")

    def one(item):
        name, f = item
        seg = SEGS[name[:name.index("/") + 1] if "/" in name else ""]
        va = lambda x: int(x, 16) | SEG_BASE[seg]
        # the cache keeps neither the image that built it (a shorter capture ends a function early)
        # nor a hosted shard's host reason (two seed markers), so each is tried: the name is the proof
        status = "sem imagem"
        for image in f["images"]:
            cap = by_img[image]
            if cap.get("producer") or cap.get("producer_ranges"):
                # shortcut: no capture of BoF4/WA2/FM2 has producer ranges; add them here when one does
                status = "registro com producer_ranges"
                continue
            data = base64.b64decode(cap["bytes_b64"])
            phys = int(cap["load_addr"], 16) & PHYS
            load, size = phys | SEG_BASE[seg], int(cap["size"])
            for reason in ([None] if "hosts" not in f else ["DIRECT_JAL_TARGET", "DISPATCH_ENTRY"]):
                specs = reason and {
                    va(t): {"host": va(h), "owner_identity": (va(h), int(c, 16), ((va(h), int(n, 16)),)),
                            "host_reason": reason} for t, (h, c, n) in f["hosts"].items()}
                with co.image_segment(co.segment_of(load)):
                    ids, status = co.compile_fragment_batch(
                        list(specs) if specs else [va(x) for x in f["entries"]], data, load, size, phys,
                        co.segment_cache_dir(cache, co.segment_of(load)), args, env, toml,
                        hosted_owners=specs, manifest_provenance=f["provenance"] or None,
                        guard_bytes=co.capture_guard_bytes(cap, size, name))
                if ids is None:
                    continue
                status = "saiu %08X_%08X" % (phys, co.fragment_shard_key(ids, f["provenance"] or None))
                if status.endswith(name.split("/")[-1]):
                    return None, None
        return name, status

    with ThreadPoolExecutor(jobs or 1) as ex:
        return [r for r in ex.map(one, sorted(fragments.items())) if r[0]]


def cmd_compile(a):
    b = Path(a.build).resolve()
    tk = b / "overlay_toolchain"
    sys.path.insert(0, str(tk))
    import compile_overlays as co   # the build's own copy
    recs, dlls, entries, fragments = load_keep(a.keep)
    regions, by_img = protected_captures(b, recs, entries, co._callable_legacy_seed)
    env = dict(os.environ, PSX_OVERLAY_FLAVOR=str(a.flavor), PSX_OVERLAY_ARCH_ABI="win-x64",
               PSX_OVERLAY_CACHE_DIR=str(b / "cache"))
    env.pop("PSX_OVERLAY_CAPTURES", None)
    merged = b / "overlay_keep_regions.json"
    merged.write_text(json.dumps(regions), encoding="utf-8")
    print("regiões: %d" % len(regions), flush=True)
    # the same line as the games' own full compile (WA2/FM2 tools/compile_areas.py)
    cmd = [str(tk / "python" / "python.exe"), str(tk / "compile_overlays.py"),
           "--captures", str(merged), "--game-toml", str(b / "game.toml"),
           "--recompiler", str(tk / "psxrecomp-game.exe"), "--runtime-include", str(tk / "include"),
           "--project-root", str(tk), "--out-dir", str(b / "cache"), "--cps",
           "--compiler", "gcc", "--gcc", (tk / "compiler.txt").read_text(encoding="utf-8").strip(),
           "--flavor", str(a.flavor)]
    if a.jobs:
        cmd += ["--jobs", str(a.jobs)]
    rc = subprocess.run(cmd, env=env).returncode
    print("fragmentos: %d" % len(fragments), flush=True)
    bad = build_fragments(co, b, fragments, by_img, a.flavor, a.jobs)
    have = set()
    for _, d in cache_dirs(b):
        if _ == codegen_hash(b):
            have |= {dll_name(str(p)) for p in d.rglob("*.dll")}
    missing = sorted(set(dlls) - have)
    print("DLLs da lista no cache: %d de %d" % (len(dlls) - len(missing), len(dlls)))
    for n, why in bad:
        print("  fragmento %s: %s" % (n, why))
    for n in missing:
        print("  falta %s" % n)
    return rc or (1 if missing else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("mark")
    m.add_argument("--build", required=True)
    m.add_argument("--loaded", nargs="+", required=True)
    m.add_argument("--out", required=True)
    p = sub.add_parser("prune")
    p.add_argument("--build", required=True)
    p.add_argument("--keep", required=True)
    p.add_argument("--apply", action="store_true")
    p.add_argument("--drop-other-hashes", action="store_true")
    c = sub.add_parser("compile")
    c.add_argument("--build", required=True)
    c.add_argument("--keep", required=True)
    c.add_argument("--jobs", type=int)
    c.add_argument("--flavor", type=int, default=2)
    a = ap.parse_args()
    return {"mark": cmd_mark, "prune": cmd_prune, "compile": cmd_compile}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
