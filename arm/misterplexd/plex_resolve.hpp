#pragma once
// Slim Plex resolve for MiSTerPlex Phase 2/4 (transitional ARM decode).
// Harvested from mistercast-linux lessons; full feature set ports later.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "libmisterplex/source_aspect.hpp"
#include "libmisterplex/video_backend.hpp"

namespace misterplex {

struct ResolveResult {
    bool ok = false;
    bool transcoded = false;
    std::string playable;     // path or http(s) URL for FFmpeg
    std::string httpHeaders;  // FFmpeg -headers (CRLF), if any
    std::string title;
    std::string detail;
    int64_t durationMs = 0;
    int64_t viewOffsetMs = 0;
    std::string ratingKey;
    // Phase 4 match-source-Hz / Content FPS hints from PMS metadata.
    std::string videoFrameRate; // raw Media@videoFrameRate (e.g. 24p, NTSC, 60p)
    std::string frameRate;      // raw Stream@frameRate (e.g. 23.976)
    int sourceFpsHint = 0;      // nearest OSD Content FPS: 12/24/30/60, or 0 unknown
    // Exact content rate as a rational (24000/1001 for 23.976). 0/0 = unknown.
    // Drives A/V pacing; must NOT be bucketed (23.976 vs 24 = ~1 ms/s of lipsync drift).
    int fpsNum = 0;
    int fpsDen = 0;
    // Source Media/Stream coded size from PMS metadata (0 = unknown). Used to decide
    // whether PMS can deliver DECODE without on-device FOAR+pad. Never trust
    // videoResolution alone (L38: FOAR @720p request can still be 720×480).
    int mediaWidth = 0;
    int mediaHeight = 0;
    // PMS Media scanType=interlaced or videoFrameRate ending in i (30i/60i).
    bool mediaInterlaced = false;
    // rc2.1: PMS scanType=progressive or videoFrameRate ending in p (24p/30p). false = unknown.
    bool mediaProgressive = false;
    // True when metadata lists an audio Stream (streamType=2). False when metadata
    // was parsed and only video is present (e.g. Grid720 freckle map). Unknown
    // (no metadata) stays true so product audio is not suppressed by accident.
    // FFmpeg dual-out aborts the whole process if pipe:3 is opened with no audio.
    bool hasAudio = true;
    // Original display aspect sent to MiSTer's native scaler. Invalid means the
    // caller must probe the actual stream before starting playback.
    SourceAspect sourceAspect{};
    // The X-Plex-Client-Profile-Name this play's PMS requests used (timeline repeats it).
    std::string clientProfileName;
    // SPS-gate L3: playable is the ladder helper's /ladder.ts (the stream is the helper's, GatePlayContext).
    bool viaLadderProxy = false;
    // PMS universal decision output geometry (Goal F menu reload). Filled when ensureUniversalDecision
    // accepts; do not rely on thread_local lastDecisionVideo() across resolve threads.
    int decisionWidth = 0;
    int decisionHeight = 0;
};

struct QueueItem {
    std::string key;
    std::string ratingKey;
    std::string playQueueItemId;
    std::string title;
    int64_t durationMs = 0;
    std::string type;   // <Video type=...>: "clip" = a PMS pre-roll / cinema trailer (BUG2-PREROLL)
};

struct PlayQueue {
    bool ok = false;
    std::string detail;
    std::string playQueueId;
    std::string playQueueVersion;
    std::string containerKey; // /playQueues/N
    std::vector<QueueItem> items;
    int currentIndex = 0;
};

std::string urlDecode(const std::string& in);
std::string plexHttpGet(const std::string& url, int timeoutSec = 15,
                        const std::string& extraHeaders = {});
std::string urlEncodeQuery(const std::string& s);

// Normalize a PMS base: trim, strip trailing slash, add http:// if bare host[:port].
std::string normalizePlexBase(const std::string& raw);

// Split comma/semicolon/whitespace-separated server list into normalized bases.
// Empty tokens skipped. Order preserved (first = default).
std::vector<std::string> parsePlexServerList(const std::string& csvOrSingle);

// Merge PLEX_SERVERS CSV + repeated PLEX_BASE values into a unique ordered list.
std::vector<std::string> mergePlexServers(const std::string& serversCsv,
                                          const std::vector<std::string>& baseLines);

// host, host:port, or a full http(s) URL. A port already in the address is kept.
std::string joinPlexHostPort(const std::string& protocol, const std::string& address,
                             const std::string& port);

// Timeline attributes are separate. A port already in the address is the port.
struct PlexEndpoint {
    std::string host;
    std::string port;
    std::string protocol;  // set only when address was an http(s) URL
    bool portInAddress = false;
};
PlexEndpoint splitPlexHostPort(const std::string& address, const std::string& port);

// Build http(s)://host:port base. Rewrites Docker-bridge plex.direct to empty
// (caller should supply LAN fallback).
std::string buildPlexBase(const std::string& protocol, const std::string& address,
                          const std::string& port, const std::string& lanFallback = {});

// Static ARM libav cannot open HTTPS. Convert cast PMS bases to plain HTTP when
// possible without mapping a *foreign* shared server onto conf PLEX_BASE.
// - https://198-51-100-153.<hash>.plex.direct:32400 → http://198.51.100.153:32400
// - docker-bridge plex.direct / 172.1x hosts → confLanFallback when provided
// - foreign private home LAN (different /24 than conf) → empty (use public lookup)
// - already-http bases pass through
// Returns empty if no openable base can be derived.
std::string companionOpenablePmsBase(const std::string& selectedBase,
                                     const std::string& confLanFallback = {});

// When cast pins a foreign private LAN address, resolve a public HTTP base via
// plex.tv resources for machineIdentifier + cast token. Prefer non-local,
// non-relay connections rewritten to plain http://A.B.C.D:port.
std::string companionPublicPmsBaseFromResources(const std::string& machineIdentifier,
                                                const std::string& token);

struct PlexTranscodeProfile {
    std::string name;
    std::string videoResolution;
    int maxVideoBitrateKbps = 0;
    int videoQuality = 0;
    // H.264 constraints advertised to PMS. Keep these aligned with the FPGA/host
    // decoder; do not ask PMS for Main/High/CABAC when only baseline-ish vectors
    // are proven.
    std::string h264Profile;
    int h264Level = 0; // Plex capability/profile-extra integer form: 30 == Level 3.0.
};

// One decision/resolve table for the three display modes. Shared request params
// (h264 baseline/main + AAC/stereo) live here once; 720p stays on plexTranscodeProfiles().
struct ModeResolveRow {
    const char* mode;              // "240p" | "480p" | "480i"
    const char* videoResolution;   // "320x240" | "640x480" | "720x480"
    int maxVideoBitrateKbps;
    int videoQuality;
    const char* videoCodec;        // "h264"
    const char* h264Profile;       // FPGA/host contract ("baseline")
    const char* h264ProfileParam;  // PMS query ("baseline,main")
    int h264Level;
    const char* audioCodec;        // "aac"
    int audioChannels;             // stereo
};

const std::vector<ModeResolveRow>& modeResolveTable();
const ModeResolveRow* modeResolveRow(const std::string& modeOrResolution);
bool applyModeResolve(const std::string& modeOrResolution, struct WeakLadder& weak);

// Built-in PMS universal transcode profiles. 240p/480p/480i come from modeResolveTable();
// 480p is the server-negotiation half of the 640x480 pipeline. 720p is ARM-only.
const std::vector<PlexTranscodeProfile>& plexTranscodeProfiles();

// Match by profile name ("240p"/"480p"/"480i") or exact WxH resolution.
bool applyPlexTranscodeProfile(const std::string& nameOrResolution,
                               struct WeakLadder& weak);

// Weak ladder params for PMS universal transcoder (dual-A9 safe defaults).
struct WeakLadder {
    std::string profileName = "240p";
    std::string videoResolution = "320x240"; // e.g. 320x240, 480x360, 640x480
    int maxVideoBitrateKbps = 1000;
    int videoQuality = 40;
    std::string videoCodec = "h264";
    std::string audioCodec = "aac";
    std::string h264Profile = "baseline";
    // PMS videoProfile= query. Empty → h264Profile. Mode table uses "baseline,main".
    std::string h264ProfileParam;
    // Loaded core is 0x48A64AED (Plex_high, MPX_HIGH_NO_CAVLC). A CAVLC PPS fails
    // closed as decoder_header_error 16. The ladder asks PMS for Main/CABAC
    // (one ref, no B, no 8x8, no weighted pred). Not a named client profile.
    bool highCabac = false;
    int h264Level = 30;
    int audioChannels = 2;
    // Server-side PMS profile that forces Baseline/CAVLC/ref=1 transcodes.
    // If the XML is absent, tested PMS versions fall back gracefully to Generic.
    std::string clientProfileName = "MiSTerPlex";
    // The FPGA stream-contract variant (MiSTerPlex-FPGA-<IP|IDR>-<tier>-<rate>-filter-<on|off>, or the
    // NTSC -src30- one) the ladder was built from. INTERNAL ONLY since 2026-10-11: it is never sent as
    // X-Plex-Client-Profile-Name (a remote PMS logged "Unable to find client profile
    // MiSTerPlex-FPGA-IP-480-23976-filter-off"); the header is plexRequestProfileName(). Non-empty
    // = an FPGA Baseline ladder (isFpgaPlexProfile).
    std::string fpgaProfileVariant;
    // Phase 4: ask PMS to burn subtitles into the universal ladder when set.
    bool burnSubtitles = false;
    int subtitleStreamId = -1; // -1 = PMS default / first
    int fpgaFpsNum = 0;
    int fpgaFpsDen = 0;
    // MPX_BROWSE_V2 plays of an mpx-menu "ladder" row only (empty for every cast): the ladder
    // input the helper chose (lead Q4: the original Part up to ~8 Mbps, the owning server's
    // own transcode above). Used instead of the metadata Part in the /ladder.ts src=; the
    // ladder still seeks it with &offset_ms.
    std::string ladderSrcOverride;
    // MPX_BROWSE_V2 play ticket mpxLadderInput="server-transcode" (lead Q4, Part above
    // ~8 Mbps): the ladder input is the owning server's own universal transcode of this item,
    // started at the offset, instead of the original Part over the owner's uplink. If its
    // decision fails, the Part as before. Never set for casts.
    bool ladderServerTranscode = false;
    // NO-REQUIRED-PROFILE: the one lowered PMS retry (lowerPmsLadder) after a decision or SPS mismatch.
    bool pmsLowered = false;
    // PlexOB row. Direct play on LAN and WAN. Profile-Extra carries core limits only.
    // No X-Plex-Client-Profile-Name and no Baseline VideoEncodeFlags.
    bool owenb = false;
    // BUG1 (lab 7AE78D80): a maxVideoBitrate cap for the PlexOB ladder only (0 = none). ladderForPlaySession
    // rebuilds the OB ladder from scratch, so a cap set on the legacy ladder (the CAVLC retry, the per-PMS memory)
    // travels here; the legacy FPGA paths ignore it. Never a Plex client profile.
    int obMaxKbps = 0;
};

// NO-REQUIRED-PROFILE (docs/plans/NO-REQUIRED-PROFILE.md section 4/5): the transcode decision's video fields,
// checked against what the loaded core takes. Only present fields are checked; the first AU's SPS (the gate)
// stays the authority.
struct DecisionVideo {
    std::string codec, profile, hwEncoding;
    int level = 0, refFrames = 0, bitDepth = 0, width = 0, height = 0;
    bool found = false;
    // Part@decision / Part@key from the universal decision body (directplay vs transcode).
    std::string partDecision;
    std::string partKey;
};
DecisionVideo parseDecisionVideo(const std::string& decisionXml);
bool decisionIsDirectPlay(const DecisionVideo& v);
// Last parsed decision on this thread (ensureUniversalDecision). Empty until a decision runs.
DecisionVideo lastDecisionVideo();
// "" when the decision fits the ladder's core: codec h264 and OUTPUT size <= the ladder's. Profile, refs, level
// and bit depth are left to the first-AU SPS gate (Media@videoProfile is the source's).
std::string decisionCoreMismatch(const DecisionVideo& v, const WeakLadder& weak);
// The lowered request: bitrate down a step (kept above the tier floor), every bound isRequired on the
// transcode target (profile, level, refFrames, size, 8-bit); pmsLowered set.
WeakLadder lowerPmsLadder(const WeakLadder& weak);
// BUG1-CAP (V2-BUGS-1017 #1): the lowered retry after an OB play ended "CAVLC at N kbps ..." asks the server for
// at most 85 % of OB_CAVLC_MAX_KBPS (never under 1000): the same server's encoder answers CAVLC again (a lab MiSTer
// log: 5228 then 5411 kbps at the unchanged 6000k ladder), so only the rate can bring it under the rewrite's
// ceiling (owenb docs/cavlc.md §8a option F). Any other ladder or reason comes back unchanged.
WeakLadder owenbCavlcRetryLadder(const WeakLadder& weak, const std::string& reason, int cavlcMaxKbps);
// BUG1 pre-play (owner, 2026-10-17): the SOURCE's entropy coding from the PMS metadata's video Stream.
// cabac: 1 / 0 from a cabac="1|0" attribute when PMS exposes one, else 0 for a Baseline / Constrained Baseline
// profile (Baseline has no CABAC), else -1 (unknown: Main / High may be either). bitrateKbps: the video
// Stream's bitrate, else Media@bitrate, else 0.
struct ObSourceEntropy {
    std::string codec;   // the video Stream's codec (lower case; Media@videoCodec when absent), "" unknown
    int cabac = -1;
    int bitrateKbps = 0;
    std::string profile;
};
ObSourceEntropy parseObSourceEntropy(const std::string& metadataXml);
// A PlexOB ladder whose source is known CAVLC above hardKbps (OB_CAVLC_HARD_MAX_KBPS) asks for 85 % of
// capKbps (OB_CAVLC_MAX_KBPS) on the FIRST request. Returns true when it lowered the ladder. A CABAC or
// unknown source, a source at or under hardKbps, or a non-PlexOB (legacy core) ladder is left unchanged:
// 4000-7000 kbps stays with the in-session load gate. Only a direct play is predicted this way; a PMS
// transcode's output entropy is not in the metadata (BUG1-CAP.md §6).
bool owenbPrePlayCavlcLadder(WeakLadder& weak, const ObSourceEntropy& src, int hardKbps, int capKbps);
// BUG1 (lab 7AE78D80: a remote PMS's HEVC Main10 source, transcoder CAVLC at 94-98 % rewrite load): a PlexOB ladder whose
// source video is known and not H.264 (HEVC, VP9, AV1, MPEG-2...: PMS must transcode, its output entropy is
// unpredictable) asks at most safeKbps (OB_TRANSCODE_SAFE_KBPS, 3200; ~55-60 % load measured) on the first
// request, keeping 720p. True when it lowered the ladder. H.264, an unknown codec or a legacy ladder: unchanged.
bool owenbTranscodeSafeLadder(WeakLadder& weak, const ObSourceEntropy& src, int safeKbps);
// The last ensureUniversalDecision mismatch on this thread (cleared on read).
std::string takeDecisionMismatch();
// Guard rails for built-in and configured ladders.
bool validateWeakLadder(const WeakLadder& weak, std::string* why = nullptr);

// AU-safe clamp values (maxVideoBitrateKbps, videoQuality) for an FPGA play start:
// flat (seekFlat) only when a non-zero offset restart goes through the resolve seek
// clamp (mid-title IDRs must stay small); otherwise the start ladder ceiling, by
// tier. main.cpp's doPlay ladder and plex_resolve's seek path share these numbers
// here so neither site keeps its own copy. maxBitrateKbps is kept at or above the
// validateWeakLadder() floor for the tier (750 kbps 240p seek-safe / 2000 kbps 480)
// so the daemon's own ladder always validates as an FPGA PMS ladder.
struct FpgaAuSafeClamp {
    int maxBitrateKbps;
    int maxQuality;
};
FpgaAuSafeClamp fpgaAuSafeClamp(bool tier480, long long offsetMs, bool seekFlat = false);
// 480i-specific min (kbps). Same numeric as the 480p AU-safe / validateWeakLadder
// 2000 floor so 480p/240p rungs stay put; 480i must not take the 240p 750 floor.
constexpr int kBitrateFloor480iKbps = 2000;
// 240p-specific min (kbps). Same numeric as the existing AU-safe 240p floor
// so 480p/480i 2000 rungs stay put; 240p must not use an invented 500.
constexpr int kBitrateFloor240pKbps = 750;
// True for 480p and 480i daemon rows (named profile or 480-tier geometry).
// Callers pass this into fpgaAuSafeClamp so 480i cannot take the 240p 750 floor.
bool fpgaAuSafeTier480(const WeakLadder& weak);

// The FPGA raster mode ("240p", "480p" or "480i") a play on this ladder resolves to.
// Order: MPX_FPGA_MODE, else live CoreGate paired raster (own-core CAPS), else conf
// ladder (480-tier → 480p, else 240p).
std::string fpgaModeForLadder(const WeakLadder& weak);
// True when the FPGA raster is NTSC-rate (240p, 480i, or the 480i present selected by
// a truthy MPX_PRESENT_480I, same test as envFlagWanted). Those rasters run ~60 Hz /
// 59.94 fields, so 24-family is 3:2 and 30-family is 2:2. Used for ladder fps=src,
// 24/30/60-family refuse, and companion fieldRate. False for 480p without the 480i
// present and for "" (not FPGA). Content clocks still remap (Goal D cadence).
bool fpgaRasterNtsc(const std::string& mode);
// Rate the FPGA path requests and paces (b21db60d signature). Every raster:
// 24/1 → 24000/1001, 30/1 → 30000/1001, 24000/1001 and 30000/1001 kept,
// 60/1 and 60000/1001 → 30000/1001, everything else (25/50) → 24000/1001.
// passthrough30 is unused (kept for ABI); NTSC rasters share the 480p clocks.
// Returns true when out differs from the source rate.
bool fpgaContentFps(int srcNum, int srcDen, bool passthrough30, int& outNum, int& outDen);
// Named 480i admission floor (~24 fps). 24/1 and 24000/1001 pass; a rate
// strictly below 24000/1001 fails closed (0/0 fails). Integer compare so
// 23.976 film is not rejected as "23".
constexpr int kMinFps480i = 24;
bool fpga480iMinFpsOk(int fpsNum, int fpsDen);
// Named 240p admission floor (~24 fps). 24/1 and 24000/1001 pass; a rate
// strictly below 24000/1001 fails closed (0/0 fails). Integer compare so
// 23.976 film is not rejected as "23".
constexpr int kMinFps240p = 24;
bool fpga240pMinFpsOk(int fpsNum, int fpsDen);
// Named 480p admission floor (~24 fps). 24/1 and 24000/1001 pass; a rate
// strictly below 24000/1001 fails closed (0/0 fails). Integer compare so
// 23.976 film is not rejected as "23". 480p-only; 240p/480i refuse text
// stays the NTSC family path.
constexpr int kMinFps480p = 24;
bool fpga480pMinFpsOk(int fpsNum, int fpsDen);
// Internal 30-family variant (240p / 480i / 480p):
// MiSTerPlex-FPGA-<IP|IDR>-<raster>-src30-filter-<on|off>. Never sent as
// X-Plex-Client-Profile-Name. noprofile Extra pins -r 30000/1001.
std::string fpgaNtscSourceProfileName(const std::string& prototype, const std::string& raster,
                                      bool filteringOff);
// TRUE640 A/B switch: FPGA 480p coded-width ceiling. Default 624 (39 MB/row,
// legacy). 640 (40 MB/row, PLEX_DECODE_CODED_640 cores) only when
// MPX_FPGA_480P_CODED_W=640 (env, or conf FPGA_480P_CODED_W=640 which main()
// exports when the env is unset). Any other value, incl. 624, forces 624.
int fpga480pCodedWidthCeiling();
// FPGA PMS ladder. mode "240p" is default product; "480p" is opt-in when the
// live core/DECODE path is the 624×480 DDR-DPB build (allowReservedExperiment).
bool applyFpgaPlexProfile(WeakLadder& weak, const std::string& prototype,
                          int fpsNum, int fpsDen, bool filteringOff,
                          std::string* why = nullptr,
                          const std::string& mode = "240p",
                          bool allowReservedExperiment = false);
// Fit the PMS transcode raster inside the configured ladder without adding bars.
// The host expands this anamorphically to the FPGA bank and MiSTer restores DAR.
WeakLadder fitWeakLadderToAspect(const WeakLadder& weak, const SourceAspect& aspect);
// namedProfile (Round E 1011): an X-Plex-Client-Profile-Name is also sent, so the named XML owns the transcode
// target and its encoder settings: no add-transcode-target(...replace=true) terms and no VideoEncodeFlags term
// (replace=true dropped MiSTerPlex-High.xml's cabac/bframes/ref=4), and the limitations go without replace=true.
std::string plexClientProfileExtra(const WeakLadder& weak, bool namedProfile = false);
// PlexOB ladder: 1280x720 cap, High/Main/Baseline, no named client profile.
// WAN starts at 6000 kb/s. LAN starts at 8000 kb/s.
WeakLadder owenbWeakLadder(bool wan);
// A verified PlexOB session asks for the Owenb ladder (direct play, 1280x720 cap).
// The configured FPGA ladder (624x480 baseline, directPlay=0) is for the legacy cores.
// A lowered retry keeps pmsLowered so the URL forces one transcode.
inline WeakLadder ladderForPlaySession(VideoBackend session, const WeakLadder& legacy, bool wan) {
    if (session != VideoBackend::OwenbRing)
        return legacy;
    const bool lowered = legacy.pmsLowered;
    WeakLadder w = owenbWeakLadder(wan);
    w.pmsLowered = lowered;
#ifndef MUT_OB_CAP_DROPPED
    w.obMaxKbps = legacy.obMaxKbps;
    if (legacy.obMaxKbps > 0 && legacy.obMaxKbps < w.maxVideoBitrateKbps)
        w.maxVideoBitrateKbps = legacy.obMaxKbps;
#endif
    return w;
}
// CAVLC rewrite on the ARM is for a positive bitrate at or under 4000 kb/s.
constexpr int kOwenbCavlcMaxKbps = 4000;
inline bool owenbCavlcRewriteOk(int bitrateKbps) {
    return bitrateKbps > 0 && bitrateKbps <= kOwenbCavlcMaxKbps;
}
// NO-REQUIRED-PROFILE: the encoder flags the per-variant server XML carried (VideoEncodeFlags), rebuilt from the
// FPGA Baseline variant so they travel in X-Plex-Client-Profile-Extra as add-transcode-target-settings. PMS
// appends them verbatim to the encoder command with no profile installed (home PMS 1011 S1/S2) and the
// add-transcode-target replace=true no longer drops an installed XML's flags. Empty for non-FPGA (High) ladders.
std::string plexVideoEncodeFlags(const WeakLadder& weak);
std::string plexClientCapabilities(const WeakLadder& weak);
inline bool isFpgaPlexProfile(const WeakLadder& weak) {
    return !weak.fpgaProfileVariant.empty();
}
// The fixed PMS client profile names. Every request (decision, start probe, FFmpeg start/segment
// reads, metadata, /:/timeline, transcode ping) sends exactly one of these as
// X-Plex-Client-Profile-Name: kBaselineClientProfileName for the 480p/240p Baseline cores,
// kHighClientProfileName for a High core. CLIENT_PROFILE_NAME (conf) / MPX_CLIENT_PROFILE_NAME (env,
// wins) replaces the name for a server whose owner installed the XML under another name.
inline constexpr const char* kBaselineClientProfileName = "MiSTerPlex";
void setClientProfileNameConf(const std::string& name);
// The operator override ("" = none); source = "env" | "conf" | "default".
std::string clientProfileNameOverride(std::string* source = nullptr);
std::string baselineClientProfileName(std::string* source = nullptr);
// The X-Plex-Client-Profile-Name this ladder's requests send (never a variant name).
// LAN/private-address PMS (a LAN server has the 16 MiSTerPlex-FPGA-* XMLs; its MiSTerPlex.xml
// picks NVENC, which ignores the x264 ref=1 flags, so the prod core refuses ref=3): a Baseline FPGA ladder
// sends its per-variant name (selectFpgaPmsProfile). Remote/WAN PMS and an unknown
// target get the fixed MiSTerPlex / MiSTerPlex-High. The CLIENT_PROFILE_NAME override wins everywhere.
// NO-REQUIRED-PROFILE (docs/plans/NO-REQUIRED-PROFILE.md, owner 1011 20:59 CT): by default NO
// X-Plex-Client-Profile-Name is sent (empty result); correctness comes from the params + Profile-Extra.
// Opt-in for testing: CLIENT_PROFILE_NAME / MPX_CLIENT_PROFILE_NAME (that exact name everywhere);
// SEND_PROFILE_NAME / MPX_SEND_PROFILE_NAME = fixed (MiSTerPlex / MiSTerPlex-High) or variant (the
// per-variant MiSTerPlex-FPGA-* name to a LAN/private PMS, fixed elsewhere).
std::string plexRequestProfileName(const WeakLadder& weak, const std::string& targetUrl = {});
enum class SendProfileNameMode { None, Fixed, Variant };
void setSendProfileNameConf(const std::string& value);
SendProfileNameMode sendProfileNameMode();
const char* sendProfileNameModeName(SendProfileNameMode m);
// The startup log's statement of the profile name actually sent and where it comes from (Round E 1011: the old
// line printed send=none next to an override that does send the name).
std::string profileNameSendDescription();
// True when the URL's host is loopback/RFC1918 (the location=lan rule of the start/decision URLs).
bool plexTargetIsPrivateLan(const std::string& url);
// The name for requests without a ladder (metadata, probe identity block, timeline): the loaded core's.
std::string defaultRequestProfileName();
// Extra is Name / MPX_SEND_PROFILE_EXTRA / 720p caps only. forceLadderCaps does not
// send Extra on WAN for 240p/480p/480i.
// 720p: remote PMS has no MiSTerPlex.xml and emits 640x480 unless we advertise
// 1280x720 (40868 probe=640x480 → unique ~15). 240p/480p/480i Extra is opt-in.
inline bool plexSendClientLadderCaps(const WeakLadder& weak) {
    // FPGA ≤480p: Extra carries CAVLC/refFrames=1/8-bit without a named
    // MiSTerPlex.xml (Goal B). URL videoProfile=baseline,main alone is not
    // enough (110 L1 BBB SPS refuse). 720p still sends caps Extra.
    if (isFpgaPlexProfile(weak))
        return true;
    return weak.videoResolution == "1280x720";
}
// Extra is on for FPGA unnamed ladders and 720p caps; MPX_SEND_PROFILE_EXTRA=0 does not
// disable FPGA Extra (that would revive the BBB SPS refuse).
bool plexSendProfileExtra();
bool plexClientSendsProfileExtra(const WeakLadder& weak, bool forceLadderCaps,
                                 const std::string& profileName);
// "&mediaBufferSize=N" only when MPX_REMOTE_MEDIA_BUFFER_KB is set (A/B proof), else "" (no acceptWav).
std::string remoteMediaBufferParams();
std::string buildUniversalTranscodeUrl(const std::string& base,
                                       const std::string& metadataKey,
                                       const std::string& token,
                                       const std::string& session,
                                       int64_t offsetMs,
                                       const WeakLadder& weak,
                                       std::string* why = nullptr);
// MPX_BROWSE_V2 (lead Q4): the owning server's own universal transcode as a ladder src, started
// at offsetMs (a negative offset counts as 0): buildUniversalTranscodeUrl's start URL with the
// X-Plex set libav sends carried as query parameters, because the ladder fetches it without
// headers. Empty when there is no transcode URL for this raster. *startUrl, if given, receives the
// start URL (for the transcode decision). Extracted from resolve (T-BROWSE-DOUBLE-SEEK F-B,
// H264 n2) so the unit test builds its vectors with the producer resolve uses.
std::string ownerTranscodeLadderSrc(const std::string& plexBase, const std::string& key,
                                    const std::string& token, const std::string& session,
                                    int64_t offsetMs, const WeakLadder& fit, bool wan,
                                    std::string* startUrl = nullptr);
std::string buildUniversalDecisionUrl(const std::string& startUrl,
                                      const std::string& sessionId,
                                      const std::string& token,
                                      const WeakLadder& weak);

// True when metadata Media@videoCodec looks like H.264/AVC (direct Part friendly for STREAM).
bool mediaVideoIsH264(const std::string& plexMetadataXml);

// Derive display aspect from PMS metadata. Explicit display/DAR metadata wins;
// otherwise coded dimensions are combined with explicit sample/pixel aspect.
// Coded dimensions alone are used only when PMS explicitly marks non-anamorphic.
SourceAspect sourceAspectFromPlexMetadata(const std::string& plexMetadataXml,
                                         int codedWidth, int codedHeight);

// Resolve a playMedia key against PMS, or pass through local/http paths.
// weakAlways: always request PMS universal H.264 ladder (recommended on dual A9 / STREAM=0).
// preferDirectH264: when true (STREAM=1 product path), use direct Part stream if source is
// already H.264 so host CAVLC recon can run on Baseline/Main without High/CABAC remux.
// Non-H.264 still falls through to the weak universal ladder.
// decodeW/H: when >0 and Media size exactly matches the bank, prefer direct Part on
// STREAM=0 (skip PMS re-encode for true-720→720). Larger sources into smaller banks
// must use the weak ladder so content mode reaches PMS encode (G0b).
// forceLadder / gateDetail (SPS-gate L3, the gate fallback of the SAME item): take the ladder helper even when no
// conf term selects it. With no helper configured, resolve the same item as PMS universal (never a
// direct Part, never a hard refuse). r.detail then starts "gate fallback: <gateDetail>".
ResolveResult resolvePlayTarget(const std::string& rawKeyOrPath, const std::string& plexBase,
                                const std::string& token, int64_t offsetMs = 0,
                                bool weakAlways = true, const WeakLadder& weak = {},
                                bool preferDirectH264 = false, int decodeW = 0,
                                int decodeH = 0, bool forceLadder = false,
                                const std::string& gateDetail = {});

// SPS-gate L3: a ladder helper is configured (MPX_REMOTE_LADDER_PROXY, else REMOTE_LADDER_PROXY, non-empty).
// Product default is empty (direct PMS). Non-empty is debug opt-in. The one read shared by resolve
// and the gate's GatePlayContext, so they cannot disagree on "helper configured".
bool ladderProxyConfigured();

// Fetch /playQueues/{id} for next-episode / skipNext. currentKey or playQueueItemId
// selects currentIndex when present.
// BUG2-PREROLL: parse a /playQueues/{id} MediaContainer (split out of fetchPlayQueue for the unit test).
PlayQueue parsePlayQueueXml(const std::string& xml, const std::string& id, const std::string& currentKey,
                            const std::string& playQueueItemId);

PlayQueue fetchPlayQueue(const std::string& queueIdOrContainerKey, const std::string& plexBase,
                         const std::string& token, const std::string& currentKey = {},
                         const std::string& playQueueItemId = {});

// Companion offsets are milliseconds; PMS universal `offset=` is whole seconds.
// Rounds half-up so 1500 ms → 2 s (matches buildUniversal), without the signed overflow of
// (offsetMs + 500) / 1000 near INT64_MAX: doPlay resolves with the raw cast offset before it
// clamps the start (same arithmetic as transcode_offset::roundMsToSec).
inline int64_t universalOffsetSeconds(int64_t offsetMs) {
    if (offsetMs <= 0)
        return 0;
    return offsetMs / 1000 + (offsetMs % 1000 >= 500 ? 1 : 0);
}

// Chrome-profile FFmpeg headers required by PMS universal transcoder.
// Extra is Name / MPX_SEND_PROFILE_EXTRA / 720p caps only (forceLadderCaps is ignored for Extra).
std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token);
std::string plexFfmpegHeaders(const std::string& sessionId, const std::string& token,
                              const WeakLadder& weak, bool forceLadderCaps = false,
                              const std::string& targetUrl = {});
// The remote universal start alive-probe headers. A High/CABAC ladder also
// sends Profile-Extra (no Profile-Name): the probe is the first start.mp4 for
// the session, and a probe without VideoEncodeFlags makes PMS open a CAVLC
// session (Plex_high error 16). Baseline probes stay unchanged.
std::vector<std::pair<std::string, std::string>> plexStartProbeHeaders(const std::string& sessionId,
                                                                       const std::string& token,
                                                                       const WeakLadder& weak,
                                                                       const std::string& startUrl);
// MPX_PREWARM: OFF unless MPX_PREWARM is set to something other than "" / "0".
bool plexPrewarmProbeEnabled();
// MPX_PREWARM's single-GET probe: status + the body bytes (binary-safe).
struct PlexStartProbe {
    int httpStatus = 0;
    std::string body;
};
PlexStartProbe plexHttpGetStatusAndBody(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& clientProfileName, int timeoutSec);
// Seek coalescing: the resolve thread's "superseded?" check. A decision is not issued once it returns
// true (a later seek/play owns the session), so one coalesced seek never runs two decisions.
void setResolveAbortCheck(std::function<bool()> check);
bool resolveAborted();

// Best-effort PMS GET using the same curl-based Plex client identity as resolve.
// Headers are name/value pairs; response body is discarded by callers.
// Success is HTTP 2xx only — a non-empty 401 HTML body is a failure (not "ok").
struct PlexHttpNoBodyResult {
    bool ok = false;
    int httpStatus = 0; // 0 = transport/parse failure; otherwise curl %{http_code}
};

// True iff status is an HTTP success (2xx). status<=0 is never ok.
bool plexHttpStatusOk(int httpStatus);

// Parse curl -w '%{http_code}' output (trims whitespace). Returns 0 if not a 3-digit code.
int parseCurlHttpCode(const std::string& curlWriteOut);

PlexHttpNoBodyResult plexHttpGetNoBodyResult(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers = {},
    int timeoutSec = 4);

bool plexHttpGetNoBody(const std::string& url,
                       const std::vector<std::pair<std::string, std::string>>& headers = {},
                       int timeoutSec = 4);

// Call /universal/decision before start.mp4 (PMS 1.43+).
// forceLadderCaps: send the ladder caps whatever the profile, as plexFfmpegHeaders does for a src
// that forces them (the owning-server transcode on WAN), so the session's decision matches its src.
bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token);
bool ensureUniversalDecision(const std::string& startUrl, const std::string& sessionId,
                             const std::string& token, const WeakLadder& weak,
                             bool forceLadderCaps = false);

// Map PMS videoFrameRate / frameRate strings to OSD Content FPS (12/24/30/60).
// Returns 0 when unknown. Prefers numeric frameRate when present.
int contentFpsHint(const std::string& videoFrameRate, const std::string& frameRate = {});

// Apply conf SOURCE_FPS (auto|12|24|30|60|off) over a resolved hint.
// "auto"/empty → use resolvedHint; "off" → 0; numeric → forced bucket.
int applySourceFpsConf(const std::string& sourceFpsConf, int resolvedHint);

// Exact content frame rate as a rational — NOT bucketed.
// Prefers numeric Stream@frameRate ("23.976"), falls back to Media@videoFrameRate
// tokens ("24p", "NTSC", "PAL"). Snaps near-values onto the standard broadcast family
// so 23.976 becomes exactly 24000/1001 rather than a lossy decimal.
// Returns false and leaves num/den at 0 when the rate cannot be determined.
bool parseExactFps(const std::string& videoFrameRate, const std::string& frameRate,
                   int& num, int& den);

// Parse conf AV_CONTENT_FPS override ("auto"|"off"|"23.976"|"24000/1001"|"25").
// "auto"/empty keeps the resolved rate; "off" forces unknown (0/0).
// Returns true when num/den were set from the conf (including the "off" case).
bool applyContentFpsConf(const std::string& conf, int& num, int& den);

// Cap unique present rate at maxHz (product: 30). 60→30, 60000/1001→30000/1001.
// 23.976 / 24 / 25 / 29.97 stay themselves. Returns true if num/den changed.
bool capExactFpsToMaxHz(int& num, int& den, double maxHz = 30.0);

} // namespace misterplex
