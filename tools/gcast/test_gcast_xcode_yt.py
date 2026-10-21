#!/usr/bin/env python3
# rc3: gcast_xcode returns itags as a list; itags_s is the joined form.
"""FIELD-1018 F1 unit test for gcast_xcode YouTube resolve (no network). Mutant: GCAST_MUT=noclientretry|nofmt140|noevict.
A fake yt-dlp that (like a lab MiSTer 08:22 CT) returns nothing on the default client and formats on another player client;
the 2026 AAC itag 140-5 (DRC variant) must be selectable; a 403 / empty stream drops the cached URLs."""
import os, select, signal, stat, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcast_xcode as g
mut = os.environ.get("GCAST_MUT", "")
if mut == "noclientretry": g.YT_CLIENT_RETRY = ([],)
if mut == "nofmt140": g.YT_FMT = "(136/135/134)+140/18"
if mut == "noevict": g.yt_evict = lambda vid: None
fails = 0
def check(c, what):
    global fails
    if not c: fails += 1; print("FAIL", what)
fake = os.path.join(tempfile.mkdtemp(), "yt-dlp")
open(fake, "w").write("#!/usr/bin/env python3\nimport json,sys\na=' '.join(sys.argv)\n"
    "if 'player_client' not in a: sys.stderr.write('ERROR: Sign in to confirm you are not a bot\\n'); sys.exit(1)\n"
    "print(json.dumps({'title':'t','duration':10,'requested_formats':["
    "{'format_id':'136','url':'https://v/vid','vcodec':'avc1.4d401f','width':1280,'height':720,'fps':30},"
    "{'format_id':'140-5','url':'https://v/aud','vcodec':'none'}]}))\n")
os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)
g.YT_CACHE.clear()
r = g.yt_resolve("QVv3mQzlrN0", fake)
check(bool(r) and r["itags_s"] == "136+140-5", "yt: default client empty -> another player client resolves (140-5)")
check("ba[ext=m4a]" in g.YT_FMT and "+140/" not in g.YT_FMT, "yt: audio by m4a, not bare itag 140")
if r:
    g.yt_evict("QVv3mQzlrN0")
    check("QVv3mQzlrN0" not in g.YT_CACHE, "yt: 403 evicts the cached URLs")
def read_until_eof(cmd, env, budget):
    """Bytes from cmd, and whether stdout reached EOF inside budget."""
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            preexec_fn=os.setsid, env=env)
    data = b""
    eof = False
    try:
        while time.monotonic() - t0 < budget:
            ready, _, _ = select.select([proc.stdout], [], [], 0.05)
            if not ready:
                continue
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                eof = True
                break
            data += chunk
    finally:
        if proc.poll() is None:
            try: os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            proc.wait()
        else:
            proc.wait()
    return data, eof, time.monotonic() - t0

def check_two_itag_403_falls_through():
    """A 403 on the first itag must close the pipe so the fallback can answer."""
    work = tempfile.mkdtemp()
    ytdlp = os.path.join(work, "yt-dlp")
    ffmpeg = os.path.join(work, "ffmpeg")
    open(ytdlp, "w").write(
        "#!/usr/bin/env python3\n"
        "import os, sys, time\n"
        "args = sys.argv\n"
        "itag = args[args.index('-f') + 1]\n"
        "if itag == '136':\n"
        "    sys.stderr.write('ERROR: HTTP Error 403: Forbidden\\n')\n"
        "    # fd 1 is the fifo. fd 7 is the subshell stdout, dup'd before that\n"
        "    # redirect. Hold it. The fixed script points fd 7 at /dev/null, so\n"
        "    # holding it does not keep the response pipe open. A revert leaves\n"
        "    # fd 7 on the response pipe and this blocks the reader past 2s.\n"
        "    held = True\n"
        "    try:\n"
        "        os.fstat(7)\n"
        "    except OSError:\n"
        "        held = False\n"
        "    try:\n"
        "        os.close(1)\n"
        "    except OSError:\n"
        "        pass\n"
        "    if held:\n"
        "        time.sleep(30)\n"
        "    sys.exit(1)\n"
        "if itag == '140':\n"
        "    try:\n"
        "        os.close(1)\n"
        "    except OSError:\n"
        "        pass\n"
        "    sys.exit(0)\n"
        "if itag == '18':\n"
        "    sys.stdout.buffer.write(b'VIDEO'); sys.exit(0)\n"
        "sys.exit(2)\n")
    open(ffmpeg, "w").write(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "args = sys.argv[1:]\n"
        "ins = []\n"
        "i = 0\n"
        "while i < len(args):\n"
        "    if args[i] == '-i' and i + 1 < len(args):\n"
        "        ins.append(args[i + 1]); i += 2\n"
        "    else:\n"
        "        i += 1\n"
        "if len(ins) >= 2:\n"
        "    fs = [open(p, 'rb') for p in ins]\n"
        "    fs[0].read(); fs[1].read()\n"
        "    sys.exit(0)\n"
        "sys.stdout.buffer.write(b'FALLBACK:' + sys.stdin.buffer.read())\n"
        "sys.exit(0)\n")
    os.chmod(ytdlp, os.stat(ytdlp).st_mode | stat.S_IEXEC)
    os.chmod(ffmpeg, os.stat(ffmpeg).st_mode | stat.S_IEXEC)
    budget = 3.0
    t0 = time.monotonic()
    failed, env = g.build_yt_pipe_cmd(ffmpeg, ytdlp, "dQw4w9WgXcQ", ["136", "140"], 0)
    check("exec 7>&1" in failed[2], "failing subshell dups the response fd before the fifo redirect")
    if os.environ.get("GCAST_MUT") != "nopipenull":
        check(">/dev/null" in failed[2], "subshell stdout is discarded")
    _data, eof, _dt = read_until_eof(failed, env, budget)
    check(eof, "403 on the first itag closes the two-itag pipe")
    fallback, env = g.build_yt_pipe_cmd(ffmpeg, ytdlp, "dQw4w9WgXcQ", ["18"], 0)
    data, eof2, _dt2 = read_until_eof(fallback, env, max(0.2, budget - (time.monotonic() - t0)))
    elapsed = time.monotonic() - t0
    check(eof2 and data == b"FALLBACK:VIDEO", "fallback itag answers after the 403 (%r)" % data)
    check(elapsed < 2.0, "403 then fallback finished in %.3fs, under 2s (17.5s hang)" % elapsed)
    if os.environ.get("GCAST_MUT") != "nopipenull":
        reverted = [failed[0], failed[1], failed[2].replace(" >/dev/null &", " &")]
        check(">/dev/null" not in reverted[2], "revert mutant dropped the subshell redirect")
        check("exec 7>&1" in reverted[2], "revert mutant still holds the response fd")
        _rdata, reof, rdt = read_until_eof(reverted, env, 2.4)
        check((not reof) and rdt >= 2.0, "revert mutant blocked %.3fs eof=%s" % (rdt, reof))

check_two_itag_403_falls_through()
print("test_gcast_xcode_yt: %s" % ("FAIL %d" % fails if fails else "OK"))
sys.exit(1 if fails else 0)
