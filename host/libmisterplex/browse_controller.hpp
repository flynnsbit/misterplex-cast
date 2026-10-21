#pragma once
// MPX_BROWSE_V2: the browser's state and its single "mpx-browse" worker (ARM Plex browse P1).
//
// The input thread only posts ops (post()): it never fetches, paints or waits. The worker runs
// the ops in order. Every post moves the generation, and a fetch in flight polls "is my
// generation still current?", so a newer input cancels an older fetch (browse_fetch.hpp
// kills curl). LOADING... is painted before a fetch starts.
//
// Root (Open): the mpx-menu helper first (connect 1.5 s, whole answer 3 s); if it does not
// answer, the status line says HELPER OFFLINE and the list is today's direct PMS
// /library/sections fetch (conf token), so browsing is never worse than before the helper.
// Helper Directory rows open their helper key; "mpxpage" rows (NEXT >, < PREV) replace the
// page; PMS rows open libraryListPath as before. Play rows go to hooks.play with a ticket
// (browse_v2.hpp): "local" helper rows and PMS rows by ratingKey on the conf PMS, "ladder"
// rows after a GET of their key (the helper's play ticket).
//
// A play hides the browser and keeps its stack. Stop on the remote during a play this browser
// started (StopAndReopen) stops it and shows that level again, refreshed (the new resume
// point); Open with a kept stack does the same. Back pops a level (each level keeps its
// cursor) and hides at the root.
//
// Locking: mu_ guards the browser state and is never held across a hook or a fetch; qMu_
// guards the queue. Hooks run on the worker, except log.

#include "browse_fetch.hpp"
#include "browse_v2.hpp"
#include "input_mailbox.hpp"
#include "library_browser.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace misterplex {

enum class BrowseOp : uint8_t {
    Open,          // show the browser: the kept stack (refreshed) or the root
    Select,        // the current row: enter, page, or play
    Back,          // pop a level; hide at the root
    Up,
    Down,
    PageUp,        // < PREV row if the page has one, else a screen up
    PageDown,      // NEXT > row if the page has one, else a screen down
    Info,          // details of the current row in the status line
    StopAndReopen, // stop a play this browser started, then show its level again
    Left,          // Plex UI: horizontal focus (chips / grid columns)
    Right,
};

inline const char* browseOpName(BrowseOp op) {
    switch (op) {
    case BrowseOp::Open: return "open";
    case BrowseOp::Select: return "select";
    case BrowseOp::Back: return "back";
    case BrowseOp::Up: return "up";
    case BrowseOp::Down: return "down";
    case BrowseOp::PageUp: return "pageup";
    case BrowseOp::PageDown: return "pagedown";
    case BrowseOp::Info: return "info";
    case BrowseOp::StopAndReopen: return "stop+reopen";
    case BrowseOp::Left: return "left";
    case BrowseOp::Right: return "right";
    }
    return "?";
}

// A command while the browser is visible. Nothing here reaches playback.
inline bool browseOpForVisibleCommand(PlaybackCommand c, BrowseOp& op) {
    switch (c) {
    case PlaybackCommand::PlayPause: op = BrowseOp::Select; return true;
    case PlaybackCommand::Stop:
    case PlaybackCommand::Back: op = BrowseOp::Back; return true;
    case PlaybackCommand::SkipForward:
    case PlaybackCommand::Down: op = BrowseOp::Down; return true;
    case PlaybackCommand::SkipBack:
    case PlaybackCommand::Up: op = BrowseOp::Up; return true;
    case PlaybackCommand::PageUp: op = BrowseOp::PageUp; return true;
    case PlaybackCommand::PageDown: op = BrowseOp::PageDown; return true;
    case PlaybackCommand::Info: op = BrowseOp::Info; return true;
    case PlaybackCommand::Browse: op = BrowseOp::Open; return true;
    case PlaybackCommand::None: return false;
    }
    return false;
}

class BrowseController {
public:
    using HttpGetFn = std::function<BrowseHttpResult(const std::string& url, int connectMs,
                                                     int timeoutMs, const BrowseCancelFn&)>;
    struct Hooks {
        HttpGetFn get;                       // default: browseHttpGet (curl)
        std::function<void()> repaint;       // paint the current state now (synchronous)
        std::function<bool()> playing;
        std::function<void()> stopPlayback;  // MediaPlayer::stop
        // Start a play (main.cpp: PlayRequest -> stagePlay -> doPlay). Returns the play
        // generation it dispatched, 0 if it could not.
        std::function<uint64_t(const BrowsePlayTicket&)> play;
        std::function<void(const std::string&)> log;
    };
    struct Config {
        std::string menuUrl;   // "" = no helper: the PMS list only
        std::string pmsBase;   // conf PMS ("" = none)
        std::string pmsToken;  // conf token: in the PMS URL only, never logged
        int cols = 48;         // title columns of the raster, sent to the helper
        int rows = 23;         // rows on screen (PageUp/PageDown without paging rows)
        int menuConnectMs = kMenuTimeoutMs;
        int menuRootTimeoutMs = 3000;
        int menuTimeoutMs = 8000;
        int pmsConnectMs = 6000;
        int pmsTimeoutMs = kPmsBrowseTimeoutMs;
        int ticketTimeoutMs = 8000;
    };

    BrowseController(Config cfg, Hooks hooks) : cfg_(std::move(cfg)), hooks_(std::move(hooks)) {
        if (!hooks_.get)
            hooks_.get = [](const std::string& url, int connectMs, int timeoutMs,
                            const BrowseCancelFn& c) {
                return browseHttpGet(url, connectMs, timeoutMs, c);
            };
    }
    ~BrowseController() { stop(); }
    BrowseController(const BrowseController&) = delete;
    BrowseController& operator=(const BrowseController&) = delete;

    void start(const std::function<void()>& nameThread = {}) {
        std::lock_guard<std::mutex> lk(qMu_);
        if (worker_.joinable() || stopping_)
            return;
        worker_ = std::thread([this, nameThread] {
            if (nameThread)
                nameThread();
            run();
        });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(qMu_);
            stopping_ = true;
            ++gen_;
        }
        qCv_.notify_all();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
            worker_.join();
    }

    // Input thread. Cancels the fetch in flight; ops run in post order.
    void post(BrowseOp op) {
        {
            std::lock_guard<std::mutex> lk(qMu_);
            if (stopping_)
                return;
            if (q_.size() >= kMaxQueue)
                q_.pop_front();
            q_.push_back({op, ++gen_});
        }
        qCv_.notify_one();
        log(std::string("media: browse input ") + browseOpName(op));
    }

    // Any play (MediaPlayer::playInternal): hide the browser, keep its stack, cancel fetches.
    void onPlayStarted(uint64_t generation) {
        ++gen_;
        bool wasVisible = false;
        bool owned = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            wasVisible = br_.visible;
            br_.visible = false;
            if (!br_.stack.empty())
                reopen_ = true;
            lastStartedGen_ = generation;
            ownsSession_ = generation != 0 && generation == browsePlayGen_;
            owned = ownsSession_;
        }
        if (wasVisible || owned)
            log("media: browse hidden by play gen " + std::to_string(generation) +
                (owned ? " (started from the browser)" : " (cast or other source)"));
    }

    bool visible() const {
        std::lock_guard<std::mutex> lk(mu_);
        return br_.visible;
    }

    // The session playing now was started from this browser.
    bool ownsSession() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ownsSession_;
    }

    BrowseView view(int w, int h) const {
        std::lock_guard<std::mutex> lk(mu_);
        return browseWindow(br_, header_, w, h);
    }

    // R1 (hostIdleFb): the whole current level, for browse_r1_adapter.hpp.
    BrowseR1View r1View() const {
        std::lock_guard<std::mutex> lk(mu_);
        BrowseR1View v;
        v.visible = br_.visible;
        v.status = br_.status;
        v.header = header_;
        if (!br_.stack.empty()) {
            v.rows = br_.stack.back();
            v.cursor = br_.cursor();
        }
        if (!levels_.empty()) {
            v.mpxPage = levels_.back().mpxPage;
            v.levelUrl = levels_.back().url;
            v.fromMenu = levels_.back().fromMenu;
        }
        return v;
    }

    // ---- tests ----
    struct Snapshot {
        bool visible = false;
        size_t depth = 0;
        int cursor = 0;
        std::string status;
        std::string header;
        std::vector<std::string> titles; // the top level
        std::vector<int> cursors;
        bool reopen = false;
        bool ownsSession = false;
    };
    Snapshot snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        Snapshot s;
        s.visible = br_.visible;
        s.depth = br_.stack.size();
        s.cursor = br_.cursor();
        s.status = br_.status;
        s.header = header_;
        if (!br_.stack.empty())
            for (const auto& r : br_.stack.back())
                s.titles.push_back(r.title);
        s.cursors = br_.cursors;
        s.reopen = reopen_;
        s.ownsSession = ownsSession_;
        return s;
    }
    // Queue empty and no op running.
    bool idle() const {
        std::lock_guard<std::mutex> lk(qMu_);
        return q_.empty() && !busy_;
    }
    uint64_t generation() const { return gen_.load(); }

private:
    static constexpr size_t kMaxQueue = 32;
    struct Job {
        BrowseOp op;
        uint64_t gen;
    };
    struct Level {
        bool fromMenu = false;
        std::string url;    // helper URL, or PMS path
        std::string header;
        std::string mpxPage; // /a1 page indicator of this level
    };
    struct Fetched {
        bool cancelled = false;
        bool ok = false;
        bool fromMenu = false;
        std::string url;
        BrowsePage page;
        std::string status;
    };

    void log(const std::string& line) const {
        if (hooks_.log)
            hooks_.log(line);
    }

    BrowseCancelFn cancelFor(uint64_t gen) const {
        return [this, gen] {
            return gen_.load() != gen || (hooks_.playing && hooks_.playing());
        };
    }

    std::string pmsUrl(const std::string& path) const {
        std::string url = cfg_.pmsBase;
        if (!path.empty() && path[0] == '/')
            url += path;
        else if (!path.empty())
            url += "/" + path;
        if (!cfg_.pmsToken.empty())
            url += (url.find('?') == std::string::npos ? "?" : "&") +
                   std::string("X-Plex-Token=") + cfg_.pmsToken;
        return url;
    }

    void run() {
        for (;;) {
            Job j{};
            {
                std::unique_lock<std::mutex> lk(qMu_);
                qCv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
                if (stopping_)
                    return;
                j = q_.front();
                q_.pop_front();
                busy_ = true;
            }
            handle(j);
            bool empty = false;
            {
                std::lock_guard<std::mutex> lk(qMu_);
                empty = q_.empty();
            }
            if (empty && dirty_.exchange(false) && hooks_.repaint)
                hooks_.repaint();
            {
                std::lock_guard<std::mutex> lk(qMu_);
                busy_ = false;
            }
        }
    }

    void handle(const Job& j) {
        switch (j.op) {
        case BrowseOp::Open: open(j.gen, false); break;
        case BrowseOp::StopAndReopen: open(j.gen, true); break;
        case BrowseOp::Select: select(j.gen); break;
        case BrowseOp::Back: back(); break;
        case BrowseOp::Up: move(-1); break;
        case BrowseOp::Down: move(1); break;
        case BrowseOp::PageUp: page(j.gen, -1); break;
        case BrowseOp::PageDown: page(j.gen, 1); break;
        case BrowseOp::Info: info(); break;
        case BrowseOp::Left:
        case BrowseOp::Right:
            break;
        }
    }

    void setStatus(const std::string& s, bool paintNow) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            br_.status = s;
        }
        if (paintNow && hooks_.repaint)
            hooks_.repaint();
        else
            dirty_.store(true);
    }

    void clearLoading() {
        std::lock_guard<std::mutex> lk(mu_);
        if (br_.status == "LOADING...")
            br_.status.clear();
        dirty_.store(true);
    }

    BrowseHttpResult get(const std::string& url, int connectMs, int timeoutMs, uint64_t gen) {
        BrowseHttpResult r = hooks_.get(url, connectMs, timeoutMs, cancelFor(gen));
        if (r.cancelled)
            log("media: browse fetch " + urlForLog(url) + " cancelled after " +
                std::to_string(r.elapsedMs) + " ms (newer input)");
        else
            log("media: browse fetch " + urlForLog(url) + " http=" + std::to_string(r.status) +
                " bytes=" + std::to_string(r.body.size()) + " ms=" + std::to_string(r.elapsedMs) +
                (r.unreachable() ? " (no answer)" : ""));
        return r;
    }

    Fetched fetchRoot(uint64_t gen) {
        Fetched f;
        bool offline = false;
        if (!cfg_.menuUrl.empty()) {
            const std::string url =
                withQueryParam(cfg_.menuUrl, "cols", std::to_string(cfg_.cols));
            const BrowseHttpResult r = get(url, cfg_.menuConnectMs, cfg_.menuRootTimeoutMs, gen);
            if (r.cancelled) {
                f.cancelled = true;
                return f;
            }
            if (r.ok()) {
                BrowsePage p = parseBrowseXml(r.body, true);
                if (p.isMediaContainer) {
                    f.ok = true;
                    f.fromMenu = true;
                    f.url = url;
                    f.page = std::move(p);
                    if (f.page.rows.empty())
                        f.status = "EMPTY";
                    return f;
                }
            }
            offline = true;
            log("media: browse helper did not answer: HELPER OFFLINE, local PMS sections");
        }
        if (cfg_.pmsBase.empty()) {
            f.status = offline ? "HELPER OFFLINE" : "NO PLEX SERVER";
            return f;
        }
        const std::string path = "/library/sections";
        const BrowseHttpResult r =
            get(pmsUrl(path), cfg_.pmsConnectMs, cfg_.pmsTimeoutMs, gen);
        if (r.cancelled) {
            f.cancelled = true;
            return f;
        }
        f.url = path;
        f.ok = r.ok();
        f.page = parseBrowseXml(r.body, false);
        if (!f.ok)
            f.status = offline ? "HELPER OFFLINE  FETCH FAILED" : "FETCH FAILED";
        else
            f.status = offline ? "HELPER OFFLINE" : "";
        return f;
    }

    void showRoot(Fetched& f) {
        std::lock_guard<std::mutex> lk(mu_);
        br_.show(std::move(f.page.rows), f.status);
        levels_.clear();
        levels_.push_back({f.fromMenu, f.url, f.page.header, f.page.mpxPage});
        header_ = f.page.header;
        reopen_ = false;
        dirty_.store(true);
    }

    void open(uint64_t gen, bool afterStop) {
        if (hooks_.playing && hooks_.playing() && hooks_.stopPlayback) {
            log(afterStop ? "media: browse: stop of a browse play; showing its level again"
                          : "media: browse: stopping playback to open the browser");
            hooks_.stopPlayback();
        }
        bool kept = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            kept = reopen_ && !br_.stack.empty() && !levels_.empty();
            if (kept) {
                br_.visible = true;
                reopen_ = false;
            }
        }
        if (kept) {
            refreshTop(gen);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            br_.show({}, "LOADING...");
            levels_.clear();
            header_.clear();
        }
        if (hooks_.repaint)
            hooks_.repaint();
        Fetched f = fetchRoot(gen);
        if (f.cancelled) {
            clearLoading();
            return;
        }
        showRoot(f);
    }

    void refreshTop(uint64_t gen) {
        Level top;
        {
            std::lock_guard<std::mutex> lk(mu_);
            top = levels_.back();
        }
        setStatus("LOADING...", true);
        const std::string url = top.fromMenu ? top.url : pmsUrl(top.url);
        const BrowseHttpResult r =
            top.fromMenu ? get(url, cfg_.menuConnectMs, cfg_.menuTimeoutMs, gen)
                         : get(url, cfg_.pmsConnectMs, cfg_.pmsTimeoutMs, gen);
        if (r.cancelled) {
            clearLoading();
            return;
        }
        if (!r.ok()) {
            setStatus(top.fromMenu && r.unreachable() ? "HELPER OFFLINE" : "FETCH FAILED", false);
            return;
        }
        BrowsePage p = parseBrowseXml(r.body, top.fromMenu);
        std::lock_guard<std::mutex> lk(mu_);
        if (br_.stack.empty())
            return;
        const int c = br_.cursors.empty() ? 0 : br_.cursors.back();
        br_.stack.back() = std::move(p.rows);
        if (!br_.cursors.empty())
            br_.cursors.back() = c;
        br_.clamp();
        br_.status = br_.stack.back().empty() ? "EMPTY" : "";
        levels_.back().header = p.header;
        levels_.back().mpxPage = p.mpxPage;
        header_ = p.header;
        dirty_.store(true);
    }

    // Enter a Directory row (push), or a paging row (replace the page).
    void enter(uint64_t gen, const LibraryRow& row) {
        const bool menu = row.fromMenu;
        const std::string url = menu ? menuUrlForKey(cfg_.menuUrl, row.key, 0)
                                     : pmsUrl(libraryListPath(row));
        setStatus("LOADING...", true);
        const BrowseHttpResult r =
            menu ? get(url, cfg_.menuConnectMs, cfg_.menuTimeoutMs, gen)
                 : get(url, cfg_.pmsConnectMs, cfg_.pmsTimeoutMs, gen);
        if (r.cancelled) {
            clearLoading();
            return;
        }
        if (!r.ok()) {
            setStatus(menu && r.unreachable() ? "HELPER OFFLINE" : "FETCH FAILED", false);
            return;
        }
        BrowsePage p = parseBrowseXml(r.body, menu);
        const std::string levelUrl = menu ? url : libraryListPath(row);
        std::lock_guard<std::mutex> lk(mu_);
        if (!br_.visible || br_.stack.empty())
            return;
        const bool empty = p.rows.empty();
        if (browseRowIsPage(row) && !levels_.empty()) {
            br_.stack.back() = std::move(p.rows);
            br_.cursors.back() = 0;
            br_.clamp(); // onto the first selectable row, the one the renderer highlights
            levels_.back() = {menu, levelUrl, p.header, p.mpxPage};
        } else {
            br_.push(std::move(p.rows));
            levels_.push_back({menu, levelUrl, p.header, p.mpxPage});
        }
        header_ = p.header;
        br_.status = empty ? "EMPTY" : "";
        dirty_.store(true);
    }

    void play(uint64_t gen, const LibraryRow& row) {
        BrowsePlayTicket t;
        // A helper play row with a key (wT: "ladder" rows, /menu/resolve/...; the live helper and
        // the build kit: every action row, /menu/ticket/...) is played from its ticket; a
        // contract "local" row without one, and a PMS row, by ratingKey on the conf PMS.
        if (row.fromMenu && !row.key.empty() && row.mpxSource != "local") {
            setStatus("LOADING...", true);
            const std::string url = menuUrlForKey(cfg_.menuUrl, row.key, 0);
            const BrowseHttpResult r =
                get(url, cfg_.menuConnectMs, cfg_.ticketTimeoutMs, gen);
            if (r.cancelled) {
                clearLoading();
                return;
            }
            if (!r.ok()) {
                const std::string helperError = r.unreachable() ? "" : browseContainerError(r.body);
                setStatus(r.unreachable() ? "HELPER OFFLINE"
                                          : (helperError.empty() ? "CANNOT PLAY" : helperError),
                          false);
                return;
            }
            t = parseBrowseTicket(r.body, row);
            t.ticketUrl = url;
            if (!t.error.empty()) {
                log("media: browse play ticket refused: " + t.error);
                setStatus(t.error, false);
                return;
            }
        } else {
            t = localBrowseTicket(row);
        }
        if (gen_.load() != gen) { // a newer input came in while the ticket was fetched
            clearLoading();
            return;
        }
        log("media: browse play ratingKey=" + t.ratingKey + " server=" +
            (t.mpxServer.empty() ? "-" : t.mpxServer) + " source=" + t.source + " start=" +
            (t.offsetPresent ? std::to_string(t.offsetMs) + "ms" : "pms-viewOffset") + " via " +
            (t.address.empty() ? std::string("conf PMS")
                               : t.protocol + "://" + t.address + ":" + t.port));
        {
            std::lock_guard<std::mutex> lk(mu_);
            br_.status.clear();
            br_.visible = false; // the play paints over the menu; Stop brings this level back
            reopen_ = true;
        }
        const uint64_t pg = hooks_.play ? hooks_.play(t) : 0;
        std::lock_guard<std::mutex> lk(mu_);
        browsePlayGen_ = pg;
        if (pg != 0 && lastStartedGen_ == pg)
            ownsSession_ = true; // the play started before this line
        if (pg == 0) {
            br_.visible = true;
            reopen_ = false;
            br_.status = "CANNOT PLAY";
            dirty_.store(true);
        }
    }

    void select(uint64_t gen) {
        LibraryRow row;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const LibraryRow* cur = br_.current();
            if (!br_.visible || !cur)
                return;
            row = *cur;
        }
        if (!browseRowActivatable(row)) {
            log("media: browse row not activatable (" +
                (row.mpxKind.empty() ? std::string("no key") : row.mpxKind) + ")");
            return;
        }
        if (row.directory)
            enter(gen, row);
        else
            play(gen, row);
    }

    void back() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!br_.visible)
            return;
        if (br_.stack.size() > 1) {
            br_.stack.pop_back();
            br_.cursors.pop_back();
            if (!levels_.empty())
                levels_.pop_back();
            header_ = levels_.empty() ? std::string() : levels_.back().header;
            br_.status.clear();
        } else {
            br_.reset();
            levels_.clear();
            header_.clear();
            reopen_ = false;
        }
        dirty_.store(true);
    }

    void move(int delta) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!br_.visible)
            return;
        br_.move(delta);
        if (br_.status.rfind("INFO ", 0) == 0)
            br_.status.clear();
        dirty_.store(true);
    }

    void page(uint64_t gen, int dir) {
        LibraryRow nav;
        bool haveNav = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!br_.visible || br_.stack.empty())
                return;
            for (const auto& r : br_.stack.back())
                if (dir > 0 ? browsePageRowIsNext(r) : browsePageRowIsPrev(r)) {
                    nav = r;
                    haveNav = true;
                    break;
                }
            if (!haveNav) {
                const int n = static_cast<int>(br_.stack.back().size());
                if (n > 0) {
                    const int step = std::max(1, cfg_.rows - 1);
                    int c = br_.cursors.back() + dir * step;
                    c = std::max(0, std::min(n - 1, c));
                    br_.cursors.back() = c;
                    br_.clamp(); // a screen step never rests on an info/header row
                    dirty_.store(true);
                }
                return;
            }
        }
        enter(gen, nav);
    }

    void info() {
        std::lock_guard<std::mutex> lk(mu_);
        const LibraryRow* cur = br_.current();
        if (!br_.visible || !cur)
            return;
        std::string s = "INFO " + (cur->directory ? std::string("FOLDER") : cur->type);
        if (!cur->ratingKey.empty())
            s += " RK " + cur->ratingKey;
        if (!cur->mpxServer.empty())
            s += " " + cur->mpxServer;
        if (cur->offsetPresent && cur->offsetMs > 0) {
            const int64_t secs = cur->offsetMs / 1000;
            char b[32];
            std::snprintf(b, sizeof b, " AT %lld:%02lld", static_cast<long long>(secs / 60),
                          static_cast<long long>(secs % 60));
            s += b;
        }
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        br_.status = s;
        dirty_.store(true);
    }

    Config cfg_;
    Hooks hooks_;

    mutable std::mutex mu_;
    LibraryBrowser br_;
    std::vector<Level> levels_; // parallel to br_.stack
    std::string header_;
    bool reopen_ = false;           // hidden by a play: Open shows the kept stack again
    uint64_t browsePlayGen_ = 0;    // the last play this browser dispatched
    uint64_t lastStartedGen_ = 0;
    bool ownsSession_ = false;

    mutable std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<Job> q_;
    bool stopping_ = false;
    bool busy_ = false;
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> dirty_{false};
    std::thread worker_;
};

// MPX_BROWSE_V2 doPlay: the ladder input for a play of a browse ticket starting at off. The
// helper's server-transcode relay (mpx-menu /menu/tsrc/<slug>/<rk>/o<ms>) carries its start in
// its path, so a play at another start (a seek or a restart re-dispatches the same request)
// fetches the ticket again there (contract §9.3); if that fails, the original Part (src "" and
// serverTranscode false). Every other input passes through unchanged.
struct BrowseLadderInput {
    std::string src;
    bool serverTranscode = false;
    bool refetched = false; // the ticket was fetched again; note says how it went
    std::string note;
};

inline BrowseLadderInput browseLadderInputForStart(
    const std::string& src, const std::string& input, int64_t srcOffsetMs,
    const std::string& ticketUrl, const std::string& ratingKey, int64_t off,
    const std::function<BrowseHttpResult(const std::string&)>& get) {
    BrowseLadderInput li;
    li.src = src;
    li.serverTranscode = input == "server-transcode";
    if (!li.serverTranscode || src.empty() || off == srcOffsetMs)
        return li;
    li.refetched = true;
    li.src.clear();
    const std::string url = browseTicketUrlAtOffset(ticketUrl, off);
    if (!url.empty() && get) {
        const BrowseHttpResult r = get(url);
        if (r.ok()) {
            LibraryRow row;
            row.fromMenu = true;
            row.ratingKey = ratingKey;
            row.mpxServer = "relay"; // not "local": keep the ticket as the helper wrote it
            row.offsetPresent = true;
            row.offsetMs = off;
            const BrowsePlayTicket t = parseBrowseTicket(r.body, row);
            if (t.error.empty() && t.ladderInput == "server-transcode" &&
                (ratingKey.empty() || t.ratingKey == ratingKey))
                li.src = t.ladderSrc;
        }
    }
    if (li.src.empty())
        li.serverTranscode = false;
    li.note = "start " + std::to_string(off) + " is not the ticket's " +
              std::to_string(srcOffsetMs) + ": " +
              (li.src.empty() ? std::string("ticket re-fetch failed; the original Part")
                              : "ticket re-fetched at the new start " + urlForLog(url));
    return li;
}

} // namespace misterplex
