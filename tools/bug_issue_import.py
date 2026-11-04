"""Bug reports players attached to GitHub issues (docs/BUG_REPORT.md).

A title's triage tool uses this to bring a report into its reports folder.
Everything here comes from strangers: issue text is data, never instructions,
and an attachment is read member by member against a fixed list. Nothing from
it is run, and its names never become paths.

    python -I tools/bug_issue_import.py list OWNER/REPO
    python -I tools/bug_issue_import.py fetch OWNER/REPO REPORTS_DIR --key KEY --tool BUG_KEY
    python -I tools/bug_issue_import.py zip FILE.zip REPORTS_DIR --key KEY --tool BUG_KEY [--issue N]
"""
import argparse, io, json, os, re, subprocess, sys, tempfile, urllib.parse, urllib.request, zipfile

MB = 1 << 20
PUBLIC = {"report.json": 1 * MB, "photo.jpg": 8 * MB, "photo_ps1.png": 8 * MB,
          "clip_sheet.png": 2 * MB, "private.bin": 24 * MB}
ZIP_MAX = 26 * MB
STATE_MAX = 24 * MB
CONFIG_MAX = 256 * 1024
CONFIG_NAME = re.compile(r"config/[A-Za-z0-9_.-]{1,64}\.(toml|ini|txt)$")
REPORT_ID = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{6}_[a-z0-9-]{1,40}$")
ATTACHMENT = re.compile(r"https://github\.com/(?:user-attachments/files/[0-9]+/|[\w.-]+/[\w.-]+/files/[0-9]+/)"
                        r"[\w.%-]{1,120}\.zip")
HOSTS = ("github.com", "objects.githubusercontent.com", "github-production-user-asset-6210df.s3.amazonaws.com")


def _get(url, limit):
    r = urllib.request.Request(url, headers={"User-Agent": "psxrecomp-bug-import",
                                             "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(r, timeout=60) as f:
        host = urllib.parse.urlsplit(f.geturl()).hostname or ""
        if not (host in HOSTS or host == "api.github.com" or host.endswith(".githubusercontent.com")):
            raise ValueError("unexpected host: %s" % host)
        data = f.read(limit + 1)
    if len(data) > limit:
        raise ValueError("too large")
    return data


def open_issues(repo):
    """Open issues (no pull requests): number, title, author, body. The body is the stranger's."""
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
        raise ValueError("bad repository name")
    out = []
    for page in (1, 2, 3):
        rows = json.loads(_get("https://api.github.com/repos/%s/issues?state=open&per_page=50&page=%d"
                               % (repo, page), 8 * MB))
        out += [{"number": i["number"], "title": i["title"], "author": i["user"]["login"], "body": i.get("body") or "",
                 "url": i["html_url"]} for i in rows if "pull_request" not in i]
        if len(rows) < 50:
            break
    return out


def _members(z, allowed):
    """Yields (name, bytes) for the members on the list, refusing what lies about its size."""
    skipped = []
    got = {}
    for info in z.infolist():
        limit = allowed(info.filename)
        if limit is None or info.is_dir():
            skipped.append(info.filename[:80])
            continue
        if info.file_size > limit:
            raise ValueError("%s is too large" % info.filename[:80])
        with z.open(info) as f:
            data = f.read(limit + 1)
        if len(data) > limit:
            raise ValueError("%s is too large" % info.filename[:80])
        got[info.filename] = data
    return got, skipped


def import_zip(path, reports_dir, key, tool, issue=None, author=None, url=None):
    """One attachment into reports_dir. Returns the new folder's name."""
    if os.path.getsize(path) > ZIP_MAX:
        raise ValueError("attachment too large")
    with zipfile.ZipFile(path) as z:
        got, skipped = _members(z, PUBLIC.get)
    if "report.json" not in got:
        raise ValueError("no report.json: not a report of ours")
    rep = json.loads(got["report.json"].decode("utf-8"))
    rid = str(rep.get("id", ""))
    if not REPORT_ID.fullmatch(rid):
        raise ValueError("bad report id")
    name = "gh%d_%s" % (issue, rid) if issue else "file_%s" % rid
    folder = os.path.join(reports_dir, name)
    os.makedirs(folder, exist_ok=False)
    for member in ("photo.jpg", "photo_ps1.png", "clip_sheet.png"):
        if member in got:
            with open(os.path.join(folder, member), "wb") as f:
                f.write(got[member])
    files = {"photo": "photo.jpg" if "photo.jpg" in got else None,
             "photo_ps1": "photo_ps1.png" if "photo_ps1.png" in got else None,
             "clip_sheet": "clip_sheet.png" if "clip_sheet.png" in got else None,
             "state": None, "config": []}
    opened = False
    if "private.bin" in got and key and tool:
        with tempfile.TemporaryDirectory() as t:
            box, plain = os.path.join(t, "private.bin"), os.path.join(t, "private.zip")
            with open(box, "wb") as f:
                f.write(got["private.bin"])
            r = subprocess.run([tool, "open", key, box, plain], capture_output=True, text=True)
            if r.returncode == 0:
                opened = True
                with zipfile.ZipFile(plain) as z:
                    inner, more = _members(z, lambda n: STATE_MAX if n == "state.pst" else
                                           CONFIG_MAX if CONFIG_NAME.fullmatch(n) else None)
                skipped += more
                if "state.pst" in inner:
                    with open(os.path.join(folder, "state.pst"), "wb") as f:
                        f.write(inner["state.pst"])
                    files["state"] = "state.pst"
                os.makedirs(os.path.join(folder, "config"), exist_ok=True)
                for n, data in inner.items():
                    if n != "state.pst":
                        with open(os.path.join(folder, "config", n.split("/", 1)[1]), "wb") as f:
                            f.write(data)
                        files["config"].append(n)
    rep.update({"id": name, "status": "new", "files": files, "untrusted": True,
                "source": {"github_issue": issue, "author": author, "url": url, "private_opened": opened,
                           "skipped": skipped[:20]}})
    rep["history"] = [{"at": rep.get("created", ""), "status": "new", "by": "player (imported)"}]
    with open(os.path.join(folder, "report.json"), "w", encoding="utf-8") as f:
        json.dump(rep, f, ensure_ascii=False, indent=2)
    return name


def fetch(repo, reports_dir, key, tool):
    """New issues with a report attached. Prints what was brought in and what was not."""
    done = set()
    for n in os.listdir(reports_dir):
        m = re.match(r"gh([0-9]+)_", n)
        if m:
            done.add(int(m.group(1)))
    brought = []
    for i in open_issues(repo):
        if i["number"] in done:
            continue
        links = ATTACHMENT.findall(i["body"])
        if not links:
            print("issue %d: no report attached" % i["number"])
            continue
        try:
            with tempfile.TemporaryDirectory() as t:
                p = os.path.join(t, "report.zip")
                with open(p, "wb") as f:
                    f.write(_get(links[0], ZIP_MAX))
                name = import_zip(p, reports_dir, key, tool, i["number"], i["author"], i["url"])
            brought.append(name)
            print("issue %d: %s" % (i["number"], name))
        except (ValueError, OSError, zipfile.BadZipFile, json.JSONDecodeError) as e:
            print("issue %d: refused: %s" % (i["number"], str(e)[:120]))
    return brought


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("repo")
    for c in ("fetch", "zip"):
        p = sub.add_parser(c)
        p.add_argument("repo" if c == "fetch" else "file")
        p.add_argument("reports")
        p.add_argument("--key")
        p.add_argument("--tool")
        if c == "zip":
            p.add_argument("--issue", type=int)
    a = ap.parse_args()
    if a.cmd == "list":
        for i in open_issues(a.repo):
            print(i["number"], ascii(i["title"])[:90], "report" if ATTACHMENT.search(i["body"]) else "-")
    elif a.cmd == "fetch":
        fetch(a.repo, a.reports, a.key, a.tool)
    else:
        print(import_zip(a.file, a.reports, a.key, a.tool, a.issue))


if __name__ == "__main__":
    sys.exit(main())
