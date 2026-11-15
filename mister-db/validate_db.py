#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""validate_db.py - check a MisterPlex Downloader database against its payload tree.

usage: validate_db.py DB(.json|.json.zip|URL) [PAYLOAD_DIR] [--db-id ID] [--sha SHA]
                      [--remote N]   (download N payload files from their URLs and check md5; -1 = all)

Checks: JSON shape (v, db_id, timestamp, files, folders, base_files_url), every path is one
Downloader accepts (no linux/, saves/, downloader/, '..', MiSTer.ini, ...), every internal file
exists in PAYLOAD_DIR with the listed md5 and size and nothing in PAYLOAD_DIR is left out,
external files carry an https url, and the MisterPlex install set is complete.
"""
import argparse, hashlib, io, json, os, sys, urllib.request, zipfile

REQUIRED = [
    "_Utility/MiSTerPlex.rbf",
    "misterplex/cores/MiSTerPlex.rbf.md5",
    "misterplex/bin/misterplexd",
    "misterplex/bin/misterplexd-spelunkr",
    "misterplex/bin/misterplexd-boot-rc3.sh",
    "misterplex/bin/misterplexd_supervise.sh",
    "misterplex/bin/misterplex_mode.sh",
    "misterplex/bin/configure-misterplex.sh",
    "misterplex/bin/uninstall-misterplex.sh",
    "misterplex/bin/yt-dlp_linux_armv7l.zip",
    "misterplex/conf/misterplex.conf.example",
    "misterplex/yt_warm_helper.py",
    "misterplex/plexui/Roboto-Regular.ttf",
    "misterplex/linux/misterplex-cacert.pem",
    "misterplex/linux/gcast/gcast_xcode.py",
    "misterplex/linux/gcast/ffmpeg",
    "misterplex/linux/airplay/airplay_xcode.py",
    "Scripts/misterplex_setup.sh",
]
FORBIDDEN_ROOTS = {"linux", "screenshots", "savestates", "downloader", "saves"}
FORBIDDEN_FILES = {"mister", "menu.rbf", "mister.ini", "mister_alt.ini", "mister_alt_1.ini", "mister_alt_2.ini",
                   "mister_alt_3.ini", "mister.new", "downloader.ini", "downloader.sh"}

def load(src):
    if src.startswith("http://") or src.startswith("https://"):
        data = urllib.request.urlopen(src, timeout=60).read()
    else:
        data = open(src, "rb").read()
    if data[:2] == b"PK":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.endswith(".json")]
            if len(names) != 1:
                sys.exit("FAIL: zip must hold exactly one .json, has %r" % z.namelist())
            data = z.read(names[0])
    return json.loads(data)

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("db"); ap.add_argument("payload", nargs="?")
    ap.add_argument("--db-id"); ap.add_argument("--sha"); ap.add_argument("--remote", type=int, default=0)
    a = ap.parse_args()
    db = load(a.db)
    errs = []
    def err(m): errs.append(m)
    for k in ("db_id", "timestamp", "files", "folders", "base_files_url"):
        if k not in db: err("missing top-level field %s" % k)
    if db.get("v", 1) != 1: err("v must be 1, is %r" % db.get("v"))
    if a.db_id and db.get("db_id") != a.db_id: err("db_id %r != %r" % (db.get("db_id"), a.db_id))
    if not isinstance(db.get("timestamp"), int): err("timestamp must be an int")
    base = db.get("base_files_url", "")
    if not base.startswith("https://"): err("base_files_url must be https: %r" % base)
    if a.sha and a.sha not in base: err("base_files_url does not pin commit %s: %s" % (a.sha, base))
    files, folders = db.get("files", {}), db.get("folders", {})
    for p in list(files) + list(folders):
        parts = p.lower().rstrip("/").split("/")
        if p.startswith(("/", ".", "\\")) or ".." in parts or parts[0] in FORBIDDEN_ROOTS or p.lower() in FORBIDDEN_FILES:
            err("path Downloader would reject: %s" % p)
        if parts[-1] in ("plexob.rbf", "misterplex.conf"):
            err("must not ship %s" % p)
        if p.lower().startswith(("misterplex/docs", "misterplex/licenses")) or parts[-1] in (
                "license", "notice", "readme.md", "readme.txt", "release-notes.md", "db-info.txt"):
            err("non-runtime file must not ship: %s" % p)
    for p, d in files.items():
        if not isinstance(d.get("hash"), str) or len(d["hash"]) != 32: err("%s: bad hash" % p)
        if not isinstance(d.get("size"), int): err("%s: bad size" % p)
        if "url" in d and not d["url"].startswith("https://"): err("%s: url must be https" % p)
        parent = p.rsplit("/", 1)[0] + "/" if "/" in p else None
        if parent and parent.rstrip("/") not in {f.rstrip("/") for f in folders}: err("%s: parent folder not listed" % p)
    for r in REQUIRED:
        if r not in files: err("install set incomplete: %s not in db" % r)
    if a.payload:
        on_disk = set()
        for root, _, names in os.walk(a.payload):
            for n in names:
                on_disk.add(os.path.relpath(os.path.join(root, n), a.payload).replace(os.sep, "/"))
        internal = {p for p, d in files.items() if "url" not in d}
        for p in sorted(internal - on_disk): err("listed but not in payload: %s" % p)
        for p in sorted(on_disk - set(files)): err("in payload but not listed: %s" % p)
        for p in sorted(internal & on_disk):
            fp = os.path.join(a.payload, p)
            if os.path.getsize(fp) != files[p]["size"]: err("%s: size mismatch" % p)
            if md5(fp) != files[p]["hash"]: err("%s: md5 mismatch" % p)
        sumf = os.path.join(a.payload, "misterplex/cores/MiSTerPlex.rbf.md5")
        if os.path.exists(sumf) and "_Utility/MiSTerPlex.rbf" in files:
            if open(sumf).read().split()[0].lower() != files["_Utility/MiSTerPlex.rbf"]["hash"]:
                err("misterplex/cores/MiSTerPlex.rbf.md5 does not match _Utility/MiSTerPlex.rbf")
    checked = 0
    if a.remote:
        todo = sorted(files.items(), key=lambda kv: kv[1]["size"])
        if a.remote > 0: todo = todo[:a.remote]
        for p, d in todo:
            url = d.get("url") or base + p
            data = urllib.request.urlopen(url, timeout=300).read()
            if len(data) != d["size"] or hashlib.md5(data).hexdigest() != d["hash"]:
                err("remote %s: size/md5 mismatch" % url)
            checked += 1
    if errs:
        for e in errs: print("FAIL:", e)
        sys.exit(1)
    ext = sum(1 for d in files.values() if "url" in d)
    print("validate_db: OK db_id=%s files=%d (external %d) folders=%d bytes=%d remote_checked=%d" % (
        db["db_id"], len(files), ext, len(folders), sum(d["size"] for d in files.values()), checked))

if __name__ == "__main__":
    main()
