#!/bin/sh
# The release README Contents list is the tarball file list. Every path it
# names is a file in the archive, and every archived file is named there.
# FFmpeg is libav inside bin/misterplexd, so the archive has no standalone
# ffmpeg program. The package builds misterplexd from this source.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
PKG="$ROOT/scripts/package_release.sh"
REL="$ROOT/docs/release.md"
fail() { echo "FAIL $*" >&2; exit 1; }

[ -f "$PKG" ] || fail "missing package_release.sh"
[ -f "$REL" ] || fail "missing docs/release.md"
[ -f "$ROOT/packaging/licenses/ffmpeg/COPYING.LGPLv2.1" ] || fail "missing vendored FFmpeg LGPL text"
[ -f "$ROOT/packaging/licenses/openssl/LICENSE.txt" ] || fail "missing vendored OpenSSL license"
grep -q 'LGPL-2.1-or-later' "$PKG" || fail "package_release does not name LGPL-2.1-or-later"
grep -q 'scp -r bin scripts docs licenses' "$PKG" || fail "package_release dropped the licenses scp line"
grep -q 'cut -d" " -f1' "$PKG" || fail "README md5 check does not use cut"
grep -q 'awk "{print \$1}"' "$PKG" && fail "README install still emits awk print \$1"
grep -F -q 'static armhf FFmpeg 7.0.2' "$PKG" && fail "package_release still describes a GPLv3 ffmpeg program"
grep -F -q '| `bin/ffmpeg` |' "$REL" && fail "docs/release.md still lists bin/ffmpeg"
grep -q 'LGPL-2.1-or-later' "$REL" || fail "docs/release.md does not describe LGPL libav"
grep -q 'Apache-2.0' "$REL" || fail "docs/release.md does not name the OpenSSL license"
grep -q 'GNU General Public License version 3' "$REL" || fail "docs/release.md does not name GPLv3"
grep -q 'scp -r bin scripts docs licenses' "$REL" || fail "docs/release.md dropped the licenses scp line"
grep -q 'probes bin/ffmpeg' "$ROOT/assets/misterplex.conf.example" && fail "conf example still probes bin/ffmpeg"
grep -q 'FFmpeg 7.0.2' "$ROOT/docs/release-notes-v0.4.0.md" && fail "v0.4.0 notes still name FFmpeg 7.0.2"
grep -q 'github.com/flynnsbit/misterplex-cast/releases/tag/rc5' "$PKG" || fail "package_release dropped the published-source line"
grep -q 'MPX_SOURCE_MODE' "$PKG" && fail "package_release still has MPX_SOURCE_MODE"
grep -q 'MPX_SOURCE_CONTACT' "$PKG" && fail "package_release still has MPX_SOURCE_CONTACT"
grep -q 'MPX_SOURCE_INTERNAL' "$PKG" && fail "package_release still has MPX_SOURCE_INTERNAL"
mkdir -p "$ROOT/build"
if env -u PLEXOB_RBF_MD5 PACKAGE_REUSE_ARM_BIN=1 bash "$PKG" >"$ROOT/build/srcmode-reuse.out" 2>&1; then
  fail "PACKAGE_REUSE_ARM_BIN=1 was accepted"
fi
grep -q 'PACKAGE_REUSE_ARM_BIN=1 cannot ship' "$ROOT/build/srcmode-reuse.out" || fail "reuse was not refused"
grep -q 'Building arm' "$ROOT/build/srcmode-reuse.out" && fail "reuse refusal reached the daemon build"

ARM="$ROOT/build/arm/misterplexd"
RBF="${PLEXOB_RBF:-$ROOT/release_artifacts/plexob-yc-0b59c11a/PlexOB.rbf}"
[ -f "$RBF" ] || fail "missing seed-10 PlexOB.rbf (set PLEXOB_RBF)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
YTDLP_ZIP=""
for cand in \
  /tmp/mpx-dist-63d7b2f/stage-misterplex/linux/yt-dlp_linux_armv7l.zip \
  "$ROOT/build/deps/yt-dlp_linux_armv7l.zip"
do
  if [ -f "$cand" ]; then
    YTDLP_ZIP=$cand
    break
  fi
done
if [ -n "$YTDLP_ZIP" ]; then
  YTDLP_SUM=$(dirname "$YTDLP_ZIP")/SHA2-256SUMS
  [ -f "$YTDLP_SUM" ] || fail "missing $YTDLP_SUM"
  export YTDLP_ZIP_URL="file://$YTDLP_ZIP"
  export YTDLP_SUM_URL="file://$YTDLP_SUM"
else
  export YTDLP_ZIP_URL="file:///no/such/yt-dlp.zip"
  export YTDLP_SUM_URL="file:///no/such/SHA2-256SUMS"
fi

unset PACKAGE_REUSE_ARM_BIN MPX_SOURCE_MODE MPX_SOURCE_CONTACT MPX_SOURCE_INTERNAL
export PLEXOB_RBF="$RBF"
export VERSION="readme-paths"
export OUT_DIR="$work"
if ! bash "$PKG" >"$work/package.log" 2>&1; then
  cat "$work/package.log" >&2
  fail "package_release failed"
fi
grep -q 'Building arm misterplexd' "$work/package.log" || fail "package did not build misterplexd"
grep -q 'Reusing arm misterplexd' "$work/package.log" && fail "package reused a daemon"
after=$(md5sum "$ARM" | awk '{print $1}')

TAR="$work/misterplex-readme-paths.tar.gz"
[ -f "$TAR" ] || fail "missing $TAR"
python3 - "$TAR" "$after" "$ROOT/cores/PlexOB.rbf.md5" <<'PY'
import hashlib, re, subprocess, sys
tar_path, daemon_md5, pin_file = sys.argv[1:]
pin = open(pin_file).read().strip().lower()
members = subprocess.check_output(["tar", "-tzf", tar_path], text=True).splitlines()
if not members:
    sys.exit("empty tarball")
prefix = members[0].split("/", 1)[0]
files = []
for member in members:
    if not member.startswith(prefix + "/"):
        sys.exit("unexpected member " + member)
    rel = member[len(prefix) + 1:]
    if not rel or rel.endswith("/"):
        continue
    files.append(rel)
files = sorted(files)
if "bin/ffmpeg" in files:
    sys.exit("tarball contains bin/ffmpeg")

def member(rel):
    return subprocess.check_output(["tar", "-xOf", tar_path, prefix + "/" + rel])

readme = member("README.txt").decode()
install = member("docs/INSTALL.txt").decode()
if readme != install:
    sys.exit("INSTALL.txt differs from README.txt")
if "GNU General Public License version 3" not in readme:
    sys.exit("README missing GNU General Public License version 3")
published_gen = "source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later)."
published_op = "source is published at https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5 (GPL-3.0-or-later)."
if published_gen not in readme:
    sys.exit("README missing the published-source line")
if not readme.startswith("MiSTerPlex release package\nversion: rc5\n"):
    sys.exit("README.txt version line is not rc5")
if "Boot (rc5)" not in readme or "Boot (rc4)" in readme or "Boot (rc3)" in readme:
    sys.exit("README.txt boot heading is not Boot (rc5)")
if "misterplexd-boot-rc3.sh" not in readme:
    sys.exit("README.txt dropped the boot hook filename")
if "Corresponding source" in readme:
    sys.exit("README still has a corresponding-source offer")
index = member("licenses/README.md").decode()
if published_gen not in index:
    sys.exit("licenses/README.md missing the published-source line")
if any(name.endswith("-src.tar.gz") for name in files):
    sys.exit("binary tarball contains a source archive")
if readme.find("\nLicense\n") > readme.find("\nContents\n"):
    sys.exit("License section is not before Contents")
lines = readme.splitlines()
i = lines.index("Contents")
listed = []
for line in lines[i + 2:]:
    if line == "":
        break
    if not line.startswith("  "):
        sys.exit("bad Contents line " + line)
    listed.append(line[2:])
if listed != files:
    missing = [p for p in files if p not in listed]
    extra = [p for p in listed if p not in files]
    sys.exit("README Contents does not match the tarball\nmissing %s\nextra %s" % (missing, extra))
for rel in listed:
    # Existence is the member extract; a missing path makes tar exit non-zero.
    member(rel)
got = hashlib.md5(member("bin/misterplexd")).hexdigest()
if got != daemon_md5:
    sys.exit("bin/misterplexd md5 %s != reused daemon %s" % (got, daemon_md5))
for rel in ("cores/PlexOB.rbf", "PlexOB.rbf", "_Utility/PlexOB.rbf"):
    h = hashlib.md5(member(rel)).hexdigest()
    if h != pin:
        sys.exit("%s md5 %s != pin %s" % (rel, h, pin))
prov = member("licenses/ffmpeg/PROVENANCE.md").decode()
cfg = re.search(r"```\n(.*)\n```", prov, re.S)
if not cfg or "--enable-openssl" not in cfg.group(1).split():
    sys.exit("tarball ffmpeg provenance lost the configure line")
if re.search(r"(^|\s)--enable-gpl(\s|$)", cfg.group(1)):
    sys.exit("configure line passes --enable-gpl")
if "/workspace" in prov or "/tmp" in prov:
    sys.exit("ffmpeg provenance still contains a build-host path")
ssl = member("licenses/openssl/PROVENANCE.md").decode()
if "3.0.15" not in ssl or "Apache-2.0" not in ssl:
    sys.exit("openssl provenance incomplete")
need = (
    "licenses/ffmpeg/COPYING.LGPLv2.1",
    "licenses/openssl/LICENSE.txt",
    "licenses/libplist/COPYING.LESSER",
    "licenses/uxplay/LICENSE",
    "licenses/playfair/LICENSE.md",
    "licenses/llhttp/LICENSE-MIT",
    "licenses/mdnsd/COPYING.LGPLv2.1",
    "licenses/csrp/LICENSE",
    "licenses/owenb-h264ring/LICENSE",
    "licenses/gcc/copyright",
    "licenses/glibc/COPYING.LGPLv2.1",
    "licenses/rbf/LICENSE",
    "licenses/rbf/PROVENANCE.md",
    "licenses/stb/NOTICE",
    "licenses/qoi/NOTICE",
    "licenses/spleen/LICENSE",
    "licenses/mozilla-ca/NOTICE",
    "licenses/ytdlp/NOTICE",
    "linux/gcast/gcast_xcode.py",
    "linux/gcast/run-gcast-xcode.sh",
    "linux/gcast/ffmpeg",
    "linux/gcast/ffprobe",
    "licenses/gcast-ffmpeg/NOTICE",
    "licenses/gcast-ffmpeg/Apache-2.0.txt",
    "docs/architecture.png",
    "docs/VERIFY.md",
)
for rel in need:
    if rel not in files:
        sys.exit("tarball missing " + rel)
if "README.md" not in files or "RELEASE-NOTES.md" not in files:
    sys.exit("tarball root is missing README.md or RELEASE-NOTES.md")
if "scripts/install_paired_all.sh" in files:
    sys.exit("tarball still contains install_paired_all.sh")
op = member("README.md").decode()
notes = member("RELEASE-NOTES.md").decode()
if "GNU General Public License version 3" not in op or published_op not in op:
    sys.exit("README.md is missing the GPLv3 License section")
if "Corresponding source" in op:
    sys.exit("README.md still has a corresponding-source offer")
conf_install = "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf"
op_conf = "test -f /media/fat/linux/misterplex.conf || cp /tmp/misterplex.conf.example /media/fat/linux/misterplex.conf"
if op_conf not in op or conf_install not in readme or conf_install not in install:
    sys.exit("README.md, README.txt, and INSTALL must install the conf at /media/fat/linux/misterplex.conf")
bare_conf = "\n  scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf\n"
conf_guard = "test -f /media/fat/linux/misterplex.conf"
hook_stop = "/media/fat/misterplex/bin/misterplexd-boot-rc3.sh stop"
if bare_conf in readme or bare_conf in install:
    sys.exit("README.txt/INSTALL.txt copy misterplex.conf.example unconditionally")
if conf_guard not in readme or conf_guard not in install:
    sys.exit("README.txt/INSTALL.txt do not guard the conf copy")
if hook_stop not in readme or hook_stop not in install:
    sys.exit("README.txt/INSTALL.txt do not stop the hook before copying")
bad_conf = "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/misterplex/misterplex.conf"
if bad_conf in op or bad_conf in readme or bad_conf in install:
    sys.exit("a top-level doc still copies the conf to /media/fat/misterplex/")
if "/media/fat/misterplex/bin/misterplexd-boot-rc3.sh install" not in op:
    sys.exit("README.md does not install the boot hook")
if "https://github.com/flynnsbit/misterplex-cast/releases" not in op:
    sys.exit("README.md download link is not the Releases page")
if "TODO(lab, item 19)" in op or "TODO(review, item 20)" in op:
    sys.exit("README.md still has the item 19 or 20 TODO marker")
item19 = "Casting from a shared server through plex.tv works. Casting from a shared server addressed by its local network address can fail, and browsing a shared server on the TV is untested."
item20 = "No SDRAM board needed; PlexOB uses DDRAM only."
if item19 not in op or item20 not in op:
    sys.exit("README.md is missing the item 19 or 20 sentence")
unguarded_conf = "scp conf/misterplex.conf.example root@<mister-ip>:/media/fat/linux/misterplex.conf"
if (unguarded_conf + "\n") in op or conf_guard not in op:
    sys.exit("README.md step 5 copies the conf example without the test -f guard")
if "misterplexd-boot-rc3.sh uninstall" not in op:
    sys.exit("README.md does not document uninstall")
airplay = "AirPlay works on the MiSTer by itself; a helper on a computer is optional"
if airplay not in op or airplay not in readme or airplay not in install:
    sys.exit("README/INSTALL missing the on-card AirPlay sentence")
gcast_scp = "scp -r linux/gcast root@<mister-ip>:/media/fat/linux/"
if gcast_scp not in op or gcast_scp not in readme or gcast_scp not in install:
    sys.exit("README/INSTALL does not install linux/gcast")
if "127.0.0.1:18781" not in op and "127.0.0.1 port 18781" not in op:
    sys.exit("README.md does not name the on-card helper address")
notice = member("licenses/gcast-ffmpeg/NOTICE").decode()
for bit in ("0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee",
            "scripts/build_gcast_ffmpeg.sh",
            "scripts/build_arm_airplay.sh",
            "https://github.com/flynnsbit/misterplex-cast/releases/tag/rc5",
            "ffmpeg-8.1.2.tar.xz",
            "ffmpeg-8.1.2.tar.xz.asc",
            "x264-0480cb05fa188d37ae87e8f4fd8f1aea3711f7ee.tar.bz2",
            "openssl-3.0.15.tar.gz",
            "build_gcast_ffmpeg.sh",
            "SHA256SUMS",
            "MD5SUMS",
            "464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c",
            "f05c59f2e83d494c36307025dca2d3afc6b4d185f3a3453d06cc4fecd7094057",
            "23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533",
            "https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz",
            "https://code.videolan.org/videolan/x264",
            "https://www.openssl.org/source/openssl-3.0.15.tar.gz",
            "available on request from the distributor",
            "physical medium for three years",
            "licenses/rbf/GPL-2",
            "GPL-2.0-or-later",
            "ffprobe is LGPL-2.1-or-later with OpenSSL",
            "--enable-gpl --enable-version3 --enable-libx264 --enable-openssl",
            "--enable-openssl --enable-static --disable-shared --disable-ffmpeg",
            "Apache License 2.0",
            "licenses/gcast-ffmpeg/Apache-2.0.txt",
            "OpenSSL 3.0.15"):
    if bit not in notice:
        sys.exit("gcast ffmpeg NOTICE missing: " + bit)
if "mirrored" in notice:
    sys.exit("gcast ffmpeg NOTICE still names misterplex-cast or a mirror")
for phrase in ("198.51.100.12", "203.0.113.12", "203.0.113.41"):
    if phrase in op or phrase in readme or phrase in install:
        sys.exit("a top-level doc still contains " + phrase)
svg = member("docs/architecture.svg").decode()
if "AirPlay works on the MiSTer by itself" not in svg or "a helper on a computer is optional" not in svg:
    sys.exit("architecture diagram AirPlay box does not say the helper is on the MiSTer")
for banned in ("docs/GLASS-COMMANDS.md", "docs/glass-baseline-pair.md",
               "docs/display-resolution.md", "docs/crt-lcd-matrix.md",
               "scripts/install_paired_all.sh", "bin/ffmpeg"):
    if banned in files:
        sys.exit("tarball still contains " + banned)
if b"203.0.113.41" in member("bin/misterplexd"):
    sys.exit("daemon still contains 203.0.113.41")
if b'OPENSSLDIR: "/workspace' in member("bin/misterplexd"):
    sys.exit("daemon still bakes a workspace OPENSSLDIR")
if "bin/misterplexd-boot-rc3.sh" not in files or "bin/update_ytdlp.sh" not in files:
    sys.exit("README.md commands need bin/misterplexd-boot-rc3.sh and bin/update_ytdlp.sh")
hook = member("bin/misterplexd-boot-rc3.sh").decode()
if "/media/fat/linux/misterplex.conf" not in hook:
    sys.exit("boot hook no longer names the linux conf")
if "start_gcast_helper" not in hook or "stop_gcast_helper" not in hook:
    sys.exit("boot hook does not start and stop the helper")
if "127.0.0.1" not in hook or "18781" not in hook:
    sys.exit("boot hook does not bind the helper to 127.0.0.1:18781")
if "ffprobe" not in hook:
    sys.exit("boot hook does not copy ffprobe")
if "uninstall)" not in hook:
    sys.exit("boot hook is missing uninstall")
if "198.51.100.12" in hook or "203.0.113.12" in hook:
    sys.exit("boot hook comments still use a lab address")
conf = member("conf/misterplex.conf.example").decode()
legacy = conf.split("Legacy Plex.rbf only", 1)
if len(legacy) != 2 or "SUBTITLES" not in legacy[1]:
    sys.exit("SUBTITLES is not in the legacy conf block")
if "SUBTITLES" in legacy[0]:
    sys.exit("SUBTITLES is still in the main conf block")
import re
# Backtick spans and fenced commands. A leading slash is a device path
# (/media/fat/...), not a member of the tarball.
named = set(re.findall(
    r'(?<![\w/`])((?:bin|scripts|docs|licenses|conf|cores|linux)/[A-Za-z0-9_+-]+(?:[./][A-Za-z0-9_+-]+)*|SHA256SUMS)',
    op))
for rel in sorted(named):
    rel = rel.rstrip("/")
    if rel in files:
        continue
    if any(f == rel or f.startswith(rel + "/") for f in files):
        continue
    sys.exit("README.md path is not in the tarball: " + rel)
if "Known issues" not in notes:
    sys.exit("RELEASE-NOTES.md has no Known issues section")
print("tarball files", len(files))
print("daemon", got)
print("rbf", pin)
print("readme.md paths", len(named))
PY

echo "test_package_readme_paths: ok"
