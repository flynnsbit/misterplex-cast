#pragma once
// REMEMBER_PICTURE (misterplex.conf) / MPX_REMEMBER_PICTURE (env wins). Default off.
//
// Off or unset: play matches this tip. remember_picture.txt is not read and not
// written. The Program menu has no picture-mode row. PICTURE_14_9, when that
// flag is on, still toggles this play only.
//
// On (1/on/yes/true): the Program menu row cycles the picture modes owenb can
// apply without a new RBF. The choice is stored on this MiSTer in
// remember_picture.txt, keyed the same way as show memory (episode
// grandparentRatingKey, otherwise ratingKey). The next episode of that show
// starts in that mode. A show with no row is a no-op: the tip's aspect guard
// and the PICTURE_14_9 flag are unchanged.
//
// Modes, as the core exposes them (h264ring.h Analog Aspect / Aspect Ratio)
// plus the daemon's 14:9 side crop:
//   letterbox  Analog Aspect Letterbox (2)
//   14:9       Letterbox plus the SPS 14:9 window
//   zoom43     Analog Aspect Crop (3) and Aspect Ratio 4:3
//   stretch    Analog Aspect Fit (1) and Aspect Ratio 16:9 (anamorphic stretch)
//   fitcrop    Analog Aspect Crop (3), Aspect Ratio left as it is
// The user's core menu is restored at the end of the play, the same way the
// aspect guard already puts Analog Aspect back. This file is the per-show copy.

#include "track_select.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace misterplex {

inline bool rememberPictureEnvOn(const char* e) {
    if (!e || !e[0])
        return false;
    char buf[16];
    std::size_t n = 0;
    for (; e[n] && n + 1 < sizeof buf; ++n)
        buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(e[n])));
    buf[n] = 0;
    return std::strcmp(buf, "1") == 0 || std::strcmp(buf, "true") == 0 || std::strcmp(buf, "yes") == 0 ||
           std::strcmp(buf, "on") == 0;
}

// Default off. MPX_REMEMBER_PICTURE wins over REMEMBER_PICTURE when it is set. Unset is off.
// Precedence: with REMEMBER_PICTURE on, a remembered per-show mode wins over
// the PICTURE_14_9 default for that show (pictureToApply). Shows with no row
// keep the PICTURE_14_9 / default picture.
inline bool rememberPictureEnabled() {
#ifdef MUT_REMEMBER_PICTURE_FLAG_DEFAULT_ON
    return true;
#else
    if (const char* e = std::getenv("MPX_REMEMBER_PICTURE"); e && e[0])
        return rememberPictureEnvOn(e);
    return rememberPictureEnvOn(std::getenv("REMEMBER_PICTURE"));
#endif
}

enum class PictureMode : int {
    Letterbox = 0,
    Fourteen9 = 1,
    Zoom43 = 2,
    Stretch = 3,
    FitCrop = 4,
};

inline constexpr int kPictureModeCount = 5;

inline bool pictureModeOk(PictureMode mode) {
    const int n = static_cast<int>(mode);
    return n >= 0 && n < kPictureModeCount;
}

inline const char* pictureModeToken(PictureMode mode) {
    switch (mode) {
    case PictureMode::Letterbox: return "letterbox";
    case PictureMode::Fourteen9: return "14:9";
    case PictureMode::Zoom43: return "zoom43";
    case PictureMode::Stretch: return "stretch";
    case PictureMode::FitCrop: return "fitcrop";
    }
    return "letterbox";
}

inline const char* pictureModeLabel(PictureMode mode) {
    switch (mode) {
    case PictureMode::Letterbox: return "Letterbox";
    case PictureMode::Fourteen9: return "14:9";
    case PictureMode::Zoom43: return "Zoom 4:3";
    case PictureMode::Stretch: return "Stretch";
    case PictureMode::FitCrop: return "Fit-crop";
    }
    return "Letterbox";
}

inline std::optional<PictureMode> pictureModeFromToken(const std::string& token) {
    if (token == "letterbox")
        return PictureMode::Letterbox;
    if (token == "14:9")
        return PictureMode::Fourteen9;
    if (token == "zoom43")
        return PictureMode::Zoom43;
    if (token == "stretch")
        return PictureMode::Stretch;
    if (token == "fitcrop")
        return PictureMode::FitCrop;
    return std::nullopt;
}

// What the daemon asks the core for. analogAspect is H264RING_OPT_ANALOG_ASPECT
// (0 Auto, 1 Fit, 2 Letterbox, 3 Crop). aspectRatio is H264RING_OPT_ASPECT
// (0 Auto, 1 4:3, 2 16:9) when setAspectRatio is set.
struct PictureSpec {
    PictureMode mode = PictureMode::Letterbox;
    unsigned analogAspect = 2;
    bool setAspectRatio = false;
    unsigned aspectRatio = 0;
    bool crop149 = false;
};

inline PictureSpec pictureSpec(PictureMode mode) {
    PictureSpec s;
    s.mode = mode;
    switch (mode) {
    case PictureMode::Letterbox:
        s.analogAspect = 2;
        break;
    case PictureMode::Fourteen9:
#ifdef MUT_REMEMBER_PICTURE_DROP_149
        s.mode = PictureMode::Letterbox;
        s.analogAspect = 2;
        s.crop149 = false;
#else
        s.analogAspect = 2;
        s.crop149 = true;
#endif
        break;
    case PictureMode::Zoom43:
        s.analogAspect = 3;
        s.setAspectRatio = true;
        s.aspectRatio = 1;
        break;
    case PictureMode::Stretch:
        s.analogAspect = 1;
        s.setAspectRatio = true;
        s.aspectRatio = 2;
        break;
    case PictureMode::FitCrop:
        s.analogAspect = 3;
        break;
    }
    return s;
}

// -1 (or anything outside the enum) displays as Letterbox, which is the tip's
// progressive default, and the first Select advances to 14:9.
inline PictureMode pictureModeShown(int stored) {
    if (stored < 0 || stored >= kPictureModeCount)
        return PictureMode::Letterbox;
    return static_cast<PictureMode>(stored);
}

inline PictureMode pictureModeCycle(PictureMode mode) {
    int n = static_cast<int>(pictureModeShown(static_cast<int>(mode))) + 1;
    if (n >= kPictureModeCount)
        n = 0;
    return static_cast<PictureMode>(n);
}

// The mode the open play is using. -1 until a row is restored or chosen.
inline std::atomic<int>& pictureSessionMode() {
    static std::atomic<int> mode{-1};
    return mode;
}

struct RememberedPicture {
    std::string showKey;
    PictureMode mode = PictureMode::Letterbox;
};

inline std::optional<PictureSpec> pictureToApply(const std::optional<RememberedPicture>& row) {
#ifndef MUT_REMEMBER_PICTURE_OFF_STILL_APPLIES
    if (!rememberPictureEnabled())
        return std::nullopt;
#endif
#ifdef MUT_REMEMBER_PICTURE_IGNORED
    (void)row;
    return std::nullopt;
#endif
    if (!row || !pictureModeOk(row->mode))
        return std::nullopt;
    return pictureSpec(row->mode);
}

inline std::optional<PictureSpec> pictureSpecForArm(int mode) {
    if (mode < 0 || mode >= kPictureModeCount)
        return std::nullopt;
    RememberedPicture row;
    row.showKey = "play";
    row.mode = static_cast<PictureMode>(mode);
    return pictureToApply(row);
}

inline constexpr const char* kPictureMagic = "MPXPICTURE 1";

inline bool pictureSplit(const std::string& line, std::vector<std::string>& f) {
    f.clear();
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
            f.push_back(std::move(cur));
            cur.clear();
            continue;
        }
        if (c == '\r')
            continue;
        cur.push_back(c);
    }
    if (esc)
        return false;
    f.push_back(std::move(cur));
    return true;
}

inline std::string pictureEncode(const RememberedPicture& row) {
    if (row.showKey.empty() || !showPrefFieldOk(row.showKey) || !pictureModeOk(row.mode))
        return {};
    return showPrefEscape(row.showKey) + "|" + pictureModeToken(row.mode);
}

inline bool pictureDecode(const std::string& line, RememberedPicture& out) {
    std::vector<std::string> f;
    if (!pictureSplit(line, f) || f.size() != 2)
        return false;
    if (f[0].empty() || !showPrefFieldOk(f[0]) || !showPrefFieldOk(f[1]))
        return false;
    const std::optional<PictureMode> mode = pictureModeFromToken(f[1]);
    if (!mode)
        return false;
    RememberedPicture row;
    row.showKey = f[0];
    row.mode = *mode;
    if (pictureEncode(row).empty())
        return false;
    out = std::move(row);
    return true;
}

struct RememberPictureLoad {
    bool loaded = false;
    bool recovered = false;
    size_t kept = 0;
    size_t skipped = 0;
};

class RememberPictureStore {
public:
    RememberPictureLoad load(const std::string& path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = path;
        rows_.clear();
        dirty_ = false;
        RememberPictureLoad st;
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
        if (magic != kPictureMagic) {
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
            RememberedPicture row;
            if (!pictureDecode(line, row)) {
                ++st.skipped;
                st.recovered = true;
                continue;
            }
            putLocked(std::move(row));
            ++st.kept;
        }
        if (st.recovered)
            saveLocked();
        return st;
    }

    std::optional<RememberedPicture> find(const std::string& showKey) const {
        std::lock_guard<std::mutex> g(mu_);
        for (const RememberedPicture& row : rows_)
            if (row.showKey == showKey)
                return row;
        return std::nullopt;
    }

    // Flag off: no write. A bad key or mode is a no-op.
    bool note(const std::string& showKey, PictureMode mode) {
#ifndef MUT_REMEMBER_PICTURE_OFF_STILL_SAVES
        if (!rememberPictureEnabled())
            return false;
#endif
        if (showKey.empty() || !pictureModeOk(mode))
            return false;
        RememberedPicture row;
        row.showKey = showKey;
        row.mode = mode;
        if (pictureEncode(row).empty())
            return false;
        std::lock_guard<std::mutex> g(mu_);
        for (const RememberedPicture& have : rows_)
            if (have.showKey == row.showKey && have.mode == row.mode)
                return true; // unchanged: nothing to write
        putLocked(std::move(row));
        dirty_ = true;
        return true;
    }

    bool dirty() const {
        std::lock_guard<std::mutex> g(mu_);
        return dirty_;
    }

    bool flush() {
        // One writer at a time, and snapshot order == write order, so two
        // callers (menu debounce vs stop/exit) never write twice for one
        // change or let an older body land after a newer one. R3 nit 1.
#ifndef MUT_REMEMBER_PICTURE_NO_WRITE_LOCK
        std::lock_guard<std::mutex> w(writeMu_);
#endif
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
        if (testBeforeWrite)
            testBeforeWrite();   // test-only: hold a snapshot before its write
        if (writeShowTrackFile(path, body)) {
            std::lock_guard<std::mutex> g(mu_);
            ++writes_;
            return true;
        }
        std::lock_guard<std::mutex> g(mu_);   // failed write: keep it for the next flush
        dirty_ = true;
        return false;
    }

    // Debounced flush for menu cycling: write at most once per minGap. A
    // skipped write stays dirty and goes out on the next due call or flush()
    // (next play open). Flag off never dirties, so this never writes.
    // Known limit (by design): SIGKILL or a power cut inside the gap loses
    // the pending change; stop and normal exit flush it (flushPending). An
    // exit flush that fails is only logged (no retry at exit).
    bool flushIfDue(std::chrono::steady_clock::time_point now, std::chrono::milliseconds minGap) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!dirty_)
                return true;
#ifndef MUT_REMEMBER_PICTURE_NO_DEBOUNCE
            if (haveFlushed_ && now - lastFlush_ < minGap)
                return true;
#endif
        }
        const bool ok = flush();
        if (ok) {   // a failed write stays dirty and may retry on the next call
            std::lock_guard<std::mutex> g(mu_);
            haveFlushed_ = true;
            lastFlush_ = now;
        }
        return ok;
    }

    // Stop / daemon exit: a debounced write must not wait for the next play
    // (a power cut inside the gap would lose it). Writes now if dirty.
    bool flushPending() {
#ifdef MUT_REMEMBER_PICTURE_NO_STOP_FLUSH
        return true;
#else
        return flush();
#endif
    }

    void setPath(std::string path) {
        std::lock_guard<std::mutex> g(mu_);
        path_ = std::move(path);
    }

    // Test hook: runs after flush() snapshots and before it writes. Unset in
    // production. Lets a test hold an older snapshot while a newer flush runs.
    std::function<void()> testBeforeWrite;

    size_t writeCount() const {
        std::lock_guard<std::mutex> g(mu_);
        return writes_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> g(mu_);
        return rows_.size();
    }

private:
    std::mutex writeMu_;   // serializes flush() snapshots+writes
    size_t writes_ = 0;
    void putLocked(RememberedPicture row) {
        if (row.showKey.empty())
            return;
        for (auto it = rows_.begin(); it != rows_.end(); ++it) {
            if (it->showKey == row.showKey) {
                rows_.erase(it);
                break;
            }
        }
        rows_.push_back(std::move(row));
        while (rows_.size() > kMaxShowPrefs)
            rows_.erase(rows_.begin());
    }

    std::string bodyLocked() const {
        std::string body = std::string(kPictureMagic) + "\n";
        for (const RememberedPicture& row : rows_) {
            const std::string line = pictureEncode(row);
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
                out << kPictureMagic << "\n";
        }
        rows_.clear();
        dirty_ = false;
    }

    mutable std::mutex mu_;
    std::string path_;
    std::vector<RememberedPicture> rows_;
    bool dirty_ = false;
    bool haveFlushed_ = false;
    std::chrono::steady_clock::time_point lastFlush_{};
};

inline RememberPictureStore& processRememberPicture() {
    static RememberPictureStore store;
    return store;
}

inline RememberPictureLoad loadProcessRememberPicture(const std::string& path) {
    return processRememberPicture().load(path);
}

} // namespace misterplex
