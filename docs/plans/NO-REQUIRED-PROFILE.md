# No required PMS profile (owner 1011 20:59 CT)

**Rule.** MiSTerPlex must play correctly on any PMS with no MiSTerPlex XML installed. Our XMLs
(MiSTerPlex, MiSTerPlex-High, the home PMS's 16 MiSTerPlex-FPGA-*) are an optional quality boost
for testing only. Correctness comes from the request params plus X-Plex-Client-Profile-Extra, and
the daemon checks what actually came back (decision response, then the first AU's SPS) against the
loaded core before any picture is shown.

Legend: **[V]** verified in our own logs/tests, **[D]** Plex docs / public API description,
**[U]** unverified (inference; needs a glass or lab-PMS check).

## 1. Which profile PMS uses
- `X-Plex-Client-Profile-Name: N` selects `Profiles/N.xml` when it exists. The public transcoder API
  says this header "generally should only be used to specify the Generic profile" [D].
- If the named file is missing PMS logs `Unable to find client profile N, falling back to traditional
  profile detection` and matches X-Plex-Product/Platform instead. With our identity (Product "Plex Web",
  Platform Chrome) that is the built-in **Web** profile (`using augmented profile Web`) [V: lab PMS log,
  review C1 20:04 CT]. With no name sent the result is the same Web profile with no error line [U: same
  matching path, not yet logged].
- PMS reports this only in its own log. The decision response does not say which profile it used, and
  no cheap API lists installed profiles [U: none found], so the daemon cannot detect "profile missing"
  per server. Sending a name is therefore opt-in (section 5).

## 2. What PMS honors with no profile (request params)
| param | effect | |
|---|---|---|
| `videoResolution=WxH` | upper bound on the transcode size | [V] (624x480 / 640x480 on every glass run) |
| `maxVideoBitrate=K` | bitrate cap for the transcode; also the direct-play/stream bitrate gate | [V] (1500 cap visible in A3) |
| `videoQuality` | quality slider; secondary to maxVideoBitrate | [V] |
| `protocol=http`, `container=mpegts` | progressive TS from start.mp4 | [V] |
| `directPlay=0&directStream=0` | forces a video transcode (no copy) | [V] (our FPGA URLs) |
| `copyts`, `fastSeek`, `offset` | timestamps / seek start | [V] |
| `location=lan/wan` | local vs remote bandwidth rules | [V] |
| `autoAdjustQuality=0` | no server-side quality ladder changes | [V] |
| `mediaBufferSize` | **do not send**: caused the ~5:30 EOFs on WAN (dropped 1011) | [V] |
| a "use software encoder" param | **none known**. HW encoding is a server setting (Settings > Transcoder > "Use hardware-accelerated video encoding", Plex Pass) | [D]/[U: no client override found] |

## 3. X-Plex-Client-Profile-Extra (profile augmentations)
Syntax `add-limitation(...)+add-transcode-target(...)+append-transcode-target-codec(...)`, each `&replace=true`
to override the base profile's entry [D: transcoder API example, Plex Web traffic].
- `add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mpegts&videoCodec=h264&audioCodec=aac)`:
  sets the output container/codec. Honored [V] (every transcode we get is h264/aac TS).
- `add-limitation(scope=videoCodec&scopeName=h264&type=match|upperBound&name=video.profile|video.level|video.refFrames|video.width|video.height|video.bitDepth|video.frameRate)`:
  **decides whether the SOURCE may be direct played/streamed** (a source above a bound is transcoded) [D/V:
  High level-41 titles transcode, A3/C1].
- The same limitations with `scope=videoTranscodeTarget ... isRequired=true` (what we send for profile and
  level): steer the **encoder's** profile/level [U for every server; V that x264 servers produce Baseline
  L3.0 when asked].
- `video.refFrames` as a transcode-target bound: **not known to reach the encoder** [U]. libx264 gets refs
  only from the profile XML's x264 options (`ref=1`), which no Extra can set [V: MiSTerPlex-FPGA-* XMLs].
- **NVENC.** When the server owner enabled HW encoding PMS picks `h264_nvenc` itself; NVENC ignores the
  profile XML's x264 options, so `ref=1` is lost: a LAN PMS with MiSTerPlex.xml produced `h264_nvenc
  -preset hq` with **max_num_ref_frames 3**, refused by the prod core (`limit 1`) [V: review C2 20:16 CT].
  Whether NVENC honors a transcode-target `video.profile=baseline` is [U]; refs are not client-controllable
  on an NVENC server as far as we know [U].

## 4. Decision response: what we can check
`/video/:/transcode/universal/decision` returns a MediaContainer with `Metadata/Media/Part/Stream`. Read:
`Media@videoCodec, videoProfile, width, height, bitrate, container, protocol`; video `Stream@codec,
profile, level, refFrames, bitDepth, width, height, decision`; `TranscodeSession@transcodeHwRequested /
transcodeHwEncoding` when present [D: field names in the API schema; U: which of them PMS fills for a
transcode target, especially refFrames and the HW encoder, which may appear only in /transcode/sessions
after start]. The daemon checks only the fields that are present; the **first AU's SPS** (the gate:
profile, CAVLC/CABAC, refs, level, size) stays the authority [V].

## 5. Daemon design (both branches)
1. **No profile name by default.** No `X-Plex-Client-Profile-Name` on decision, start probe, FFmpeg
   start/segments, metadata, timeline or ping. Opt-in for testing: `CLIENT_PROFILE_NAME=<name>` (conf;
   env `MPX_CLIENT_PROFILE_NAME`, wins) sends exactly that name; `SEND_PROFILE_NAME=fixed` sends
   MiSTerPlex / MiSTerPlex-High; `SEND_PROFILE_NAME=variant` sends the per-variant MiSTerPlex-FPGA-* name to a
   LAN/private PMS (a server that has the 16 XMLs) and the fixed names elsewhere.
2. **Extra opt-in (C939).** 240p/480p/480i send no Name/Extra unless `MPX_SEND_PROFILE_EXTRA` / a profile name
   opts in. 720p caps Extra remains. Plain URL params carry h264 baseline/main + AAC/stereo.
3. **Decision check.** The decision's video fields are checked against the loaded core (Baseline core:
   h264 baseline/constrained baseline, level <= 3.0, <= the core width, <= 480 lines, 8-bit, refs <= 1;
   High core: baseline/main/high, level <= 3.0, <= 640x480, refs <= 4). A mismatch gets **one lowered retry**
   (new session: lower size/bitrate step, Extra with every bound `isRequired` on the transcode target).
4. **SPS check** (the gate, first AU). An OUT verdict on a PMS transcode, with no ladder helper configured,
   replays the item **once** as the lowered PMS request (gate row R6p). With a helper configured the existing
   ladder fallback (R6) runs first. A second refusal shows the on-screen gate error (R7 text), never a black
   screen. NVENC 3-ref home PMS: lowered retry, then the ladder helper (prod a lab MiSTer has one) or the clear error.
   High core: a CAVLC/Baseline stream also takes the clear error until the CAVLC hand-off
   (HIGH-CAVLC-CORE-HANDOFF.md) lands.

## 6. Open / to verify on glass
- No-name request on a remote PMS and a LAN PMS: confirm Web profile + Extra yields Baseline L3.0 (x264 server)
  and what NVENC does with the profile/level targets.
- Whether any client param or header forces software encoding (none known).
- Which decision fields PMS fills for a transcode target (log the decision body once per play).

## 7. Findings 1011 night (verified unless marked)

- **One remote server (item 29729) forces Baseline server-side.** That PMS produced CAVLC Constrained Baseline with no
  MisterPlex profile and no profile name (B2 12.7 min on prodfix). The request carried nothing that selects it, so
  that server's config does (its own profile or encoder setup; the exact knob is **unverified**).
  It is not something a generic PMS does: a LAN PMS with the same request gave High/CABAC/refs 4.
- **The no-profile fallback drops `-profile`.** With no matching profile PMS uses its augmented generic (Plex Web)
  profile. Its x264 command has no `-profile:v` and Plex's default x264opts, so a software encode is High/CABAC.
  The Extra `add-limitation`/`isRequired` profile and refFrames bounds do not change the encoder command.
- **The Extra flags term works with no profile installed.** `add-transcode-target-settings(type=videoProfile&
  context=streaming&protocol=http&VideoEncodeFlags=<url-encoded flags>)` is appended verbatim to the encoder
  command (home PMS, software x264: Constrained Baseline, CAVLC, refs 1, no B, keyint per variant). This is the main
  Baseline-core fix now (`plexVideoEncodeFlags`, FPGA Baseline ladders only; the High core sends none).
- **With a profile name, the Extra must not be destructive (Round E).** `add-transcode-target(...replace=true)`
  replaced the named XML's TranscodeTarget and its VideoEncodeFlags Setting (MiSTerPlex-High.xml's
  cabac/bframes/ref=4 never reached x264). With a name sent, the daemon now sends only limitations, without
  replace=true, and no target or flags term.
- **The decision Stream fields are not the output.** level/refFrames are 0 or absent, and the profile can be the
  source's (remote PMS: `profile=main`; home: Media@videoProfile `high` for a Baseline output). Only codec and output
  size are checked; 0/absent fields are unknown. The first-AU SPS gate decides profile/refs/level.
- **NVENC refs: the prod core refuses the stream itself.** `h264_nvenc` ignores `-refs 1`/x264opts (max_num_ref_frames
  2 or 3), and its SPS also carries NAL HRD and pic_struct_present. The prod `sps_parser.sv` fails all three
  (`ST_REFS` ue>1, `ST_NAL_HRD`, `ST_PIC_STRUCT`). The decode path itself is fine: every NVENC slice uses one active
  ref, no list modification, no MMCO, all pictures reference, and h264_dpb_one_ref holds the previous picture. With
  the SPS rewritten (refs 1, no HRD/pic_struct, SEI dropped) the prod-core sim of the 30-frame H2 capture is
  pixel-exact against ffmpeg (30/30, mismatched_bytes 0). So the daemon gate keeps refs <= CAPS maxRefs, and an
  NVENC home PMS needs either software encoding (HardwareAcceleratedEncoders=0, current home setting) or an RTL
  change (sps_parser: accept max_num_ref_frames <= 2 with the per-slice daemon check, and skip HRD/pic_struct).
- **Gap (not fixed, recommended):** the daemon gate does not parse VUI past colour, so an SPS with NAL HRD or
  pic_struct but refs 1 would pass the daemon and fail in the core. Mirror ST_NAL_HRD/ST_VCL_HRD/ST_PIC_STRUCT in the
  gate (refuse unless CAPS kSpsRelax).
