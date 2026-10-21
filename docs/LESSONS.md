# Lessons learned — measurement discipline

Every rule below was paid for with a real wrong conclusion in this project. They are
recorded so the same mistake is not made twice. `AGENTS.md` rule 0 (**no guessing**)
is the parent rule; this file is the accumulated case law under it.

Read this before reasoning about a build, a gate, a device symptom, or an area budget.

---

## 1. Verify the artifact you are actually holding

**L1 — Pair by release, not by file date.**
A release core was tested against a *development* daemon, showed bars, and was written off.
Testing a release core against a mismatched daemon proves nothing about either. The matched
pair later played video fine. Always deploy and test core + daemon as a matched set.
*(Miss #22)*

**L2 — Measure the tree you are claiming about.**
A claim that five 720p present modules had landed on main was broadcast to nine agents as
fact. It came from measuring the wrong worktree. Before any claim about `main`, resolve the
worktree path and print the HEAD you actually measured.
*(Miss #19)*

**L3 — "It passes on my branch" is not evidence about main.**
Gate results are properties of a specific tree at a specific commit. Re-run on the target
tree; do not carry a result across trees.

**L4 — One grep in one file does not prove absence.**
A symbol was declared unconfirmed after grepping a single file; the real definition was in a
different path. Absence requires searching the whole relevant surface, and — for RTL —
checking *instantiation*, not just file membership.
*(Miss #7)*

---

## 2. Exit codes and gate output

**L5 — `rc=0` is not PASS if the run contains skips.**
A `make unit` rc=0 was read as PASS while the run contained `SKIP-NOT-PASS` markers. Read the
skip summary, never just the exit code.
*(Miss #20)*

**L6 — Exit 77 is a soft-skip, not a pass.** `UNSCORED` is not a pass either.

**L7 — Capture exit codes directly, never through a pipe.**
Use `cmd > out.log 2>&1; echo "true rc=$?"`. A pipe reports the exit code of the last stage.

**L8 — A test that has never gone RED proves nothing.**
Show every new assertion RED against the unfixed source and GREEN after, both with captured
exit codes. Include at least one negative case a naive wrong implementation would fail.

**L9 — Agreement of values is not correctness of selection.**
Two constants matching does not mean the right one is being used. Check what the code
actually reads at runtime.

---

## 3. FPGA build reality

**L10 — A file absent from `files.qip` is invisible to Quartus.**
The entire H.264 decoder hierarchy was absent from the build manifest while appearing present
in the source tree. `stream_path.sv` instantiated a diagnostic painter in its place.

**L11 — QIP membership does not mean the module is in the bitstream.**
`h264_cavlc_residual` was in `files.qip`, but its only instantiation lived in a file that was
*not*. Quartus compiled it and pruned it: zero logic in the RBF. **Reachability**, not
membership, is the property that matters. This is what the pre-fit reachability gate exists
to catch.

**L12 — Do not plan area recovery against unmeasured assumptions.**
"Delete CABAC and recover area" was stated as fact to several lanes and was worth **zero** —
measurement against the fitted hierarchy showed ALM 0, M10K 0. Deleting something that
contributes nothing frees nothing. Measure before promising.
*(Miss #17)*

**L13 — Timing measured on logic you intend to delete is not your timing.**
A route was killed by an Fmax whose critical path ran through a stub scheduled for removal.
Check what the critical path actually traverses before accepting a timing verdict.
*(Miss #18)*

**L14 — Cycle budgets for a pipeline that is not in the build describe nothing.**
Confirm the module is reachable in the fit before trusting any performance number about it.

---

## 4. Hardware measurement

**L15 — Never thrash `load_core`.**
Reloading a core underneath a running daemon invalidates its FPGA handles and yields a black
screen that looks exactly like a dead bitstream. Correct harness: **load core → bounce daemon
→ capture**. A control reload of a known-good core is what exposed this artifact.
*(Miss #21)*

**L16 — Never use a bare `ffmpeg -frames:v 1`.**
The MJPEG grabber needs ~15 frames to lock, so a single-frame grab returns a **false black**.
Use `scripts/hdmi_capture_idle.sh`, which discards warm-up, retries, and prints scoreable
metrics.

**L17 — Verify the system is in the state you think before sampling.**
An "ARM saturated at idle" reading was taken while a transcode from an earlier cast was still
running. Re-measured with the process count verified *in-sample*, the finding held — but the
first sample was invalid. Assert the precondition inside the sample, not before it.

**L18 — Judge a frame by viewing it, not by `MEAN` alone.**

**L19 — Playback stop is delayed.** `POST .../playback/stop` returns 200 while ffmpeg persists
several seconds. That is not a leak.

**L20 — Distinguish "the log does not contain X" (evidence) from "X did not happen"
(inference).** Say which one you have.

---

**L26 — Always capture the UNCONTENDED baseline before declaring a performance wall.**
Native 720p decode measured `speed=0.939×` and was declared arithmetically dead. The
measurement had been taken with `misterplexd` running. With the daemon stopped and
everything else identical, the same asset and command gave **`1.31×` (32 fps, idle
0.0% → 49.4%)** — a 43% speedup that had been sitting invisible behind background load.
A contended measurement is an upper bound on the *system*, never on the *capability*.
When a measurement is blocked (flaky SSH, dropped session), mark it **UNMEASURED** and
come back for it — do not let a contended number quietly become the accepted ceiling.

**L27 — Run long device measurements detached (`nohup setsid …`), writing to a file.**
The clean baseline above went uncaptured for days because SSH dropped mid-run on flaky
WiFi. Detaching on-device makes the result survive the transport.

---

## 5. Concurrency and flaky infrastructure

**L21 — Prove flakiness; never assume it.**
Attributing a failure to "contention", "flakiness" or "environment" without evidence is the
single most common failure mode here. Re-run it and read the actual error. One RED blamed on
contention was a real failure: a lane had committed a hardcoded lab IP.

**L22 — Known race: `test_main_guard`.**
The post-`SIGSTOP` state-T check used a bounded 40 × 50 ms = 2.0 s window and
flaked ("fake MiSTer did not reach state T") under load. On this branch that
wait is one 5 s deadline (`waitForState(pid, 'T', 5000)`). The resume check
stays at 2.0 s. Do not apply this note to a different error.

**L23 — Build unconditional restore into any script that stops the daemon.**
Device connectivity drops intermittently; a half-finished experiment must not leave the device
without its supervisor.

---

## 6. Storage and provenance

**L24 — `/tmp` is tmpfs and is erased on reboot.**
It has already destroyed session history once. Nothing of value lives there: neither evidence
nor git worktrees. Permanent store is `Memory/` (see `AGENTS.md`).

**L25 — Never record a Plex token.**
A live `X-Plex-Token` is visible in ffmpeg's argv on device, so any local process can read
`/proc/<pid>/cmdline`. Tokens are never copied into the store and are redacted from anything
that is.

---

**L28 — The deployed bitstream is NOT your source tree. Verify provenance before inferring
anything from the device.**
`Plex.rbf` md5 `dfebf2bf` was assumed to match current RTL. It is the **G-VID1** build from
FPGA commit `0139f2c5`, where `ddr_frame_store.sv` **does not exist** — that file was added
later in `d0ea6dac` (`git merge-base --is-ancestor 0139f2c5 d0ea6dac` → rc=0). Mailbox
probes against that core returned zeros, and the zeros were read as "the core fails to
publish PLXD". The real reason is that the running core has **no PLXD writer at all**.
Before drawing any conclusion from a device probe, answer: *which commit built the
bitstream I am probing?* If that cannot be answered in one command, the tooling is the bug.

**L29 — `git worktree move` cannot relocate a worktree off `/tmp`.**
It uses `rename(2)`, which fails across filesystems with `Invalid cross-device link`
(tmpfs → btrfs). Use `cp -a <src> <dest>` then `git worktree repair <dest>`, then remove
the original. Commits in a *branch-named* worktree are safe in the shared object DB, but a
**detached HEAD** worktree's commits are GC-reachable only while registered — tag them
before pruning.

**L30 — Unit-green is not device-green. Capture the screen before believing a daemon fix.**
`w-path`'s idle busy-loop fix (`e22deaf7`) had an honest RED→GREEN negative test and
`make unit` **true rc=0**. Deployed, it rendered stub garbage instead of the Plex logo —
HDMI capture `ORANGE_PX` collapsed from **33852** (chevron, correct) to **1187** and then
**43**, with content *varying between captures*, which is itself the tell for garbage
rather than a stable painted frame. Rolling back restored the logo immediately. The suite
agreed with a binary that broke the product because **no test exercised the idle frame
paint**. When a change touches the frame path, a capture is part of the evidence, not a
formality. Suspected mechanism: a 2 ms wall cap on the SPI ACK wait aborting legitimate
transactions and leaving partial writes — *silent partial success* is the real defect class.

**L31 — Deploy to the path that is actually running.**
`scripts/deploy_misterplexd.sh` installs to `/media/fat/misterplex/bin/`, but the live
daemon runs from `/media/fat/misterplex_v2/bin/`. Using the script would have shipped
nothing while reporting success. Always confirm the target path against `ps` output.

**L32 — `ssh host 'cmd &'` hangs until the backgrounded child closes its fds.**
A backgrounded supervisor inherits stdout and holds the SSH channel open. Use
`nohup setsid cmd >/dev/null 2>&1 &`, or run the whole experiment detached on-device and
poll a log file.

**L33 — Moving an evidence store into the repo puts it inside every repo-wide scan.**
`check_ddr_yuv_only_repo()` walks `ROOT.rglob("*")`. Relocating the lab store from
`a private lab notes repo` into `Memory/` turned a **verbatim quotation of a historical
daemon log line** into a build failure. Git-ignored is not scan-ignored — `.gitignore` and
a test's `skip_parts` are two independent lists, and only the second one governs `rglob`.
When a repo-wide invariant fires on an evidence file, the honest fix is the **scan scope**.
Redacting a true quotation to make a source rule pass would falsify the record, and
loosening the rule itself would be gate self-weakening. Fix scope, then prove the gate
still bites by planting the same string somewhere that must fail.

**L34 — When you weaken anything a gate touches, hand it to an adversary.**
A scope change and a coverage cut are indistinguishable from the commit message alone.
Prove it four ways — skipped location green, product location still red, fix reverted
reproduces the original failure, real artifact green — then have an independent lane try
to break the claim. Especially when the author is the orchestrator.

## L35 — Give every probe a positive control; a search that cannot succeed is not evidence of absence

I "proved" the entire fabric H.264 decoder was dead code with:

    grep -rlE "^\s*$m\s*(#|\()" --include='*.sv' .   for m in h264_cavlc_residual ...

All eight modules reported **NOBODY**, and I published that as a headline structural
finding. The probe searched for a module whose **name equals its filename**. In this
codebase *no module is named after its file* — `rtl/h264_dpb.sv` declares seven modules
and none of them is called `h264_dpb`. The probe could only ever return NOBODY. It was
incapable of returning anything else.

Running it once against a module I *knew* was instantiated would have returned NOBODY too
and exposed it in seconds. **Before trusting a negative result, confirm the probe can
produce a positive.**

The corrected probe enumerates real declared module names and asks who instantiates each:
`LIVE=4 DEAD=28`. The substance survived — there is still no fabric decode — but the
numbers I broadcast to nine agents were wrong, and one agent shipped wrapper modules named
after files purely to satisfy the bad grep. **A bad metric doesn't just mislead you, it
deforms the code other people write.** Fix the probe, never the code shaped to satisfy it.

Sibling of L34: adversarially audit your own scope changes → also audit your own *searches*.

---

## L36 — Select processes by `/proc/<pid>/comm`, never by grepping `ps`

To test whether the MiSTer framework's busy-spin could be deprioritised:

    M=$(ps aux | grep -w "[M]iSTer" | awk '{print $1}' | head -1)
    renice -n 19 -p $M

Decode got **worse** — 1.30× → 0.906×, twice in a row. I was one step from publishing
"renicing MiSTer hurts decode".

The tells: the PID changed on every invocation (19729 → 14253 → 14806), and
`/proc/<pid>/status` said `Name: bash`. The `[M]iSTer` bracket trick defeats the classic
self-match, but **my own ssh command line contained the string**, so `grep` matched the
shell running the experiment. `renice` hit that shell, and `ffmpeg` — its child —
*inherited* nice 19. I had measured my own harness sabotaging itself.

Correct selection, and always verify the target and yourself:

    for d in /proc/[0-9]*; do
      [ "$(cat $d/comm 2>/dev/null)" = "MiSTer" ] && M=${d#/proc/} && break
    done

With the real target (`pid=19729 comm=MiSTer`, my shell confirmed distinct and left at
nice 0, transition read from `/proc/19729/stat` as `nice=0 prio=20` → `nice=19 prio=39`),
the true result was the **opposite**: **1.30× → 1.49× → 1.54×**, display bit-identical
across three captures at `ORANGE_PX=33852`.

Two lessons: an experiment that perturbs its own harness produces a confident, reproducible,
completely wrong number — *reproducibility is not validity*. And always capture the
controlled variable on **both** the target and yourself.

---

## L37 — Agent scratch directories must be git-ignored before they exist

A parent `git add -A` swept **7,398 lines** of `.agent-work/w-nostub/` build logs and
scratch RTL into a commit whose message claimed it was a lab pre-registration. Caught only
by inspecting the commit instead of trusting the `COMMITTED` echo it printed.

The trap was repo-wide, not personal: every lane writes scratch under `.agent-work/`, and
nothing ignored it, so *any* `git add -A` in *any* worktree would commit that lane's
scratch under someone else's message. Fixed in `0b7851ed` (both `.agent-work` and
`.agent-work/` per L33).

**An echoed success message is not a captured result.** `&&` chains print the success of
the *last* command, not the correctness of what it did. Inspect what a commit actually
contains. And prefer explicit paths over `git add -A` in any shared tree.

---

## L38 — Never trust PMS `videoResolution` as the coded frame size

Auto `pms_match_decode` scale bypass assumed weak-ladder `videoResolution=1280x720`
meant the HTTP body was already 1280×720 I420. Host probe of the same FOAR URL
proved PMS delivered **h264 720×480**. Skipping swscale packed SD stride into a HD
bank → rainbow banding. push_frame and local play-file stayed PASS, so the core was
blamed first.

**Rule:** only skip scale for (1) explicit lab `FFMPEG_SWS_FLAGS=skip|none|off|identity`,
(2) a **local** file already at DECODE bank size, or (3) PMS **Media/Stream coded size
exactly matches DECODE** (`width==outW && height==outH`) — never `videoResolution`
alone, and never “source ≥ bank” (that skipped the weak ladder for FOAR into 240/480
and broke content-mode→PMS encode). FOAR (rk139) is 720×480 → FOAR+pad at 1280×720;
Grid720 (rk143) is true 1280×720 → `pms_source_matches_bank` bypass is safe. Prove with
metadata + HDMI-USB (`/dev/video0`), not DDR dumps alone.

**Rate (v0.4.0 ladder):** 1280×720 glass is **possible**. Product 24 fps is still open:
identity play-file ~21 pfps (historical pipeline peak 25.8 with free A9); true-720 PMS
~18 pfps after scale skip; FOAR@720 bank ~9–10. Bottleneck = uncached bank memcpy
(~14 ms) + decode contention. Path to 24 = KernelDma/WC, not more RBF thrash.
See `Memory/lab/status/SCORE_v040_720_RATE_LADDER.txt`.

## L39 — Dual-out FFmpeg aborts when the source has no audio

With `AUDIO=on`, single-process spawn opens `pipe:1` (rawvideo) + `pipe:3` (s16le).
Grid720 freckle map has **no** `streamType=2` audio. FFmpeg still builds the second
output; optional `-map 0:a:0?` leaves it empty → `Output file does not contain any
stream` → process exit → `pipeline short read got=0` and idle chevron while timeline
claims playing. Global `-an` before a later audio map makes the same class of failure
worse.

**Rule:** parse PMS metadata for audio (`hasAudio`); if false, spawn **video-only**
(`-an`, no pipe:3). Never open an empty audio output. Probe local files the same way.
FOAR (AAC) keeps dual-out. HDMI-USB + `ffmpeg.err` on USB lab path are the glass proof.

---

## Incident index

| # | Wrong conclusion | Reality |
|---|---|---|
| 7 | Symbol unconfirmed after one grep | Wrong file; it existed elsewhere |
| 8 | SD playback would fail (pre-registered) | Played fine; recorded failure was transient |
| 14 | PMS rewrites the source profile | PMS **preserves** it through transcode |
| 17 | Deleting CABAC recovers area | Worth **zero** — ALM 0, M10K 0 |
| 18 | Route A killed by Fmax | Critical path ran through a stub slated for deletion |
| 19 | Five 720p modules landed on main | Measured the wrong tree; broadcast to 9 agents |
| 20 | `make unit` rc=0 means PASS | Run contained SKIP-NOT-PASS markers |
| 21 | Black captures ⇒ dead RBFs | `load_core` thrash invalidated daemon FPGA handles |
| 22 | Release core was broken | Had been paired with a mismatched dev daemon |
| — | ARM saturated at idle | First sample contained a live transcode; retracted, then re-established |
| — | Two PLXG constants conflicted | `0x300FF000 + 0x800 == 0x300FF800` — identical; a pure duplicate |
| — | PLXD alive at the doorbell-relative address | Zero at **every** candidate; only the host-written doorbell was live |
| — | Those PLXD zeros indicted the RTL | The deployed RBF predates `ddr_frame_store.sv` entirely — it has **no PLXD writer**. Probe was invalid; conclusion withdrawn |
| — | `misterplexd` ~136% + `MiSTer` ~131% at idle | Arithmetically impossible: 16.02 CPU-s claimed inside a 12 CPU-s dual-core window. Split withdrawn |
| — | Native 720p24 is arithmetically dead at 0.939× | **1.31× (32 fps)** with the daemon stopped. The wall was a daemon busy-loop, not physics |
| — | A green `make unit` meant the daemon fix was safe | It rendered stub garbage on HDMI; `ORANGE_PX` 33852 → 43. Reverted |
| — | Main was RED on a forbidden DDR pattern | The hit was a **quoted historical daemon log line** in a git-ignored evidence store I had just moved into the repo tree. Scan scope was wrong, not the record |
| — | All 8 h264 leaf modules instantiated by NOBODY | Probe searched for module names equal to filenames; **no module here is named after its file**, so it could only return NOBODY. Corrected: `LIVE=4 DEAD=28` |
| — | Renicing the MiSTer spinner slows decode (1.30×→0.906×) | `grep` matched my **own ssh shell**; `ffmpeg` inherited nice 19. Real result was the opposite: **1.30×→1.54×** |
| — | A commit labelled "pre-register Sweep 119" | Contained 7,398 lines of another lane's `.agent-work` scratch logs. `git add -A` in an un-ignored tree |
| — | softc24 / daemon glass dead (PMS FOAR rainbow) | PMS returned **720×480**; `pms_match_decode` scale bypass desynced rawvideo. Fix: never auto-skip scale on HTTP |
| — | Web cast frames=0 with AUDIO=on | Grid720 has **no audio**; dual pipe:3 aborted ffmpeg (L39) |
| — | 240/480 mode skipped PMS encode | “covers bank” direct Part on FOAR; need **exact** bank match (L38) |

Full narrative for each: `Memory/lab/parent/misterplex-parent-720p-decode-verdict.txt`.

## Scheduler /loop: do not cancel before PASS (2026-08-07)

Parent cancelled a "until 24 fps" loop after conf knobs were exhausted, without
user request and without PASS. User rejected that. Rule: never `scheduler_delete`
a pass-gated loop unless PASS is proven or the user cancels. Pivot the prompt
to architecture work instead. Lab note:
`Memory/lab/status/LESSON_DO_NOT_CANCEL_LOOP_BEFORE_PASS.txt`.

## L40 — DDR 2-slot ring must call onProgress (Web scrubber freeze)

**Symptom:** Plex Web / companion timeline stuck at plant offset (e.g. `time=3000`) while
`frames`/`pfps`/`audio_s` advance and HDMI shows content.

**Cause:** `PRESENT=fpga` uses `present_pipeline=2slot_cached_ring`. That path stored
`positionMs_` on the 1 Hz log tick but **never called `onProgress_`**. Companion only
received the initial `playing@startMs` plant → scrubber freeze. Separate from the older
plant-hold bug (`0abee0b6` far-ahead release + `seedPlaybackPosition`).

**Fix:** Call `onProgress_("playing", startMs+wall, dur)` alongside `positionMs_.store` in
the DDR pipe loop. Keep `0abee0b6` plant-ahead release + `seedPlaybackPosition` in
companion. Unit: `tests/unit/test_companion_plant_seek.cpp`.

**Evidence:** SCORE_v040_MATRIX_WEB_TIMELINE.txt — timeline advances on 240/480/720 after fix.

## L41 — PMS `/:/timeline` must be HTTP 2xx + cast token (Web 0:00)

**Symptom:** Companion `/player/timeline/poll` advances, media plays, but Plex Web scrubber
stays at **0:00**. User cast often uses `*.plex.direct` (different machine id than conf
`PLEX_BASE`).

**Cause (device evidence `timeline-stuck-20260730T173000Z` / commits `eeae7943`, `0abee0b6`):**
1. `plexHttpGetNoBody` treated **non-empty body as OK** — 401 HTML (91 B) logged as success.
2. Timeline session mixed **cast host** with **conf `PLEX_TOKEN`** from another PMS → silent 401.
3. Playing cadence was 10s; Web-bound “Now Playing” needs ~1s updates.

**Fix:** `plexHttpGetNoBodyResult` uses curl `%{http_code}` (2xx only); cast token preferred
(conf token only when cast did not pin a host); `updateToken` from later `/player/` requests;
`type=video` on `/:/timeline`; `clientIdentifier` = player id; log `http=N` always; playing
cadence **1s**. Port of `eeae7943` + plant-ahead `0abee0b6` + L40.

**Evidence:** Lab cast rk=143 — companion time 2s→11s; `pms timeline: update ok http=200`
with advancing `time=`; PMS `/status/sessions` `viewOffset` matches player.
