// Auto picture mode (issue #74): decide which raster a SOURCE belongs to
// (480p, 480i, or exact-NTSC 240p) and which cadence it shows on a fixed
// 59.94 Hz grid. Pure and header-only.
//
// This pin only advises. It never loads an RBF, never calls load_core, and
// never writes video_mode or any other HDMI mode. The paired core still runs
// the picture. A mismatch is a log field, not an action.
//
// The phone status page still reads LastAdvice (summaryLine). That row is a
// compatibility shim beside pictureAdviceLog. It does not load a core either.
//
// Spec: 240P-OWENB-DECISIONS-1024 and 240P-OWENB-RULING-1024-0140, and the
// decision table on takeover-1023/auto-picture-mode. Frame rate is never
// changed. 29.97 shows 2:2, 23.976 shows 3:2, 59.94 shows 1:1. A nominal
// 60.000 / 30.000 / 24.000 source repeats about once per 16.7 s on the
// 59.94 grid; that is reported, not corrected.
#pragma once

#include <cmath>
#include <mutex>
#include <string>

namespace misterplex {
namespace autopic {

enum class Raster { Unknown, P480, I480, P240 };
enum class Cadence { Unknown, OneToOne, TwoTwo, ThreeTwo, NominalRepeat };

struct Source {
    int width = 0;
    int height = 0;
    bool interlaced = false;
    int fpsNum = 0;  // exact frame rate (parseExactFps). 0/0 = unknown.
    int fpsDen = 0;
};

struct Decision {
    Raster raster = Raster::Unknown;
    Cadence cadence = Cadence::Unknown;
    bool exactNtsc = false;  // fps is on the NTSC 1000/1001 family
    const char* grid = "none";  // ntsc-59.94, pal, or none
    const char* reason = "no-info";
};

inline const char* rasterName(Raster r) {
    switch (r) {
    case Raster::P480: return "480p";
    case Raster::I480: return "480i";
    case Raster::P240: return "240p";
    default: return "unknown";
    }
}

inline const char* cadenceName(Cadence c) {
    switch (c) {
    case Cadence::OneToOne: return "1:1";
    case Cadence::TwoTwo: return "2:2";
    case Cadence::ThreeTwo: return "3:2";
    case Cadence::NominalRepeat: return "nominal-repeat";
    default: return "unknown";
    }
}

namespace detail {
inline bool near(double v, double want) { return std::fabs(v - want) <= want * 0.0005; }
}  // namespace detail

inline double frameFps(const Source& s) {
    if (s.fpsNum <= 0 || s.fpsDen <= 0)
        return 0.0;
    double f = static_cast<double>(s.fpsNum) / static_cast<double>(s.fpsDen);
    // Some PMS rows give the interlaced FIELD rate (59.94 for 480i). A 480i frame is never above 30.
    if (s.interlaced && f > 40.0)
        f /= 2.0;
    return f;
}

// Cadence on the 59.94006 Hz grid. exact = NTSC 1000/1001 family, nominal = the integer rate.
inline Cadence cadenceFor(double fps, bool* exact) {
    using detail::near;
    if (exact)
        *exact = false;
    if (fps <= 0.0)
        return Cadence::Unknown;
    if (near(fps, 30000.0 / 1001.0)) {
        if (exact)
            *exact = true;
        return Cadence::TwoTwo;
    }
    if (near(fps, 24000.0 / 1001.0)) {
        if (exact)
            *exact = true;
        return Cadence::ThreeTwo;
    }
    if (near(fps, 60000.0 / 1001.0)) {
        if (exact)
            *exact = true;
        return Cadence::OneToOne;
    }
    if (near(fps, 30.0) || near(fps, 24.0) || near(fps, 60.0))
        return Cadence::NominalRepeat;
    return Cadence::Unknown;
}

inline Decision classify(const Source& s) {
#ifdef MUT_AP_ALWAYS_SAME
    // Test-only mutant: every source gets the same advice.
    (void)s;
    Decision d;
    d.raster = Raster::P480;
    d.cadence = Cadence::TwoTwo;
    d.exactNtsc = true;
    d.grid = "ntsc-59.94";
    d.reason = "always";
    return d;
#else
    Decision d;
    if (s.width <= 0 || s.height <= 0)
        return d;
    const double fps = frameFps(s);
    d.cadence = cadenceFor(fps, &d.exactNtsc);
    const bool pal = fps > 0.0 && (detail::near(fps, 25.0) || detail::near(fps, 50.0));
    if (pal)
        d.grid = "pal";
    else if (d.cadence != Cadence::Unknown)
        d.grid = "ntsc-59.94";
    if (s.interlaced && s.height <= 486 && !pal) {
        d.raster = Raster::I480;
        d.reason = "interlaced-480";
        return d;
    }
    // 240p: progressive, 250 lines or fewer, width up to 720, on the NTSC grid
    // (exact or nominal). PAL 288p, odd rates, and anything else are never forced
    // onto the 240p raster (no remap). 1280x720 and 1920x1080 stay on the 480p
    // ladder: this pin does not name a 720p mode and does not write video_mode.
#ifdef MUT_AP_240_ANY_FPS
    const bool grid = true;
#else
    const bool grid = d.cadence != Cadence::Unknown;
#endif
#ifdef MUT_AP_240_HEIGHT
    const bool low = s.height <= 288;
#else
    const bool low = s.height <= 250;
#endif
    if (!s.interlaced && low && s.width <= 720 && grid) {
        d.raster = Raster::P240;
        d.reason = "progressive-240-ntsc-grid";
        return d;
    }
    d.raster = Raster::P480;
    d.reason = (!s.interlaced && low && s.width <= 720) ? "low-res-off-ntsc-grid" : "progressive-or-other";
    return d;
#endif
}

struct Advice {
    Raster use = Raster::Unknown;            // what the daemon runs: the paired core's raster when known
    bool mismatch = false;                   // the source belongs to another raster than the paired core
    Raster recommendCore = Raster::Unknown;  // set only when mismatch; advice, never an action
};

inline Raster rasterFromName(const std::string& n) {
    if (n == "480p") return Raster::P480;
    if (n == "480i") return Raster::I480;
    if (n == "240p") return Raster::P240;
    return Raster::Unknown;
}

inline Advice advise(const Decision& d, Raster paired) {
    Advice a;
    if (paired == Raster::Unknown) {
        a.use = d.raster;
        return a;
    }
#ifdef MUT_AP_PAIRED_LOSES
    a.use = d.raster == Raster::Unknown ? paired : d.raster;
#else
    a.use = paired;
#endif
    if (d.raster != Raster::Unknown && d.raster != paired) {
        a.mismatch = true;
        a.recommendCore = d.raster;
    }
    return a;
}

// One short line for the phone status page: "<WxH>[i|p] -> <raster> <cadence>, core <raster>[, MISMATCH:
// use <raster> core]". Only fixed vocabulary and numbers; no title, path or URL goes in.
// The daemon log uses formatAdviseLog / pictureAdviceLog, not this line.
inline std::string summaryLine(const Source& s, const Decision& d, const Advice& a) {
    std::string t = std::to_string(s.width) + "x" + std::to_string(s.height) + (s.interlaced ? "i" : "p") + " -> " +
                    rasterName(d.raster) + " " + cadenceName(d.cadence) + ", core " + rasterName(a.use);
#ifndef MUT_AP_SUMMARY_NO_MISMATCH
    if (a.mismatch) t += std::string(", MISMATCH: use ") + rasterName(a.recommendCore) + " core";
#endif
    return t;
}

// The last decision, shared between the play path (writer) and the status page (reader).
class LastAdvice {
  public:
    void set(const std::string& line) {
        std::lock_guard<std::mutex> l(mu_);
        line_ = line;
    }
    void clear() { set(std::string()); }
    // enabled=false -> "off"; enabled but nothing decided yet -> "waiting for a play".
    std::string text(bool enabled) const {
#ifdef MUT_AP_SUMMARY_IGNORES_OFF
        (void)enabled;
#else
        if (!enabled) return "off";
#endif
        std::lock_guard<std::mutex> l(mu_);
        return line_.empty() ? std::string("waiting for a play") : line_;
    }

  private:
    mutable std::mutex mu_;
    std::string line_;
};

// AUTO_PICTURE_MODE: only the exact word "advise" turns the log on.
// Empty, "off", "on", "1", and any other spelling stay off. Advise does not
// switch a core. Default when neither env nor conf is set is off, so an
// unset daemon emits nothing and playback is unchanged.
inline bool adviseEnabled(const std::string& conf) {
#ifdef MUT_AP_CONF_LOOSE
    return !conf.empty() && conf != "off";
#else
    return conf == "advise";
#endif
}

// Env wins when it is non-empty. Otherwise the conf value. Otherwise "off".
inline std::string resolveAutoPictureMode(const char* env, const std::string& conf) {
    if (env && env[0])
        return std::string(env);
    if (!conf.empty())
        return conf;
#ifdef MUT_AP_DEFAULT_ADVISE
    return "advise";
#else
    return "off";
#endif
}

// One log line. Fields are raster, cadence, grid, and the paired core.
// No URL, token, or address is accepted here, so none can be printed.
inline std::string formatAdviseLog(const Source& s, const Decision& d, const Advice& a) {
    std::string line = "AUTO_PICTURE_MODE=advise";
    line += " source=" + std::to_string(s.width) + "x" + std::to_string(s.height);
    line += s.interlaced ? "i" : "p";
    line += " fps=" + std::to_string(s.fpsNum) + "/" + std::to_string(s.fpsDen);
    line += " raster=";
    line += rasterName(d.raster);
    line += " cadence=";
    line += cadenceName(d.cadence);
    line += " grid=";
    line += d.grid ? d.grid : "none";
    line += " exactNtsc=";
    line += d.exactNtsc ? "1" : "0";
    line += " reason=";
    line += d.reason ? d.reason : "";
    line += " core=";
    line += rasterName(a.use);
    if (a.mismatch)
        line += " MISMATCH advice-only";
    return line;
}

struct PictureLog {
    bool emit = false;
    std::string line;
};

// The play path prints misterplexd: plus this line, and only when emit is set.
// Off, and every non-advise value, returns emit false.
inline PictureLog pictureAdviceLog(const Source& s, const std::string& pairedName, const std::string& mode) {
#ifdef MUT_AP_SKIP_LOG
    // Test-only mutant: advise is enabled and the decision is dropped.
    (void)s;
    (void)pairedName;
    (void)mode;
    return {};
#else
    PictureLog out;
    if (!adviseEnabled(mode))
        return out;
    const Decision d = classify(s);
    const Advice a = advise(d, rasterFromName(pairedName));
    out.line = formatAdviseLog(s, d, a);
    out.emit = !out.line.empty();
    return out;
#endif
}

}  // namespace autopic
}  // namespace misterplex
