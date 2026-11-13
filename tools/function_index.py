#!/usr/bin/env python3
"""Index of the framework's functions: docs/FUNCTIONS.md.

One section per source file (runtime/, recompiler/), one line per function
defined at file level: name, line, what it does. Locations come from the
code every run; descriptions are kept from the existing index (and from any
extra part files given), so a rerun after a change only adds the new
functions, with the comment above them as a first description.

  python tools/function_index.py                 # refresh docs/FUNCTIONS.md
  python tools/function_index.py --split 8 DIR   # write DIR/part_N.md to fill
  python tools/function_index.py --keep DIR/*.md # refresh, taking text from parts
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "FUNCTIONS.md"
SCOPE = ("runtime/src/", "runtime/include/", "recompiler/src/", "recompiler/include/")
# recompiler/lib is third-party (ELFIO, fmt, rabbitizer, toml11): not indexed
EXT = (".c", ".cpp", ".h", ".hpp", ".inc")
NOT_FUNC = {"if", "for", "while", "switch", "return", "sizeof", "else", "do",
            "case", "defined", "decltype", "static_assert", "alignof"}
HEAD = re.compile(r"^(?!\s)(?!#)(?!//)(?!/\*)(?!\})[^;={}]*?\b([A-Za-z_]\w*(?:::~?\w+)*)\s*\(")
ENTRY = re.compile(r"^- `([^`]+)` \(L(\d+)\)(?: — (.*))?$")
HEADER = "# Framework functions\n\n" \
    "Every function defined at file level in runtime/ and recompiler/, by file: name, line, what it does.\n" \
    "Look here first to find what a function does, how it is called and where it lives; then open the file at the line.\n" \
    "Lines are refreshed by `python tools/function_index.py`; a commit that adds, removes or changes what a function\n" \
    "does reruns it and writes the new descriptions in the same commit.\n"


def files():
    out = subprocess.run(["git", "ls-files", *SCOPE], cwd=ROOT, capture_output=True,
                         text=True, check=True).stdout.split()
    return [f for f in out if f.endswith(EXT) and "generated" not in f]


def comment_above(lines, i):
    got = []
    j = i - 1
    while j >= 0 and lines[j].strip().startswith("template"):
        j -= 1
    while j >= 0:
        s = lines[j].strip()
        if not (s.startswith(("//", "/*", "*")) or s.endswith("*/")):
            break
        got.insert(0, s)
        j -= 1
    text = " ".join(re.sub(r"^(/\*+|\*+/?|//+)\s*|\s*\*+/$", "", s) for s in got).strip()
    text = re.sub(r"\s+", " ", text)
    m = re.match(r"(.{20,}?[.!?])(\s|$)", text)
    text = m.group(1) if m else text
    return text[:200].replace("`", "'")


def scan(path):
    lines = (ROOT / path).read_text(encoding="utf-8", errors="replace").splitlines()
    found = {}
    for i, line in enumerate(lines):
        m = HEAD.match(line)
        if not m:
            continue
        name = m.group(1)
        base = name.split("::")[-1]
        if base in NOT_FUNC or base.isupper() or name in found:
            continue
        # a definition: '{' comes before ';' within a few lines
        tail = " ".join(lines[i:i + 8])
        tail = tail[tail.find("(", m.start(1)):]
        b, s = tail.find("{"), tail.find(";")
        if b < 0 or (0 <= s < b):
            continue
        found[name] = (i + 1, comment_above(lines, i))
    return found


def declared(all_files):
    """The comment above each function declared in a header (public API docs)."""
    docs = {}
    for f in all_files:
        if not f.endswith((".h", ".hpp")):
            continue
        lines = (ROOT / f).read_text(encoding="utf-8", errors="replace").splitlines()
        for i, line in enumerate(lines):
            m = HEAD.match(line)
            if m and line.rstrip().endswith(";") and m.group(1) not in docs:
                c = comment_above(lines, i)
                if c:
                    docs[m.group(1)] = c
    return docs


def read_known(paths):
    known, summary = {}, {}
    for p in paths:
        p = Path(p)
        if not p.is_file():
            continue
        cur = None
        for line in p.read_text(encoding="utf-8").splitlines():
            if line.startswith("### "):
                cur = line[4:].strip()
                continue
            if cur is None:
                continue
            m = ENTRY.match(line)
            if m:
                if m.group(3):
                    known[(cur, m.group(1))] = m.group(3).strip()
            elif line.strip() and not line.startswith("#") and line.strip() != "(what this file is for)":
                summary[cur] = line.strip()
    return known, summary


def build(known, summary):
    parts = []
    all_files = files()
    decl = declared(all_files)
    for f in all_files:
        funcs = scan(f)
        if not funcs:
            continue
        sec = [f"### {f}", "", summary.get(f, "(what this file is for)"), ""]
        for name, (ln, cmt) in funcs.items():
            text = known.get((f, name)) or cmt or decl.get(name, "")
            sec.append(f"- `{name}` (L{ln})" + (f" — {text}" if text else ""))
        parts.append("\n".join(sec) + "\n")
    return parts


def main(argv):
    keep = [OUT]
    if argv[:1] == ["--split"]:
        n, out = int(argv[1]), Path(argv[2])
        out.mkdir(parents=True, exist_ok=True)
        known, summary = read_known(keep)
        secs = build(known, summary)
        size = sum(s.count("\n") for s in secs) / n
        chunk, k, acc = [], 1, 0
        for s in secs:
            chunk.append(s)
            acc += s.count("\n")
            if acc >= size and k < n:
                (out / f"part_{k}.md").write_text("\n".join(chunk), encoding="utf-8")
                chunk, k, acc = [], k + 1, 0
        (out / f"part_{k}.md").write_text("\n".join(chunk), encoding="utf-8")
        print(f"{len(secs)} files in {k} parts")
        return
    if argv[:1] == ["--keep"]:
        keep += argv[1:]
    known, summary = read_known(keep)
    secs = build(known, summary)
    OUT.write_text(HEADER + "\n" + "\n".join(secs), encoding="utf-8", newline="\n")
    n = sum(s.count("\n- `") for s in secs)
    empty = sum(1 for s in secs for l in s.splitlines() if ENTRY.match(l) and " — " not in l)
    print(f"{len(secs)} files, {n} functions, {empty} without description -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main(sys.argv[1:])
