#pragma once
#include "burn_subs_fix.hpp"
#include "forced_subs.hpp"
#include "plex_text.hpp"
#include "subtitle_sink.hpp"
// Per-show audio and subtitle selection for the Plex companion.
//
// Protocol (Plex Companion, the same verbs Plex Web and the mobile apps send):
//   GET /player/playback/setStreams?audioStreamID=<id>&subtitleStreamID=<id>&type=video&commandID=<n>
//   Either query parameter may be omitted. subtitleStreamID=0 means subtitles off.
//   The player answers with the same timeline document a poll returns (HTTP 200,
//   application/xml, X-Plex-Client-Identifier, state still playing). videoDecision
//   is the real session (direct or transcode) once resolve knows; it is omitted
//   until then (plex-mpv-shim never sends the attribute). A lie of
//   videoDecision=direct on a transcode play is a client-side Playback Error.
//   GDM /resources protocolCapabilities must include audioStream,subtitleStream or
//   Plex Web re-sends playMedia and treats a buffering ACK as Playback Error.
//   /resources Player also advertises this box's LAN IPv4 + companion port.
//   Timeline.address/port/protocol while playing are the PMS LAN IPv4 the
//   player actually opened (never a playMedia ULA, *.plex.direct host, or the
//   player's own VLAN gateway) so Web can fetch metadata for the in-play
//   stream menu. PMS /:/timeline must carry the same audioStreamID/subtitleStreamID
//   as the companion poll (plex-mpv-shim GetCurrentTimeline sends one dict to both).
//
//   Sources:
//   - Plex Media Player, src/input/InputComponent.cpp: the "setStreams" player command
//     reads audioStreamID and subtitleStreamID (0 disables subtitles).
//     https://github.com/plexinc/plex-media-player/blob/master/src/input/InputComponent.cpp
//   - Plex Media Player, src/player/PlayerComponent.cpp: the timeline state carries the
//     active audio and subtitle stream ids for subscribers.
//     https://github.com/plexinc/plex-media-player/blob/master/src/player/PlayerComponent.cpp
//   - python-plexapi plexapi/client.py setAudioStream / setSubtitleStream: GET
//     /player/playback/setStreams with those query keys (subtitleStreamID=0 disables).
//     https://github.com/pkkid/python-plexapi/blob/master/plexapi/client.py
//   - OpenPHT xbmc/plex/Client (PlexHTTPRemoteHandler / timeline XML): the video
//     <Timeline> element echoes audioStreamID and subtitleStreamID. Plex Web reads
//     those attributes on each poll and highlights the matching stream.
//     https://github.com/RasPlex/OpenPHT
//
// PMS remembers a part's default the same way the official clients do:
//   PUT /library/parts/{partId}?audioStreamID=<id>&subtitleStreamID=<id>
//   subtitleStreamID=0 clears the default subtitle.
//   python-plexapi plexapi/media.py Part.setDefaultAudioStream / setDefaultSubtitleStream /
//   resetDefaultSubtitleStream.
//   https://github.com/pkkid/python-plexapi/blob/master/plexapi/media.py
//
// Browse Play with no picker pick leaves subtitleStreamID unset. Resolve then
// applies SUBTITLE_LANG (G7): matching soft text only. A title with only a
// non-matching language stays off. PMS selected="1" text wins only when
// SUBTITLE_LANG is off/empty. A per-show choice (grandparentRatingKey for an
// episode, ratingKey for a movie) is matched by language, then codec, then
// the forced flag. Image selected="1" is never auto-picked (G10).
// FORCED_SUBS (default off; MPX_FORCED_SUBS wins) is the only extra: on, a
// play or re-resolve whose pick is still off selects a forced="1" stream.
// Soft text stays on the overlay. Image uses the existing burn path. Off,
// unset, a stored subtitle choice, and subtitleStreamID including 0 do not.
//
// Soft text (sidecar SRT/ASS/SSA) is offered to mpx::SubtitleSink and stays
// Direct Play. Image and embedded tracks are not auto-picked; an explicit
// setStreams / playMedia image id still burns via PMS. subtitleStreamID=0 is off.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace misterplex {

enum class StreamKind : uint8_t { Other, Video, Audio, Subtitle };

struct MediaStream {
    int id = 0;
    int index = -1;                 // container index (PMS Stream@index), -1 unknown
    StreamKind kind = StreamKind::Other;
    std::string codec;
    std::string language;           // canonical short tag when we know one
    std::string languageRaw;
    bool selected = false;          // PMS selected="1" (account / server default)
    bool forced = false;
    bool canAutoSync = false;       // PMS canAutoSync="1"; the renderer may auto-adjust only then
    int partId = 0;                 // PMS Part@id that owns this stream (0 if the tag had none)
    std::string key;                // sidecar: /library/streams/{id}
    std::string title;
    std::string displayTitle;
    bool hearingImpaired = false;
    std::string location;           // "external" vs embedded
};

struct StreamCatalog {
    std::vector<MediaStream> streams;
    bool operator==(const StreamCatalog& o) const {
        if (streams.size() != o.streams.size())
            return false;
        for (size_t i = 0; i < streams.size(); ++i) {
            const MediaStream& a = streams[i];
            const MediaStream& b = o.streams[i];
            if (a.id != b.id || a.index != b.index || a.kind != b.kind || a.codec != b.codec ||
                a.language != b.language || a.selected != b.selected || a.forced != b.forced ||
                a.canAutoSync != b.canAutoSync || a.partId != b.partId || a.key != b.key ||
                a.displayTitle != b.displayTitle || a.hearingImpaired != b.hearingImpaired ||
                a.location != b.location)
                return false;
        }
        return true;
    }
};

// One remembered choice for a show (or a movie).
struct ShowPreference {
    std::string showKey;
    std::string audioLanguage;
    std::string audioCodec;
    bool subtitlesOff = true;
    std::string subtitleLanguage;
    std::string subtitleCodec;
    bool subtitleForced = false;
};

struct TrackPick {
    bool valid = false;
    int audioStreamId = 0;
    int audioIndex = -1;
    int subtitleStreamId = 0;       // 0 = off
    int subtitleIndex = -1;         // -1 = off / not a text stream we can open
    bool subtitlesOff = true;
    bool fromPreference = false;
    bool imageSubtitleSkipped = false;
    std::string imageNote;
    std::string audioLanguage;
    std::string audioCodec;
    std::string subtitleLanguage;
    std::string subtitleCodec;
    bool subtitleForced = false;
    // SOFT_SUBS on and this pick is embedded text (packet decode, not a sidecar fetch).
    bool packetText = false;
    // REMEMBER_TRACKS applied this subtitle, including Off. The ladder then names
    // it the way an explicit setStreams pick does. Flag off leaves this false.
    bool rememberedSubtitle = false;
};

// What /player/playback/setStreams carried. Absent parameters are left unset.
struct SetStreamsRequest {
    bool hasAudio = false;
    bool hasSubtitle = false;
    int audioStreamId = 0;
    int subtitleStreamId = 0;       // 0 = off, only meaningful when hasSubtitle
};

// What the player applied. The companion echoes these on the timeline ACK.
struct SetStreamsResult {
    bool known = false;
    int audioStreamId = 0;
    int subtitleStreamId = 0;
    std::string note;
};

inline std::string trackLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string trackTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// languageCode (eng, en) or a display name (English) -> a short tag. Unknown tags pass through lowercased.
inline std::string canonLanguage(const std::string& raw) {
    std::string s = trackLower(trackTrim(raw));
    if (s.empty() || s == "und" || s == "unknown" || s == "xn")
        return {};
    const auto sp = s.find_first_of(" _-(");
    if (sp != std::string::npos)
        s = s.substr(0, sp);
    struct Row { const char* a; const char* b; };
    static constexpr Row kMap[] = {
        {"english", "en"}, {"eng", "en"}, {"en", "en"},
        {"french", "fr"}, {"fra", "fr"}, {"fre", "fr"}, {"fr", "fr"},
        {"german", "de"}, {"deu", "de"}, {"ger", "de"}, {"de", "de"},
        {"spanish", "es"}, {"spa", "es"}, {"es", "es"},
        {"japanese", "ja"}, {"jpn", "ja"}, {"ja", "ja"},
        {"chinese", "zh"}, {"zho", "zh"}, {"chi", "zh"}, {"zh", "zh"},
        {"korean", "ko"}, {"kor", "ko"}, {"ko", "ko"},
        {"italian", "it"}, {"ita", "it"}, {"it", "it"},
        {"portuguese", "pt"}, {"por", "pt"}, {"pt", "pt"},
        {"russian", "ru"}, {"rus", "ru"}, {"ru", "ru"},
        {"dutch", "nl"}, {"nld", "nl"}, {"dut", "nl"}, {"nl", "nl"},
        {"swedish", "sv"}, {"swe", "sv"}, {"sv", "sv"},
        {"norwegian", "no"}, {"nor", "no"}, {"no", "no"},
        {"danish", "da"}, {"dan", "da"}, {"da", "da"},
        {"finnish", "fi"}, {"fin", "fi"}, {"fi", "fi"},
        {"polish", "pl"}, {"pol", "pl"}, {"pl", "pl"},
        {"arabic", "ar"}, {"ara", "ar"}, {"ar", "ar"},
        {"hindi", "hi"}, {"hin", "hi"}, {"hi", "hi"},
        {"turkish", "tr"}, {"tur", "tr"}, {"tr", "tr"},
        {"czech", "cs"}, {"ces", "cs"}, {"cze", "cs"}, {"cs", "cs"},
        {"hungarian", "hu"}, {"hun", "hu"}, {"hu", "hu"},
        {"greek", "el"}, {"ell", "el"}, {"gre", "el"}, {"el", "el"},
        {"hebrew", "he"}, {"heb", "he"}, {"he", "he"},
        {"thai", "th"}, {"tha", "th"}, {"th", "th"},
        {"vietnamese", "vi"}, {"vie", "vi"}, {"vi", "vi"},
        {"indonesian", "id"}, {"ind", "id"}, {"id", "id"},
        {"ukrainian", "uk"}, {"ukr", "uk"}, {"uk", "uk"},
    };
    for (const Row& r : kMap)
        if (s == r.a)
            return r.b;
    return s;
}

inline bool languagesMatch(const std::string& a, const std::string& b) {
    const std::string ca = canonLanguage(a), cb = canonLanguage(b);
    return !ca.empty() && ca == cb;
}

inline bool subtitleCodecIsImage(const std::string& codec) {
    const std::string c = trackLower(codec);
    return c == "pgs" || c == "pgssub" || c == "hdmv_pgs_subtitle" || c == "vobsub" || c == "dvd_subtitle" ||
           c == "dvdsub" || c == "dvb_subtitle" || c == "dvbsub" || c == "xsub";
}

inline bool subtitleCodecIsSoftText(const std::string& codec) {
    const std::string c = trackLower(codec);
    return c == "srt" || c == "subrip" || c == "ass" || c == "ssa";
}

inline bool subtitleCodecIsText(const std::string& codec) {
    const std::string c = trackLower(codec);
    if (c.empty())
        return true;               // PMS sometimes omits it; libav still refuses an image codec at open
    if (subtitleCodecIsImage(c))
        return false;
    return subtitleCodecIsSoftText(c) || c == "mov_text" || c == "text" || c == "webvtt" || c == "vtt" ||
           c == "ttml" || c == "subt" || c == "sub" || c == "smi";
}

inline bool subtitleIsSidecar(const MediaStream& s) {
    const std::string loc = trackLower(s.location);
    if (loc == "external")
        return true;
    if (s.key.rfind("/library/streams/", 0) == 0)
        return true;
    // PMS often omits key on selected external SRT/ASS. Soft codecs still qualify.
    return subtitleCodecIsSoftText(s.codec);
}

inline bool subtitleIsSoftSidecar(const MediaStream& s) {
    return s.kind == StreamKind::Subtitle && subtitleCodecIsSoftText(s.codec) && subtitleIsSidecar(s);
}

inline bool subtitleCodecIsWebVtt(const std::string& codec) {
    const std::string c = trackLower(codec);
    return c == "webvtt" || c == "vtt";
}

inline bool subtitleCodecIsMovText(const std::string& codec) {
    const std::string c = trackLower(codec);
    return c == "mov_text" || c == "text";
}

inline bool subtitleExplicitlyExternal(const MediaStream& s) {
    if (trackLower(s.location) == "external")
        return true;
    return s.key.rfind("/library/streams/", 0) == 0;
}

inline bool subtitleExplicitlyEmbedded(const MediaStream& s) {
    return trackLower(s.location) == "embedded";
}

// Embedded text the packet decoder shows. mov_text / "text" overlay even with
// SOFT_SUBS off (#260). Other packet codecs still need the flag.
// External SRT/ASS stay on the sidecar fetch. External WebVTT is a sidecar too.
inline bool subtitleIsPacketTextTrack(const MediaStream& s) {
    if (s.kind != StreamKind::Subtitle || subtitleCodecIsImage(s.codec))
        return false;
    if (subtitleCodecIsMovText(s.codec))
        return trackLower(s.location) != "external"; // embedded rows may still carry a streams key
    if (!softSubsEnabled())
        return false;
    if (subtitleCodecIsWebVtt(s.codec))
        return !subtitleExplicitlyExternal(s);
    if (subtitleCodecIsSoftText(s.codec))
        return subtitleExplicitlyEmbedded(s);
    return false;
}

inline bool subtitleIsExternalWebVtt(const MediaStream& s) {
    return softSubsEnabled() && s.kind == StreamKind::Subtitle && subtitleCodecIsWebVtt(s.codec) &&
           subtitleExplicitlyExternal(s);
}

// External / key-bearing text the soft sink can GET from PMS (or a local cache).
// Includes srt/subrip/ass/ssa and, when external or keyed, vtt/webvtt/mov_text.
// Embedded-only mov_text stays PacketText overlay (GET /library/streams is 501).
inline bool subtitleCodecIsSoftFetchable(const std::string& codec) {
    const std::string c = trackLower(codec);
    return subtitleCodecIsSoftText(c) || subtitleCodecIsWebVtt(c) || subtitleCodecIsMovText(c);
}

inline bool subtitleIsSoftFetchSidecar(const MediaStream& s) {
    if (s.kind != StreamKind::Subtitle || !subtitleCodecIsSoftFetchable(s.codec))
        return false;
    if (subtitleExplicitlyEmbedded(s))
        return false;
    if (subtitleCodecIsSoftText(s.codec))
        return true;   // PMS often omits location/key on selected SRT/ASS
    return subtitleExplicitlyExternal(s);
}

inline mpx::SubtitleTrackSpec::Kind softFetchKindForCodec(const std::string& codec) {
    const std::string c = trackLower(codec);
    if (c == "ass" || c == "ssa")
        return mpx::SubtitleTrackSpec::Kind::TextAss;
    if (subtitleCodecIsSoftFetchable(c))
        return mpx::SubtitleTrackSpec::Kind::TextSrt;
    return mpx::SubtitleTrackSpec::Kind::None;
}

// Sidecar SRT/ASS always. Packet text when SOFT_SUBS is on, or mov_text (#260).
inline bool subtitleIsSoftNow(const MediaStream& s) {
    if (subtitleIsSoftSidecar(s))
        return true;
    return subtitleIsPacketTextTrack(s) || subtitleIsExternalWebVtt(s);
}

// Direct Play stays when the overlay can show the codec. Flag off: SRT/ASS/SSA and mov_text.
inline bool subtitleCodecKeepsDirectPlay(const std::string& codec) {
    if (subtitleCodecIsSoftText(codec))
        return true;
    if (packetTextAlwaysOverlay(codec))
        return true;
    if (!softSubsEnabled())
        return false;
    return subtitleCodecIsWebVtt(codec);
}

// Container index for AvSource when the pick is embedded text. -1 when off or sidecar.
inline int textSubtitleContainerIndex(const TrackPick& p) {
    if (!p.packetText || p.subtitlesOff || p.subtitleIndex < 0)
        return -1;
    if (softSubsEnabled() || packetTextAlwaysOverlay(p.subtitleCodec))
        return p.subtitleIndex;
    return -1;
}

inline std::string subtitlePickerLabel(const MediaStream& s) {
    if (!s.displayTitle.empty())
        return s.displayTitle;
    if (!s.title.empty())
        return s.title;
    std::string o = s.language.empty() ? (s.languageRaw.empty() ? std::string("Unknown") : s.languageRaw)
                                       : s.language;
    if (!s.codec.empty())
        o += " " + s.codec;
    if (s.forced)
        o += " forced";
    if (s.hearingImpaired)
        o += " SDH";
    return o;
}

// Conf default language. Empty / "off" = no auto-lang. forcedOnly = forced tracks of that lang.
struct SubtitleDefaults {
    std::string lang;
    bool forcedOnly = false;
};

inline SubtitleDefaults& processSubtitleDefaults() {
    static SubtitleDefaults d;
    return d;
}

inline bool subtitleLangIsOff(const std::string& raw) {
    const std::string s = trackLower(trackTrim(raw));
    return s.empty() || s == "off" || s == "0" || s == "none";
}

// Plex stream id for OpenRequest / selectStreams. 0 = off. Never -1: the session
// must see Off as a real choice so the caption sink clears.
inline int subtitleIndexForOpen(const TrackPick& p) {
    if (p.subtitlesOff || p.subtitleStreamId <= 0)
        return 0;
    return p.subtitleStreamId;
}

// playMedia subtitleStreamID for the in-flight resolve. -1 = unset.
inline int& pendingPlayMediaSubtitleId() {
    static int id = -1;
    return id;
}

// playMedia / browse-picker audioStreamID for the in-flight resolve. -1 = unset.
inline int& pendingPlayMediaAudioId() {
    static int id = -1;
    return id;
}

inline std::string xmlTagAttr(const std::string& tag, const char* name) {
    const std::string k = std::string(name) + "=\"";
    const auto p = tag.find(k);
    if (p == std::string::npos)
        return {};
    const auto b = p + k.size();
    const auto e = tag.find('"', b);
    if (e == std::string::npos)
        return {};
    return xmlUnescape(tag.substr(b, e - b));
}

inline int xmlAttrInt(const std::string& tag, const char* name, int fallback = 0) {
    const std::string v = xmlTagAttr(tag, name);
    if (v.empty())
        return fallback;
    errno = 0;
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    if (!end || end == v.c_str() || *end != '\0' || errno == ERANGE)
        return fallback;
    if (n > static_cast<long long>(INT_MAX) || n < static_cast<long long>(INT_MIN))
        return fallback;
    return static_cast<int>(n);
}

inline StreamCatalog parseStreamCatalog(const std::string& xml) {
    StreamCatalog cat;
    size_t sp = 0;
    int partId = 0;
    while (sp < xml.size()) {
        const auto part = xml.find("<Part", sp);
        const auto stream = xml.find("<Stream", sp);
        if (part == std::string::npos && stream == std::string::npos)
            break;
        const bool takePart = part != std::string::npos && (stream == std::string::npos || part < stream);
        const auto at = takePart ? part : stream;
        const auto end = xml.find('>', at);
        if (end == std::string::npos)
            break;
        const std::string tag = xml.substr(at, end - at);
        if (takePart) {
            partId = xmlAttrInt(tag, "id", 0);
            sp = end + 1;
            continue;
        }
        MediaStream s;
        s.partId = partId;
        s.id = xmlAttrInt(tag, "id", 0);
        const bool hasIndex = tag.find("index=\"") != std::string::npos;
        s.index = hasIndex ? xmlAttrInt(tag, "index", -1) : -1;
        const std::string st = xmlTagAttr(tag, "streamType");
        const std::string ty = trackLower(xmlTagAttr(tag, "type"));
        if (st == "1" || ty == "video")
            s.kind = StreamKind::Video;
        else if (st == "2" || ty == "audio")
            s.kind = StreamKind::Audio;
        else if (st == "3" || ty == "subtitle" || ty == "subtitles")
            s.kind = StreamKind::Subtitle;
        s.codec = trackLower(xmlTagAttr(tag, "codec"));
        s.languageRaw = xmlTagAttr(tag, "languageCode");
        if (s.languageRaw.empty())
            s.languageRaw = xmlTagAttr(tag, "language");
        s.language = canonLanguage(s.languageRaw);
        s.selected = xmlTagAttr(tag, "selected") == "1";
        s.forced = xmlTagAttr(tag, "forced") == "1";
        s.canAutoSync = xmlTagAttr(tag, "canAutoSync") == "1";
        s.key = xmlTagAttr(tag, "key");
        s.title = xmlTagAttr(tag, "title");
        s.displayTitle = xmlTagAttr(tag, "displayTitle");
        if (s.displayTitle.empty())
            s.displayTitle = xmlTagAttr(tag, "extendedDisplayTitle");
        s.hearingImpaired = xmlTagAttr(tag, "hearingImpaired") == "1";
        s.location = trackLower(xmlTagAttr(tag, "location"));
        if (s.kind != StreamKind::Other)
            cat.streams.push_back(std::move(s));
        sp = end + 1;
    }
    return cat;
}

// Episode -> grandparentRatingKey. Movie (and anything else with no grandparent) -> ratingKey.
inline std::string showMemoryKeyFromXml(const std::string& xml) {
    auto firstOpen = [&](const char* el) -> std::string {
        const auto p = xml.find(std::string("<") + el);
        if (p == std::string::npos)
            return {};
        const auto e = xml.find('>', p);
        if (e == std::string::npos)
            return {};
        return xml.substr(p, e - p);
    };
    const std::string video = firstOpen("Video");
    const std::string tag = !video.empty() ? video : firstOpen("MediaContainer");
    if (tag.empty())
        return {};
    const std::string type = trackLower(xmlTagAttr(tag, "type"));
    const std::string gp = xmlTagAttr(tag, "grandparentRatingKey");
    const std::string rk = xmlTagAttr(tag, "ratingKey");
    if (type == "episode" || !gp.empty())
        return !gp.empty() ? gp : rk;
    return rk;
}

inline int partIdFromXml(const std::string& xml) {
    const auto p = xml.find("<Part");
    if (p == std::string::npos)
        return 0;
    const auto e = xml.find('>', p);
    if (e == std::string::npos)
        return 0;
    return xmlAttrInt(xml.substr(p, e - p), "id", 0);
}

// partId > 0 keeps a later part's stream from being applied to the part now playing.
inline const MediaStream* findStreamById(const StreamCatalog& cat, int id, StreamKind kind, int partId = 0) {
    if (id <= 0)
        return nullptr;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != kind || s.id != id)
            continue;
        if (partId > 0 && s.partId > 0 && s.partId != partId)
            continue;
        return &s;
    }
    return nullptr;
}

// The one subtitle kind the player opens (MediaPlayer::makeSubtitleSpec calls this),
// after refuseSoftOverlayWhenOff. The resolve asks the same function, so a pick is
// either painted by the band or burned by PMS, never both (#201-r3).
// s: the catalog row (nullptr on a catalog miss). pickCodec: the pick's codec on a miss.
inline mpx::SubtitleTrackSpec::Kind playerSubtitleKind(const MediaStream* s, const std::string& pickCodec) {
    mpx::SubtitleTrackSpec spec;
    spec.streamId = 1;
    spec.codec = s ? s->codec : pickCodec;
    spec.kind = mpx::kindForCodec(spec.codec);
    if (s && subtitleIsPacketTextTrack(*s))
        spec.kind = mpx::SubtitleTrackSpec::Kind::PacketText;
    else if (s && subtitleIsSoftFetchSidecar(*s))
        spec.kind = softFetchKindForCodec(s->codec);
    else if (!s && subtitleCodecIsSoftFetchable(pickCodec))
        spec.kind = softFetchKindForCodec(pickCodec);
    else if (s && subtitleIsExternalWebVtt(*s))
        spec.kind = mpx::SubtitleTrackSpec::Kind::TextSrt;
    return mpx::refuseSoftOverlayWhenOff(spec).kind;
}

inline bool subtitleKindPaints(mpx::SubtitleTrackSpec::Kind k) {
    return mpx::kindIsSoftText(k) || k == mpx::SubtitleTrackSpec::Kind::PacketText;
}

// Will the soft band paint this pick? Mirrors the player exactly (playerSubtitleKind).
inline bool overlayWillPaintPick(const StreamCatalog& cat, const TrackPick& p, int partId = 0) {
    // No imageSubtitleSkipped check: that is a pick-time note, and the player
    // paints by kind regardless, so this must not use it either.
    if (p.subtitlesOff || p.subtitleStreamId <= 0)
        return false;
#ifdef MUT_R3_MIRROR_CODEC_ONLY
    (void)cat;
    (void)partId;
    return softOverlayAllowed() && subtitleCodecKeepsDirectPlay(p.subtitleCodec);
#else
    const MediaStream* s = findStreamById(cat, p.subtitleStreamId, StreamKind::Subtitle, partId);
    return subtitleKindPaints(playerSubtitleKind(s, s ? std::string() : p.subtitleCodec));
#endif
}

// Final spec (reviewer, 2026-11-01): SRT/ASS/SSA never burn, flag on or off. They
// always go to the overlay. BURN_SUBS_FIX forces the transcode burn for image subs only.
inline bool subtitlePickMayBurn(const StreamCatalog& cat, const TrackPick& p, int partId = 0) {
#ifdef MUT_R3_SRT_BURNS_FLAG_ON
    if (burnSubsFixEnabled() && subtitleCodecIsSoftText(p.subtitleCodec) && !softSubsEnabled())
        return true;
#endif
    if (p.subtitlesOff || p.subtitleStreamId <= 0 || subtitleCodecIsSoftText(p.subtitleCodec))
        return false;
    // autoBurnEnabled: explicit AUTO_BURN wins; legacy BURN_SUBS_FIX=1 is on
    // only when AUTO_BURN is unset. Neither key is off.
    if (!autoBurnEnabled())
        return false;
    return !overlayWillPaintPick(cat, p, partId);
}

inline bool burnSubsFixBurnsImage(const TrackPick& p) {
#ifdef MUT_R3_FORCE_ANY_BURN
    return burnSubsFixEnabled();
#elif defined(MUT_R3_MOV_TEXT_AS_IMAGE)
    return burnSubsFixEnabled() && (subtitleCodecIsImage(p.subtitleCodec) || subtitleCodecIsMovText(p.subtitleCodec));
#else
    // mov_text is a text sub (reviewer 2026-11-01): BURN_SUBS_FIX never touches it.
    return burnSubsFixEnabled() && subtitleCodecIsImage(p.subtitleCodec);
#endif
}

namespace track_detail {

inline int scoreAudio(const MediaStream& s, const ShowPreference& pref) {
    int score = 0;
    if (languagesMatch(s.language, pref.audioLanguage))
        score += 4;
    if (!pref.audioCodec.empty() && trackLower(s.codec) == trackLower(pref.audioCodec))
        score += 2;
    if (s.selected)
        score += 1;
    return score;
}

inline int scoreSubtitle(const MediaStream& s, const ShowPreference& pref) {
    int score = 0;
    if (languagesMatch(s.language, pref.subtitleLanguage))
        score += 4;
    if (!pref.subtitleCodec.empty() && trackLower(s.codec) == trackLower(pref.subtitleCodec))
        score += 2;
    // +3 so a forced-flag match beats a selected non-forced track (+1 selected, +1 consolation).
    if (pref.subtitleForced == s.forced)
        score += 3;
    else if (!s.forced)
        score += 1;                 // a non-forced track still beats a forced one when the user did not ask for forced
    if (s.selected)
        score += 1;
    if (!subtitleCodecIsText(s.codec))
        score -= 8;                 // image streams lose to any text stream of the same language
    return score;
}

inline bool streamInPart(const MediaStream& s, int partId) {
    return partId <= 0 || s.partId <= 0 || s.partId == partId;
}

inline const MediaStream* bestAudio(const StreamCatalog& cat, const ShowPreference* pref, int partId = 0) {
    const MediaStream* best = nullptr;
    int bestScore = -1;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Audio || !streamInPart(s, partId))
            continue;
        int score = 0;
        if (pref && !pref->audioLanguage.empty())
            score = scoreAudio(s, *pref);
        else
            score = s.selected ? 2 : 0;
        if (!best || score > bestScore || (score == bestScore && s.selected && !best->selected)) {
            best = &s;
            bestScore = score;
        }
    }
    if (pref && !pref->audioLanguage.empty() && best && bestScore < 4) {
        // No language hit: the account default, else the first audio stream.
        const MediaStream* sel = nullptr;
        const MediaStream* first = nullptr;
        for (const MediaStream& s : cat.streams) {
            if (s.kind != StreamKind::Audio || !streamInPart(s, partId))
                continue;
            if (!first)
                first = &s;
            if (s.selected)
                sel = &s;
        }
        return sel ? sel : first;
    }
    return best;
}

inline const MediaStream* bestSoftDefault(const StreamCatalog& cat, const SubtitleDefaults& def, bool& imageOnly,
                                          int partId) {
    imageOnly = false;
    const MediaStream* sel = nullptr;
    bool anyImage = false;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !streamInPart(s, partId))
            continue;
        if (s.selected && subtitleCodecIsImage(s.codec))
            anyImage = true;
        // Sidecar SRT/ASS, and external WebVTT when SOFT_SUBS is on. A later
        // sidecar replaces an earlier one. Packet text is the fallback.
        if (s.selected && (subtitleIsSoftSidecar(s) || subtitleIsExternalWebVtt(s)) &&
            (!def.forcedOnly || s.forced))
            sel = &s;
        else if (!sel && s.selected && subtitleIsPacketTextTrack(s) && (!def.forcedOnly || s.forced))
            sel = &s;
    }
    // G7: SUBTITLE_LANG, when set, is the browse-Play-with-no-pick default.
    // Do not fall back to PMS selected="1" or the only other language.
    if (!subtitleLangIsOff(def.lang)) {
        const std::string want = canonLanguage(def.lang);
        const MediaStream* lang = nullptr;
        const MediaStream* forced = nullptr;
        for (const MediaStream& s : cat.streams) {
            if (!subtitleIsSoftNow(s) || !streamInPart(s, partId))
                continue;
            if (!languagesMatch(s.language, want) && !languagesMatch(s.languageRaw, want))
                continue;
            if (!lang)
                lang = &s;
            if (s.forced)
                forced = &s;
        }
        if (def.forcedOnly)
            return forced;
        if (lang)
            return lang;
        if (anyImage)
            imageOnly = true;
        return nullptr;
    }
    if (sel)
        return sel;
    if (anyImage)
        imageOnly = true;
    return nullptr;
}

inline const MediaStream* bestTextSubtitle(const StreamCatalog& cat, const ShowPreference* pref, bool& imageOnly,
                                            int partId = 0, const SubtitleDefaults* def = nullptr) {
    imageOnly = false;
#ifdef MUT_G7_IGNORE_LANG
    const SubtitleDefaults empty;
    const SubtitleDefaults local = def ? *def : empty;
#else
    const SubtitleDefaults local = def ? *def : processSubtitleDefaults();
#endif
    if (!pref || pref->subtitlesOff || pref->subtitleLanguage.empty())
        return bestSoftDefault(cat, local, imageOnly, partId);
    const MediaStream* best = nullptr;
    int bestScore = -100;
    bool anyImageLang = false;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !streamInPart(s, partId))
            continue;
        if (!languagesMatch(s.language, pref->subtitleLanguage) && !s.language.empty())
            continue;
        if (s.language.empty() && !languagesMatch(s.languageRaw, pref->subtitleLanguage))
            continue;
        if (!subtitleIsSoftNow(s)) {
            if (subtitleCodecIsImage(s.codec) && languagesMatch(s.language, pref->subtitleLanguage))
                anyImageLang = true;
            continue;
        }
        const int score = scoreSubtitle(s, *pref);
        if (!best || score > bestScore) {
            best = &s;
            bestScore = score;
        }
    }
    if (!best)
        imageOnly = anyImageLang;
    // A language match is required (score's language term is +4). Codec/forced only break ties.
    if (best && bestScore < 4)
        return nullptr;
    return best;
}

// Forced="1" on this part. SUBTITLE_LANG, when set, keeps G7: no match, no pick.
// Soft text the overlay can show beats other text, which beats image. selected="1"
// breaks a tie. Document order breaks the rest. Image is eligible: the caller
// hands it to the burn path already on this tip.
inline const MediaStream* bestForcedSubtitle(const StreamCatalog& cat, int partId, const SubtitleDefaults& def) {
    const bool langOn = !subtitleLangIsOff(def.lang);
    const std::string want = langOn ? canonLanguage(def.lang) : std::string();
    const MediaStream* best = nullptr;
    int bestTier = 0;
    int bestSel = 0;
    for (const MediaStream& s : cat.streams) {
        if (s.kind != StreamKind::Subtitle || !s.forced || s.id <= 0 || !streamInPart(s, partId))
            continue;
#ifdef MUT_FORCED_SUBS_SKIP_IMAGE
        if (subtitleCodecIsImage(s.codec))
            continue;
#endif
        if (langOn && !languagesMatch(s.language, want) && !languagesMatch(s.languageRaw, want))
            continue;
        int tier = 1;
        if (subtitleIsSoftNow(s))
            tier = 0;
        else if (subtitleCodecIsImage(s.codec))
            tier = 2;
        const int sel = s.selected ? 0 : 1;
        if (!best || tier < bestTier || (tier == bestTier && sel < bestSel)) {
            best = &s;
            bestTier = tier;
            bestSel = sel;
        }
    }
    return best;
}

} // namespace track_detail

// Flag off returns p unchanged. Flag on fills a forced track only when this
// pick is still off and the user did not already choose subtitles.
inline TrackPick applyForcedSubsHonour(const StreamCatalog& cat, TrackPick p, int partId, bool userDeclined,
                                       const SubtitleDefaults& def) {
    bool on = forcedSubsEnabled();
#ifdef MUT_FORCED_SUBS_OFF_STILL_PICKS
    on = true;
#endif
    if (!on)
        return p;
#ifdef MUT_FORCED_SUBS_IGNORED
    (void)cat;
    (void)partId;
    (void)userDeclined;
    (void)def;
    return p;
#endif
#ifndef MUT_FORCED_SUBS_OVERRIDE_PICK
    if (userDeclined)
        return p;
    if (!p.subtitlesOff && p.subtitleStreamId > 0)
        return p;
#else
    (void)userDeclined;
#endif
    const MediaStream* s = track_detail::bestForcedSubtitle(cat, partId, def);
    if (!s)
        return p;
    p.subtitlesOff = false;
    p.subtitleStreamId = s->id;
    p.subtitleIndex = s->index;
    p.subtitleLanguage = s->language;
    p.subtitleCodec = s->codec;
    p.subtitleForced = true;
    p.packetText = subtitleIsPacketTextTrack(*s);
    const bool soft = subtitleIsSoftNow(*s);
    p.imageSubtitleSkipped = !soft;
    p.imageNote = soft ? std::string()
                       : "subtitles (" + (s->codec.empty() ? std::string("embedded") : s->codec) +
                             ") are burned by PMS";
    return p;
}

// pref null: SUBTITLE_LANG (when set), else selected="1" text sidecar, else off.
// Image selected="1" stays off. pref set: match language / codec / forced among
// text sidecars only. pendingPlayMediaSubtitleId() >= 0 is an explicit pick
// (cast playMedia / browse picker, including Off=0) and overrides the default.
inline TrackPick pickTracks(const StreamCatalog& cat, const ShowPreference* pref, int partId = 0,
                            const SubtitleDefaults* def = nullptr) {
    TrackPick p;
    bool any = false;
    for (const MediaStream& s : cat.streams)
        if ((s.kind == StreamKind::Audio || s.kind == StreamKind::Subtitle) &&
            track_detail::streamInPart(s, partId))
            any = true;
    if (!any && !pref)
        return p;
    p.valid = any || pref;
    p.fromPreference = pref != nullptr;
    if (const MediaStream* a = track_detail::bestAudio(cat, pref, partId)) {
        p.audioStreamId = a->id;
        p.audioIndex = a->index;
        p.audioLanguage = a->language;
        p.audioCodec = a->codec;
    }
    bool imageOnly = false;
    if (pref && pref->subtitlesOff) {
        p.subtitlesOff = true;
        p.subtitleStreamId = 0;
        p.subtitleIndex = -1;
        p.packetText = false;
    } else if (const MediaStream* s = track_detail::bestTextSubtitle(cat, pref, imageOnly, partId, def)) {
        p.subtitlesOff = false;
        p.subtitleStreamId = s->id;
        p.subtitleIndex = s->index;
        p.subtitleLanguage = s->language;
        p.subtitleCodec = s->codec;
        p.subtitleForced = s->forced;
        p.packetText = subtitleIsPacketTextTrack(*s);
    } else {
        p.subtitlesOff = true;
        p.subtitleStreamId = 0;
        p.subtitleIndex = -1;
        p.packetText = false;
        if (imageOnly) {
            p.imageSubtitleSkipped = true;
            p.imageNote = "image subtitles (PGS/VobSub) are not shown";
        }
    }
    if (!any)
        p.valid = false;
    const SubtitleDefaults used = def ? *def : processSubtitleDefaults();
    // A saved "off", or a saved subtitle language, is a user choice. An explicit
    // subtitleStreamID is applied by the caller after this returns.
    const bool userDeclined = pref && (pref->subtitlesOff || !pref->subtitleLanguage.empty());
    return applyForcedSubsHonour(cat, p, partId, userDeclined, used);
}

inline ShowPreference preferenceFromPick(const std::string& showKey, const TrackPick& pick) {
    ShowPreference p;
    p.showKey = showKey;
    p.audioLanguage = pick.audioLanguage;
    p.audioCodec = pick.audioCodec;
    p.subtitlesOff = pick.subtitlesOff || pick.subtitleStreamId <= 0;
    p.subtitleLanguage = p.subtitlesOff ? std::string() : pick.subtitleLanguage;
    p.subtitleCodec = p.subtitlesOff ? std::string() : pick.subtitleCodec;
    p.subtitleForced = !p.subtitlesOff && pick.subtitleForced;
    return p;
}

// Apply an explicit setStreams choice. An id that is not on this part is rejected:
// the current id stays, and the caller must not echo the unknown one. Image and
// embedded subtitles are accepted for PMS burn-in.
inline TrackPick pickFromSetStreams(const StreamCatalog& cat, const TrackPick& current, const SetStreamsRequest& req,
                                    int partId = 0) {
    TrackPick p = current;
    p.valid = true;
    p.fromPreference = true;
    if (req.hasAudio) {
        if (const MediaStream* a = findStreamById(cat, req.audioStreamId, StreamKind::Audio, partId)) {
            p.audioStreamId = a->id;
            p.audioIndex = a->index;
            p.audioLanguage = a->language;
            p.audioCodec = a->codec;
        }
        // Unknown audioStreamID: keep current. Do not copy the requested id.
    }
    if (req.hasSubtitle) {
        if (req.subtitleStreamId <= 0) {
            p.subtitlesOff = true;
            p.subtitleStreamId = 0;
            p.subtitleIndex = -1;
            p.subtitleLanguage.clear();
            p.subtitleCodec.clear();
            p.subtitleForced = false;
            p.packetText = false;
            p.imageSubtitleSkipped = false;
            p.imageNote.clear();
        } else if (const MediaStream* s = findStreamById(cat, req.subtitleStreamId, StreamKind::Subtitle, partId)) {
            p.subtitlesOff = false;
            p.subtitleStreamId = s->id;
            p.subtitleIndex = s->index;
            p.subtitleLanguage = s->language;
            p.subtitleCodec = s->codec;
            p.subtitleForced = s->forced;
            p.packetText = subtitleIsPacketTextTrack(*s);
            const bool soft = subtitleIsSoftNow(*s);
            p.imageSubtitleSkipped = !soft;
            p.imageNote = soft ? std::string()
                               : "subtitles (" + (s->codec.empty() ? std::string("embedded") : s->codec) +
                                     ") are burned by PMS";
        }
        // Unknown subtitleStreamID: keep current. Do not copy the requested id.
    }
    return p;
}

// Explicit audio pick (browse picker / cast playMedia). audioId <= 0 is unset and
// leaves the default pick; an id that is not an audio stream of this part is ignored
// (the default stays, never a silent guess).
inline TrackPick pickAudioForPlay(const StreamCatalog& cat, const TrackPick& current, int audioId, int partId = 0) {
    if (audioId <= 0)
        return current;
    SetStreamsRequest forced;
    forced.hasAudio = true;
    forced.audioStreamId = audioId;
    return pickFromSetStreams(cat, current, forced, partId);
}

// True when the explicit audio pick differs from the default pick. A direct Part
// plays the container's default audio, so a non-default pick must go through the
// PMS universal transcode (which honours audioStreamID).
inline bool audioPickIsNonDefault(const TrackPick& defaultPick, const TrackPick& picked) {
    return picked.audioStreamId > 0 && picked.audioStreamId != defaultPick.audioStreamId;
}

// PMS universal start.mp4 transcodes the part's *selected* audio and ignores the audioStreamID URL param (glass
// 2026-10-25), so a non-default pick is also written to the part, the way Plex clients do:
//   PUT /library/parts/{partId}?audioStreamID={id}&allParts=1   (no token; the caller adds it)
inline std::string partAudioSelectionPath(int partId, int audioStreamId) {
    if (partId <= 0 || audioStreamId <= 0)
        return {};
    return "/library/parts/" + std::to_string(partId) + "?audioStreamID=" + std::to_string(audioStreamId) +
           "&allParts=1";
}

// The container's own default audio (what a direct Part plays): the first audio stream of the part.
inline int firstAudioStreamId(const StreamCatalog& cat, int partId) {
    for (const MediaStream& s : cat.streams)
        if (s.kind == StreamKind::Audio && (partId <= 0 || s.partId == 0 || s.partId == partId))
            return s.id;
    return 0;
}

// The stream PMS has marked selected for the first part's audio (0 = none marked).
inline int pmsSelectedAudioId(const StreamCatalog& cat, int partId) {
    for (const MediaStream& s : cat.streams)
        if (s.kind == StreamKind::Audio && s.selected && (partId <= 0 || s.partId == 0 || s.partId == partId))
            return s.id;
    return 0;
}

// The PUT mutates PMS's per-user selection, so remember what was selected before our first PUT on a part and put it
// back the next time that part is played without a pick (or with the original picked). Process memory only.
struct AudioPartPutPlan {
    int putAudioId = 0;     // 0 = no PUT needed
    bool restore = false;   // the PUT puts the original selection back
};
class AudioSelectionMemory {
public:
    AudioPartPutPlan plan(int partId, int pmsSelected, int picked) {
        AudioPartPutPlan p;
        if (partId <= 0)
            return p;
        std::lock_guard<std::mutex> g(mu_);
        const auto it = orig_.find(partId);
        if (picked > 0) {
            if (it != orig_.end()) {
                const int o = it->second;
                if (picked == o) {
                    orig_.erase(it);
                    if (o > 0 && pmsSelected != o) {
                        p.putAudioId = o;
                        p.restore = true;
                    }
                } else if (picked != pmsSelected) {
                    p.putAudioId = picked;
                }
            } else if (picked != pmsSelected) {
                orig_[partId] = pmsSelected;
                p.putAudioId = picked;
            }
        } else if (it != orig_.end()) {
            const int o = it->second;
            orig_.erase(it);
            if (o > 0 && pmsSelected != o) {
                p.putAudioId = o;
                p.restore = true;
            }
        }
        return p;
    }
    // A failed PUT leaves PMS unchanged: forget the memory entry we just made.
    void forget(int partId) {
        std::lock_guard<std::mutex> g(mu_);
        orig_.erase(partId);
    }
    size_t size() {
        std::lock_guard<std::mutex> g(mu_);
        return orig_.size();
    }
private:
    std::mutex mu_;
    std::map<int, int> orig_;
};
inline AudioSelectionMemory& processAudioSelection() {
    static AudioSelectionMemory m;
    return m;
}

// FU14 / #234: PMS universal start.mp4 burns the part's selected subtitle and ignores
// subtitleStreamID on the URL. With BURN_SUBS_FIX explicitly on, write the burn pick
// before the decision and put the original selection back on the next play that burns
// nothing. The flag default is off, so this never runs unless conf opts in.
//   PUT /library/parts/{partId}?subtitleStreamID={id}&allParts=1   (caller adds the token)
inline std::string partSubtitleSelectionPath(int partId, int subtitleStreamId) {
    if (partId <= 0 || subtitleStreamId < 0)
        return {};
    return "/library/parts/" + std::to_string(partId) + "?subtitleStreamID=" + std::to_string(subtitleStreamId) +
           "&allParts=1";
}

inline int pmsSelectedSubtitleId(const StreamCatalog& cat, int partId) {
    for (const MediaStream& s : cat.streams)
        if (s.kind == StreamKind::Subtitle && s.selected && (partId <= 0 || s.partId == 0 || s.partId == partId))
            return s.id;
    return 0;
}

struct SubtitlePartPutPlan {
    bool put = false;
    int putSubtitleId = 0;
    bool restore = false;
};
class SubtitleSelectionMemory {
public:
    // burnPick > 0: the subtitle this play burns. <= 0: this play burns nothing.
    SubtitlePartPutPlan plan(int partId, int pmsSelected, int burnPick) {
        SubtitlePartPutPlan p;
        if (partId <= 0)
            return p;
        std::lock_guard<std::mutex> g(mu_);
        const auto it = orig_.find(partId);
        if (burnPick > 0) {
            if (burnPick != pmsSelected) {
                if (it == orig_.end())
                    orig_[partId] = pmsSelected;
                p.put = true;
                p.putSubtitleId = burnPick;
            }
            return p;
        }
#ifndef MUT_BURN_SUBS_SELECT_NO_RESTORE
        if (it != orig_.end()) {
            const int o = it->second;
            orig_.erase(it);
            if (pmsSelected != o) {
                p.put = true;
                p.putSubtitleId = o;
                p.restore = true;
            }
        }
#endif
        return p;
    }
    void forget(int partId) {
#ifndef MUT_BURN_SUBS_SELECT_KEEP_ON_FAIL
        std::lock_guard<std::mutex> g(mu_);
        orig_.erase(partId);
#else
        (void)partId;
#endif
    }
    size_t size() {
        std::lock_guard<std::mutex> g(mu_);
        return orig_.size();
    }
private:
    std::mutex mu_;
    std::map<int, int> orig_;
};
inline SubtitleSelectionMemory& processSubtitleSelection() {
    static SubtitleSelectionMemory m;
    return m;
}

// Resolve / browse Play: pendingSubtitleId < 0 means no explicit pick (apply
// SUBTITLE_LANG). 0 is Off. >0 is that Plex stream id (cast or picker).
inline TrackPick pickTracksForPlay(const StreamCatalog& cat, int pendingSubtitleId, int partId = 0,
                                   const SubtitleDefaults* def = nullptr) {
    TrackPick p = pickTracks(cat, nullptr, partId, def);
    if (pendingSubtitleId < 0)
        return p;
    SetStreamsRequest forced;
    forced.hasSubtitle = true;
    forced.subtitleStreamId = pendingSubtitleId;
    p = pickFromSetStreams(cat, p, forced, partId);
#ifdef MUT_FORCED_SUBS_OVERRIDE_PICK
    // Explicit subtitleStreamID, including 0, must win. This mutant applies the
    // forced track again afterwards.
    const SubtitleDefaults used = def ? *def : processSubtitleDefaults();
    return applyForcedSubsHonour(cat, p, partId, false, used);
#else
    return p;
#endif
}

// Query-string (or a full HTTP request line) parser. Keys must be a real parameter,
// not a suffix of another name.
inline std::optional<std::string> trackQueryParam(const std::string& req, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t pos = 0;
    while ((pos = req.find(k, pos)) != std::string::npos) {
        const bool boundary = pos == 0 || req[pos - 1] == '?' || req[pos - 1] == '&' || req[pos - 1] == ' ';
        pos += k.size();
        if (!boundary)
            continue;
        const auto end = req.find_first_of(" &#\r\n", pos);
        return req.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    }
    return std::nullopt;
}

// Ids are parsed as 64-bit and rejected when they do not fit in a signed 32-bit
// value. 4294967297 must not truncate to 1. A leading '+' or any trailing junk fails.
inline bool trackParseInt(const std::string& s, int& out) {
    if (s.empty() || s.size() > 18)
        return false;
    if (s[0] == '+' || s[0] == '-' || s[0] == ' ')
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long n = std::strtoull(s.c_str(), &end, 10);
    if (!end || *end != '\0' || end == s.c_str() || errno == ERANGE)
        return false;
    if (n > static_cast<unsigned long long>(INT_MAX))
        return false;
    out = static_cast<int>(n);
    return true;
}

// The request-target only. Header lines are not a source of stream ids.
inline std::string trackRequestTarget(const std::string& httpOrQuery) {
    const auto nl = httpOrQuery.find_first_of("\r\n");
    const std::string line = httpOrQuery.substr(0, nl);
    if (line.rfind("GET ", 0) == 0 || line.rfind("PUT ", 0) == 0 || line.rfind("POST ", 0) == 0 ||
        line.rfind("HEAD ", 0) == 0) {
        const auto a = line.find(' ');
        const auto b = line.find(' ', a + 1);
        if (a == std::string::npos)
            return line;
        return line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    }
    return line;
}

inline SetStreamsRequest parseSetStreams(const std::string& httpOrQuery) {
    SetStreamsRequest r;
    const std::string target = trackRequestTarget(httpOrQuery);
    if (const auto a = trackQueryParam(target, "audioStreamID")) {
        int v = 0;
        if (trackParseInt(*a, v) && v > 0) {
            r.hasAudio = true;
            r.audioStreamId = v;
        }
    }
    if (const auto s = trackQueryParam(target, "subtitleStreamID")) {
        int v = 0;
        if (trackParseInt(*s, v) && v >= 0) {
            r.hasSubtitle = true;
            r.subtitleStreamId = v;
        }
    }
    return r;
}

// Attributes for the video <Timeline> element. Empty until a selection is known.
// subtitleStreamID=0 is included on purpose: Plex Web treats 0 as "off" and a missing
// attribute as "unknown", which snaps the menu back to the server default.
inline std::string timelineStreamAttrs(bool known, int audioStreamId, int subtitleStreamId) {
    if (!known)
        return {};
    if (audioStreamId < 0)
        audioStreamId = 0;
    if (subtitleStreamId < 0)
        subtitleStreamId = 0;
    return "audioStreamID=\"" + std::to_string(audioStreamId) + "\" subtitleStreamID=\"" +
           std::to_string(subtitleStreamId) + "\" ";
}

// track_pref_server_sync in misterplex.conf. Default off (missing, 0, or anything
// other than 1/true/yes/on). The part PUT is a persistent manual pick for that
// Plex user and turns off their auto-select for that item in every Plex app.
inline bool trackPrefServerSyncEnabled(const std::string& confValue) {
    return confValue == "1" || confValue == "true" || confValue == "yes" || confValue == "on";
}

// PUT /library/parts/{id}?audioStreamID=&subtitleStreamID=  (no token; the caller adds it).
// Sent only when track_pref_server_sync is on. Local show prefs are separate.
inline std::string partStreamSelectionPath(int partId, int audioStreamId, int subtitleStreamId) {
    if (partId <= 0)
        return {};
    if (audioStreamId < 0)
        audioStreamId = 0;
    if (subtitleStreamId < 0)
        subtitleStreamId = 0;
    return "/library/parts/" + std::to_string(partId) + "?audioStreamID=" + std::to_string(audioStreamId) +
           "&subtitleStreamID=" + std::to_string(subtitleStreamId);
}

inline std::string partStreamSelectionUrl(const std::string& base, int partId, int audioStreamId,
                                         int subtitleStreamId) {
    const std::string path = partStreamSelectionPath(partId, audioStreamId, subtitleStreamId);
    if (path.empty())
        return {};
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    return b + path;
}

// ---- per-show file -------------------------------------------------------
// One record a line. A bad line is skipped. A file whose first line is not the
// magic is quarantined (renamed to <path>.bad) and the store starts empty.

inline constexpr const char* kShowTrackMagic = "MPXSHOWTRACKS 1";
inline constexpr size_t kMaxShowPrefs = 256;
inline constexpr size_t kMaxShowPrefField = 64;
inline constexpr size_t kMaxShowTrackFileBytes = 256u * 1024u;

inline bool showPrefFieldOk(const std::string& s) { return s.size() <= kMaxShowPrefField; }

inline std::string showPrefEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '|') {
            o.push_back('\\');
            o.push_back(c);
            continue;
        }
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        if (c == '\r') {
            o += "\\r";
            continue;
        }
        o.push_back(c);
    }
    return o;
}

inline bool showPrefDecode(const std::string& line, ShowPreference& out) {
    std::vector<std::string> f;
    std::string cur;
    bool esc = false;
    for (char c : line) {
        if (esc) {
            if (c == 'n')
                cur.push_back('\n');
            else if (c == 'r')
                cur.push_back('\r');
            else
                cur.push_back(c);
            esc = false;
            continue;
        }
        if (c == '\\') {
            esc = true;
            continue;
        }
        if (c == '|') {
            f.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == '\r')
            continue;
        cur.push_back(c);
    }
    f.push_back(cur);
    if (f.size() != 7 || f[0].empty() || !showPrefFieldOk(f[0]))
        return false;
    for (const std::string& field : f)
        if (!showPrefFieldOk(field))
            return false;
    if (f[3] != "0" && f[3] != "1")
        return false;
    if (f[6] != "0" && f[6] != "1")
        return false;
    out = ShowPreference{};
    out.showKey = f[0];
    out.audioLanguage = canonLanguage(f[1]);
    out.audioCodec = trackLower(f[2]);
    out.subtitlesOff = f[3] == "1";
    out.subtitleLanguage = out.subtitlesOff ? std::string() : canonLanguage(f[4]);
    out.subtitleCodec = out.subtitlesOff ? std::string() : trackLower(f[5]);
    out.subtitleForced = !out.subtitlesOff && f[6] == "1";
    return true;
}

inline std::string showPrefEncode(const ShowPreference& p) {
    if (!showPrefFieldOk(p.showKey) || p.showKey.empty() || !showPrefFieldOk(p.audioLanguage) ||
        !showPrefFieldOk(p.audioCodec) || !showPrefFieldOk(p.subtitleLanguage) || !showPrefFieldOk(p.subtitleCodec))
        return {};
    return showPrefEscape(p.showKey) + "|" + showPrefEscape(p.audioLanguage) + "|" + showPrefEscape(p.audioCodec) +
           "|" + (p.subtitlesOff ? "1" : "0") + "|" + showPrefEscape(p.subtitlesOff ? "" : p.subtitleLanguage) + "|" +
           showPrefEscape(p.subtitlesOff ? "" : p.subtitleCodec) + "|" + (p.subtitleForced ? "1" : "0");
}

struct ShowTrackLoad {
    bool loaded = false;            // a file was there
    bool recovered = false;         // corrupt magic, or at least one bad line skipped
    size_t kept = 0;
    size_t skipped = 0;
};

inline bool writeShowTrackFile(const std::string& path, const std::string& body) {
    if (path.empty())
        return false;
    static std::atomic<uint32_t> seq{0};
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(::getpid())) + "." +
                            std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    auto fail = [&] {
        ::close(fd);
        ::unlink(tmp.c_str());
        return false;
    };
    const char* p = body.data();
    size_t left = body.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return fail();
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0)
        return fail();
    if (::close(fd) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

class ShowTrackStore {
public:
    ShowTrackLoad load(const std::string& path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = path;
        rows_.clear();
        dirty_ = false;
        ShowTrackLoad st;
        struct stat info;
        if (::stat(path.c_str(), &info) == 0 && static_cast<uint64_t>(info.st_size) > kMaxShowTrackFileBytes) {
            st.loaded = true;
            st.recovered = true;
            quarantineLocked(path);
            return st;
        }
        std::ifstream in(path);
        if (!in)
            return st;
        st.loaded = true;
        std::string magic;
        if (!std::getline(in, magic)) {
            st.recovered = true;
            quarantineLocked(path);
            return st;
        }
        if (!magic.empty() && magic.back() == '\r')
            magic.pop_back();
        if (magic != kShowTrackMagic) {
            st.recovered = true;
            in.close();
            quarantineLocked(path);
            return st;
        }
        std::string line;
        size_t bytes = magic.size();
        while (std::getline(in, line)) {
            bytes += line.size() + 1;
            if (bytes > kMaxShowTrackFileBytes) {
                st.recovered = true;
                rows_.clear();
                in.close();
                quarantineLocked(path);
                return st;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            ShowPreference p;
            if (!showPrefDecode(line, p)) {
                ++st.skipped;
                st.recovered = true;
                continue;
            }
            putLocked(std::move(p));
            ++st.kept;
        }
        if (st.recovered)
            saveLocked();           // rewrite without the bad lines
        return st;
    }

    bool save(const std::string& path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = path;
        dirty_ = true;
        return saveLocked();
    }

    std::optional<ShowPreference> find(const std::string& showKey) const {
        std::lock_guard<std::mutex> g(mu_);
        for (const ShowPreference& p : rows_)
            if (p.showKey == showKey)
                return p;
        return std::nullopt;
    }

    // Memory only. The caller writes the file off the companion thread (flush or the I/O worker).
    bool stage(ShowPreference p) {
        if (showPrefEncode(p).empty())
            return false;
        std::lock_guard<std::mutex> g(mu_);
        putLocked(std::move(p));
        dirty_ = true;
        return true;
    }

    // Snapshot under the store lock, then fsync+rename outside it.
    bool flush() {
        std::string path;
        std::string body;
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!dirty_ || path_.empty())
                return true;
            dirty_ = false;
            path = path_;
            body = bodyLocked();
        }
        return writeShowTrackFile(path, body);
    }

    bool put(ShowPreference p) {
        if (!stage(std::move(p)))
            return false;
        return flush();
    }

    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return rows_.size();
    }

    void setPath(std::string path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = std::move(path);
    }

    std::string path() const {
        std::lock_guard<std::mutex> g(mu_);
        return path_;
    }

private:
    void putLocked(ShowPreference p) {
        if (p.showKey.empty())
            return;
        for (auto it = rows_.begin(); it != rows_.end(); ++it) {
            if (it->showKey == p.showKey) {
                rows_.erase(it);
                break;
            }
        }
        rows_.push_back(std::move(p));
        while (rows_.size() > kMaxShowPrefs)
            rows_.erase(rows_.begin());
    }

    std::string bodyLocked() const {
        std::string body = std::string(kShowTrackMagic) + "\n";
        for (const ShowPreference& p : rows_) {
            const std::string line = showPrefEncode(p);
            if (!line.empty())
                body += line + "\n";
        }
        return body;
    }

    bool saveLocked() {
        if (path_.empty())
            return false;
        dirty_ = false;
        return writeShowTrackFile(path_, bodyLocked());
    }

    void quarantineLocked(const std::string& path) {
        const std::string bad = path + ".bad";
        std::remove(bad.c_str());
        if (std::rename(path.c_str(), bad.c_str()) != 0) {
            std::ofstream out(path, std::ios::trunc);
            if (out)
                out << kShowTrackMagic << "\n";
        }
        rows_.clear();
    }

    mutable std::mutex mu_;
    std::string path_;
    std::vector<ShowPreference> rows_;
    bool dirty_ = false;
};

// One store for the process. Resolve reads it while a play is built; setStreams writes it.
inline ShowTrackStore& processShowTracks() {
    static ShowTrackStore store;
    return store;
}

inline ShowTrackLoad loadProcessShowTracks(const std::string& path) {
    return processShowTracks().load(path);
}

// ---- what a setStreams command may change --------------------------------
// Report only ids that are already open, or that this decision commits onto a
// PMS universal URL. Unknown ids are rejected (the current id stays). A repeat
// of the open ids is not a user change. Nothing is saved when no play is live.

struct OpenedStreams {
    bool known = false;
    bool playing = false;
    bool universal = false;          // the open URL is a PMS universal transcode
    int partId = 0;
    int audioStreamId = 0;
    int subtitleStreamId = 0;        // 0 = off
};

struct SetStreamsDecision {
    bool rejected = false;
    bool changed = false;            // a real change of an id that exists on this part
    bool save = false;               // eligible to remember; the player saves only after it opens
    bool restart = false;            // rebuild the universal URL and seek to now
    bool inPlaceAudio = false;       // direct-play audio switch inside libav
    int audioStreamId = 0;
    int subtitleStreamId = 0;
    int audioIndex = -1;
    bool burn = false;
    bool softText = false;           // srt/ass: offer to mpx::SubtitleSink
    std::string note;
};

inline SetStreamsDecision decideSetStreams(const StreamCatalog& cat, const OpenedStreams& opened,
                                          const SetStreamsRequest& req) {
    SetStreamsDecision d;
    d.audioStreamId = opened.audioStreamId;
    d.subtitleStreamId = opened.subtitleStreamId;
    if (!opened.playing) {
        d.note = "no active play";
        return d;
    }
    bool audioChange = false;
    bool subChange = false;
    if (req.hasAudio) {
        if (!(opened.known && req.audioStreamId == opened.audioStreamId)) {
            if (const MediaStream* a = findStreamById(cat, req.audioStreamId, StreamKind::Audio, opened.partId)) {
                if (a->id != opened.audioStreamId) {
                    audioChange = true;
                    d.audioStreamId = a->id;
                    d.audioIndex = a->index;
                }
            } else {
                d.rejected = true;
                d.note = "unknown audioStreamID";
            }
        }
    }
    if (req.hasSubtitle) {
        if (req.subtitleStreamId <= 0) {
            if (opened.subtitleStreamId != 0) {
                subChange = true;
                d.subtitleStreamId = 0;
            }
        } else if (!(opened.known && req.subtitleStreamId == opened.subtitleStreamId)) {
            if (const MediaStream* s = findStreamById(cat, req.subtitleStreamId, StreamKind::Subtitle, opened.partId)) {
                if (s->id != opened.subtitleStreamId) {
                    subChange = true;
                    d.subtitleStreamId = s->id;
                    d.softText = subtitleIsSoftNow(*s);
                }
            } else {
                d.rejected = true;
                d.note = d.note.empty() ? "unknown subtitleStreamID" : d.note + "; unknown subtitleStreamID";
            }
        }
    }
    if (!audioChange && !subChange)
        return d;
    d.changed = true;
    d.save = true;
    d.burn = d.subtitleStreamId > 0;
    // A transcode has one audio stream. Selecting by the source index is a no-op
    // unless the PMS URL is rebuilt with the new audioStreamID.
    if (subChange || (audioChange && opened.universal))
        d.restart = true;
    else if (audioChange)
        d.inPlaceAudio = true;
    return d;
}

// Soft-to-soft, soft-to-off, and off-to-soft leave subtitleStreamID=0 on the wire.
// A burn entering or leaving that URL still restarts, and so does an audio change.
// wasBurned is a subtitle id that was opened on the URL (the sink was not showing it).
inline bool subtitleChangeSkipsRestart(bool subChanged, bool audioChanged, bool burnOnWire, bool wasBurned) {
    return subChanged && !audioChanged && !burnOnWire && !wasBurned;
}

// ---- universal URL rewrite ------------------------------------------------
// PMS offset is whole seconds, half-up, matching transcode_offset::roundMsToSec.

inline bool trackUrlIsUniversal(const std::string& url) {
    return url.find("transcode/universal") != std::string::npos || url.find("/video/:/transcode/") != std::string::npos;
}

inline std::string trackWithQuery(std::string url, const std::string& key, const std::string& value) {
    const std::string assignment = key + "=" + value;
    const auto hash = url.find('#');
    const auto end = hash == std::string::npos ? url.size() : hash;
    const auto q = url.find('?');
    const std::string frag = hash == std::string::npos ? std::string() : url.substr(hash);
    if (q == std::string::npos || q > end)
        return url.substr(0, end) + "?" + assignment + frag;
    auto pos = q + 1;
    while ((pos = url.find(key + "=", pos)) != std::string::npos && pos < end) {
        const bool atKey = pos == q + 1 || url[pos - 1] == '&';
        if (atKey) {
            auto valEnd = url.find('&', pos);
            if (valEnd == std::string::npos || valEnd > end)
                valEnd = end;
            return url.substr(0, pos) + assignment + url.substr(valEnd);
        }
        pos += key.size() + 1;
    }
    return url.substr(0, end) + "&" + assignment + frag;
}

inline std::string trackStripQuery(std::string url, const std::string& key) {
    const auto hash = url.find('#');
    const auto end = hash == std::string::npos ? url.size() : hash;
    const auto q = url.find('?');
    if (q == std::string::npos || q > end)
        return url;
    const std::string frag = hash == std::string::npos ? std::string() : url.substr(hash);
    std::string head = url.substr(0, q);
    std::string query = url.substr(q + 1, end - (q + 1));
    std::string out;
    size_t i = 0;
    while (i < query.size()) {
        auto amp = query.find('&', i);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string part = query.substr(i, amp - i);
        const bool drop = part.rfind(key + "=", 0) == 0;
        if (!drop) {
            if (!out.empty())
                out.push_back('&');
            out += part;
        }
        i = amp + 1;
    }
    if (out.empty())
        return head + frag;
    return head + "?" + out + frag;
}

inline int64_t trackOffsetSeconds(int64_t ms) {
    if (ms <= 0)
        return 0;
    return ms / 1000 + (ms % 1000 >= 500 ? 1 : 0);
}

// Fresh PMS transcode session for a mid-play burn. PMS binds subtitle decisions to
// the session id; rewriting subtitles=burn onto a Direct Play session is ignored.
inline std::string mintSetStreamsSessionId() {
    static std::atomic<uint32_t> n{1};
    const uint32_t v = n.fetch_add(1, std::memory_order_relaxed) * 2654435761u;
    char buf[24];
    std::snprintf(buf, sizeof buf, "mplex-ss-%08x", v);
    return buf;
}

// audioStreamId <= 0 leaves any existing audioStreamID in place when keepAudio is false
// and the caller passed the id it wants written (> 0). subtitleStreamId 0 clears burn.
// #269: an explicit burn (mid-play PGS) must force a video transcode. PMS ignores
// subtitles=burn when directPlay=1 / directStream=1. Do not gate this on BURN_SUBS_FIX:
// that flag only opts in auto-burn at resolve; setStreams is already an explicit pick.
inline std::string universalUrlForStreams(const std::string& url, int64_t offsetMs, int audioStreamId,
                                         int subtitleStreamId, bool burn) {
    if (!trackUrlIsUniversal(url))
        return {};
    std::string u = url;
    if (audioStreamId > 0)
        u = trackWithQuery(u, "audioStreamID", std::to_string(audioStreamId));
    if (burn && subtitleStreamId > 0) {
        u = trackWithQuery(u, "subtitleStreamID", std::to_string(subtitleStreamId));
        u = trackWithQuery(u, "subtitles", "burn");
#ifdef MUT_BURN_SUBS_FIX_KEEP_DIRECT
        if (burnSubsFixForcesTranscode(true)) {
            u = trackWithQuery(u, "directPlay", "0");
            u = trackWithQuery(u, "directStream", "0");
        }
#else
        u = trackWithQuery(u, "directPlay", "0");
        u = trackWithQuery(u, "directStream", "0");
#endif
    } else {
        u = trackWithQuery(u, "subtitleStreamID", "0");
        u = trackStripQuery(u, "subtitles");
    }
    return trackWithQuery(u, "offset", std::to_string(trackOffsetSeconds(offsetMs)));
}

// Direct Play current URL + the resolve's unused universal template. Empty when
// neither is a universal start URL (nowhere to attach subtitles=burn).
inline std::string universalUrlForStreamChange(const std::string& current, const std::string& templ, int64_t offsetMs,
                                              int audioStreamId, int subtitleStreamId, bool burn) {
    const std::string base = trackUrlIsUniversal(current) ? current : templ;
    return universalUrlForStreams(base, offsetMs, audioStreamId, subtitleStreamId, burn);
}

// A Soft sink is already showing the text. The opened URL must not also burn it.
// Audio, offset, and the rest of the query stay. Non-universal URLs are unchanged.
inline std::string universalUrlWithoutBurn(const std::string& url) {
    if (!trackUrlIsUniversal(url))
        return url;
    return trackStripQuery(trackWithQuery(url, "subtitleStreamID", "0"), "subtitles");
}

// Ids actually named on a universal URL. Missing audioStreamID is not invented
// from a planned pick. A subtitle id counts only when subtitles=burn is set,
// or when subtitleStreamID=0 (off).
inline bool openedFromUniversalUrl(const std::string& url, int& audioId, int& subtitleId) {
    audioId = 0;
    subtitleId = 0;
    if (!trackUrlIsUniversal(url))
        return false;
    bool audio = false;
    bool sub = false;
    if (const auto a = trackQueryParam(url, "audioStreamID")) {
        int v = 0;
        if (trackParseInt(*a, v) && v > 0) {
            audioId = v;
            audio = true;
        }
    }
    const bool burn = url.find("subtitles=burn") != std::string::npos;
    if (const auto s = trackQueryParam(url, "subtitleStreamID")) {
        int v = 0;
        if (trackParseInt(*s, v) && v >= 0) {
            if (v == 0 || burn) {
                subtitleId = v;
                sub = true;
            }
        }
    }
    return audio || sub;
}

// What the timeline should echo for the stream that just opened.
// A universal URL names its own ids. With no audioStreamID, the catalog stream
// marked selected="1" is the one PMS transcodes. The container index is not a
// stream in that file and is ignored. Direct play maps the container index to
// the catalog id. A soft subtitle stays selected when the URL says subtitle 0
// (the sink is showing it, so the ladder must not also burn it).
struct OpenedIds {
    bool known = false;
    int audioId = 0;
    int subtitleId = 0;
};

inline OpenedIds openedIdsForPlay(const std::string& url, const StreamCatalog& cat, int partId,
                                   int containerAudioIndex, bool softSubtitle, int softSubtitleId) {
    OpenedIds out;
    int urlAudio = 0;
    int urlSub = 0;
    const bool fromUrl = openedFromUniversalUrl(url, urlAudio, urlSub);
    if (trackUrlIsUniversal(url)) {
        if (urlAudio > 0) {
            out.audioId = urlAudio;
        } else {
            for (const MediaStream& s : cat.streams) {
                if (s.kind == StreamKind::Audio && s.selected && (partId <= 0 || s.partId <= 0 || s.partId == partId)) {
                    out.audioId = s.id;
                    break;
                }
            }
        }
        if (softSubtitle && softSubtitleId > 0 && urlSub == 0)
            out.subtitleId = softSubtitleId;
        else if (fromUrl)
            out.subtitleId = urlSub;
        out.known = out.audioId > 0 || fromUrl || out.subtitleId > 0;
        return out;
    }
    for (const MediaStream& s : cat.streams) {
        if (s.kind == StreamKind::Audio && s.index == containerAudioIndex &&
            (partId <= 0 || s.partId <= 0 || s.partId == partId)) {
            out.audioId = s.id;
            break;
        }
    }
    if (softSubtitle && softSubtitleId > 0)
        out.subtitleId = softSubtitleId;
    out.known = out.audioId > 0 || out.subtitleId > 0;
    return out;
}

// Offer the sink, then the URL playback opens. Soft strips subtitles=burn.
// The player calls this before start. A null sink leaves the burned URL.
inline std::string urlAfterSubtitleOffer(mpx::SubtitleControl& ctl, const mpx::SubtitleTrackSpec& spec,
                                         const std::string& url, bool& sinkSoft) {
    const mpx::SubtitleRoute route = ctl.playStarted(spec);
    sinkSoft = route == mpx::SubtitleRoute::Soft;
    return sinkSoft ? universalUrlWithoutBurn(url) : url;
}

// #221: PMS reports subtitleStreamID=0 when no embedded track is active. A cached
// OpenSubtitles sidecar still paints unless the user explicitly chose Off (live
// setStreams/playMedia or a stored show preference).
inline bool keepCachedOpenSubsOnDemuxerOff(bool hasLocalSoftPath, bool explicitLiveSubtitleOff,
                                           bool preferenceExplicitOff) {
    if (!hasLocalSoftPath)
        return false;
#ifdef MUT_LIVE_SUB_OFF_IGNORED
    (void)explicitLiveSubtitleOff;
    return !preferenceExplicitOff;
#else
    return !(explicitLiveSubtitleOff || preferenceExplicitOff);
#endif
}

// What a subtitle change does to the sink and to the open transcode.
struct SubtitleChangePlan {
    bool skipRestart = false;
    bool burnOnWire = false;
    bool sinkSoft = false;
    mpx::SubtitleRoute route = mpx::SubtitleRoute::Off;
};

// Offers the new spec when the subtitle id changed, then decides whether the
// universal transcode restarts. Soft-to-soft and soft-to-off stay on this URL.
// Soft-to-image clears the painted text and restarts so PMS can burn it.
inline SubtitleChangePlan planSubtitleChange(mpx::SubtitleControl& ctl, uint64_t epoch,
                                             const mpx::SubtitleTrackSpec& next, bool subChanged, bool audioChanged,
                                             bool wasSoft, bool openedHadSubtitle, bool decisionBurn) {
    SubtitleChangePlan plan;
    plan.sinkSoft = wasSoft;
    if (subChanged) {
        if (next.streamId <= 0) {
            ctl.subtitlesOff(epoch);
            plan.route = mpx::SubtitleRoute::Off;
            plan.sinkSoft = false;
        } else {
            plan.route = ctl.trackChanged(next);
            plan.sinkSoft = plan.route == mpx::SubtitleRoute::Soft;
        }
    }
    const bool wasBurned = !wasSoft && openedHadSubtitle;
    plan.burnOnWire = decisionBurn && !plan.sinkSoft;
    plan.skipRestart = subtitleChangeSkipsRestart(subChanged, audioChanged, plan.burnOnWire, wasBurned);
    return plan;
}

} // namespace misterplex
