#pragma once
// Pluto split-HLS live remux (#1066 Vevo class): skip ad pods by segment path and play
// from a growing byte reserve.
//
// Pluto stitches ads into the same media playlists with no SCTE/CUE/DATERANGE tags and
// the same CDN host, so hlsLineIsAdSplice never fires. Content segments live under
// /live/v1/prd/{channel}/; ads are /prd/creative/, /v1/jit/, /mp4/v1/, /v1/mp4/ or an
// _ad path under /prd/. While any segment in either rendition window is an ad the relay
// holds (ad-break card, no ffmpeg), then respawns on a window that is all content.
// DISCONTINUITY, DISCONTINUITY-SEQUENCE and short EXTINF are never return signals.
//
// Kill switches: MPX_PLUTO_ADSKIP=0 restores v1.0.1 play-all; MPX_LIVE_RESERVE_S=0
// restores the -1 live-edge start with no relay reserve.
//
// MUT_ADSKIP_PLAYS_AD: never cut on an ad open (v1.0.1 play-all; FAIL).
// MUT_ADSKIP_CARD_DROPS_EARLY: card clears on the v1.0.1 hold timeout mid-pod (FAIL).
// MUT_ADSKIP_NEVER_RESUMES: content return is ignored (FAIL).
// MUT_LIVE_RESERVE_NO_GROW: -1 start and a 5 s cap (FAIL).
// MUT_LIVE_RESPAWN_DROPS_RESERVE: content respawn drops the reserve (FAIL).
// MUT_TUNE_BLOCKS_INPUT: relay start fetches synchronously and stop joins the poller (FAIL).

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace misterplex {
namespace pluto_adskip {

inline bool adSkipEnabled() {
    const char* e = std::getenv("MPX_PLUTO_ADSKIP");
    return !(e && std::strcmp(e, "0") == 0);
}

constexpr int kReserveDefaultS = 30;
constexpr int kReserveMaxS = 60;

inline int parseReserveTargetS(const char* e) {
    if (!e || !*e) return kReserveDefaultS;
    char* end = nullptr;
    errno = 0;
    const long v = std::strtol(e, &end, 10);
    if (end == e || *end != '\0' || errno == ERANGE) return kReserveDefaultS;
    return static_cast<int>(std::clamp<long>(v, 0, kReserveMaxS));
}

inline int liveReserveTargetS() { return parseReserveTargetS(std::getenv("MPX_LIVE_RESERVE_S")); }

constexpr size_t kReserveCapMaxBytes = size_t(32) << 20;
constexpr size_t kReserveCapMinBytes = size_t(1) << 20;
constexpr int64_t kReserveDefaultBps = 6000000;
constexpr int kPlutoSegmentS = 5;
// Pluto 1066 windows are 5 segments (sometimes 6-7); -5 is the oldest of the common case.
constexpr int kOldestTypicalIndex = -5;

inline size_t reserveCapBytes(int targetS, int64_t bps) {
    if (targetS <= 0) return 0;
    if (bps <= 0) bps = kReserveDefaultBps;
#ifdef MUT_LIVE_RESERVE_NO_GROW
    targetS = std::min(targetS, kPlutoSegmentS);
#endif
    const double want = double(bps) / 8.0 * double(targetS) * 1.25;
    return std::clamp<size_t>(size_t(want), kReserveCapMinBytes, kReserveCapMaxBytes);
}

inline int firstSpawnStartIndex(int targetS) {
#ifdef MUT_LIVE_RESERVE_NO_GROW
    (void)targetS;
    return -1;
#else
    if (targetS <= 0) return -1;
    const int segs = (targetS + kPlutoSegmentS - 1) / kPlutoSegmentS;
    return -std::min(segs, -kOldestTypicalIndex);
#endif
}

enum class SegKind { Content, Ad, Unknown };

inline const char* segKindName(SegKind k) {
    switch (k) {
        case SegKind::Content: return "content";
        case SegKind::Ad: return "ad";
        default: return "unknown";
    }
}

// Path of a segment URI with scheme, host, query and fragment removed.
// Accepts absolute URLs, "crypto+" URLs, host/path patterns and bare paths.
inline std::string segmentPathOf(const std::string& uri) {
    std::string s = uri;
    const size_t cut = s.find_first_of("?#");
    if (cut != std::string::npos) s.resize(cut);
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) {
        const size_t slash = s.find('/', scheme + 3);
        return slash == std::string::npos ? std::string("/") : s.substr(slash);
    }
    if (!s.empty() && s[0] == '/') return s;
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return s;
    const std::string first = s.substr(0, slash);
    if (first.find('.') != std::string::npos || first.find(':') != std::string::npos)
        return s.substr(slash);
    return "/" + s;
}

inline std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline SegKind classifySegmentPath(const std::string& uri) {
    const std::string p = lowerAscii(segmentPathOf(uri));
    if (p.find("/prd/creative/") != std::string::npos) return SegKind::Ad;
    if (p.find("/v1/jit/") != std::string::npos) return SegKind::Ad;
    if (p.find("/mp4/v1/") != std::string::npos) return SegKind::Ad;
    if (p.find("/v1/mp4/") != std::string::npos) return SegKind::Ad;
    if (p.find("creative%2f") != std::string::npos) return SegKind::Ad;
    if (p.compare(0, 13, "/live/v1/prd/") == 0) return SegKind::Content;
    const size_t prd = p.find("/prd/");
    if (prd != std::string::npos && p.find("_ad", prd) != std::string::npos) return SegKind::Ad;
    return SegKind::Unknown;
}

struct Seg {
    int64_t seq = -1;
    SegKind kind = SegKind::Unknown;
    double extinf = 0.0;
    bool discBefore = false;
};

struct Window {
    bool ok = false;
    int64_t mediaSeq = -1;
    int64_t discSeq = 0;
    double targetS = 0.0;
    bool endList = false;
    std::vector<Seg> segs;
};

inline std::string trimLine(std::string l) {
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
    size_t i = 0;
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) ++i;
    return l.substr(i);
}

inline bool tagValue(const std::string& line, const char* tag, std::string& out) {
    const size_t n = std::strlen(tag);
    if (line.compare(0, n, tag) != 0) return false;
    out = line.substr(n);
    return true;
}

inline Window parseMediaPlaylist(const std::string& body) {
    Window w;
    if (body.compare(0, 7, "#EXTM3U") != 0) return w;
    int64_t seq = 0;
    bool haveSeq = false;
    bool disc = false;
    double extinf = 0.0;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) nl = body.size();
        const std::string line = trimLine(body.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        std::string v;
        if (tagValue(line, "#EXT-X-MEDIA-SEQUENCE:", v)) {
            char* end = nullptr;
            const long long s = std::strtoll(v.c_str(), &end, 10);
            if (end == v.c_str()) return Window{};
            seq = s;
            haveSeq = true;
        } else if (tagValue(line, "#EXT-X-DISCONTINUITY-SEQUENCE:", v)) {
            w.discSeq = std::strtoll(v.c_str(), nullptr, 10);
        } else if (tagValue(line, "#EXT-X-TARGETDURATION:", v)) {
            w.targetS = std::strtod(v.c_str(), nullptr);
        } else if (line == "#EXT-X-DISCONTINUITY") {
            disc = true;
        } else if (tagValue(line, "#EXTINF:", v)) {
            extinf = std::strtod(v.c_str(), nullptr);
        } else if (line == "#EXT-X-ENDLIST") {
            w.endList = true;
        } else if (line[0] != '#') {
            Seg s;
            s.seq = seq++;
            s.kind = classifySegmentPath(line);
            s.extinf = extinf;
            s.discBefore = disc;
            w.segs.push_back(s);
            disc = false;
            extinf = 0.0;
        }
    }
    if (w.segs.empty()) return Window{};
    w.mediaSeq = haveSeq ? w.segs.front().seq : 0;
    w.ok = true;
    return w;
}

inline bool windowHasAd(const Window& w) {
    for (const Seg& s : w.segs)
        if (s.kind == SegKind::Ad) return true;
    return false;
}

inline bool windowAllContent(const Window& w) {
    if (!w.ok || w.segs.empty()) return false;
    for (const Seg& s : w.segs)
        if (s.kind != SegKind::Content) return false;
    return true;
}

inline int64_t firstAdSeq(const Window& w) {
    for (const Seg& s : w.segs)
        if (s.kind == SegKind::Ad) return s.seq;
    return -1;
}

// -live_start_index per input so both renditions start at the same media sequence
// (the oldest both windows still hold). Pluto numbers video and audio in lockstep.
struct StartIndex {
    bool ok = false;
    int video = -1;
    int audio = -1;
    int64_t seq = -1;
};

inline StartIndex alignedOldestStart(const Window& v, const Window& a) {
    StartIndex r;
    if (!v.ok || !a.ok || v.segs.empty() || a.segs.empty()) return r;
    const int64_t vEnd = v.segs.back().seq + 1;
    const int64_t aEnd = a.segs.back().seq + 1;
    const int64_t s = std::max(v.segs.front().seq, a.segs.front().seq);
    if (s >= vEnd || s >= aEnd) return r;
    r.ok = true;
    r.seq = s;
    r.video = int(s - vEnd);
    r.audio = int(s - aEnd);
    return r;
}

// ffmpeg logs "[hls @ 0x..] Opening '<url>' for reading" at info level for each
// segment and key it opens. Only .ts segment opens are classified.
inline bool parseOpenLine(const std::string& line, SegKind& kind) {
    const size_t o = line.find("Opening '");
    if (o == std::string::npos) return false;
    const size_t b = o + 9;
    const size_t e = line.find("' for reading", b);
    if (e == std::string::npos) return false;
    const std::string url = line.substr(b, e - b);
    const std::string path = segmentPathOf(url);
    if (path.size() < 3 || lowerAscii(path.substr(path.size() - 3)) != ".ts") return false;
    kind = classifySegmentPath(url);
    return true;
}

// Byte reserve between the remux child and the player socket. Positions are absolute
// stream offsets so a cut can land on an exact earlier byte.
class ByteReserve {
public:
    void append(const char* p, size_t n) {
        if (!n) return;
        if (!chunks_.empty() && chunks_.back().size() < kChunk) {
            std::string& b = chunks_.back();
            const size_t take = std::min(n, kChunk - b.size());
            b.append(p, take);
            p += take;
            n -= take;
            bytes_ += take;
            in_ += take;
        }
        while (n) {
            const size_t take = std::min(n, kChunk);
            chunks_.emplace_back(p, take);
            p += take;
            n -= take;
            bytes_ += take;
            in_ += take;
        }
    }
    size_t size() const { return bytes_; }
    uint64_t inPos() const { return in_; }
    uint64_t outPos() const { return in_ - bytes_; }
    const char* front(size_t& n) const {
        if (chunks_.empty()) {
            n = 0;
            return nullptr;
        }
        n = chunks_.front().size() - head_;
        return chunks_.front().data() + head_;
    }
    void consume(size_t n) {
        n = std::min(n, bytes_);
        bytes_ -= n;
        while (n) {
            const size_t avail = chunks_.front().size() - head_;
            if (n < avail) {
                head_ += n;
                return;
            }
            n -= avail;
            chunks_.pop_front();
            head_ = 0;
        }
        if (chunks_.empty()) head_ = 0;
    }
    // Drop everything at or after absolute position pos (bytes already sent stay sent).
    void truncateTo(uint64_t pos) {
        pos = std::max(pos, outPos());
        while (in_ > pos) {
            std::string& b = chunks_.back();
            const size_t base = chunks_.size() == 1 ? head_ : 0;
            const size_t drop = size_t(std::min<uint64_t>(in_ - pos, b.size() - base));
            b.resize(b.size() - drop);
            in_ -= drop;
            bytes_ -= drop;
            if (b.size() == base) {
                chunks_.pop_back();
                if (chunks_.empty()) head_ = 0;
            }
        }
    }
    void clear() { truncateTo(outPos()); }

private:
    static constexpr size_t kChunk = 64 * 1024;
    std::deque<std::string> chunks_;
    size_t head_ = 0;
    size_t bytes_ = 0;
    uint64_t in_ = 0;
};

// A content respawn keeps the reserve so the player drains it while the child restarts.
inline void onContentRespawn(ByteReserve& r) {
#ifdef MUT_LIVE_RESPAWN_DROPS_RESERVE
    r.clear();
#else
    (void)r;
#endif
}

enum class Phase { Content, Holding, Resuming };

inline const char* phaseName(Phase p) {
    switch (p) {
        case Phase::Content: return "content";
        case Phase::Holding: return "holding";
        default: return "resuming";
    }
}

// Content -> Holding on an ad open (or child loss while the window holds an ad);
// Holding -> Resuming on a poll where both windows are all content;
// Resuming -> Content once the new child's content is on screen.
class Machine {
public:
    struct PollResult {
        bool podEntered = false;  // first poll that shows this pod
        int64_t adSeq = -1;
    };

    // startedMs: when the poll request went out (same clock as hold()). A poll that started before the hold,
    // or a clean window no poll in this hold has seen the pod before, could put the resume back before the cut.
    PollResult onPoll(const Window& v, const Window& a, int64_t startedMs) {
        PollResult r;
        const bool ad = windowHasAd(v) || windowHasAd(a);
        const bool clean = windowAllContent(v) && windowAllContent(a);
        if (ad) {
            if (!podInWindow_) {
                r.podEntered = true;
                r.adSeq = firstAdSeq(windowHasAd(v) ? v : a);
            }
            podInWindow_ = true;
        } else if (clean) {
            podInWindow_ = false;
        }
        if (phase_ == Phase::Holding) {
            const bool afterHold = startedMs > holdStartMs_;
            if (ad && afterHold)
                adSeen_ = true;
            if (clean && afterHold && (adSeen_ || startedMs - holdStartMs_ >= kBlindHoldMs)) {
                start_ = alignedOldestStart(v, a);
                ready_ = start_.ok;
            } else {
                ready_ = false;
            }
        }
        return r;
    }

    bool podInWindow() const { return podInWindow_; }

    // ffmpeg opened a segment; true means cut the stream before this segment's bytes.
    bool cutOnOpen(SegKind k) const {
#ifdef MUT_ADSKIP_PLAYS_AD
        (void)k;
        return false;
#else
        if (phase_ == Phase::Holding) return false;
        if (k == SegKind::Ad) return true;
        return k == SegKind::Unknown && podInWindow_;
#endif
    }

    void hold(int64_t nowMs) {
        if (phase_ != Phase::Holding) {
            holdStartMs_ = nowMs;
            adSeen_ = podInWindow_;
        }
        phase_ = Phase::Holding;
        ready_ = false;
    }

    bool resumeReady() const {
#ifdef MUT_ADSKIP_NEVER_RESUMES
        return false;
#else
        return phase_ == Phase::Holding && ready_;
#endif
    }
    const StartIndex& resumeStart() const { return start_; }
    int64_t holdStartMs() const { return holdStartMs_; }

    void resumed() {
        phase_ = Phase::Resuming;
        ready_ = false;
    }
    void contentOnScreen() {
        if (phase_ == Phase::Resuming) phase_ = Phase::Content;
    }

    Phase phase() const { return phase_; }

    // Card policy while ad skip owns the card: up once buffered content has run out,
    // held for the whole pod, down only when the resumed content is on screen.
    static bool cardHeld(int64_t cardShownForMs, int64_t legacyHoldMs) {
#ifdef MUT_ADSKIP_CARD_DROPS_EARLY
        return cardShownForMs < legacyHoldMs;
#else
        (void)cardShownForMs;
        (void)legacyHoldMs;
        return true;
#endif
    }

private:
    Phase phase_ = Phase::Content;
    // Longer than a 7-segment Pluto window: an ad no poll saw has left the window by then.
    static constexpr int64_t kBlindHoldMs = 40000;
    bool podInWindow_ = false;
    bool adSeen_ = false;
    bool ready_ = false;
    StartIndex start_;
    int64_t holdStartMs_ = 0;
};

}  // namespace pluto_adskip
}  // namespace misterplex
