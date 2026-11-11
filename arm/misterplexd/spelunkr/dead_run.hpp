#pragma once

#include "spelunkr/archive_resolve.hpp"

#include <mutex>
#include <string>

namespace misterplex {
namespace spelunkr {

struct DeadRunOutcome {
    ArchiveResolve resolved;
    int skipped = 0;
    int unsupported = 0;
    int bannerDraws = 0;
    bool landed = false;
};

inline const char* deadRunWhy(ArchiveResolveKind k) {
    if (k == ArchiveResolveKind::DeadUnsupported)
        return "unsupported resolution";
    if (k == ArchiveResolveKind::DeadTailMoov)
        return "tail moov cold";
    return "dead item";
}

template <class Resolve, class Advance, class Draw>
DeadRunOutcome crossDeadRun(Resolve resolve, Advance advance, Draw draw) {
    DeadRunOutcome out;
    for (;;) {
        out.resolved = resolve();
        if (out.resolved.kind == ArchiveResolveKind::Cancelled)
            return out;
        if (out.resolved.kind == ArchiveResolveKind::Ok) {
            out.landed = true;
            if (out.skipped > 0 && out.unsupported > 0) {
                draw("unsupported resolution");
                ++out.bannerDraws;
            }
            return out;
        }
        if (!advance(deadRunWhy(out.resolved.kind)))
            return out;
        ++out.skipped;
        if (out.resolved.kind == ArchiveResolveKind::DeadUnsupported)
            ++out.unsupported;
    }
}

// GBE #625 P3-4: main.cpp's play-job wiring lives here so the unit test drives the real code.
// live false -> Cancelled. An Ok verdict without an allowed URL is a dead item (as before #625).
inline ArchiveResolve deadRunVerdict(ArchiveResolve r, bool live) {
    if (!live) {
        r.kind = ArchiveResolveKind::Cancelled;
        return r;
    }
    if (r.kind == ArchiveResolveKind::Ok && (r.url.empty() || !hostAllowed(r.url)))
        r.kind = ArchiveResolveKind::Failed;
    return r;
}

// GBE #625 P2-1: the reason IS the banner title (showSpelunkrBanner's 3rd arg). Do not fill the
// item title here; the landed item's title is filled at tune, after the reason has been on screen.
template <class Ui>
void drawDeadRunBanner(Ui& ui, int dial, const std::string& channelName, const char* why) {
    ui.showSpelunkrBanner("CH " + std::to_string(dial), channelName, why, true);
}

// One play-job resolve pass as main.cpp runs it. live(): not cancelled and still the current tune.
// resolveRaw(): resolveArchivePlay for the caller's current item. skip(why): the caller's takeSkip
// that advances its current item WITHOUT drawing. The banner is drawn under mu (the TV mutex,
// GBE #625 P3-1) and only while still live, so a zap cannot get a stale CH banner.
template <class Mutex, class Live, class ResolveRaw, class Skip, class Ui, class Np>
DeadRunOutcome playJobDeadRun(Mutex& mu, Live live, ResolveRaw resolveRaw, Skip skip, Ui& ui, const Np& np) {
    return crossDeadRun(
        [&] {
            ArchiveResolve r;
            if (live())
                r = resolveRaw();
            return deadRunVerdict(std::move(r), live());
        },
        [&](const char* why) { return skip(why); },
        [&](const char* why) {
            std::lock_guard<Mutex> lock(mu);
            if (live())
                drawDeadRunBanner(ui, np.dial, np.channelName, why);
        });
}

// One stderr line for a refused run (the end screen comes from deadRunEndMessage).
inline const char* deadRunEndLog(ArchiveResolveKind k) {
    switch (k) {
    case ArchiveResolveKind::NotMp4:
        return "not mp4";
    case ArchiveResolveKind::DeadTailMoov:
        return "tail moov cold";
    case ArchiveResolveKind::DeadUnsupported:
        return "unsupported resolution (metadata)";
    default:
        return "archive failed";
    }
}

// reviewer 11-10: the end screen of a refused run. A real connect/metadata failure keeps ARCHIVE FAILED;
// a run that ended because its items are too big for the core (DeadUnsupported) says so instead.
inline std::string deadRunEndMessage(const DeadRunOutcome& run, const std::string& title) {
    switch (run.resolved.kind) {
    case ArchiveResolveKind::NotMp4:
        return "NOT MP4 " + title;
    case ArchiveResolveKind::DeadTailMoov:
        return "TAIL MOOV COLD " + title;
    case ArchiveResolveKind::DeadUnsupported:
        return "unsupported resolution: NO NEXT";
    default:
        break;
    }
    return "ARCHIVE FAILED" + (run.resolved.err.empty() ? std::string{} : " " + run.resolved.err);
}

}  // namespace spelunkr
}  // namespace misterplex
