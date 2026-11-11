#include "plexui_controller.hpp"

#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/browse_v2.hpp"
#include "filter_4_3.hpp"
#include "grid_page.hpp"
#include "plexui_config.hpp"
#include "plexui_data.hpp"
#include "hide_empty_hubs.hpp"
#include "plexui_http.hpp"
#include "plexui_link.hpp"
#include "plexui_perf.hpp"
#include "plexui_audio.hpp"
#include "plexui_subtitles.hpp"
#include "libmisterplex/track_select.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <thread>
#include <unordered_set>

namespace misterplex {
namespace plexui {

PlexUiController::PlexUiController(Config cfg, Hooks hooks)
    : cfg_(std::move(cfg)), hooks_(std::move(hooks)) {
    setPlexuiProbeLog([this](const std::string& line) { logLine(line); });
    if (!hooks_.get)
        hooks_.get = makePlexuiHttpGet();
    if (!hooks_.post)
        hooks_.post = makePlexuiHttpPost();
    if (!hooks_.put)
        hooks_.put = makePlexuiHttpPut();
    if (!hooks_.getHeaders)
        hooks_.getHeaders = makePlexuiHeaderGet();
    std::string err;
    if (!renderer_.initFonts(cfg_.fontDir, err) && hooks_.log)
        hooks_.log("plexui: font load failed: " + err);
    else if (!err.empty() && hooks_.log)
        hooks_.log("plexui: " + err);
    renderer_.setImageCache(&images_);
}

PlexUiController::~PlexUiController() {
    stop();
    setPlexuiProbeLog({});
}

void PlexUiController::start(const std::function<void()>& nameThread) {
    images_.start(
        makePlexuiImageHttpGet(),
        [this](const ImageCacheKey& key) {
            renderer_.markImageReady(key);
            coalescedImageRepaint();
        },
        [this](const std::string& line) { logLine(line); });
    std::lock_guard<std::mutex> lk(qMu_);
    if (worker_.joinable() || stopping_)
        return;
    worker_ = std::thread([this, nameThread] {
        if (nameThread)
            nameThread();
        run();
    });
}

void PlexUiController::stop() {
    stopping_ = true;
    ++gen_;
    upNextToken_.fetch_add(1);
    linkGen_.fetch_add(1);
    linkWaitCv_.notify_all();
    qCv_.notify_all();
    images_.stop();
    // The worker can be inside armUpNext. Join it before the helper thread so it
    // cannot replace upNextThread_ while this thread joins that object.
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        worker_.join();
    {
        std::lock_guard<std::mutex> lk(helperMu_);
        joinOwnedThreadUnlocked(upNextThread_);
        joinOwnedThreadUnlocked(linkThread_);
    }
    // The worker has finished, so it will not start another refresh. Join the ones
    // it already started: their cancel captures this controller.
    plexUiApiCacheDrain();
}

void PlexUiController::postHorizontal(int delta, const char* src, int64_t recvMs) {
    postIngress(delta > 0 ? BrowseOp::Right : BrowseOp::Left, src, recvMs);
}

void PlexUiController::applyHorizontal(int delta) {
    if (!visible_)
        return;
    if (view_ == ViewId::Home) {
        if (focus_.sidebar == 0 && delta < 0 && focus_.col > 0) {
            // Inside a hub row: LEFT walks back along the row (it used to jump to
            // the sidebar from any column).
            --focus_.col;
        } else if (focus_.sidebar == 0 && delta < 0 && !sidebarEntries_.empty()) {
            // LEFT past the first tile: sidebar focus lands on Home, the item the
            // hub rows belong to (D7: it used to land on sidebar entry 1).
            focus_.sidebar = kSidebarHomeFocus;
            syncSidebarScroll();
        } else if (focus_.sidebar != 0 && delta > 0) {
            focus_.sidebar = 0; // RIGHT out of the sidebar returns to the content
            syncSidebarScroll();
        } else if (focus_.sidebar == 0) {
            focus_.col = std::max(0, focus_.col + delta);
            if (!hubs_.empty() && focus_.row < static_cast<int>(hubs_.size())) {
                const int maxCol =
                    static_cast<int>(hubs_[static_cast<std::size_t>(focus_.row)].items.size()) - 1;
#ifndef MUT_HOME_EMPTY_ROW_COL
                // #257: an empty hub row (maxCol -1) left RIGHT unclamped, so col ran past the row
                // and the focus patch restored an off-screen rect (SIGSEGV on  lab, row 3 col 3).
                focus_.col = std::max(0, std::min(maxCol, focus_.col));
#else
                if (maxCol >= 0)
                    focus_.col = std::min(maxCol, focus_.col);
#endif
            }
        }
    } else if (view_ == ViewId::LibraryGrid) {
        if (focus_.sidebar != 0) {
            if (delta > 0) {
                focus_.sidebar = 0; // RIGHT out of the drawer returns to the grid
                syncSidebarScroll();
            }
        } else if (focus_.gridOnLetters) {
            int n = focus_.gridLetter + delta;
            if (n < 0)
                n = kAzJumpSlots - 1;
            if (n >= kAzJumpSlots)
                n = 0;
            focus_.gridLetter = n;
        } else if (focus_.gridOnChips) {
            if (gridChips_.empty())
                return;
            focus_.gridChip =
                std::max(0, std::min(static_cast<int>(gridChips_.size()) - 1, focus_.gridChip + delta));
            syncChipScroll();
        } else if (delta < 0 && focus_.col == 0 && !sidebarEntries_.empty()) {
            // LEFT at column 0: open the sidebar on the library this grid belongs to.
            focus_.sidebar = librarySidebarFocusLocked();
            syncSidebarScroll();
        } else {
            focus_.col = std::max(0, std::min(kGridCols() - 1, focus_.col + delta));
        }
    } else if (view_ == ViewId::SeasonList) {
        const GridLayout gl = seasonGridLayout();
        const int cols = std::max(1, gl.cols);
        const int n = static_cast<int>(seasons_.size());
        if (n <= 0)
            return;
        int idx = focus_.row * cols + focus_.col + delta;
        idx = std::max(0, std::min(n - 1, idx));
        focus_.row = idx / cols;
        focus_.col = idx % cols;
        syncSeasonScroll();
    } else if (view_ == ViewId::Detail) {
        const int maxBtn = std::max(0, detailButtonCount(detail_) - 1);
        focus_.detailButton = std::max(0, std::min(maxBtn, focus_.detailButton + delta));
    } else if (view_ == ViewId::UpNext) {
        focus_.detailButton = std::max(0, std::min(1, focus_.detailButton + delta));
#ifndef MUT_RC1037_REMOTE_NOLR
    // Vertical lists, including the server list where remote hosts are rows and the
    // episode list of a show on any host. Left/Right move the same way Up/Down do.
    } else if (view_ == ViewId::ServerList) {
        if (!servers_.empty())
            focus_.row = std::max(0, std::min(static_cast<int>(servers_.size()) - 1, focus_.row + delta));
    } else if (view_ == ViewId::EpisodeList) {
        const int n = static_cast<int>(episodes_.size());
        if (n > 0) {
            focus_.row = std::max(0, std::min(n - 1, focus_.row + delta));
            syncEpisodeScroll();
        }
#endif
    } else if (view_ == ViewId::HomeUsers) {
        if (!homeUsers_.empty())
            focus_.row = std::max(0, std::min(static_cast<int>(homeUsers_.size()) - 1, focus_.row + delta));
    } else if (view_ == ViewId::HomePin) {
        homePinSlot_ = std::max(0, std::min(4, homePinSlot_ + delta));
    }
    bumpAnim();
    dirty_.store(true);
}

void PlexUiController::postIngress(BrowseOp op, const char* src, int64_t recvMs) {
    const int64_t recv = recvMs > 0 ? recvMs : perfSteadyMs();
    const int64_t postMs = perfSteadyMs();
    NavTrace nt{};
    if (op != BrowseOp::Open && op != BrowseOp::StopAndReopen) {
        static std::atomic<uint64_t> navSeq{0};
        nt.id = ++navSeq;
        nt.src = src && *src ? src : "http";
        std::snprintf(nt.op, sizeof(nt.op), "%s", browseOpName(op));
        nt.recvMs = recv;
        nt.postMs = postMs;
        activeNav_ = nt;
        navTraceOpen_.store(true);
        navVisibleOp_ = nt.op;
        navVisibleStart_ = std::chrono::steady_clock::now();
        navVisiblePending_.store(true);
    }
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (stopping_)
            return;
        if (q_.size() >= 32)
            q_.pop_front();
        q_.push_back({op, ++gen_});
    }
    qCv_.notify_one();
    logLine(std::string("media: plexui input ") + browseOpName(op));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "plexui: perf nav op=%s t_ms=%lld", browseOpName(op),
                  static_cast<long long>(postMs));
    logLine(buf);
}

void PlexUiController::post(BrowseOp op) { postIngress(op, "http", 0); }

void PlexUiController::onR1FlipAck(int64_t plxhMs, int64_t plxoMs) {
    bool tick = false;
    if (animWaitAck_.load()) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (focusAnim_ < 3) {
                ++focusAnim_;
                dirty_.store(true);
                // onR1FlipAck runs inside paintIdle (r1PaintMu_ held on the R1 path).
                // hooks_.repaint() is paintIdle, so a synchronous call re-locks r1PaintMu_
                // and then mu_ inside copyFrame. Wake the worker instead; it repaints
                // with no plexui lock held.
#if defined(MUT_PLEXUI_FLIPACK_REPAINT)
                if (focusAnim_ < 3 && hooks_.repaint)
                    hooks_.repaint();
#endif
                if (focusAnim_ >= 3)
                    animWaitAck_.store(false);
                tick = true;
            } else
                animWaitAck_.store(false);
        }
        if (tick) {
            {
                std::lock_guard<std::mutex> g(qMu_);
                animTick_ = true;
            }
            qCv_.notify_one();
        }
    }
    if (navTraceOpen_.load()) {
        NavTrace nt = activeNav_;
        nt.plxhMs = plxhMs;
        nt.plxoMs = plxoMs;
        const int64_t total = plxoMs > 0 ? plxoMs - nt.recvMs : perfSteadyMs() - nt.recvMs;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "plexui: navtrace id=%llu src=%s recv=%lld post=%lld state=%lld "
                      "render_start=%lld render_end=%lld plxh=%lld plxo=%lld total=%lld",
                      static_cast<unsigned long long>(nt.id), nt.src, static_cast<long long>(nt.recvMs),
                      static_cast<long long>(nt.postMs), static_cast<long long>(nt.stateMs),
                      static_cast<long long>(nt.renderStartMs),
                      static_cast<long long>(nt.renderEndMs), static_cast<long long>(nt.plxhMs),
                      static_cast<long long>(nt.plxoMs), static_cast<long long>(total));
        logLine(buf);
        navTraceOpen_.store(false);
    }
    if (!navVisiblePending_.load())
        return;
    // A library Select paints LOADING before the grid XML is back. That frame is not the
    // result: keep the press timer open until the grid (or an error) is what gets acked.
    if (navDeferVisible_.load())
        return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (status_ == "LOADING...")
            return;
    }
    navVisiblePending_.store(false);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - navVisibleStart_)
                        .count();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "plexui: perf nav_visible op=%s t_ms=%lld",
                  navVisibleOp_.c_str(), static_cast<long long>(ms));
    logLine(buf);
}

void PlexUiController::onPlayStarted(uint64_t generation) {
    ++gen_;
    std::lock_guard<std::mutex> lk(mu_);
    if (visible_) {
        visible_ = false;
        reopen_ = true;
    }
    lastStartedGen_ = generation;
    if (queuePlayArmed_) {
        browsePlayGen_ = generation;
        ownsSession_ = generation != 0;
        queuePlayArmed_ = false;
        // A new play of the same rating key must be able to arm again when it ends.
        upNextArmedKey_.clear();
    } else if (generation != 0 && generation == browsePlayGen_) {
        ownsSession_ = true;
    } else {
        // A cast, movie, or any other play that this UI did not queue.
        ownsSession_ = false;
        clearActiveQueueLocked();
    }
    dirty_.store(true);
}

bool PlexUiController::visible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return visible_;
}

bool PlexUiController::ownsSession() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ownsSession_;
}

namespace {

const char* viewIdName(ViewId v) {
    switch (v) {
    case ViewId::ServerList:
        return "servers";
    case ViewId::Home:
        return "home";
    case ViewId::LibraryGrid:
        return "grid";
    case ViewId::Detail:
        return "detail";
    case ViewId::SeasonList:
        return "seasons";
    case ViewId::EpisodeList:
        return "episodes";
    case ViewId::UpNext:
        return "upnext";
    case ViewId::LinkSignIn:
        return "link";
    case ViewId::SubtitlePicker:
        return "subtitles";
    case ViewId::AudioPicker:
        return "audio";
    case ViewId::HomeUsers:
        return "home-users";
    case ViewId::HomePin:
        return "home-pin";
    }
    return "unknown";
}

bool summaryDataReady(const PlexUiController::FrameSummary& s) {
    if (s.loading)
        return false;
    if (s.view == "servers")
        return s.servers > 0 || s.status == "NO SERVERS";
    if (s.view == "home")
        return s.hubs > 0 || s.sections > 0 || s.status == "LOAD FAILED";
    if (s.view == "grid")
        return s.grid > 0;
    if (s.view == "detail" || s.view == "seasons" || s.view == "episodes" || s.view == "upnext" ||
        s.view == "link" || s.view == "home-users" || s.view == "home-pin")
        return true;
    return false;
}

} // namespace

bool PlexUiController::workerIdle() {
    // busy_ flips in the same qMu_ section that pops the job, and it is cleared
    // under qMu_ only after handle() returns. Sampling it before that lock races
    // the handoff: the queue is already empty while Back or StopAndReopen is still
    // writing visible_/view_, so a waiter would read the pre-op UI.
    std::lock_guard<std::mutex> lk(qMu_);
    return !busy_.load() && q_.empty();
}

PlexUiController::FrameSummary PlexUiController::frameSummary() const {
    FrameSummary s;
    std::lock_guard<std::mutex> lk(mu_);
    s.view = viewIdName(view_);
    s.servers = static_cast<int>(servers_.size());
    s.sections = static_cast<int>(sidebarEntries_.size());
    s.hubs = static_cast<int>(hubs_.size());
    for (const auto& hub : hubs_)
        s.hubItems += static_cast<int>(hub.items.size());
    s.grid = static_cast<int>(grid_.size());
    s.loading = status_ == "LOADING...";
    s.status = status_;
    s.imageBytes = images_.bytesUsed();
    return s;
}

bool PlexUiController::waitForFrame(int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    bool scheduledArt = false;
    for (;;) {
        if (workerIdle()) {
            const FrameSummary s = frameSummary();
            if (visible() && summaryDataReady(s)) {
                const bool wantsArt = s.view == "home" || s.view == "grid" || s.view == "detail" ||
                                      s.view == "seasons" || s.view == "episodes";
                if (!scheduledArt) {
                    std::vector<std::uint8_t> scratch;
                    copyFrame(scratch);
                    scheduledArt = wantsArt;
                }
                if (!wantsArt)
                    return true;
                if (images_.busy())
                    continue;
                if (!visibleArtReady())
                    continue;
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

bool PlexUiController::paintReady() {
    if (!visible())
        return false;
    const FrameSummary s = frameSummary();
    if (s.loading || !summaryDataReady(s))
        return false;
    const bool wantsArt = s.view == "home" || s.view == "grid" || s.view == "detail" ||
                          s.view == "seasons" || s.view == "episodes";
    if (!wantsArt)
        return true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scheduleImages();
    }
#if defined(MUT_PLEXUI_SKIP_ART_READY)
    if (!visibleArtReady())
        return true;
#endif
    if (visibleArtReady())
        return true;
    if (images_.busy())
        return false;
    return false;
}

bool PlexUiController::setRenderTarget(int w, int h, PixelFormat fmt) {
    std::lock_guard<std::mutex> lk(mu_);
    const int oldCols = kGridCols();
    const ScreenGeom before = screenGeom();
    setScreenGeom(w, h);
    const bool changed = before.w != kScreenW() || before.h != kScreenH() || fmt != fmt_;
    fmt_ = fmt;
    if (!changed)
        return false;
    setImageGeometry(kScreenW(), kScreenH(), P(8), kHubPosterW(), kHubPosterH());
    const int newCols = kGridCols();
    if (newCols != oldCols && !focus_.gridOnChips && !focus_.gridOnLetters) {   // keep the focused grid item
        const int idx = focus_.row * oldCols + focus_.col;
        focus_.row = idx / newCols;
        focus_.col = idx % newCols;
    }
    syncGridScroll();
    syncSidebarScroll();
    syncChipScroll();
    renderer_.markStaticDirty();
    dirty_.store(true);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "plexui: render target %dx%d %s scale=%.2f grid_cols=%d", kScreenW(),
                  kScreenH(), fmt == PixelFormat::RGB24 ? "rgb24" : "bgra8888", uiScale(), newCols);
    logLine(buf);
    return true;
}

void PlexUiController::applyUiSafeLayout(int marginPct, UiSize size) {
    std::lock_guard<std::mutex> lk(mu_);
    const int oldCols = kGridCols();
    setUiSafePrefs(marginPct, size);
    setImageGeometry(kScreenW(), kScreenH(), P(8), kHubPosterW(), kHubPosterH());
    const int newCols = kGridCols();
    if (newCols != oldCols && !focus_.gridOnChips && !focus_.gridOnLetters) {
        const int idx = focus_.row * oldCols + focus_.col;
        focus_.row = idx / newCols;
        focus_.col = idx % newCols;
    }
    syncGridScroll();
    syncSidebarScroll();
    syncChipScroll();
    renderer_.markStaticDirty();
    dirty_.store(true);
}

UiSize PlexUiController::uiSize() const { return uiSafePrefs().size; }

int PlexUiController::uiSafeMarginPct() const { return uiSafePrefs().marginPct; }

PixelFormat PlexUiController::renderFormat() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_;
}

bool PlexUiController::copyFrame(std::vector<std::uint8_t>& frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!visible_)
        return false;
    scheduleImages();
    if (view_ == ViewId::LibraryGrid && gridArtLoadGen_ != 0 &&
        gridArtLoggedGen_ != gridArtLoadGen_ && gridVisiblePostersCached()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - gridArtLoadStart_)
                            .count();
        gridArtLoggedGen_ = gridArtLoadGen_;
        char buf[128];
        const GridLayout gridLayout = computeGridLayout(true);
        const int visibleRows = gridVisibleRowCount(gridLayout);
        int ok = 0, fail = 0, n = 0;
        for (int row = gridScrollRow_; row < gridScrollRow_ + visibleRows; ++row) {
            for (int col = 0; col < gridLayout.cols; ++col) {
                const int idx = row * gridLayout.cols + col;
                if (idx < 0 || idx >= static_cast<int>(grid_.size()))
                    continue;
                ++n;
                const auto& it = grid_[static_cast<std::size_t>(idx)];
                if (it.thumb.empty())
                    continue;
                const ImageCacheKey ik = thumbCacheKey(server_.machineIdentifier, it.ratingKey,
                                                       gridLayout.posterW, gridLayout.posterH,
                                                       itemThumbRevision(it));
                if (images_.get(ik))
                    ++ok;
                else
                    ++fail;
            }
        }
        const ImageCache::SrcStats src = images_.srcStats();
        std::snprintf(buf, sizeof(buf),
                      "plexui: perf grid_art_ms=%lld ok=%d fail=%d ram=%d disk=%d net=%d items=%d",
                      static_cast<long long>(ms), ok, fail, src.ram, src.disk, src.net, n);
        logLine(buf);
    }
    const int64_t renderStart = perfSteadyMs();
    if (navTraceOpen_.load())
        activeNav_.renderStartMs = renderStart;
    buildRenderState(frameRs_);
    Renderer::RenderPerf rp;
    renderer_.render(frameRs_, frame, &rp, fmt_);
    images_.setLayerBytes(renderer_.layerCacheBytes());   // 0012: layer tier of the global budget
    const int64_t renderEnd = perfSteadyMs();
    if (navTraceOpen_.load())
        activeNav_.renderEndMs = renderEnd;
    // NEVER call hooks_.repaint() here: copyFrame runs inside the daemon's paintIdle (player
    // mutex held) with mu_ held; re-entering paintIdle self-deadlocked on  lab (pass 3).
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "plexui: perf render_ms=%lld static_ms=%lld blit_ms=%lld focus_ms=%lld "
                  "rebuild=%d reason=%s imgcache_bytes=%zu/%zu",
                  static_cast<long long>(rp.renderMs), static_cast<long long>(rp.staticMs),
                  static_cast<long long>(rp.blitMs), static_cast<long long>(rp.focusMs), rp.rebuild,
                  rp.reason.c_str(), images_.bytesUsed(), images_.maxBytes());
    logLine(buf);
    return true;
}

void PlexUiController::logLine(const std::string& s) const {
    if (hooks_.log)
        hooks_.log(s);
}

void PlexUiController::logSidebarFocus(const char* event) const {
    const int idx = std::max(0, focus_.sidebar); // kSidebarHomeFocus logs as Home (0)
    const char* section = "-";
    const char* title = "Home";
    std::string sectionStore;
    std::string titleStore;
    if (idx > 0 && idx <= static_cast<int>(sidebarEntries_.size())) {
        const auto& e = sidebarEntries_[static_cast<std::size_t>(idx - 1)];
        sectionStore = e.sectionKey.empty() ? "-" : e.sectionKey;
        titleStore = e.title.empty() ? "-" : e.title;
        section = sectionStore.c_str();
        title = titleStore.c_str();
    }
    char buf[384];
    if (focus_.sidebar == 0 && view_ == ViewId::Home)   // content focus, not the Home pill
        std::snprintf(buf, sizeof(buf), "plexui: sidebar event=%s focus=0 content row=%d/%d col=%d",
                      event ? event : "", focus_.row, static_cast<int>(hubs_.size()), focus_.col);
    else
        std::snprintf(buf, sizeof(buf), "plexui: sidebar event=%s focus=%d section=%s title=%s",
                      event ? event : "", idx, section, title);
    logLine(buf);
}

BrowseCancelFn PlexUiController::cancelFor(uint64_t gen) const {
    return [this, gen] {
        return gen_.load() != gen || (hooks_.playing && hooks_.playing());
    };
}

void PlexUiController::bumpAnim() {
    focusAnim_ = 0;
    dirty_.store(true);
}

void PlexUiController::coalescedImageRepaint() {
    const auto now = std::chrono::steady_clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastImgRepaint_);
    // Called on an image worker thread (no locks held). Coalesce: if another worker already
    // has a repaint scheduled, drop this one; otherwise wait out the 50 ms window, then paint.
    if (imgRepaintPending_.exchange(true))
        return;
    if (since.count() < 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50) - since);
    lastImgRepaint_ = std::chrono::steady_clock::now();
    imgRepaintPending_.store(false);
    dirty_.store(true);
    if (hooks_.repaint)
        hooks_.repaint();
}

void PlexUiController::parseServerUri(const PlexServer& s, std::string& address,
                                      std::string& port, std::string& protocol) const {
    if (s.connections.empty() || !s.baseUrl.empty()) {  // the probed (reachable) base wins
        const size_t p = s.baseUrl.find("://");
        protocol = p == std::string::npos ? "http" : s.baseUrl.substr(0, p);
        const size_t host = p == std::string::npos ? 0 : p + 3;
        const size_t slash = s.baseUrl.find('/', host);
        std::string hostport =
            slash == std::string::npos ? s.baseUrl.substr(host) : s.baseUrl.substr(host, slash - host);
        const size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            address = hostport.substr(0, colon);
            port = hostport.substr(colon + 1);
        } else {
            address = hostport;
            port = protocol == "https" ? "443" : "80";
        }
        return;
    }
    const auto& c = s.connections.front();
    protocol = c.protocol.empty() ? "http" : c.protocol;
    address = c.address;
    port = c.port.empty() ? (protocol == "https" ? "443" : "80") : c.port;
}

bool PlexUiController::startPlay(const PlexItem& item, int64_t offsetMs, const PlayQueueBind* queue,
                                bool forceStart) {
    const bool queued = queue && !queue->id.empty();
    PlexServer srv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!queued)
            clearActiveQueueLocked();
        queuePlayArmed_ = false;
        if (!hooks_.play || !haveServer_) {
            logLine(std::string("plexui: play ignored") +
                    (!hooks_.play ? " (no play hook)" : " (no server)"));
            return false;
        }
        upNextArmedKey_.clear();
        queuePlayArmed_ = queued;
        srv = server_;
    }
    BrowsePlayTicket t;
    t.ratingKey = item.ratingKey;
    t.key = item.key.empty() ? ("/library/metadata/" + item.ratingKey) : item.key;
    t.title = item.title;
    t.source = "pms";
    t.mpxServer = "plexui";
    parseServerUri(srv, t.address, t.port, t.protocol);
    t.token = srv.accessToken;
    t.machineIdentifier = srv.machineIdentifier;
    if (offsetMs > 0 || forceStart) {
        t.offsetPresent = true;
        t.offsetMs = offsetMs > 0 ? offsetMs : 0;
    }
    if (queue && !queue->id.empty()) {
        t.playQueueId = queue->id;
        t.playQueueItemId = queue->itemId;
        t.playQueueVersion = queue->version;
        t.containerKey = queue->containerKey.empty() ? ("/playQueues/" + queue->id) : queue->containerKey;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Only an explicit picker choice is stamped. No pick → daemon applies
        // SUBTITLE_LANG (G7). Off is stream id 0 and must stay off.
        if (pickedSubtitleId_ >= 0) {
            t.hasSubtitleStreamId = true;
            t.subtitleStreamId = pickedSubtitleId_;
        }
        // Same for audio: only an explicit pick is sent; no pick keeps the default.
        if (pickedAudioId_ > 0) {
            t.hasAudioStreamId = true;
            t.audioStreamId = pickedAudioId_;
        }
    }
    const uint64_t pg = hooks_.play(t);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pg) {
            browsePlayGen_ = pg;
            ownsSession_ = true;
            viewBeforePlay_ = view_;
            logLine("plexui: play ratingKey=" + item.ratingKey +
                    (t.offsetPresent ? (" start=" + std::to_string(t.offsetMs)) : " start=pms") +
                    (t.hasSubtitleStreamId ? (" subtitleStreamID=" + std::to_string(t.subtitleStreamId))
                                           : " subtitleStreamID=default") +
                    (t.hasAudioStreamId ? (" audioStreamID=" + std::to_string(t.audioStreamId))
                                        : " audioStreamID=default"));
        } else {
            queuePlayArmed_ = false;
            logLine("plexui: play hook returned 0 ratingKey=" + item.ratingKey);
        }
    }
    return pg != 0;
}

bool PlexUiController::isHomeSurface() const {
    if (view_ == ViewId::Home)
        return true;
    if (view_ == ViewId::ServerList && haveServer_ && status_ == "LOADING...")
        return true;
    return false;
}

void PlexUiController::syncGridScroll() {
    if (view_ != ViewId::LibraryGrid || grid_.empty())
        return;
    const GridLayout gl = computeGridLayout(!gridChips_.empty());
    const int vis = gridVisibleRowCount(gl);
    const int maxRow = (static_cast<int>(grid_.size()) - 1) / gl.cols;
    if (focus_.row < gridScrollRow_)
        gridScrollRow_ = focus_.row;
    else if (focus_.row >= gridScrollRow_ + vis)
        gridScrollRow_ = std::max(0, focus_.row - vis + 1);
    gridScrollRow_ = std::max(0, std::min(gridScrollRow_, maxRow));
}

// Caller holds mu_ (scheduleImages, from copyFrame / paintReady). rc3: the inner lock_guard
// on the non-recursive mu_ self-deadlocked the first copyFrame with a server (plexui daa2c2398).
void PlexUiController::updateImageCacheBudget() {
    // Caller must hold mu_. copyFrame and paintReady both call this via scheduleImages.
    int hubItems = 0;
    const GridLayout gl = computeGridLayout(view_ == ViewId::LibraryGrid);
#if defined(MUT_PLEXUI_CACHE_RELOCK)
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& hub : hubs_)
            hubItems += static_cast<int>(hub.items.size());
    }
#else
    for (const auto& hub : hubs_)
        hubItems += static_cast<int>(hub.items.size());
#endif
    const std::size_t budget = computeRamCacheBudgetBytes(
        hubItems, gl.posterW, gl.posterH, gridVisibleRowCount(gl), gl.cols);
    images_.setMaxBytes(budget);
    // 0012: the view-layer cache is a tier of the same budget, capped at a quarter of it.
    renderer_.setLayerCacheCap(budget / 4u);
    images_.setLayerBytes(renderer_.layerCacheBytes());
    images_.logBudgetOnce([this](const std::string& s) { logLine(s); });
}

void PlexUiController::scheduleImages() {
    if (!haveServer_)
        return;
    updateImageCacheBudget();
    const uint64_t vgen = ++imgViewGen_;
    images_.beginView(vgen);
    constexpr int kPriorityBackdrop = 300;
    constexpr int kPriorityPrefetchRow = 200;
    constexpr int kPriorityVisible = 100;
    const int kHubPW = kHubPosterW();
    const int kHubPH = kHubPosterH();
    const GridLayout gridLayout = computeGridLayout(view_ == ViewId::LibraryGrid);
    const int kGridPosterW = gridLayout.posterW;
    const int kGridPosterH = gridLayout.posterH;
    constexpr int kBackdropW = 160;
    constexpr int kBackdropH = 90;
    std::vector<ImageCacheKey> pins;
    auto reqThumb = [&](const PlexServer& srv, const PlexItem& it, int w, int h, int priority) {
        if (it.ratingKey.empty())
            return;
        const ImageCacheKey tk =
            thumbCacheKey(srv.machineIdentifier, it.ratingKey, w, h, itemThumbRevision(it));
        if (!it.thumb.empty()) {
            pins.push_back(tk);
            images_.request(tk, photoTranscodeUrl(srv.baseUrl, it.thumb, srv.accessToken, w, h),
                            vgen, priority);
        }
        const ImageCacheKey ak = artCacheKey(srv.machineIdentifier, it.ratingKey, kBackdropW,
                                             kBackdropH, itemArtRevision(it));
        if (!it.art.empty())
            images_.request(ak, photoTranscodeUrl(srv.baseUrl, it.art, srv.accessToken, kBackdropW,
                                                  kBackdropH),
                            vgen, kPriorityBackdrop);
        else if (!it.thumb.empty())
            images_.request(ak, photoTranscodeUrl(srv.baseUrl, it.thumb, srv.accessToken, kBackdropW,
                                                  kBackdropH),
                            vgen, kPriorityBackdrop);
    };
    if (view_ == ViewId::Home) {
        int order = 0;
        for (size_t ri = 0; ri < hubs_.size(); ++ri) {
            const auto& hub = hubs_[ri];
            for (size_t ci = 0; ci < hub.items.size(); ++ci) {
                const auto& it = hub.items[ci];
                const PlexServer& srv = serverForHubItem(it);
                const int pri =
                    (static_cast<int>(ri) == focus_.row && focus_.sidebar == 0)
                        ? kPriorityVisible + static_cast<int>(ci)
                        : kPriorityPrefetchRow + static_cast<int>(ri) * 50 + static_cast<int>(ci);
                if (!it.thumb.empty()) {
                    const ImageCacheKey tk = thumbCacheKey(srv.machineIdentifier, it.ratingKey,
                                                           kHubPW, kHubPH, itemThumbRevision(it));
                    pins.push_back(tk);
                    images_.request(tk, photoTranscodeUrl(srv.baseUrl, it.thumb, srv.accessToken,
                                                          kHubPW, kHubPH),
                                    vgen, pri);
                }
                const ImageCacheKey ak =
                    artCacheKey(srv.machineIdentifier, it.ratingKey, kBackdropW, kBackdropH,
                                itemArtRevision(it));
                if (!it.art.empty())
                    images_.request(ak, photoTranscodeUrl(srv.baseUrl, it.art, srv.accessToken,
                                                          kBackdropW, kBackdropH),
                                    vgen, kPriorityBackdrop + order++);
                else if (!it.thumb.empty())
                    images_.request(ak, photoTranscodeUrl(srv.baseUrl, it.thumb, srv.accessToken,
                                                          kBackdropW, kBackdropH),
                                    vgen, kPriorityBackdrop + order++);
            }
        }
    } else if (view_ == ViewId::LibraryGrid) {
        syncGridScroll();
        const int visibleRows = gridVisibleRowCount(gridLayout);
        const int pageRows = visibleRows + 1;
        for (int page = 0; page < 2; ++page) {
            const int rowStart = gridScrollRow_ + page * visibleRows;
            for (int row = rowStart; row < rowStart + pageRows; ++row) {
                if (row < 0)
                    continue;
                for (int col = 0; col < gridLayout.cols; ++col) {
                    const int idx = row * gridLayout.cols + col;
                    if (idx < 0 || idx >= static_cast<int>(grid_.size()))
                        continue;
                    const int visRow = row - gridScrollRow_;
                    const int pri = visRow < visibleRows
                                        ? kPriorityVisible + visRow * gridLayout.cols + col
                                        : kPriorityPrefetchRow + page * 100 + col;
                    reqThumb(server_, grid_[static_cast<std::size_t>(idx)], kGridPosterW,
                             kGridPosterH, pri);
                }
            }
        }
        const int fidx = focus_.row * gridLayout.cols + focus_.col;
        if (!focus_.gridOnChips && !focus_.gridOnLetters && fidx >= 0 &&
            fidx < static_cast<int>(grid_.size())) {
            const auto& fit = grid_[static_cast<std::size_t>(fidx)];
            const ImageCacheKey ak = artCacheKey(server_.machineIdentifier, fit.ratingKey, kBackdropW,
                                                 kBackdropH, itemArtRevision(fit));
            const std::string& art = !fit.art.empty() ? fit.art : fit.thumb;
            if (!art.empty())
                images_.request(ak, photoTranscodeUrl(server_.baseUrl, art, server_.accessToken,
                                                      kBackdropW, kBackdropH),
                                vgen, kPriorityBackdrop);
        }
    } else if (view_ == ViewId::Detail) {
        // A grid or hub thumb already decoded can paint immediately. The detail-sized
        // request below replaces it when that fetch lands.
        if (!detail_.ratingKey.empty() && !detail_.thumb.empty()) {
            const GridLayout gl = computeGridLayout(true);
            pins.push_back(thumbCacheKey(server_.machineIdentifier, detail_.ratingKey, gl.posterW,
                                         gl.posterH, itemThumbRevision(detail_)));
            pins.push_back(thumbCacheKey(server_.machineIdentifier, detail_.ratingKey, kHubPosterW(),
                                         kHubPosterH(), itemThumbRevision(detail_)));
        }
        reqThumb(server_, detail_, P(150), P(225), kPriorityVisible);
    } else if (view_ == ViewId::SeasonList) {
        syncSeasonScroll();
        const GridLayout gl = seasonGridLayout();
        const int visibleRows = gridVisibleRowCount(gl);
        for (int row = seasonScrollRow_; row < seasonScrollRow_ + visibleRows + 1; ++row) {
            for (int col = 0; col < gl.cols; ++col) {
                const int idx = row * gl.cols + col;
                if (idx < 0 || idx >= static_cast<int>(seasons_.size()))
                    continue;
                const int vis = row - seasonScrollRow_;
                const int pri = vis < visibleRows ? kPriorityVisible + vis * gl.cols + col
                                                  : kPriorityPrefetchRow + col;
                reqThumb(server_, seasons_[static_cast<std::size_t>(idx)], gl.posterW, gl.posterH, pri);
            }
        }
    } else if (view_ == ViewId::EpisodeList) {
        syncEpisodeScroll();
        const int visible = episodeVisibleRows();
        const int tw = P(72);
        const int th = P(40);
        for (int i = episodeScrollRow_; i < episodeScrollRow_ + visible + 2; ++i) {
            if (i < 0 || i >= static_cast<int>(episodes_.size()))
                continue;
            const int pri = i < episodeScrollRow_ + visible ? kPriorityVisible + i : kPriorityPrefetchRow;
            reqThumb(server_, episodes_[static_cast<std::size_t>(i)], tw, th, pri);
        }
    } else if (view_ == ViewId::ServerList && !hubs_.empty() && !hubs_.front().items.empty()) {
        reqThumb(server_, hubs_.front().items.front(), kBackdropW, kBackdropH, kPriorityBackdrop);
    }
    images_.setPinned(pins);
}

void PlexUiController::buildRenderState(RenderState& rs) const {
    rs.view = view_;
    rs.servers = servers_;
    rs.serverIndex = serverIndex_;
    rs.server = server_;
    rs.haveServer = haveServer_;
    rs.sections = sections_;
    rs.sidebarEntries = sidebarEntries_;
    rs.sidebarScrollY = sidebarScrollY_;
    rs.hubs = hubs_;
    rs.hubItemServerIndex = hubItemServerIndex_;
    rs.grid = grid_;
    rs.detail = detail_;
    rs.focus = focus_;
    rs.focusAnim = focusAnim_;
    rs.status = status_;
    rs.queueLine = queueLine_;
    rs.gridSectionTitle = gridSectionTitle_;
    rs.gridItemCount = static_cast<int>(grid_.size());
    rs.gridChips = gridChips_;
    rs.gridActiveChip = gridActiveChip_;
    rs.gridChipScrollX = gridChipScrollX_;
    rs.gridScrollRow = gridScrollRow_;
    rs.gridLetterFilter = gridLetterFilter_;
    rs.filter43On = filter43Enabled() && filter43On_;
    rs.dataGeneration = dataGeneration_;
    if (rs.listSnap != listGeneration_) {
        rs.seasons = seasons_;
        rs.episodes = episodes_;
        rs.listSnap = listGeneration_;
    }
    rs.seasonScrollRow = seasonScrollRow_;
    rs.episodeScrollRow = episodeScrollRow_;
    rs.listTitle = listTitle_;
    rs.upNextSeconds = upNextSeconds_;
    rs.linkCode = linkCode_;
    rs.linkSeconds = linkSeconds_;
    rs.homeRows.clear();
    rs.homeRows.reserve(homeUsers_.size());
    for (size_t i = 0; i < homeUsers_.size(); ++i) {
        const HomeUser& u = homeUsers_[i];
        SidebarLibraryEntry row;
        row.title = u.title.empty() ? "User" : u.title;
        if (u.protectedPin)
            row.subtitle = "PIN";
        else if (u.admin)
            row.subtitle = "Admin";
        else if (u.restricted)
            row.subtitle = "Managed";
        else
            row.subtitle = "Home";
        if (!cfg_.homeUserId.empty() && u.id == cfg_.homeUserId)
            row.subtitle += " · On";
        rs.homeRows.push_back(std::move(row));
    }
    rs.homePin = homePin_;
    rs.homePinSlot = homePinSlot_;
    rs.homePinTitle = homePinTitle_;
    if (activeQueue_.ok && activeQueueIndex_ + 1 >= 0 &&
        activeQueueIndex_ + 1 < static_cast<int>(activeQueue_.items.size())) {
        const auto& n = activeQueue_.items[static_cast<std::size_t>(activeQueueIndex_ + 1)];
        rs.upNext.ratingKey = n.ratingKey;
        rs.upNext.title = n.title;
        rs.upNext.parentIndex = n.parentIndex;
        rs.upNext.index = n.index;
        rs.upNext.runtimeMs = n.runtimeMs;
    }
    rs.gridFilterChipTitle.clear();
    if (!gridChips_.empty() && gridActiveChip_ >= 0 &&
        gridActiveChip_ < static_cast<int>(gridChips_.size()))
        rs.gridFilterChipTitle = gridChips_[static_cast<std::size_t>(gridActiveChip_)].title;
    rs.detailBackdrop = view_ == ViewId::Detail;
    rs.backdropUrl.clear();
    rs.backdropKeyValid = false;
    if (haveServer_) {
        const PlexItem* focusItem = nullptr;
        if (view_ == ViewId::Detail)
            focusItem = &detail_;
        else if (view_ == ViewId::Home && focus_.sidebar == 0 && focus_.row >= 0 &&
                 focus_.row < static_cast<int>(hubs_.size())) {
            const auto& hub = hubs_[static_cast<std::size_t>(focus_.row)];
            if (focus_.col >= 0 && focus_.col < static_cast<int>(hub.items.size()))
                focusItem = &hub.items[static_cast<std::size_t>(focus_.col)];
            else if (!hub.items.empty())
                focusItem = &hub.items.front();
        } else if (view_ == ViewId::SeasonList) {
            const GridLayout gl = seasonGridLayout();
            const int idx = focus_.row * gl.cols + focus_.col;
            if (idx >= 0 && idx < static_cast<int>(seasons_.size()))
                focusItem = &seasons_[static_cast<std::size_t>(idx)];
        } else if (view_ == ViewId::EpisodeList) {
            if (focus_.row >= 0 && focus_.row < static_cast<int>(episodes_.size()))
                focusItem = &episodes_[static_cast<std::size_t>(focus_.row)];
        } else if (view_ == ViewId::LibraryGrid && !focus_.gridOnChips && !focus_.gridOnLetters) {
            const int idx = focus_.row * kGridCols() + focus_.col;
            if (idx >= 0 && idx < static_cast<int>(grid_.size()))
                focusItem = &grid_[static_cast<std::size_t>(idx)];
        } else if (view_ == ViewId::ServerList && !hubs_.empty() && !hubs_.front().items.empty()) {
            focusItem = &hubs_.front().items.front();
        }
        if (!focusItem && view_ == ViewId::Home && !hubs_.empty() && !hubs_.front().items.empty())
            focusItem = &hubs_.front().items.front();
        if (focusItem) {
            const PlexServer& bSrv = serverForHubItem(*focusItem);
            const std::string& art = !focusItem->art.empty() ? focusItem->art : focusItem->thumb;
            if (!art.empty() && !focusItem->ratingKey.empty()) {
                rs.backdropUrl =
                    photoTranscodeUrl(bSrv.baseUrl, art, bSrv.accessToken, 160, 90);
                rs.backdropKey = artCacheKey(bSrv.machineIdentifier, focusItem->ratingKey, 160, 90,
                                             itemArtRevision(*focusItem));
                rs.backdropKeyValid = true;
            }
        }
    }
}

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

namespace {

bool isTestPlexServer(const PlexServer& s) {
    return isFixturePlexServerName(s.name);
}

} // namespace

bool PlexUiController::serverMatches(const PlexServer& s, const std::string& needle) const {
    if (needle.empty())
        return false;
    const std::string n = lowerCopy(needle);
    if (!s.sourceTitle.empty() && lowerCopy(s.sourceTitle).find(n) != std::string::npos)
        return true;
    if (lowerCopy(s.name).find(n) != std::string::npos)
        return true;
    for (const auto& c : s.connections) {
        if (lowerCopy(c.address).find(n) != std::string::npos)
            return true;
        if (!c.uri.empty() && lowerCopy(c.uri).find(n) != std::string::npos)
            return true;
    }
    const size_t p = s.baseUrl.find("://");
    if (p != std::string::npos && lowerCopy(s.baseUrl.substr(p + 3)).find(n) != std::string::npos)
        return true;
    return false;
}

int PlexUiController::findServerIndex(const std::string& needle) const {
    if (!needle.empty()) {
        for (size_t i = 0; i < servers_.size(); ++i) {
            if (serverMatches(servers_[i], needle))
                return static_cast<int>(i);
        }
    }
    static const char* kPrefer[] = {"serverA", "192.0.2.10"};
    for (const char* pref : kPrefer) {
        for (size_t i = 0; i < servers_.size(); ++i) {
            if (isTestPlexServer(servers_[i]))
                continue;
            if (serverMatches(servers_[i], pref))
                return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < servers_.size(); ++i) {
        if (!isTestPlexServer(servers_[i]))
            return static_cast<int>(i);
    }
    return 0;
}

void PlexUiController::setShotPreferences(const std::string& serverNeedle,
                                          const std::string& sectionNeedle) {
    std::lock_guard<std::mutex> lk(mu_);
    shotServerNeedle_ = serverNeedle;
    shotSectionNeedle_ = sectionNeedle;
}

bool PlexUiController::shotFocusServer(const std::string& needle) {
    std::lock_guard<std::mutex> lk(mu_);
    if (servers_.empty())
        return false;
    focus_.row = findServerIndex(needle.empty() ? shotServerNeedle_ : needle);
    return true;
}

int PlexUiController::findSectionIndex(const std::string& needle) const {
    if (sidebarEntries_.empty())
        return -1;
    const std::string want = needle.empty() ? shotSectionNeedle_ : needle;
    if (!want.empty()) {
        const std::string w = lowerCopy(want);
        int best = -1;
        int bestScore = 99;
        for (size_t i = 0; i < sidebarEntries_.size(); ++i) {
            const std::string t = lowerCopy(sidebarEntries_[i].title);
            int score = 99;
            if (t == w)
                score = 0;
            else if (t.find(w) != std::string::npos)
                score = 1 + static_cast<int>(t.size() - w.size());
            else
                continue;
            if (score < bestScore) {
                bestScore = score;
                best = static_cast<int>(i);
            }
        }
        if (best >= 0)
            return best;
    }
    for (size_t i = 0; i < sidebarEntries_.size(); ++i) {
        if (lowerCopy(sidebarEntries_[i].type) == "movie")
            return static_cast<int>(i);
    }
    return 0;
}

int PlexUiController::findSidebarIndex(const std::string& titleSlashSubtitle) const {
    const size_t slash = titleSlashSubtitle.find('/');
    const std::string wantTitle =
        slash == std::string::npos ? titleSlashSubtitle : titleSlashSubtitle.substr(0, slash);
    const std::string wantSub =
        slash == std::string::npos ? std::string{} : titleSlashSubtitle.substr(slash + 1);
    const std::string wt = lowerCopy(wantTitle);
    const std::string ws = lowerCopy(wantSub);
    for (size_t i = 0; i < sidebarEntries_.size(); ++i) {
        if (lowerCopy(sidebarEntries_[i].title) != wt)
            continue;
        if (!ws.empty() && lowerCopy(sidebarEntries_[i].subtitle) != ws)
            continue;
        return static_cast<int>(i);
    }
    return -1;
}

int PlexUiController::librarySidebarFocusLocked() const {
    if (gridSectionIndex_ >= 0 &&
        gridSectionIndex_ < static_cast<int>(sidebarEntries_.size()))
        return gridSectionIndex_ + 1;
    if (!gridSectionKey_.empty()) {
        for (size_t i = 0; i < sidebarEntries_.size(); ++i) {
            if (sidebarEntries_[i].sectionKey == gridSectionKey_)
                return static_cast<int>(i) + 1;
        }
    }
    if (!gridSectionTitle_.empty()) {
        const int idx = findSidebarIndex(gridSectionTitle_);
        if (idx >= 0)
            return idx + 1;
    }
    return kSidebarHomeFocus;
}

void PlexUiController::syncSidebarScroll() {
    const int listTop = P(64);
    const int listBottom = kScreenH() - kSafeBottomInset();
    const int visibleH = listBottom - listTop;
    const int rowH = kSidebarRowH();
    const int focusRow = std::max(0, focus_.sidebar);
    const int rowY = listTop + focusRow * rowH;
    if (rowY - sidebarScrollY_ < listTop)
        sidebarScrollY_ = rowY - listTop;
    else if (rowY + rowH - sidebarScrollY_ > listBottom)
        sidebarScrollY_ = focusRow * rowH + rowH - visibleH;
    if (sidebarScrollY_ < 0)
        sidebarScrollY_ = 0;
    const int totalRows = 1 + static_cast<int>(sidebarEntries_.size());
    const int maxScroll = std::max(0, totalRows * rowH - visibleH);
    if (sidebarScrollY_ > maxScroll)
        sidebarScrollY_ = maxScroll;
}

int PlexUiController::firstSidebarNavFocusLocked() const {
    for (size_t i = 0; i < sidebarEntries_.size(); ++i) {
        const auto& e = sidebarEntries_[i];
        if (e.kind == SidebarRowKind::SignIn || e.type == "signin" ||
            e.kind == SidebarRowKind::HomeUser || e.type == "homeuser")
            continue;
        return static_cast<int>(i) + 1;
    }
    return 0;
}

bool PlexUiController::homeContentNavigableLocked() const {
    for (const auto& hub : hubs_) {
        if (!hub.items.empty())
            return true;
    }
    return false;
}

void PlexUiController::focusEmptyHomeSidebarLocked() {
    if (view_ != ViewId::Home || focus_.sidebar != 0 || homeContentNavigableLocked())
        return;
    const int nav = firstSidebarNavFocusLocked();
    if (nav <= 0)
        return;
    focus_.sidebar = nav;
    focus_.row = 0;
    focus_.col = 0;
    syncSidebarScroll();
    logSidebarFocus("empty-home");
}

const PlexServer& PlexUiController::serverForHubItem(const PlexItem& item) const {
    const auto it = hubItemServerIndex_.find(item.ratingKey);
    if (it != hubItemServerIndex_.end() && it->second >= 0 &&
        it->second < static_cast<int>(servers_.size()))
        return servers_[static_cast<std::size_t>(it->second)];
    return server_;
}

bool PlexUiController::loadStartupCatalog(const BrowseCancelFn& cancel) {
    std::vector<std::string> secErrs;
    auto perSec = fetchAllServerSectionsParallel(hooks_.get, servers_, cancel, &secErrs);
    if (cancel())
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    lastLoadErr_.clear();
    int hi = pickHomeServerIndex(servers_);
    // Open raced catalog: plex.tv order may not have an RFC1918 pick yet.
    // Still bind a server so Home is not stuck LOADING with an empty sidebar.
    if (hi < 0 && !servers_.empty())
        hi = 0;
    hi = rotatePickedServerToFront(servers_, hi, &perSec, &secErrs);
    buildSidebarEntries(servers_, perSec, sidebarEntries_);
    appendSidebarSignIn(sidebarEntries_);
    if (!cfg_.accountToken.empty())
        appendSidebarHomeUser(sidebarEntries_, cfg_.homeUserTitle);
    ensureServerPickerRow(sidebarEntries_, servers_.size());
    if (hi >= 0) {
        serverIndex_ = hi;
        assignServerLocked(servers_[static_cast<std::size_t>(hi)]);
        haveServer_ = true;
        // reviewer: a 401/403 on the picked server is shown, not an empty library.
        const auto ui = static_cast<std::size_t>(hi);
        if (ui < secErrs.size() && secErrs[ui].find("(40") != std::string::npos) {
            lastLoadErr_ = secErrs[ui];
            status_ = lastLoadErr_;
            logLine("plexui: library error: " + lastLoadErr_);
        }
    }
    view_ = ViewId::Home;
    focus_ = {};
    sidebarScrollY_ = 0;
    return haveServer_;
}

bool PlexUiController::loadHomeHubs(const BrowseCancelFn& cancel) {
    PlexServer srv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!haveServer_)
            return false;
        srv = server_;
        hubItemServerIndex_.clear();
    }
    std::vector<PlexSection> sec;
    std::vector<PlexHubRow> hubs;
    std::string err;
    if (!fetchServerHome(hooks_.get, srv, cancel, sec, hubs, err)) {
        if (!err.empty() && err.find("(40") != std::string::npos) {
            std::lock_guard<std::mutex> lk(mu_);
            if (lastLoadErr_ != err)
                logLine("plexui: library error: " + err);
            lastLoadErr_ = err;
        }
        return false;
    }
    PlexHubRow mergedCw;
    std::unordered_map<std::string, int> cwServers;
    if (fetchMergedSharedContinueWatching(hooks_.get, servers_, cancel, mergedCw, cwServers) &&
        !mergedCw.items.empty()) {
        bool replaced = false;
        for (auto& h : hubs) {
            if (lowerCopy(h.title).find("continue") != std::string::npos) {
                h.items = std::move(mergedCw.items);
                replaced = true;
                break;
            }
        }
        if (!replaced)
            hubs.insert(hubs.begin(), std::move(mergedCw));
        std::lock_guard<std::mutex> lk(mu_);
        hubItemServerIndex_ = std::move(cwServers);
    }
    std::lock_guard<std::mutex> lk(mu_);
    sections_ = std::move(sec);
    filterEmptyHomeHubs(hubs, hideEmptyHubsEnabled());   // #220, default on
    hubs_ = std::move(hubs);
    ++dataGeneration_;
    status_.clear();
    lastLoadErr_.clear();
    view_ = ViewId::Home;
    return true;
}

bool PlexUiController::shotFocusLibrary(const std::string& titleSlashSubtitle) {
    std::lock_guard<std::mutex> lk(mu_);
    const int idx = findSidebarIndex(titleSlashSubtitle);
    if (idx < 0)
        return false;
    focus_.sidebar = idx + 1;
    syncSidebarScroll();
    return true;
}

bool PlexUiController::shotFocusSidebarIndex(int index) {
    std::lock_guard<std::mutex> lk(mu_);
    if (index < 0 || index > static_cast<int>(sidebarEntries_.size()))
        return false;
    focus_.sidebar = index;
    syncSidebarScroll();
    return true;
}

void PlexUiController::markGridArtLoadStart() {
    ++gridArtLoadGen_;
    gridArtLoadStart_ = std::chrono::steady_clock::now();
}

bool PlexUiController::gridVisiblePostersCached() const {
    if (!haveServer_ || grid_.empty())
        return true;
    const GridLayout gridLayout = computeGridLayout(true);
    const int w = gridLayout.posterW;
    const int h = gridLayout.posterH;
    const int visibleRows = gridVisibleRowCount(gridLayout);
    for (int row = gridScrollRow_; row < gridScrollRow_ + visibleRows; ++row) {
        for (int col = 0; col < gridLayout.cols; ++col) {
            const int idx = row * gridLayout.cols + col;
            if (idx < 0 || idx >= static_cast<int>(grid_.size()))
                continue;
            const auto& it = grid_[static_cast<std::size_t>(idx)];
            if (it.thumb.empty())
                continue;
            const ImageCacheKey ik =
                thumbCacheKey(server_.machineIdentifier, it.ratingKey, w, h, itemThumbRevision(it));
            if (!images_.get(ik))
                return false;
        }
    }
    return true;
}

#ifdef MPX_PLEXUI_UNIT_TEST
void PlexUiController::testSetLibraryGrid(PlexServer server, std::vector<PlexItem> grid,
                                          std::vector<GridCategoryChip> chips,
                                          std::vector<SidebarLibraryEntry> sidebar,
                                          int sectionIndex) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    assignServerLocked(std::move(server));
    view_ = ViewId::LibraryGrid;
    filter43On_ = false;
    gridFull_.clear();
    storeGridLocked(std::move(grid));
    gridChips_ = std::move(chips);
    gridActiveChip_ = 0;
    gridSectionKey_ = "/library/sections/1";
    gridSectionTitle_ = "Movies";
    if (!sidebar.empty())
        sidebarEntries_ = std::move(sidebar);
    if (sectionIndex >= 0)
        gridSectionIndex_ = sectionIndex;
    status_.clear();
    focus_ = {};
    gridLetterFilter_ = 0;
    clearGridPageLocked();
    dirty_.store(true);
}

void PlexUiController::testInsertImage(const ImageCacheKey& key) {
    images_.testInsertDecoded(key, 4, 4);
}

void PlexUiController::testArmAnimAck() {
    focusAnim_ = 0;
    animWaitAck_.store(true);
}

void PlexUiController::testArmAnimAt(int frame) {
    std::lock_guard<std::mutex> lk(mu_);
    focusAnim_ = frame;
    animWaitAck_.store(frame < 3);
}

void PlexUiController::testSetDetail(PlexServer server, PlexItem detail) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    assignServerLocked(std::move(server));
    detail_ = std::move(detail);
    pickedSubtitleId_ = -1;
    pickedAudioId_ = -1;
    view_ = ViewId::Detail;
    status_.clear();
    dirty_.store(true);
}

#ifdef MPX_PLEXUI_UNIT_TEST
void PlexUiController::testKickShowTheme() {
    PlexServer srv;
    PlexItem item;
    {
        std::lock_guard<std::mutex> lk(mu_);
        srv = server_;
        item = detail_;
    }
    kickShowTheme(srv, item, [] { return false; });
}
#endif

#ifdef MPX_PLEXUI_UNIT_TEST
void PlexUiController::testSetDetailButton(int button) {
    std::lock_guard<std::mutex> lk(mu_);
    focus_.detailButton = std::max(0, button);
}

void PlexUiController::testSetPickerRow(int row) {
    std::lock_guard<std::mutex> lk(mu_);
    focus_.row = std::max(0, row);
}

int PlexUiController::testDetailButton() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.detailButton;
}

int PlexUiController::testPickedAudioId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pickedAudioId_;
}

int PlexUiController::testPickedSubtitleId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pickedSubtitleId_;
}

int PlexUiController::testPickerRow() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.row;
}
#endif

void PlexUiController::testSetServers(std::vector<PlexServer> servers) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    servers_ = std::move(servers);
    haveServer_ = !servers_.empty();
    if (haveServer_)
        assignServerLocked(servers_.front());
    view_ = ViewId::ServerList;
    focus_ = {};
    status_.clear();
    dirty_.store(true);
}

void PlexUiController::testSetEpisodes(PlexServer server, std::vector<PlexItem> episodes) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    assignServerLocked(std::move(server));
    episodes_ = std::move(episodes);
    touchListsLocked();
    view_ = ViewId::EpisodeList;
    listTitle_ = "Season 1";
    focus_ = {};
    status_.clear();
    dirty_.store(true);
}

void PlexUiController::testSetHome(std::vector<SidebarLibraryEntry> entries) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    view_ = ViewId::Home;
    sidebarEntries_ = std::move(entries);
    focus_ = {};
    focus_.sidebar = 0;
    sidebarScrollY_ = 0;
    status_.clear();
    dirty_.store(true);
}

std::vector<PlexHubRow> PlexUiController::testHubs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return hubs_;
}

PlexItem PlexUiController::testDetail() const {
    std::lock_guard<std::mutex> lk(mu_);
    return detail_;
}

void PlexUiController::testArmLibrarySelect() {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    view_ = ViewId::Home;
    status_.clear();
    PlexServer s;
    s.name = "Shared";
    s.machineIdentifier = "mid-lib";
    s.baseUrl = "http://plex";
    s.accessToken = "tok";
    servers_ = {s};
    assignServerLocked(s);
    serverIndex_ = 0;
    SidebarLibraryEntry e;
    e.title = "TV Shows";
    e.sectionKey = "2";
    e.type = "show";
    e.serverIndex = 0;
    sidebarEntries_ = {e};
    focus_ = {};
    focus_.sidebar = 1;
    sidebarScrollY_ = 0;
    dirty_.store(true);
}

void PlexUiController::testSetHomeHubs(PlexServer server, std::vector<PlexHubRow> hubs,
                                       std::vector<SidebarLibraryEntry> entries) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    assignServerLocked(std::move(server));
    servers_ = {server_};
    serverIndex_ = 0;
    filterEmptyHomeHubs(hubs, hideEmptyHubsEnabled());   // #220, default on
    hubs_ = std::move(hubs);
    sidebarEntries_ = std::move(entries);
    view_ = ViewId::Home;
    focus_ = {};
    focus_.sidebar = 0;
    sidebarScrollY_ = 0;
    status_.clear();
    dirty_.store(true);
}

int PlexUiController::testFocusCol() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.col;
}

void PlexUiController::testRefreshHomeHubsKeepFocus(std::vector<PlexHubRow> hubs) {
    std::lock_guard<std::mutex> lk(mu_);
    filterEmptyHomeHubs(hubs, hideEmptyHubsEnabled());   // #220, same as the refresh site; focus_ untouched
    hubs_ = std::move(hubs);
    ++dataGeneration_;
    dirty_.store(true);
}

int PlexUiController::testFocusRow() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.row;
}

int PlexUiController::testSidebarFocus() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.sidebar;
}

int PlexUiController::testSidebarEntryCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(sidebarEntries_.size());
}

void PlexUiController::testSeedTvStack() {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    ownsSession_ = false;
    PlexServer s;
    s.name = "Home";
    s.machineIdentifier = "mid";
    s.baseUrl = "http://127.0.0.1:32400";
    s.accessToken = "tok";
    servers_ = {s};
    assignServerLocked(s);
    gridSectionIndex_ = 0;
    gridSectionTitle_ = "TV Shows";
    PlexItem show;
    show.ratingKey = "10";
    show.title = "Example Show";
    show.type = "show";
    detail_ = show;
    PlexItem season;
    season.ratingKey = "11";
    season.title = "Season 1";
    season.type = "season";
    season.index = 1;
    season.leafCount = 2;
    seasons_ = {season};
    PlexItem ep;
    ep.ratingKey = "12";
    ep.title = "Pilot";
    ep.type = "episode";
    ep.parentIndex = 1;
    ep.index = 1;
    ep.runtimeMs = 1200000;
    episodes_ = {ep};
    touchListsLocked();
    view_ = ViewId::EpisodeList;
    listTitle_ = "Season 1";
    focus_ = {};
    status_.clear();
    dirty_.store(true);
}

std::string PlexUiController::testViewName() const {
    std::lock_guard<std::mutex> lk(mu_);
    return viewIdName(view_);
}

bool PlexUiController::testVisible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return visible_;
}

int PlexUiController::testQueueIndex() const {
    std::lock_guard<std::mutex> lk(mu_);
    return activeQueueIndex_;
}

bool PlexUiController::testQueueBound() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queueBoundToServerLocked();
}

std::string PlexUiController::testLinkCode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return linkCode_;
}

std::string PlexUiController::testStatus() const {
    std::lock_guard<std::mutex> lk(mu_);
    return status_;
}

std::string PlexUiController::testQueueId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return activeQueue_.id;
}

int PlexUiController::testQueueDepth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return activeQueue_.ok ? static_cast<int>(activeQueue_.items.size()) : 0;
}

void PlexUiController::testSetFocusRow(int row) {
    std::lock_guard<std::mutex> lk(mu_);
    focus_.row = row;
}

int PlexUiController::testGridCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<int>(grid_.size());
}

std::string PlexUiController::testGridTitle(int idx) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx < 0 || idx >= static_cast<int>(grid_.size()))
        return {};
    return grid_[static_cast<std::size_t>(idx)].title;
}

bool PlexUiController::testFilter43On() const {
    std::lock_guard<std::mutex> lk(mu_);
    return filter43On_;
}

bool PlexUiController::testGridOnLetters() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.gridOnLetters;
}

int PlexUiController::testGridLetterSlot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.gridLetter;
}

char PlexUiController::testGridLetterFilter() const {
    std::lock_guard<std::mutex> lk(mu_);
    return gridLetterFilter_;
}

void PlexUiController::testArmGridPage(int totalSize, const std::string& sectionKey, int activeChip) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!sectionKey.empty())
        gridSectionKey_ = sectionKey;
    if (activeChip >= 0)
        gridActiveChip_ = activeChip;
    gridTotalSize_ = totalSize;
    gridPageFull_ = static_cast<int>(grid_.size()) >= kGridPageSize;
    gridPageBusy_ = false;
}

void PlexUiController::testHandleInfo() {
    Job j;
    j.op = BrowseOp::Info;
    j.gen = 1;
    handle(j);
}

void PlexUiController::testHandlePlayNext() {
    Job j;
    j.op = BrowseOp::Info;
    j.queueMark = 2;
    j.gen = 1;
    handle(j);
}

void PlexUiController::testHandleMark(int op) {
    Job j;
    j.op = BrowseOp::Info;
    j.markOp = op;
    j.gen = 1;
    handle(j);
}

int PlexUiController::testEpisodeViewCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (episodes_.empty())
        return -1;
    return episodes_.front().viewCount;
}

void PlexUiController::testStepNext() {
    Job j;
    j.op = BrowseOp::Select;
    j.queueDelta = 1;
    j.gen = 1;
    handle(j);
}

int PlexUiController::testUpNextSeconds() const {
    std::lock_guard<std::mutex> lk(mu_);
    return upNextSeconds_;
}

std::string PlexUiController::testAccountToken() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_.accountToken;
}

std::string PlexUiController::testHomeToken() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_.homeUserToken;
}

std::string PlexUiController::testSidebarTitle(int index) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (index < 0 || index >= static_cast<int>(sidebarEntries_.size()))
        return {};
    return sidebarEntries_[static_cast<std::size_t>(index)].title;
}
#endif

bool PlexUiController::visibleArtReady() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!haveServer_)
        return true;
    auto cachedThumb = [this](const PlexServer& srv, const PlexItem& it, int w, int h) {
        if (it.thumb.empty() || it.ratingKey.empty())
            return true;
        return images_.get(thumbCacheKey(srv.machineIdentifier, it.ratingKey, w, h,
                                         itemThumbRevision(it))) != nullptr;
    };
    auto cachedArt = [this](const PlexServer& srv, const PlexItem& it) {
        const std::string& art = !it.art.empty() ? it.art : it.thumb;
        if (art.empty() || it.ratingKey.empty())
            return true;
        return images_.get(artCacheKey(srv.machineIdentifier, it.ratingKey, 160, 90,
                                       itemArtRevision(it))) != nullptr;
    };
    if (view_ == ViewId::Home) {
        const int w = kHubPosterW(), h = kHubPosterH();
        for (const auto& hub : hubs_) {
            const int n = std::min(5, static_cast<int>(hub.items.size()));
            for (int i = 0; i < n; ++i) {
                const auto& it = hub.items[static_cast<std::size_t>(i)];
                const PlexServer& srv = serverForHubItem(it);
                if (!cachedThumb(srv, it, w, h))
                    return false;
            }
        }
        if (!hubs_.empty() && !hubs_.front().items.empty()) {
            const PlexItem& it = hubs_.front().items.front();
            const PlexServer& srv = serverForHubItem(it);
            const std::string& art = !it.art.empty() ? it.art : it.thumb;
            if (!art.empty())
                return cachedArt(srv, it);
        }
        return true;
    }
    if (view_ == ViewId::LibraryGrid) {
        const GridLayout gridLayout = computeGridLayout(view_ == ViewId::LibraryGrid);
        const int w = gridLayout.posterW;
        const int h = gridLayout.posterH;
        const int visibleRows = gridVisibleRowCount(gridLayout);
        for (int row = gridScrollRow_; row < gridScrollRow_ + visibleRows; ++row) {
            for (int col = 0; col < gridLayout.cols; ++col) {
                const int idx = row * gridLayout.cols + col;
                if (idx < 0 || idx >= static_cast<int>(grid_.size()))
                    continue;
                if (!cachedThumb(server_, grid_[static_cast<std::size_t>(idx)], w, h))
                    return false;
            }
        }
        const int idx = focus_.row * kGridCols() + focus_.col;
        if (idx >= 0 && idx < static_cast<int>(grid_.size()))
            return cachedArt(server_, grid_[static_cast<std::size_t>(idx)]);
        return true;
    }
    if (view_ == ViewId::Detail) {
        // A grid or hub thumb may already be on screen. Paint-ready waits for the
        // detail-sized poster, not that upscaled placeholder.
        const int dw = P(150), dh = P(225);
        if (!detail_.thumb.empty() && !cachedThumb(server_, detail_, dw, dh))
            return false;
        return cachedArt(server_, detail_);
    }
    if (view_ == ViewId::SeasonList) {
        const GridLayout gl = seasonGridLayout();
        const int visibleRows = gridVisibleRowCount(gl);
        for (int row = seasonScrollRow_; row < seasonScrollRow_ + visibleRows; ++row) {
            for (int col = 0; col < gl.cols; ++col) {
                const int idx = row * gl.cols + col;
                if (idx < 0 || idx >= static_cast<int>(seasons_.size()))
                    continue;
                if (!cachedThumb(server_, seasons_[static_cast<std::size_t>(idx)], gl.posterW,
                                 gl.posterH))
                    return false;
            }
        }
        return true;
    }
    if (view_ == ViewId::EpisodeList) {
        const int visible = episodeVisibleRows();
        const int tw = P(72);
        const int th = P(40);
        for (int i = episodeScrollRow_; i < episodeScrollRow_ + visible; ++i) {
            if (i < 0 || i >= static_cast<int>(episodes_.size()))
                continue;
            if (!cachedThumb(server_, episodes_[static_cast<std::size_t>(i)], tw, th))
                return false;
        }
        return true;
    }
    return true;
}

bool PlexUiController::shotFocusSection(const std::string& needle) {
    std::lock_guard<std::mutex> lk(mu_);
    const int idx = findSectionIndex(needle);
    if (idx < 0)
        return false;
    focus_.sidebar = idx + 1;
    syncSidebarScroll();
    return true;
}

void PlexUiController::run() {
    for (;;) {
        Job j{};
        bool haveJob = false;
        bool anim = false;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            qCv_.wait(lk, [this] { return stopping_ || !q_.empty() || animTick_; });
            if (stopping_ && q_.empty())
                return;
            if (!q_.empty()) {
                j = q_.front();
                q_.pop_front();
                busy_ = true;
                haveJob = true;
            } else if (animTick_) {
                animTick_ = false;
                anim = true;
            }
        }
        if (anim && !haveJob) {
            if (hooks_.repaint)
                hooks_.repaint();
            continue;
        }
        handle(j);
        if (navTraceOpen_.load())
            activeNav_.stateMs = perfSteadyMs();
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
        // Focus ease-out: one frame per flip ACK (see onR1FlipAck).
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (focusAnim_ < 3) {
                animWaitAck_.store(true);
                dirty_.store(true);
            }
        }
        if (animWaitAck_.load() && hooks_.repaint)
            hooks_.repaint();
    }
}

void PlexUiController::handle(const Job& j) {
    const BrowseCancelFn cancel = cancelFor(j.gen);

    if (j.queueDelta != 0) {
        if (j.upNextToken != 0 && j.upNextToken != upNextToken_.load())
            return;
        stepActiveQueue(j.queueDelta);
        return;
    }

    if (j.markOp != 0) {
        MarkWatchedOp op = MarkWatchedOp::None;
        if (j.markOp == static_cast<int>(MarkWatchedOp::Watched))
            op = MarkWatchedOp::Watched;
        else if (j.markOp == static_cast<int>(MarkWatchedOp::Unwatched))
            op = MarkWatchedOp::Unwatched;
        else if (j.markOp == static_cast<int>(MarkWatchedOp::DropContinue))
            op = MarkWatchedOp::DropContinue;
        applyMark(op, cancel);
        return;
    }

    if (j.op == BrowseOp::ToggleUiSize) {
#ifdef MUT_GH46_NO_TOGGLE
        return;
#endif
        const UiSize next = uiSafePrefs().size == UiSize::Large ? UiSize::Normal : UiSize::Large;
        const int pct = uiSafePrefs().marginPct;
        applyUiSafeLayout(pct, next);
        const char* name = uiSizeName(next);
        {
            std::lock_guard<std::mutex> lk(mu_);
            status_ = std::string("UI size: ") + name;
            dirty_.store(true);
        }
        if (cfg_.persistUiSize) {
            if (!cfg_.persistUiSize(name) && hooks_.log)
                hooks_.log("plexui: UI_SIZE persist failed");
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "plexui: UI_SIZE=%s", name);
        logLine(buf);
        return;
    }

    if (j.op == BrowseOp::Open) {
        // D-pad / Back / Select after Home bumps gen_ and would abort
        // fetch+catalog, leaving an empty Home until the next Open. Finish
        // this load unless the worker is stopping.
        const BrowseCancelFn catalogCancel = [this] { return stopping_.load(); };
        std::string accountToken;
        std::string homeToken;
        {
            std::lock_guard<std::mutex> lk(mu_);
            clearActiveQueueLocked();
            cancelUpNextLocked();
            accountToken = cfg_.accountToken;
            homeToken = cfg_.homeUserToken;
        }
        // A missing plex.tv token no longer forces PIN sign-in. Local / conf
        // seeds still open the library; account link is a separate sidebar path.
        // rc2.1: Browse/home opens the UI and stops a running play first, as
        // BrowseController::open does. Before this, Open during a play only set visible_
        // (the video kept the screen, the UI never drew) and later Backs walked it hidden.
        if (hooks_.playing && hooks_.playing() && hooks_.stopPlayback) {
            logLine("plexui: stopping playback to open the UI");
#ifndef MUT_PLEXUI_OPEN_KEEPS_PLAY
            hooks_.stopPlayback();
#endif
        }
        bool browseFromHidden = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            browseFromHidden = !visible_;
            visible_ = true;
            // GH#23: default view is ServerList. Opening with no catalog yet
            // must not paint the empty logo list (first Home after restart).
            // F2151: Back from that empty Home is idle Ready, not ServerList.
            if (!haveServer_ && view_ != ViewId::LinkSignIn) {
                view_ = ViewId::Home;
                focus_ = {};
                focus_.sidebar = 0;
                status_ = "LOADING...";
            }
            // Send Home (BrowseOp::Open) lands on Home even when the UI is
            // already showing a deeper view. Hidden or reopen_ still reset.
            if (haveServer_ && (browseFromHidden || view_ != ViewId::Home || reopen_)) {
                // Coming from a deeper view (library grid, detail, ...) the old focus
                // col/row index that view, not a hub tile. Left in place, one LEFT only
                // walked the stale column back and DOWN scrolled hub rows, so the
                // sidebar looked frozen after a play ended and F1 re-showed Home.
                if (view_ != ViewId::Home)
                    focus_ = {};
                view_ = ViewId::Home;
                focus_.sidebar = 0;
                status_.clear();
                gridSectionIndex_ = -1;
                gridSectionKey_.clear();
                gridChips_.clear();
                gridActiveChip_ = 0;
                gridChipScrollX_ = 0;
                filter43On_ = false;
                gridFull_.clear();
                focus_.gridOnChips = false;
                focus_.gridChip = 0;
                reopen_ = false;
                if (!browseFromHidden) {
                    // Already on screen: an empty Home still has to leave the
                    // yellow pill, or Down keeps walking a row that is not there.
                    focusEmptyHomeSidebarLocked();
                    ensureHomeUserRowLocked();
                    dirty_.store(true);
                    // A second Open while catalog is still LOADING used to return
                    // here and leave HDMI on an empty Home pill.
                    if (!hubs_.empty() && status_ != "LOADING...")
                        return;
                }
                dirty_.store(true);
            }
        }
        if (servers_.empty()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "LOADING...";
                if (view_ == ViewId::LinkSignIn)
                    view_ = ViewId::ServerList;
                else {
                    // GH#23: first Home after restart used to paint an empty
                    // ServerList (logo only) until plex.tv published rows.
                    view_ = ViewId::Home;
                    focus_ = {};
                    focus_.sidebar = 0;
                }
            }
            dirty_.store(true);
            std::vector<PlexServer> seeds = cfg_.seedServers;
            if (cfg_.discoverLocal) {
                const auto found = cfg_.discoverLocal();
                seeds = mergePlexServerLists(seeds, found);
            }
            std::string fallbackTok = accountToken;
            if (fallbackTok.empty()) {
                for (const auto& s : cfg_.seedServers) {
                    if (!s.accessToken.empty()) {
                        fallbackTok = s.accessToken;
                        break;
                    }
                }
            }
            // #328: the conf token is never handed to GDM-discovered hosts.
            if (!fallbackTok.empty() && !accountToken.empty()) {
                for (auto& s : seeds) {
                    if (s.accessToken.empty())
                        s.accessToken = fallbackTok;
                }
            }
            // 0011: the list fills in as servers resolve; a dead one turns "Offline" instead of blocking.
            bool resourcesTls = false;
            // Empty home token: libraryToken is the account token and the seed
            // list below is the tip path. A saved profile uses its own token.
            const std::string libraryToken = activeLibraryToken(accountToken, homeToken);
            std::vector<PlexServer> accountSeeds;
            if (libraryToken != accountToken) {
                accountSeeds = seeds;
                for (auto& s : seeds) {
                    if (s.accessToken.empty() || s.accessToken == accountToken)
                        s.accessToken = libraryToken;
                }
            }
            auto publishServers = [this](const std::vector<PlexServer>& snap) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    servers_ = snap;
                    // GH#23 first paint is ViewId::ServerList until the catalog
                    // load. Rotate Native to index 0 here so kube is not gold.
                    const int hi = pickHomeServerIndex(servers_);
                    rotatePickedServerToFront(servers_, hi, nullptr, nullptr);
                    ++dataGeneration_;
                    dirty_.store(true);
                }
                if (hooks_.repaint)
                    hooks_.repaint();
            };
            int resourcesStatus = 0;
            auto list = fetchPlexServersProgressive(
                hooks_.get, libraryToken, catalogCancel, publishServers, &resourcesTls, seeds,
                &resourcesStatus);
            if (catalogCancel())
                return;
            if (resourcesStatus == 401 && libraryToken != accountToken) {
                logLine("plexui: home profile rejected");
                HomeSession cleared;
                if (cfg_.persistHomeSession && !cfg_.persistHomeSession(cleared))
                    logLine("plexui: home profile was not cleared");
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    cfg_.homeUserId.clear();
                    cfg_.homeUserTitle.clear();
                    cfg_.homeUserToken.clear();
                }
                resourcesTls = false;
                resourcesStatus = 0;
                list = fetchPlexServersProgressive(
                    hooks_.get, accountToken, catalogCancel, publishServers, &resourcesTls, accountSeeds,
                    &resourcesStatus);
                if (catalogCancel())
                    return;
            }
            std::lock_guard<std::mutex> lk(mu_);
            servers_ = std::move(list);
            {
                const int hi = pickHomeServerIndex(servers_);
                rotatePickedServerToFront(servers_, hi, nullptr, nullptr);
            }
            ++dataGeneration_;
            if (servers_.empty()) {
                status_ = resourcesTls ? "Can't reach plex.tv securely" : "NO SERVERS";
                dirty_.store(true);
            } else {
                status_ = "LOADING...";
                dirty_.store(true);
            }
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            visible_ = true;
            ensureHomeUserRowLocked();
            dirty_.store(true);
        }
        bool needCatalog = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            needCatalog = !servers_.empty() && sidebarEntries_.empty();
            char sb[160];
            std::snprintf(sb, sizeof(sb),
                          "plexui: show view=%s sidebar_entries=%zu servers=%zu hubs=%zu "
                          "focus sidebar=%d col=%d row=%d reload=%d",
                          viewIdName(view_), sidebarEntries_.size(), servers_.size(),
                          hubs_.size(), focus_.sidebar, focus_.col, focus_.row,
                          needCatalog ? 1 : 0);
            logLine(sb);
        }
        if (needCatalog) {
            if (loadStartupCatalog(catalogCancel) && !catalogCancel()) {
                if (loadHomeHubs(catalogCancel)) {
                    dirty_.store(true);
                } else if (!catalogCancel()) {
                    std::lock_guard<std::mutex> lk(mu_);
                    status_ = lastLoadErr_.empty() ? "LOAD FAILED" : lastLoadErr_;
                    dirty_.store(true);
                }
            } else if (!catalogCancel()) {
                std::lock_guard<std::mutex> lk(mu_);
                if (status_ == "LOADING...")
                    status_ = lastLoadErr_.empty() ? "LOAD FAILED" : lastLoadErr_;
                dirty_.store(true);
            }
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            if (haveServer_ && hubs_.empty() && view_ != ViewId::ServerList) {
                status_ = "LOADING...";
                dirty_.store(true);
            }
        }
        bool needHubs = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            needHubs = haveServer_ && hubs_.empty() && !servers_.empty();
        }
        if (needHubs && !catalogCancel()) {
            if (!loadHomeHubs(catalogCancel) && !catalogCancel()) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = lastLoadErr_.empty() ? "LOAD FAILED" : lastLoadErr_;
                dirty_.store(true);
            }
        }
        // Hubs still empty after the show reload: focus the first server or
        // library so Down walks the sidebar without a Left first. Populated
        // hub rows stay on content focus.
        if (!cancel()) {
            std::lock_guard<std::mutex> lk(mu_);
            const int before = focus_.sidebar;
            focusEmptyHomeSidebarLocked();
            if (focus_.sidebar != before)
                dirty_.store(true);
        }
    }

    if (j.op == BrowseOp::Back) {
        bool stopPlay = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (view_ == ViewId::ServerList && serverPickerFromHome_) {
                serverPickerFromHome_ = false;
                view_ = ViewId::Home;
                focus_ = {};
                focus_.sidebar = 0;
                status_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            }
            const bool playingHidden = !visible_ && ownsSession_;
            if (playingHidden && (view_ == ViewId::EpisodeList || view_ == ViewId::UpNext)) {
                cancelUpNextLocked();
                view_ = ViewId::EpisodeList;
                visible_ = true;
                stopPlay = true;
                bumpAnim();
                dirty_.store(true);
            } else if (view_ == ViewId::UpNext) {
                cancelUpNextLocked();
                view_ = ViewId::EpisodeList;
                visible_ = true;
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::EpisodeList) {
                view_ = ViewId::SeasonList;
                episodes_.clear();
                touchListsLocked();
                listTitle_ = detail_.title.empty() ? "Seasons" : detail_.title;
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::SeasonList) {
                view_ = ViewId::Detail;
                seasons_.clear();
                episodes_.clear();
                touchListsLocked();
                listTitle_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::LinkSignIn) {
                linkGen_.fetch_add(1);
                linkWaitCv_.notify_all();
                linkCode_.clear();
                linkSeconds_ = 0;
                if (linkReturnVisible_) {
                    view_ = linkReturn_;
                    visible_ = true;
                } else {
                    visible_ = false;
                }
                status_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::HomePin) {
                view_ = ViewId::HomeUsers;
                homePinSlot_ = 0;
                status_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::HomeUsers) {
                view_ = ViewId::Home;
                focus_ = {};
                focus_.sidebar = 0;
                status_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::SubtitlePicker || view_ == ViewId::AudioPicker) {
                view_ = ViewId::Detail;
                bumpAnim();
                dirty_.store(true);
                return;
            } else if (view_ == ViewId::Detail) {
                view_ = gridSectionIndex_ >= 0 ? ViewId::LibraryGrid : ViewId::Home;
                seasons_.clear();
                episodes_.clear();
                touchListsLocked();
                clearActiveQueueLocked();
                themeKickedKey_.clear();
                bumpAnim();
                dirty_.store(true);
                return;
            }
        }
        if (stopPlay) {
            if (hooks_.stopPlayback)
                hooks_.stopPlayback();
            std::lock_guard<std::mutex> lk(mu_);
            ownsSession_ = false;
            dirty_.store(true);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
        if (view_ == ViewId::LibraryGrid) {
            if (focus_.gridOnLetters) {
                focus_.gridOnLetters = false;
                if (!gridChips_.empty()) {
                    focus_.gridOnChips = true;
                    focus_.gridChip = gridActiveChip_;
                    syncChipScroll();
                }
                bumpAnim();
                dirty_.store(true);
                return;
            }
            if (focus_.sidebar != 0) {
                focus_.sidebar = 0;
                syncSidebarScroll();
                bumpAnim();
                dirty_.store(true);
                return;
            }
            view_ = ViewId::Home;
            gridSectionIndex_ = -1;
            gridSectionKey_.clear();
            gridChips_.clear();
            gridActiveChip_ = 0;
            gridChipScrollX_ = 0;
            filter43On_ = false;
            gridFull_.clear();
            focus_.gridOnChips = false;
            focus_.gridChip = 0;
            clearGridPageLocked();
            clearActiveQueueLocked();
            bumpAnim();
            dirty_.store(true);
            return;
        }
        // Top of the UI (home, or the server list while it is still loading) leaves
        // the cast-ready idle. A server list is not the back target from home.
        if (isHomeSurface()) {
            visible_ = false;
            dirty_.store(true);
            return;
        }
        visible_ = false;
        dirty_.store(true);
        return;
        }
    }

    if (j.op == BrowseOp::StopAndReopen) {
        ViewId back = ViewId::Home;
        {
            std::lock_guard<std::mutex> lk(mu_);
            back = viewBeforePlay_;
        }
        // D2: Open resets the grid section/chips when it is not on Home. Keep them across the
        // reopen, or the later ESC from the detail has no library to return to and lands on Home.
        struct GridKeep {
            int idx; std::string key; std::vector<GridCategoryChip> chips; int active; int chipX;
            bool onChips; int chip;
        } keep{};
        {
            std::lock_guard<std::mutex> lk(mu_);
            keep = {gridSectionIndex_, gridSectionKey_, gridChips_, gridActiveChip_,
                    gridChipScrollX_, focus_.gridOnChips, focus_.gridChip};
        }
        if (hooks_.stopPlayback)
            hooks_.stopPlayback();
        handle({BrowseOp::Open, j.gen});
        if (back == ViewId::Detail || back == ViewId::LibraryGrid) {
            std::lock_guard<std::mutex> lk(mu_);
#ifndef MUT_PLEXUI_D2_DROP_GRID
            gridSectionIndex_ = keep.idx;
            gridSectionKey_ = keep.key;
            gridChips_ = keep.chips;
            gridActiveChip_ = keep.active;
            gridChipScrollX_ = keep.chipX;
            focus_.gridOnChips = keep.onChips;
            focus_.gridChip = keep.chip;
#endif
            view_ = back;
            visible_ = true;
            dirty_.store(true);
        }
        return;
    }

    if (j.op == BrowseOp::Left || j.op == BrowseOp::Right) {
        const int delta = j.op == BrowseOp::Right ? 1 : -1;
        std::lock_guard<std::mutex> lk(mu_);
        applyHorizontal(delta);
        if (view_ == ViewId::Home ||
            (view_ == ViewId::LibraryGrid && focus_.sidebar != 0))
            logSidebarFocus(j.op == BrowseOp::Left ? "left" : "right");
        return;
    }

    if (j.op == BrowseOp::Up || j.op == BrowseOp::Down || j.op == BrowseOp::PageUp ||
        j.op == BrowseOp::PageDown) {
        const int dir = (j.op == BrowseOp::Down || j.op == BrowseOp::PageDown) ? 1 : -1;
        std::unique_lock<std::mutex> lk(mu_);
        if (!visible_)
            return;
        bool wantPage = false;
        int pageStart = 0;
        int pageGen = 0;
        int pageOldMaxRow = 0;
        PlexServer pageSrv;
        std::string pageKey;
        std::string pageGenre;
        char pageLetter = 0;
        if (view_ == ViewId::ServerList) {
            focus_.row = std::max(0, std::min(static_cast<int>(servers_.size()) - 1,
                                                focus_.row + dir));
        } else if (view_ == ViewId::Home) {
            if (focus_.sidebar != 0) {
                // Sidebar order: Home (kSidebarHomeFocus), entry 1..N. 0 is content
                // focus and is never a sidebar stop, so step over it.
                int n = focus_.sidebar + dir;
                if (n == 0)
                    n = dir > 0 ? 1 : kSidebarHomeFocus;
                focus_.sidebar = std::max(
                    kSidebarHomeFocus, std::min(static_cast<int>(sidebarEntries_.size()), n));
                if (focus_.sidebar == 0) // no entries: DOWN from Home stays on Home
                    focus_.sidebar = kSidebarHomeFocus;
            } else if (!homeContentNavigableLocked()) {
                // No hub tiles: DOWN/UP must not bump an invisible content row.
                // That left the Home pill yellow and the pad looking dead (soak
                // #149, hubs=0). DOWN enters the first server or library.
                if (dir > 0) {
                    const int nav = firstSidebarNavFocusLocked();
                    if (nav > 0) {
                        focus_.sidebar = nav;
                        focus_.row = 0;
                        focus_.col = 0;
                    }
                } else if (!sidebarEntries_.empty()) {
                    focus_.sidebar = kSidebarHomeFocus;
                    focus_.row = 0;
                    focus_.col = 0;
                }
            } else {
#ifndef MUT_HOME_ROW_UNCLAMPED
                // tipA-local: DOWN past the last hub row used to keep counting (row 7 of 3), so the
                // pad looked dead ("sidebar event=down focus=0" forever) and UP needed as many presses.
                focus_.row = std::max(0, std::min(static_cast<int>(hubs_.size()) - 1, focus_.row + dir));
#else
                focus_.row = std::max(0, focus_.row + dir);
#endif
#ifndef MUT_HOME_COL_STALE
                // #206 glass: the column carried over from a longer row (col 5 into a 2-tile row)
                // drew no ring and A found no item. Clamp it to the new row.
                if (focus_.row < static_cast<int>(hubs_.size())) {
                    const int maxCol =
                        static_cast<int>(hubs_[static_cast<std::size_t>(focus_.row)].items.size()) - 1;
                    focus_.col = std::max(0, std::min(maxCol, focus_.col));
                }
#endif
            }
            syncSidebarScroll();
            logSidebarFocus(dir > 0 ? "down" : "up");
        } else if (view_ == ViewId::LibraryGrid) {
            if (focus_.sidebar != 0) {
                int n = focus_.sidebar + dir;
                if (n == 0)
                    n = dir > 0 ? 1 : kSidebarHomeFocus;
                focus_.sidebar = std::max(
                    kSidebarHomeFocus, std::min(static_cast<int>(sidebarEntries_.size()), n));
                if (focus_.sidebar == 0)
                    focus_.sidebar = kSidebarHomeFocus;
                syncSidebarScroll();
                logSidebarFocus(dir > 0 ? "down" : "up");
            } else if (focus_.gridOnLetters) {
                if (dir > 0) {
                    focus_.gridOnLetters = false;
                    if (!gridChips_.empty()) {
                        focus_.gridOnChips = true;
                        focus_.gridChip = gridActiveChip_;
                        syncChipScroll();
                    }
                }
            } else if (focus_.gridOnChips) {
                if (dir > 0) {
                    focus_.gridOnChips = false;
                    focus_.row = 0;
                } else if (azJumpEnterFromChips()) {
                    focus_.gridOnChips = false;
                    focus_.gridOnLetters = true;
                    focus_.gridLetter = azJumpSlotForLetter(gridLetterFilter_);
                }
            } else if (dir < 0 && focus_.row == 0) {
                focus_.gridOnChips = true;
                focus_.gridChip = gridActiveChip_;
                syncChipScroll();
            } else {
                // Flag off: the row step is the tip path (it is not clamped to the
                // loaded titles). Flag on: moving past the last loaded row fetches
                // the next page instead of walking into empty rows.
#ifdef MUT_GRID_PAGE_OFF_STILL_PAGES
                const bool pageOn = true;
#else
                const bool pageOn = gridPageEnabled();
#endif
                if (!pageOn) {
                    focus_.row = std::max(0, focus_.row + dir);
                } else {
                    const GridLayout gl = computeGridLayout(!gridChips_.empty());
                    const int cols = std::max(1, gl.cols);
                    const int shown = static_cast<int>(grid_.size());
                    // FILTER_4_3 on: the next PMS page starts after the fetched
                    // rows, not after the shorter 4:3 view. Flag off: loaded == shown.
                    const int loaded = libraryLoadedCountLocked();
                    const int maxRow = shown > 0 ? (shown - 1) / cols : 0;
                    const int next = focus_.row + dir;
                    bool pageable = true;
                    std::string genreId;
                    if (!gridChips_.empty() && gridActiveChip_ >= 0 &&
                        gridActiveChip_ < static_cast<int>(gridChips_.size())) {
                        const auto& chip = gridChips_[static_cast<std::size_t>(gridActiveChip_)];
                        if (chip.kind == GridCategoryChip::Kind::Genre)
                            genreId = chip.filterId;
                        else if (chip.kind == GridCategoryChip::Kind::Collection)
                            pageable = false;
                        else if (chip.kind == GridCategoryChip::Kind::Aspect43)
                            pageable = false;
                    }
                    const bool more =
                        pageable && gridMoreLoaded(loaded, gridTotalSize_, gridPageFull_);
                    if (dir > 0 && next > maxRow && more && !gridPageBusy_ && loaded > 0) {
                        wantPage = true;
                        pageStart = loaded;
                        pageGen = gridPageGen_;
                        pageOldMaxRow = maxRow;
                        pageSrv = server_;
                        pageKey = gridSectionKey_;
                        pageGenre = std::move(genreId);
                        pageLetter = azJumpEnabled() ? gridLetterFilter_ : 0;
                        gridPageBusy_ = true;
                        focus_.row = maxRow;
                    } else {
                        int row = next;
                        if (row < 0)
                            row = 0;
                        if (shown > 0 && row > maxRow)
                            row = maxRow;
                        focus_.row = row;
                    }
                }
                syncGridScroll();
            }
        } else if (view_ == ViewId::Detail) {
            const int maxBtn = std::max(0, detailButtonCount(detail_) - 1);
            focus_.detailButton = std::max(0, std::min(maxBtn, focus_.detailButton + dir));
        } else if (view_ == ViewId::SubtitlePicker) {
            const int n = static_cast<int>(subtitlePickerRows(detail_).size());
            if (n > 0)
                focus_.row = std::max(0, std::min(n - 1, focus_.row + dir));
        } else if (view_ == ViewId::AudioPicker) {
            const int n = static_cast<int>(audioPickerRows(detail_).size());
            if (n > 0)
                focus_.row = std::max(0, std::min(n - 1, focus_.row + dir));
        } else if (view_ == ViewId::SeasonList) {
            const GridLayout gl = seasonGridLayout();
            const int cols = std::max(1, gl.cols);
            const int n = static_cast<int>(seasons_.size());
            if (n > 0) {
                int idx = focus_.row * cols + focus_.col + dir * cols;
                idx = std::max(0, std::min(n - 1, idx));
                focus_.row = idx / cols;
                focus_.col = idx % cols;
                syncSeasonScroll();
            }
        } else if (view_ == ViewId::EpisodeList) {
            const int n = static_cast<int>(episodes_.size());
            if (n > 0) {
                const int step = (j.op == BrowseOp::PageUp || j.op == BrowseOp::PageDown)
                                     ? episodeVisibleRows()
                                     : 1;
                focus_.row = std::max(0, std::min(n - 1, focus_.row + dir * step));
                syncEpisodeScroll();
            }
        } else if (view_ == ViewId::UpNext) {
            focus_.detailButton = std::max(0, std::min(1, focus_.detailButton + dir));
        } else if (view_ == ViewId::HomeUsers) {
            if (!homeUsers_.empty())
                focus_.row = std::max(0, std::min(static_cast<int>(homeUsers_.size()) - 1,
                                                  focus_.row + dir));
        } else if (view_ == ViewId::HomePin) {
            if (homePinSlot_ < 4 && homePin_.size() == 4) {
                int d = homePin_[static_cast<size_t>(homePinSlot_)] - '0';
                d = (d + (dir > 0 ? 1 : -1) + 10) % 10;
                homePin_[static_cast<size_t>(homePinSlot_)] = static_cast<char>('0' + d);
            }
        }
        if (wantPage) {
            const BrowseCancelFn pageCancel = cancelFor(j.gen);
            PlexServer srv = std::move(pageSrv);
            std::string key = std::move(pageKey);
            std::string genre = std::move(pageGenre);
            const int start = pageStart;
            const int gen = pageGen;
            const int oldMax = pageOldMaxRow;
            lk.unlock();
            fetchAndAppendGridPage(std::move(srv), std::move(key), std::move(genre), start, gen,
                                   oldMax, pageCancel, pageLetter);
            lk.lock();
        }
        bumpAnim();
        dirty_.store(true);
        return;
    }

    if (j.op == BrowseOp::Info || j.queueMark != 0) {
        const QueueMark mark = j.queueMark == 2 ? QueueMark::PlayNext : QueueMark::Append;
        applyQueueMark(mark, cancel);
        return;
    }

    if (j.op == BrowseOp::Select) {
        PlexServer pickServer{};
        bool pick = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!visible_)
                return;
            const int want = std::max(0, std::min(static_cast<int>(servers_.size()) - 1, focus_.row));
            if (view_ == ViewId::ServerList && !servers_.empty() &&
                (servers_[static_cast<std::size_t>(want)].offline ||
                 servers_[static_cast<std::size_t>(want)].baseUrl.empty())) {
                status_ = "SERVER OFFLINE";   // 0011: an offline / unresolved server is not picked
                dirty_.store(true);
            } else if (view_ == ViewId::ServerList && !servers_.empty()) {
                serverIndex_ = want;
                assignServerLocked(servers_[static_cast<std::size_t>(serverIndex_)]);
                haveServer_ = true;
                serverPickerFromHome_ = false;
                view_ = ViewId::Home;
                status_ = "LOADING...";
                focus_ = {};
                pickServer = server_;
                pick = true;
                dirty_.store(true);
            }
        }
        if (pick) {
            std::vector<PlexSection> sec;
            std::vector<PlexHubRow> hubs;
            std::string err;
            if (fetchServerHome(hooks_.get, pickServer, cancel, sec, hubs, err)) {
                std::lock_guard<std::mutex> lk(mu_);
                sections_ = std::move(sec);
                filterEmptyHomeHubs(hubs, hideEmptyHubsEnabled());   // #220, default on
                hubs_ = std::move(hubs);
                ++dataGeneration_;
                status_.clear();
                dirty_.store(true);
            } else if (!cancel()) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = err.empty() ? "LOAD FAILED" : err;
                dirty_.store(true);
            }
            return;
        }

        std::unique_lock<std::mutex> lk(mu_);
        if (!visible_)
            return;

        if (view_ == ViewId::Home ||
            (view_ == ViewId::LibraryGrid && focus_.sidebar != 0)) {
            if (focus_.sidebar == kSidebarHomeFocus) {
                // SELECT on Home: already showing Home, so hand focus back to the
                // content rather than opening whatever hub tile is under the ring.
                // From the library drawer, Home leaves the grid.
                if (view_ == ViewId::LibraryGrid) {
                    view_ = ViewId::Home;
                    gridSectionIndex_ = -1;
                    gridSectionKey_.clear();
                    gridChips_.clear();
                    gridActiveChip_ = 0;
                    gridChipScrollX_ = 0;
                    filter43On_ = false;
                    gridFull_.clear();
                    focus_.gridOnChips = false;
                    focus_.gridChip = 0;
                    clearActiveQueueLocked();
                }
                focus_.sidebar = 0;
                syncSidebarScroll();
                bumpAnim();
                dirty_.store(true);
                return;
            }
            if (focus_.sidebar > 0) {
                const int si = focus_.sidebar - 1;
                const auto& ent = sidebarEntries_[static_cast<std::size_t>(si)];
                if (ent.kind == SidebarRowKind::ServerPicker || ent.type == "servers") {
                    serverPickerFromHome_ = true;
                    view_ = ViewId::ServerList;
                    focus_ = {};
                    if (!servers_.empty())
                        focus_.row = std::max(0, std::min(serverIndex_,
                                                          static_cast<int>(servers_.size()) - 1));
                    status_.clear();
                    logSidebarFocus("servers");
                    bumpAnim();
                    dirty_.store(true);
                    return;
                }
                if (ent.type == "signin" || ent.kind == SidebarRowKind::SignIn) {
                    lk.unlock();
                    startLinkSignIn(ViewId::Home, true);
                    return;
                }
                if (ent.type == "homeuser" || ent.kind == SidebarRowKind::HomeUser) {
                    lk.unlock();
                    openHomeUsers();
                    return;
                }
                if (ent.kind == SidebarRowKind::Server || ent.type == "server") {
                    if (ent.serverIndex < 0 ||
                        ent.serverIndex >= static_cast<int>(servers_.size()))
                        return;
                    serverIndex_ = ent.serverIndex;
                    assignServerLocked(servers_[static_cast<std::size_t>(serverIndex_)]);
                    haveServer_ = true;
                    view_ = ViewId::Home;
                    status_ = "LOADING...";
                    logSidebarFocus("select-server");
                    dirty_.store(true);
                    lk.unlock();
                    if (!loadHomeHubs(cancelFor(j.gen)) && !cancelFor(j.gen)()) {
                        std::lock_guard<std::mutex> lk2(mu_);
                        status_ = "LOAD FAILED";
                        dirty_.store(true);
                    }
                    return;
                }
                const std::string key = ent.sectionKey;
                PlexServer srv = servers_[static_cast<std::size_t>(ent.serverIndex)];
                serverIndex_ = ent.serverIndex;
                haveServer_ = true;
                gridSectionIndex_ = si;
                gridSectionTitle_ = ent.title;
                gridSectionKey_ = key;
                assignServerLocked(srv);
                logSidebarFocus("select");
                grid_.clear();
                gridFull_.clear();
                filter43On_ = false;
                gridLetterFilter_ = 0;
                clearGridPageLocked();
                status_ = "LOADING...";
                navDeferVisible_.store(true);
                dirty_.store(true);
                lk.unlock();
                // XML is parsed in the fetch helpers, with mu_ released, then published below.
                // The three calls share one deadline. A connect failure returns unreachable
                // and the helpers stop the ladder; an HTTP error from a live server does not.
                const auto openDeadline = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(kLibraryOpenBudgetMs);
                // By value: an api-cache refresh keeps this lambda after handle() returns.
                auto budgetedGet = [this, openDeadline](const std::string& url, int connectMs,
                                                        int timeoutMs, const BrowseCancelFn& c) {
                    const int left = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            openDeadline - std::chrono::steady_clock::now())
                            .count());
                    if (left <= 0) {
                        BrowseHttpResult r;
                        r.status = 0;
                        r.timedOut = true;
                        return r;
                    }
                    const int total = std::min(timeoutMs, left);
                    const int connect = std::min(std::max(connectMs, 1), total);
                    return hooks_.get(url, connect, std::max(total, 1), c);
                };
                std::vector<GridCategoryChip> collections;
                std::vector<GridCategoryChip> genres;
                std::vector<PlexItem> items;
                std::string err;
                auto stopLoading = [this] {
                    navDeferVisible_.store(false);
                    std::lock_guard<std::mutex> lk2(mu_);
                    if (status_ == "LOADING...") {
                        status_.clear();
                        dirty_.store(true);
                    }
                };
                auto publishUnreachable = [&] {
                    if (cancel()) {
                        stopLoading();
                        return;
                    }
                    std::lock_guard<std::mutex> lk2(mu_);
                    status_ = err.empty() ? "server unreachable" : err;
                    dirty_.store(true);
                    navDeferVisible_.store(false);
                };
                if (!fetchSectionCollections(budgetedGet, srv, key, cancel, collections, err)) {
                    publishUnreachable();
                    return;
                }
                if (cancel()) {
                    stopLoading();
                    return;
                }
                if (!fetchSectionGenres(budgetedGet, srv, key, cancel, genres, err)) {
                    publishUnreachable();
                    return;
                }
                if (cancel()) {
                    stopLoading();
                    return;
                }
                const auto chips = buildGridChips(std::move(collections), std::move(genres));
                int pageTotal = -1;
                const bool paging = gridPageEnabled();
                const bool ok = fetchSectionGrid(budgetedGet, srv, key, cancel, items, err,
                                                 paging ? &pageTotal : nullptr);
                if (ok) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    gridChips_ = chips;
                    gridActiveChip_ = 0;
                    gridChipScrollX_ = 0;
                    filter43On_ = false;
                    const int fetched = static_cast<int>(items.size());
                    storeGridLocked(std::move(items));
                    rememberGridPageLocked(fetched, pageTotal, true);
                    ++dataGeneration_;
                    view_ = ViewId::LibraryGrid;
                    focus_ = {};
                    gridScrollRow_ = 0;
                    status_.clear();
                    markGridArtLoadStart();
                    dirty_.store(true);
                    navDeferVisible_.store(false);
                } else if (!cancel()) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    status_ = err;
                    dirty_.store(true);
                    navDeferVisible_.store(false);
                } else {
                    stopLoading();
                }
                return;
            }
            if (view_ == ViewId::LibraryGrid)
                return;
            if (!hubs_.empty() && focus_.row < static_cast<int>(hubs_.size())) {
                const auto& hub = hubs_[static_cast<std::size_t>(focus_.row)];
                if (focus_.col < static_cast<int>(hub.items.size())) {
                    PlexItem pick = hub.items[static_cast<std::size_t>(focus_.col)];
                    PlexServer srv = serverForHubItem(pick);
                    const std::string rk = pick.ratingKey;
                    lk.unlock();
                    PlexItem full;
                    std::string err;
                    if (!rk.empty() &&
                        fetchMetadata(hooks_.get, srv, rk, cancelFor(j.gen), full, err))
                        pick = std::move(full);
                    lk.lock();
                    assignServerLocked(srv);
                    for (size_t i = 0; i < servers_.size(); ++i) {
                        if (servers_[i].machineIdentifier == srv.machineIdentifier) {
                            serverIndex_ = static_cast<int>(i);
                            break;
                        }
                    }
                    detail_ = std::move(pick);
                    pickedSubtitleId_ = -1;
                    pickedAudioId_ = -1;
                    view_ = ViewId::Detail;
                    focus_.detailButton = 0;
                    dirty_.store(true);
                    PlexServer srv2 = server_;
                    PlexItem it2 = detail_;
                    lk.unlock();
                    kickShowTheme(srv2, it2, cancelFor(j.gen));
                }
            }
            return;
        }

        if (view_ == ViewId::LibraryGrid) {
            if (focus_.gridOnChips) {
                const int idx = focus_.gridChip;
                if (idx < 0 || idx >= static_cast<int>(gridChips_.size()))
                    return;
                const GridCategoryChip chip = gridChips_[static_cast<std::size_t>(idx)];
                if (chip.kind == GridCategoryChip::Kind::Aspect43) {
                    // Local toggle. The fetched page stays in gridFull_. No PMS query.
                    if (!filter43Enabled())
                        return;
                    if (gridFull_.empty())
                        gridFull_ = grid_;
                    filter43On_ = !filter43On_;
                    grid_ = libraryGridFiltered(gridFull_, filter43On_);
                    focus_.row = 0;
                    focus_.col = 0;
                    gridScrollRow_ = 0;
                    ++dataGeneration_;
                    status_.clear();
                    markGridArtLoadStart();
                    dirty_.store(true);
                    logLine(std::string("plexui: FILTER_4_3 ") + (filter43On_ ? "on" : "off") +
                            " shown=" + std::to_string(grid_.size()) +
                            " loaded=" + std::to_string(gridFull_.size()));
                    return;
                }
                if (idx == gridActiveChip_)
                    return;
                PlexServer srv = server_;
                const std::string sectionKey = gridSectionKey_;
                gridActiveChip_ = idx;
                status_ = "LOADING...";
                dirty_.store(true);
                lk.unlock();
                std::vector<PlexItem> items;
                std::string err;
                int pageTotal = -1;
                const bool paging = gridPageEnabled();
                const bool pageable = chip.kind != GridCategoryChip::Kind::Collection;
                if (loadGridForChip(chip, srv, sectionKey, cancel, items, err,
                                    paging ? &pageTotal : nullptr)) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    const int fetched = static_cast<int>(items.size());
                    storeGridLocked(std::move(items));
                    rememberGridPageLocked(fetched, pageTotal, pageable);
                    ++dataGeneration_;
                    focus_.row = 0;
                    focus_.col = 0;
                    gridScrollRow_ = 0;
                    gridLetterFilter_ = 0;
                    status_.clear();
                    markGridArtLoadStart();
                    dirty_.store(true);
                } else if (!cancel()) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    status_ = err;
                    dirty_.store(true);
                }
                return;
            }
            if (focus_.gridOnLetters) {
                if (!azJumpEnabled()) {
                    focus_.gridOnLetters = false;
                    bumpAnim();
                    dirty_.store(true);
                    return;
                }
                const char letter = azJumpSlotLetter(focus_.gridLetter);
                if (letter == gridLetterFilter_) {
                    focus_.gridOnLetters = false;
                    bumpAnim();
                    dirty_.store(true);
                    return;
                }
                PlexServer srv = server_;
                const std::string sectionKey = gridSectionKey_;
                GridCategoryChip chip;
                bool haveChip = false;
                if (!gridChips_.empty() && gridActiveChip_ >= 0 &&
                    gridActiveChip_ < static_cast<int>(gridChips_.size())) {
                    chip = gridChips_[static_cast<std::size_t>(gridActiveChip_)];
                    haveChip = true;
                }
                std::string genreId;
                const bool collection =
                    haveChip && chip.kind == GridCategoryChip::Kind::Collection;
                if (haveChip && chip.kind == GridCategoryChip::Kind::Genre)
                    genreId = chip.filterId;
                status_ = "LOADING...";
                clearGridPageLocked();
                dirty_.store(true);
                lk.unlock();
                std::vector<PlexItem> items;
                std::string err;
                int pageTotal = -1;
                const bool paging = gridPageEnabled();
                bool ok = false;
                bool pageable = true;
                if (sectionKey.empty()) {
                    err = "grid letter";
                } else if (letter == 0) {
                    if (!haveChip) {
                        chip.kind = GridCategoryChip::Kind::All;
                        chip.title = "All";
                    }
                    pageable = chip.kind != GridCategoryChip::Kind::Collection;
                    ok = loadGridForChip(chip, srv, sectionKey, cancel, items, err,
                                         paging ? &pageTotal : nullptr);
                } else {
                    pageable = true;
                    ok = fetchSectionGridPage(hooks_.get, srv, sectionKey, 0, genreId, cancel, items,
                                             paging ? &pageTotal : nullptr, err, letter);
                }
                if (ok) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    const int fetched = static_cast<int>(items.size());
                    storeGridLocked(std::move(items));
                    gridLetterFilter_ = letter;
                    if (letter != 0 && collection && !gridChips_.empty() &&
                        gridChips_.front().kind == GridCategoryChip::Kind::All)
                        gridActiveChip_ = 0;
                    rememberGridPageLocked(fetched, pageTotal, pageable);
                    ++dataGeneration_;
                    focus_.gridOnLetters = false;
                    focus_.gridOnChips = false;
                    focus_.row = 0;
                    focus_.col = 0;
                    gridScrollRow_ = 0;
                    status_.clear();
                    markGridArtLoadStart();
                    dirty_.store(true);
                } else if (!cancel()) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    focus_.gridOnLetters = true;
                    status_ = err.empty() ? "LOAD FAILED" : err;
                    dirty_.store(true);
                }
                return;
            }
            const int idx = focus_.row * kGridCols() + focus_.col;
            if (idx >= 0 && idx < static_cast<int>(grid_.size())) {
                PlexItem pick = grid_[static_cast<std::size_t>(idx)];
                PlexServer srv = server_;
                const std::string rk = pick.ratingKey;
                lk.unlock();
                PlexItem full;
                std::string err;
                if (!rk.empty() &&
                    fetchMetadata(hooks_.get, srv, rk, cancelFor(j.gen), full, err))
                    pick = std::move(full);
                lk.lock();
                detail_ = std::move(pick);
                pickedSubtitleId_ = -1;
                pickedAudioId_ = -1;
                view_ = ViewId::Detail;
                focus_.detailButton = 0;
                dirty_.store(true);
                PlexServer srv2 = server_;
                PlexItem it2 = detail_;
                lk.unlock();
                kickShowTheme(srv2, it2, cancelFor(j.gen));
            }
            return;
        }

        if (view_ == ViewId::LinkSignIn) {
            const bool retry = linkCode_.empty() || status_.find("FAILED") != std::string::npos;
            const ViewId ret = linkReturn_;
            const bool vis = linkReturnVisible_;
            lk.unlock();
            if (retry)
                startLinkSignIn(ret, vis);
            return;
        }

        if (view_ == ViewId::HomeUsers) {
            if (homeUsers_.empty())
                return;
            const int idx = focus_.row;
            if (idx < 0 || idx >= static_cast<int>(homeUsers_.size()))
                return;
            const HomeUser user = homeUsers_[static_cast<size_t>(idx)];
            homeSwitchIndex_ = idx;
            lk.unlock();
            beginHomeSwitch(user, "");
            return;
        }

        if (view_ == ViewId::HomePin) {
            if (homePinSlot_ < 4) {
                ++homePinSlot_;
                bumpAnim();
                dirty_.store(true);
                return;
            }
            if (homeSwitchIndex_ < 0 || homeSwitchIndex_ >= static_cast<int>(homeUsers_.size()))
                return;
            const HomeUser user = homeUsers_[static_cast<size_t>(homeSwitchIndex_)];
            const std::string pin = homePin_;
            lk.unlock();
            beginHomeSwitch(user, pin);
            return;
        }

        if (view_ == ViewId::UpNext) {
            const bool cancel = focus_.detailButton == 1;
            if (cancel) {
                cancelUpNextLocked();
                view_ = ViewId::EpisodeList;
                visible_ = true;
                dirty_.store(true);
                return;
            }
            lk.unlock();
            stepActiveQueue(1);
            return;
        }

        if (view_ == ViewId::AudioPicker) {
            const auto rows = audioPickerRows(detail_);
            const int idx = focus_.row;
            if (idx < 0 || idx >= static_cast<int>(rows.size()))
                return;
            const AudioPickerRow& row = rows[static_cast<size_t>(idx)];
            if (!row.selectable)
                return;
            pickedAudioId_ = row.streamId;
            logLine("plexui: audio pick id=" + std::to_string(row.streamId));
            view_ = ViewId::Detail;
            const int audBtn = audioButtonIndex(detail_);
            if (audBtn >= 0)
                focus_.detailButton = audBtn;
            dirty_.store(true);
            return;
        }

        if (view_ == ViewId::SubtitlePicker) {
            const auto rows = subtitlePickerRows(detail_);
            const int idx = focus_.row;
            if (idx < 0 || idx >= static_cast<int>(rows.size()))
                return;
            const SubtitlePickerRow& row = rows[static_cast<size_t>(idx)];
            if (!row.selectable)
                return;
            const PlexSubtitleTrack* track = findSubtitleTrack(detail_, row.streamId);
            ShowPreference pref = preferenceFromSubtitleRow(detail_, row, track);
            if (!pref.showKey.empty())
                processShowTracks().stage(pref);
            pickedSubtitleId_ = row.streamId;
            view_ = ViewId::Detail;
            const int subBtn = subtitleButtonIndex(detail_);
            if (subBtn >= 0)
                focus_.detailButton = subBtn;
            dirty_.store(true);
            return;
        }

        if (view_ == ViewId::Detail) {
            if (detail_.type == "show") {
                PlexServer srv = server_;
                const std::string rk = detail_.ratingKey;
                const std::string title = detail_.title;
                status_ = "LOADING...";
                dirty_.store(true);
                lk.unlock();
                std::vector<PlexItem> kids;
                std::string err;
                const bool ok = fetchMetadataChildren(hooks_.get, srv, rk, cancel, kids, err);
                lk.lock();
                if (ok) {
                    seasons_ = std::move(kids);
                    episodes_.clear();
                    touchListsLocked();
                    view_ = ViewId::SeasonList;
                    listTitle_ = title.empty() ? "Seasons" : title;
                    focus_ = {};
                    seasonScrollRow_ = 0;
                    status_.clear();
                } else if (!cancel()) {
                    status_ = err.empty() ? "LOAD FAILED" : err;
                }
                dirty_.store(true);
                return;
            }
            const int audBtn = audioButtonIndex(detail_);
            if (audBtn >= 0 && focus_.detailButton == audBtn) {
                view_ = ViewId::AudioPicker;
                focus_.row = audioInitialRow(detail_, pickedAudioId_);
                dirty_.store(true);
                return;
            }
            const int subBtn = subtitleButtonIndex(detail_);
            if (subBtn >= 0 && focus_.detailButton == subBtn) {
                const auto rows = subtitlePickerRows(detail_);
                view_ = ViewId::SubtitlePicker;
                focus_.row = pickerRowForStreamId(rows, pickedSubtitleId_ >= 0 ? pickedSubtitleId_ : 0);
                if (pickedSubtitleId_ < 0) {
                    for (const PlexSubtitleTrack& t : detail_.subtitles) {
                        if (t.selected && t.softText) {
                            focus_.row = pickerRowForStreamId(rows, t.id);
                            break;
                        }
                    }
                }
                dirty_.store(true);
                return;
            }
            const int playNextBtn = playNextButtonIndex(detail_);
            if (playNextBtn >= 0 && focus_.detailButton == playNextBtn) {
                lk.unlock();
                applyQueueMark(QueueMark::PlayNext, cancel);
                return;
            }
            const int addQueueBtn = addToQueueButtonIndex(detail_);
            if (addQueueBtn >= 0 && focus_.detailButton == addQueueBtn) {
                lk.unlock();
                applyQueueMark(QueueMark::Append, cancel);
                return;
            }
            int64_t off = 0;
            bool fromStart = false;
            if (focus_.detailButton == 0 && detail_.viewOffsetMs > 0)
                off = detail_.viewOffsetMs;
            else if (focus_.detailButton == playFromStartButtonIndex(detail_))
                fromStart = true;
            // Call the play hook with mu_ released: doPlay -> onPlayStarted takes mu_.
            const PlexItem item = detail_;
            PlexServer srv = server_;
            lk.unlock();
            const bool started = playLeaf(item, std::move(srv), off, fromStart, cancel);
            lk.lock();
            if (started)
                visible_ = false;
            dirty_.store(true);
            return;
        }

        if (view_ == ViewId::SeasonList) {
            const GridLayout gl = seasonGridLayout();
            const int idx = focus_.row * std::max(1, gl.cols) + focus_.col;
            if (idx < 0 || idx >= static_cast<int>(seasons_.size()))
                return;
            PlexServer srv = server_;
            const PlexItem season = seasons_[static_cast<std::size_t>(idx)];
            status_ = "LOADING...";
            dirty_.store(true);
            lk.unlock();
            std::vector<PlexItem> kids;
            std::string err;
            const bool ok = fetchMetadataChildren(hooks_.get, srv, season.ratingKey, cancel, kids, err);
            lk.lock();
            if (ok) {
                episodes_ = std::move(kids);
                touchListsLocked();
                view_ = ViewId::EpisodeList;
                listTitle_ = season.title.empty() ? "Episodes" : season.title;
                focus_ = {};
                episodeScrollRow_ = 0;
                status_.clear();
            } else if (!cancel()) {
                status_ = err.empty() ? "LOAD FAILED" : err;
            }
            dirty_.store(true);
            return;
        }

        if (view_ == ViewId::EpisodeList) {
            if (focus_.row < 0 || focus_.row >= static_cast<int>(episodes_.size()))
                return;
            const PlexItem item = episodes_[static_cast<std::size_t>(focus_.row)];
            PlexServer srv = server_;
            const int64_t off = item.viewOffsetMs > 0 ? item.viewOffsetMs : 0;
            status_.clear();
            lk.unlock();
            const bool started = playLeaf(item, std::move(srv), off, false, cancel);
            lk.lock();
            if (started)
                visible_ = false;
            dirty_.store(true);
            return;
        }
    }
}

void PlexUiController::clearActiveQueueLocked() {
    const std::string line = queueLine_;
    activeQueue_ = {};
    activeQueueIndex_ = -1;
    queueMachineId_.clear();
    queueBaseUrl_.clear();
    queueToken_.clear();
    queuePlayArmed_ = false;
    upNextArmedKey_.clear();
    queueLine_.clear();
    if (!line.empty() && status_ == line)
        status_.clear();
}

void PlexUiController::refreshQueueLineLocked() {
    queueLine_ = queueFooterFor(activeQueue_, activeQueueIndex_);
    if (!queueLine_.empty())
        status_ = queueLine_;
}

void PlexUiController::bindActiveQueueLocked(PlexPlayQueue q, int index, const PlexServer& srv) {
    activeQueue_ = std::move(q);
    activeQueueIndex_ = index;
    queueMachineId_ = srv.machineIdentifier;
    queueBaseUrl_ = srv.baseUrl;
    queueToken_ = srv.accessToken;
    upNextArmedKey_.clear();
    refreshQueueLineLocked();
}

bool PlexUiController::queueBoundToServerLocked() const {
    if (!activeQueue_.ok || activeQueueIndex_ < 0 ||
        activeQueueIndex_ >= static_cast<int>(activeQueue_.items.size()))
        return false;
    return queueMachineId_ == server_.machineIdentifier && queueBaseUrl_ == server_.baseUrl &&
           queueToken_ == server_.accessToken;
}

bool PlexUiController::queueHasNext() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!activeQueue_.ok || activeQueueIndex_ < 0)
        return false;
    return activeQueueIndex_ + 1 < static_cast<int>(activeQueue_.items.size());
}

void PlexUiController::postMark(MarkWatchedOp op) {
    if (op == MarkWatchedOp::None || !markWatchedEnabled())
        return;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (stopping_)
            return;
        Job job;
        job.op = BrowseOp::Info;
        job.gen = ++gen_;
        job.markOp = static_cast<int>(op);
        q_.push_back(job);
    }
    qCv_.notify_one();
}

void PlexUiController::postQueueMark(QueueMark mark) {
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (stopping_)
            return;
        Job job;
        job.op = BrowseOp::Info;
        job.gen = ++gen_;
        job.queueMark = mark == QueueMark::PlayNext ? 2 : 1;
        q_.push_back(job);
    }
    qCv_.notify_one();
}

bool PlexUiController::focusedMarkLeafLocked(PlexItem& item) const {
    item = {};
    auto leaf = [](const PlexItem& it) {
        if (it.ratingKey.empty())
            return false;
        return it.type == "movie" || it.type == "episode" || it.type.empty();
    };
    if (!visible_ || !haveServer_)
        return false;
    if (view_ == ViewId::EpisodeList) {
        if (focus_.row < 0 || focus_.row >= static_cast<int>(episodes_.size()))
            return false;
        item = episodes_[static_cast<std::size_t>(focus_.row)];
        return leaf(item);
    }
    if (view_ == ViewId::Detail) {
        item = detail_;
        return leaf(item);
    }
    if (view_ == ViewId::LibraryGrid && !focus_.gridOnChips && !focus_.gridOnLetters) {
        const int idx = focus_.row * kGridCols() + focus_.col;
        if (idx < 0 || idx >= static_cast<int>(grid_.size()))
            return false;
        item = grid_[static_cast<std::size_t>(idx)];
        return leaf(item);
    }
    if (view_ == ViewId::Home && focus_.sidebar == 0 && focus_.row >= 0 &&
        focus_.row < static_cast<int>(hubs_.size())) {
        const auto& hub = hubs_[static_cast<std::size_t>(focus_.row)];
        if (focus_.col < 0 || focus_.col >= static_cast<int>(hub.items.size()))
            return false;
        item = hub.items[static_cast<std::size_t>(focus_.col)];
        return leaf(item);
    }
    return false;
}

void PlexUiController::noteMarkAppliedLocked(MarkWatchedOp op, const std::string& ratingKey) {
    auto touch = [&](PlexItem& it) {
        if (it.ratingKey != ratingKey)
            return;
        if (op == MarkWatchedOp::Watched) {
            if (it.viewCount < 1)
                it.viewCount = 1;
            it.viewOffsetMs = 0;
        } else if (op == MarkWatchedOp::Unwatched) {
            it.viewCount = 0;
            it.viewOffsetMs = 0;
        } else {
            it.viewOffsetMs = 0;
        }
    };
    touch(detail_);
    for (PlexItem& it : grid_)
        touch(it);
    for (PlexItem& it : episodes_)
        touch(it);
    for (PlexHubRow& hub : hubs_) {
        const bool cw = continueWatchingTitle(hub.title) || hub.hubKey.find("onDeck") != std::string::npos;
        for (auto it = hub.items.begin(); it != hub.items.end();) {
            if (cw && it->ratingKey == ratingKey) {
                it = hub.items.erase(it);
                continue;
            }
            touch(*it);
            ++it;
        }
    }
    if (focus_.row >= 0 && focus_.row < static_cast<int>(hubs_.size())) {
        const auto& hub = hubs_[static_cast<std::size_t>(focus_.row)];
        if (focus_.col >= static_cast<int>(hub.items.size()))
            focus_.col = std::max(0, static_cast<int>(hub.items.size()) - 1);
    }
    touchListsLocked();
    ++dataGeneration_;
}

void PlexUiController::applyMark(MarkWatchedOp op, const BrowseCancelFn& cancel) {
    if (!markWatchedEnabled())
        return;
    PlexItem item;
    PlexServer srv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_ || !haveServer_ || !focusedMarkLeafLocked(item)) {
            status_ = "Pick a title";
            dirty_.store(true);
            return;
        }
        srv = server_;
    }
    MarkTarget target;
    target.baseUrl = srv.baseUrl;
    target.token = srv.accessToken;
    target.ratingKey = item.ratingKey;
    target.clientId = cfg_.clientId;
    auto transport = [&](const MarkHttpRequest& req) {
        MarkTransportResult tr;
        const HttpPostFn& put = hooks_.put;
        const HttpHeaderGetFn& get = hooks_.getHeaders;
        if (req.method == "PUT") {
            if (!put)
                return tr;
            const BrowseHttpResult r = put(req.url, "", "", req.headers, 4000, 8000, cancel);
            tr.status = r.cancelled ? 0 : r.status;
            return tr;
        }
        if (!get)
            return tr;
        const BrowseHttpResult r = get(req.url, req.headers, 4000, 8000, cancel);
        tr.status = r.cancelled ? 0 : r.status;
        return tr;
    };
    const MarkApplyResult applied = applyMarkWatched(transport, op, target);
    std::lock_guard<std::mutex> lk(mu_);
    status_ = applied.osd.empty() ? std::string("MARK REFUSED") : applied.osd;
    if (!applied.refused && applied.httpStatus >= 200 && applied.httpStatus < 300)
        noteMarkAppliedLocked(op, item.ratingKey);
    dirty_.store(true);
    if (hooks_.log)
        hooks_.log("plexui: mark op=" + std::to_string(static_cast<int>(op)) + " ratingKey=" +
                   item.ratingKey + " http=" + std::to_string(applied.httpStatus));
}

bool PlexUiController::focusedQueueLeafLocked(PlexItem& item) const {
    item = {};
    if (!visible_ || !haveServer_)
        return false;
    if (view_ == ViewId::EpisodeList) {
        if (focus_.row < 0 || focus_.row >= static_cast<int>(episodes_.size()))
            return false;
        item = episodes_[static_cast<std::size_t>(focus_.row)];
        return item.type == "episode" && !item.ratingKey.empty();
    }
    if (view_ == ViewId::Detail) {
        item = detail_;
        return (item.type == "episode" || item.type == "movie") && !item.ratingKey.empty();
    }
    if (view_ == ViewId::LibraryGrid && !focus_.gridOnChips && !focus_.gridOnLetters) {
        const int idx = focus_.row * kGridCols() + focus_.col;
        if (idx < 0 || idx >= static_cast<int>(grid_.size()))
            return false;
        item = grid_[static_cast<std::size_t>(idx)];
        return (item.type == "episode" || item.type == "movie") && !item.ratingKey.empty();
    }
    return false;
}

bool PlexUiController::playLeaf(const PlexItem& item, PlexServer srv, int64_t offsetMs, bool forceStart,
                                const BrowseCancelFn& cancel) {
    PlayQueueBind bind;
    const PlayQueueBind* bindPtr = nullptr;
    bool bound = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (srv.machineIdentifier == server_.machineIdentifier && srv.baseUrl == server_.baseUrl &&
            queueBoundToServerLocked()) {
            const int idx = queueExactIndex(activeQueue_, item.ratingKey);
            if (idx >= 0) {
                activeQueueIndex_ = idx;
                const auto& qi = activeQueue_.items[static_cast<std::size_t>(idx)];
                bind.id = activeQueue_.id;
                bind.version = activeQueue_.version;
                bind.itemId = qi.playQueueItemId;
                bind.containerKey = "/playQueues/" + activeQueue_.id;
                bindPtr = &bind;
                bound = true;
                refreshQueueLineLocked();
            }
        }
    }
    if (!bound && item.type == "episode") {
        PlexPlayQueue q;
        std::string err;
        if (createPlayQueue(hooks_.post, srv, item.ratingKey, cfg_.clientId, cancel, q, err)) {
            const int idx = queueIndexFor(q, item.ratingKey);
            if (idx >= 0) {
                std::lock_guard<std::mutex> lkq(mu_);
                bindActiveQueueLocked(std::move(q), idx, srv);
                const auto& qi = activeQueue_.items[static_cast<std::size_t>(idx)];
                bind.id = activeQueue_.id;
                bind.version = activeQueue_.version;
                bind.itemId = qi.playQueueItemId;
                bind.containerKey = "/playQueues/" + activeQueue_.id;
                bindPtr = &bind;
                logLine("plexui: playQueue id=" + bind.id + " items=" +
                        std::to_string(activeQueue_.items.size()));
            } else {
                std::lock_guard<std::mutex> lkq(mu_);
                clearActiveQueueLocked();
            }
        } else {
            logLine(std::string("plexui: playQueue ") + err);
            std::lock_guard<std::mutex> lkq(mu_);
            clearActiveQueueLocked();
        }
    }
    return startPlay(item, offsetMs, bindPtr, forceStart);
}

void PlexUiController::applyQueueMark(QueueMark mark, const BrowseCancelFn& cancel) {
    const bool playing = hooks_.playing && hooks_.playing();
    PlexItem item;
    PlexServer srv;
    QueueBinding binding;
    std::string keepKey;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_)
            return;
        if (!focusedQueueLeafLocked(item)) {
            status_ = "Pick a title";
            queueLine_ = status_;
            dirty_.store(true);
            return;
        }
        srv = server_;
        binding.playing = playing;
        if (queueBoundToServerLocked()) {
            binding.bound = true;
            binding.id = activeQueue_.id;
            binding.version = activeQueue_.version;
            if (activeQueueIndex_ >= 0 &&
                activeQueueIndex_ < static_cast<int>(activeQueue_.items.size()))
                keepKey = activeQueue_.items[static_cast<std::size_t>(activeQueueIndex_)].ratingKey;
        }
    }
    const std::string ident = cfg_.clientId.empty() ? "misterplex" : cfg_.clientId;
    auto transport = [&](const QueueHttpRequest& req) {
        QueueTransportResult tr;
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"X-Plex-Token", srv.accessToken},
            {"X-Plex-Client-Identifier", ident},
            {"X-Plex-Product", "MiSTerPlex"},
            {"Accept", "application/xml"},
        };
        const HttpPostFn& send = req.method == "PUT" ? hooks_.put : hooks_.post;
        if (!send) {
            tr.status = 0;
            return tr;
        }
        // Empty content type: curl must not attach a form body to this query POST/PUT.
        const std::string contentType = req.body.empty() ? std::string() : "application/x-www-form-urlencoded";
        const BrowseHttpResult r = send(req.url, contentType, req.body, headers, 4000, 12000, cancel);
        tr.status = r.status;
        tr.cancelled = r.cancelled;
        tr.body = std::move(r.body);
        return tr;
    };
    const QueueApplyResult applied = ::misterplex::plexui::applyQueueMark(
        transport, srv.baseUrl, srv.machineIdentifier, item.ratingKey, binding, mark);
    if (applied.cancelled)
        return;
    if (applied.playNow) {
        const int64_t off = item.viewOffsetMs > 0 ? item.viewOffsetMs : 0;
        const bool started = playLeaf(item, srv, off, false, cancel);
        std::lock_guard<std::mutex> lk(mu_);
        if (started)
            visible_ = false;
        dirty_.store(true);
        return;
    }
    if (applied.refused || !applied.applied) {
        std::lock_guard<std::mutex> lk(mu_);
        queueLine_ = applied.osd.empty() ? std::string(queueRefusedLine()) : applied.osd;
        status_ = queueLine_;
        dirty_.store(true);
        logLine("plexui: playQueue refused http=" + std::to_string(applied.httpStatus));
        return;
    }
    int idx = queueExactIndex(applied.queue, keepKey);
    if (idx < 0)
        idx = queueExactIndex(applied.queue, item.ratingKey);
    {
        std::lock_guard<std::mutex> lk(mu_);
        bindActiveQueueLocked(applied.queue, idx, srv);
        dirty_.store(true);
        logLine("plexui: playQueue id=" + activeQueue_.id + " items=" +
                std::to_string(activeQueue_.items.size()));
    }
}

void PlexUiController::kickShowTheme(const PlexServer& srv, const PlexItem& item,
                                     const BrowseCancelFn& cancel) {
    if (item.type != "show" || item.ratingKey.empty())
        return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (item.ratingKey == themeKickedKey_)
            return;
        themeKickedKey_ = item.ratingKey;
    }
    if (hooks_.playing && hooks_.playing())
        return;
    std::string body;
    std::string err;
    if (!fetchShowTheme(hooks_.getHeaders, srv, item, cancel, body, err)) {
        std::lock_guard<std::mutex> lk(mu_);
        if (themeKickedKey_ == item.ratingKey)
            themeKickedKey_.clear();
        return;
    }
    logLine("plexui: theme bytes=" + std::to_string(body.size()));
    if (hooks_.onThemeBytes)
        hooks_.onThemeBytes(std::vector<std::uint8_t>(body.begin(), body.end()));
}

void PlexUiController::assignServerLocked(PlexServer next) {
    const bool changed = next.machineIdentifier != server_.machineIdentifier ||
                         next.baseUrl != server_.baseUrl || next.accessToken != server_.accessToken;
    server_ = std::move(next);
    if (changed)
        clearActiveQueueLocked();
}

void PlexUiController::joinOwnedThreadUnlocked(std::thread& th) {
    if (!th.joinable())
        return;
    if (th.get_id() == std::this_thread::get_id())
        return;
    th.join();
}

void PlexUiController::joinUpNextThread() {
    std::lock_guard<std::mutex> lk(helperMu_);
    joinOwnedThreadUnlocked(upNextThread_);
}

void PlexUiController::cancelUpNextLocked() {
    upNextToken_.fetch_add(1);
    upNextSeconds_ = 0;
}

void PlexUiController::syncSeasonScroll() {
    if (seasons_.empty())
        return;
    const GridLayout gl = seasonGridLayout();
    const int vis = gridVisibleRowCount(gl);
    const int cols = std::max(1, gl.cols);
    const int maxRow = (static_cast<int>(seasons_.size()) - 1) / cols;
    if (focus_.row < seasonScrollRow_)
        seasonScrollRow_ = focus_.row;
    else if (focus_.row >= seasonScrollRow_ + vis)
        seasonScrollRow_ = std::max(0, focus_.row - vis + 1);
    seasonScrollRow_ = std::max(0, std::min(seasonScrollRow_, maxRow));
}

void PlexUiController::syncEpisodeScroll() {
    if (episodes_.empty())
        return;
    const int vis = episodeVisibleRows();
    const int maxRow = static_cast<int>(episodes_.size()) - 1;
    if (focus_.row < episodeScrollRow_)
        episodeScrollRow_ = focus_.row;
    else if (focus_.row >= episodeScrollRow_ + vis)
        episodeScrollRow_ = std::max(0, focus_.row - vis + 1);
    episodeScrollRow_ = std::max(0, std::min(episodeScrollRow_, maxRow));
}

void PlexUiController::postQueueStep(int delta) {
    if (delta == 0)
        return;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (stopping_)
            return;
        Job job;
        job.op = BrowseOp::Select;
        job.gen = ++gen_;
        job.queueDelta = delta;
        q_.push_back(job);
    }
    qCv_.notify_one();
}

void PlexUiController::stepActiveQueue(int delta) {
    PlexItem item;
    PlayQueueBind bind;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!queueBoundToServerLocked()) {
            clearActiveQueueLocked();
            return;
        }
        const int dest = queueStepIndex(activeQueueIndex_, delta,
                                        static_cast<int>(activeQueue_.items.size()));
        if (dest < 0)
            return;
        upNextToken_.fetch_add(1);
        activeQueueIndex_ = dest;
        const auto& qi = activeQueue_.items[static_cast<std::size_t>(dest)];
        item.ratingKey = qi.ratingKey;
        item.key = qi.key;
        item.title = qi.title;
        item.type = "episode";
        item.parentIndex = qi.parentIndex;
        item.index = qi.index;
        bind.id = activeQueue_.id;
        bind.version = activeQueue_.version;
        bind.itemId = qi.playQueueItemId;
        bind.containerKey = "/playQueues/" + activeQueue_.id;
        for (int i = 0; i < static_cast<int>(episodes_.size()); ++i) {
            if (episodes_[static_cast<std::size_t>(i)].ratingKey == item.ratingKey) {
                focus_.row = i;
                break;
            }
        }
        syncEpisodeScroll();
        view_ = ViewId::EpisodeList;
    }
    const bool started = startPlay(item, 0, &bind, true);
    std::lock_guard<std::mutex> lk(mu_);
    if (started)
        visible_ = false;
    dirty_.store(true);
}

bool PlexUiController::armUpNext(const std::string& endedRatingKey) {
    if (stopping_.load())
        return false;
    uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!queueBoundToServerLocked()) {
            clearActiveQueueLocked();
            return false;
        }
        const auto& cur = activeQueue_.items[static_cast<std::size_t>(activeQueueIndex_)];
        if (endedRatingKey.empty() || cur.ratingKey != endedRatingKey)
            return false;
        const int next = queueStepIndex(activeQueueIndex_, +1,
                                        static_cast<int>(activeQueue_.items.size()));
        if (next < 0)
            return false;
        // A second "ended" for this same item must not restart the countdown.
        if (upNextArmedKey_ == endedRatingKey)
            return true;
        upNextArmedKey_ = endedRatingKey;
        token = upNextToken_.fetch_add(1) + 1;
        upNextSeconds_ = 6;
        view_ = ViewId::UpNext;
        visible_ = true;
        focus_.detailButton = 0;
        dirty_.store(true);
    }
    if (hooks_.repaint)
        hooks_.repaint();
    {
        std::lock_guard<std::mutex> lk(helperMu_);
        if (stopping_.load())
            return true;
        if (upNextThread_.joinable() && upNextThread_.get_id() == std::this_thread::get_id())
            return true;
        joinOwnedThreadUnlocked(upNextThread_);
        if (stopping_.load())
            return true;
        upNextThread_ = std::thread([this, token] {
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stopping_.load() || upNextToken_.load() != token)
                return;
            if ((i + 1) % 5 == 0) {
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (upNextSeconds_ > 0)
                        --upNextSeconds_;
                    dirty_.store(true);
                }
                if (hooks_.repaint)
                    hooks_.repaint();
            }
        }
        if (stopping_.load() || upNextToken_.load() != token)
            return;
        {
            std::lock_guard<std::mutex> lk(qMu_);
            if (stopping_.load() || upNextToken_.load() != token)
                return;
            Job job;
            job.op = BrowseOp::Select;
            job.gen = ++gen_;
            job.queueDelta = 1;
            job.upNextToken = token;
            q_.push_back(job);
        }
        qCv_.notify_one();
        });
    }
    return true;
}

void PlexUiController::parkLinkWhilePlaying(uint64_t gen) {
    std::unique_lock<std::mutex> lk(linkWaitMu_);
    linkWaitCv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
        if (stopping_.load() || linkGen_.load() != gen)
            return true;
        return !(hooks_.playing && hooks_.playing());
    });
}

void PlexUiController::joinLinkThread() {
    std::lock_guard<std::mutex> lk(helperMu_);
    joinOwnedThreadUnlocked(linkThread_);
}

std::vector<std::pair<std::string, std::string>> PlexUiController::linkHeaders() const {
    const std::string ident = cfg_.clientId.empty() ? "misterplex" : cfg_.clientId;
    return {
        {"Accept", "application/json"},
        {"X-Plex-Client-Identifier", ident},
        {"X-Plex-Product", "MiSTerPlex"},
    };
}

void PlexUiController::ensureHomeUserRowLocked() {
    if (cfg_.accountToken.empty() || sidebarEntries_.empty())
        return;
    for (auto& e : sidebarEntries_) {
        if (e.kind != SidebarRowKind::HomeUser && e.type != "homeuser")
            continue;
        if (!cfg_.homeUserTitle.empty()) {
            e.title = cfg_.homeUserTitle;
            e.subtitle = "Switch user";
        } else {
            e.title = "Switch user";
            e.subtitle = "Plex Home";
        }
        return;
    }
    appendSidebarHomeUser(sidebarEntries_, cfg_.homeUserTitle);
}

void PlexUiController::openHomeUsers() {
    std::string account;
    {
        std::lock_guard<std::mutex> lk(mu_);
        account = cfg_.accountToken;
        view_ = ViewId::HomeUsers;
        visible_ = true;
        focus_ = {};
        homeUsers_.clear();
        homeSwitchIndex_ = -1;
        status_ = account.empty() ? "Sign in first" : "LOADING...";
        dirty_.store(true);
    }
    if (account.empty())
        return;
    if (hooks_.repaint)
        hooks_.repaint();
    auto headers = linkHeaders();
    headers.emplace_back("X-Plex-Token", account);
    const BrowseCancelFn cancel = cancelFor(gen_.load());
    BrowseHttpResult got;
    if (hooks_.getHeaders)
        got = hooks_.getHeaders(kHomeUsersUrl, headers, 4000, 12000, cancel);
    if (got.cancelled || stopping_.load())
        return;
    std::vector<HomeUser> users = got.ok() ? parseHomeUsers(got.body) : std::vector<HomeUser>{};
    // A 200 with an empty list is a real no-Home account. Legacy /api/home/users
    // is only for v2 missing (404) or a transport/server error — never 401/403.
    if (!got.ok() && got.status != 401 && got.status != 403 && hooks_.getHeaders) {
        const BrowseHttpResult legacy =
            hooks_.getHeaders(kHomeUsersLegacyUrl, headers, 4000, 12000, cancel);
        if (legacy.cancelled || stopping_.load())
            return;
        if (legacy.ok()) {
            auto more = parseHomeUsers(legacy.body);
            if (!more.empty())
                users = std::move(more);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (view_ != ViewId::HomeUsers)
        return;
    homeUsers_ = std::move(users);
    focus_.row = 0;
    if (!cfg_.homeUserId.empty()) {
        for (size_t i = 0; i < homeUsers_.size(); ++i) {
            if (homeUsers_[i].id == cfg_.homeUserId) {
                focus_.row = static_cast<int>(i);
                break;
            }
        }
    }
    status_ = homeUsers_.empty() ? "No Home users" : std::string{};
    dirty_.store(true);
}

void PlexUiController::beginHomeSwitch(const HomeUser& user, const std::string& pin) {
    if (!homeIdOk(user.id)) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "SWITCH FAILED";
        dirty_.store(true);
        return;
    }
    if (homeSwitchNeedsPin(user) && !homePinOk(pin)) {
        std::lock_guard<std::mutex> lk(mu_);
        homePin_ = "0000";
        homePinSlot_ = 0;
        homePinTitle_ = user.title.empty() ? "User" : user.title;
        view_ = ViewId::HomePin;
        visible_ = true;
        status_.clear();
        dirty_.store(true);
        return;
    }
    std::string account;
    bool alreadyAccount = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        account = cfg_.accountToken;
        alreadyAccount = cfg_.homeUserToken.empty();
    }
    if (user.admin && !homeSwitchNeedsPin(user)) {
        if (alreadyAccount) {
            std::lock_guard<std::mutex> lk(mu_);
            view_ = ViewId::Home;
            focus_ = {};
            focus_.sidebar = 0;
            status_.clear();
            dirty_.store(true);
            logLine("plexui: home profile is the account");
            return;
        }
        applyHomeSession(HomeSession{});
        return;
    }
    if (account.empty() || !hooks_.post) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "SWITCH FAILED";
        dirty_.store(true);
        return;
    }
    auto headers = linkHeaders();
    headers.emplace_back("X-Plex-Token", account);
    const BrowseCancelFn cancel = cancelFor(gen_.load());
    std::string url = homeSwitchUrl(user.id);
    std::string body;
    if (!pin.empty()) {
        url += "?pin=" + pin;
        body = "pin=" + pin;
    }
    BrowseHttpResult posted =
        hooks_.post(url, "application/x-www-form-urlencoded", body, headers, 4000, 12000, cancel);
    if (posted.cancelled || stopping_.load())
        return;
    HomeSwitch sw = parseHomeSwitch(posted.body, posted.status);
    if ((!posted.ok() || !sw.ok) && posted.status == 404) {
        std::string legacy = homeSwitchLegacyUrl(user.id);
        if (!pin.empty())
            legacy += "?pin=" + pin;
        posted = hooks_.post(legacy, "application/x-www-form-urlencoded", body, headers, 4000, 12000,
                             cancel);
        if (posted.cancelled || stopping_.load())
            return;
        sw = parseHomeSwitch(posted.body, posted.status);
    }
    if (!posted.ok() || !sw.ok) {
        std::lock_guard<std::mutex> lk(mu_);
        if (view_ != ViewId::HomePin && view_ != ViewId::HomeUsers)
            return;
        const bool badPin = homeSwitchNeedsPin(user) &&
                            (posted.status == 401 || posted.status == 403 || sw.unauthorized);
        status_ = badPin ? "Wrong PIN" : "SWITCH FAILED";
        dirty_.store(true);
        logLine(badPin ? "plexui: home pin rejected" : "plexui: home switch failed");
        return;
    }
    HomeSession session;
    if (!user.admin) {
        session.id = user.id;
        session.title = homeTitleForConf(user.title);
        session.token = sw.authToken;
    }
    applyHomeSession(std::move(session));
}

bool PlexUiController::applyHomeSession(HomeSession session) {
    if (!session.token.empty() &&
        (!plexAccountTokenOk(session.token) || !homeIdOk(session.id))) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "SWITCH FAILED";
        dirty_.store(true);
        logLine("plexui: home token rejected");
        return false;
    }
    if (!session.token.empty())
        session.title = homeTitleForConf(session.title);
    bool stored = true;
    if (cfg_.persistHomeSession)
        stored = cfg_.persistHomeSession(session);
    if (!stored) {
        std::lock_guard<std::mutex> lk(mu_);
        status_ = "SWITCH FAILED";
        dirty_.store(true);
        logLine("plexui: home profile was not stored");
        return false;
    }
#ifdef MUT_HOME_CLOBBER_ACCOUNT
    if (!session.token.empty() && cfg_.persistAccountToken)
        cfg_.persistAccountToken(session.token);
#endif
    {
        std::lock_guard<std::mutex> lk(mu_);
#ifdef MUT_HOME_CLOBBER_ACCOUNT
        if (!session.token.empty())
            cfg_.accountToken = session.token;
#endif
        cfg_.homeUserId = session.id;
        cfg_.homeUserTitle = session.token.empty() ? std::string{} : session.title;
        cfg_.homeUserToken = session.token;
        servers_.clear();
        sidebarEntries_.clear();
        hubs_.clear();
        hubItemServerIndex_.clear();
        haveServer_ = false;
        homeUsers_.clear();
        homePin_ = "0000";
        homePinSlot_ = 0;
        homeSwitchIndex_ = -1;
        status_.clear();
        view_ = ViewId::Home;
        dirty_.store(true);
    }
    const std::string line = homeSessionLog(session);
    if (!logMentionsSecret(line, session.token))
        logLine(line);
    post(BrowseOp::Open);
    return true;
}

void PlexUiController::startLinkSignIn(ViewId returnView, bool returnVisible) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        linkReturn_ = returnView;
        linkReturnVisible_ = returnVisible;
        view_ = ViewId::LinkSignIn;
        visible_ = true;
        linkCode_.clear();
        linkSeconds_ = 0;
        status_ = "Requesting a code...";
        focus_ = {};
        dirty_.store(true);
    }
    const uint64_t gen = linkGen_.fetch_add(1) + 1;
    linkWaitCv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(helperMu_);
        if (stopping_.load())
            return;
        if (linkThread_.joinable() && linkThread_.get_id() == std::this_thread::get_id())
            return;
        joinOwnedThreadUnlocked(linkThread_);
        if (stopping_.load())
            return;
        linkThread_ = std::thread([this, gen] {
        auto still = [&] { return !stopping_.load() && linkGen_.load() == gen; };
        auto tlsStatus = [](const BrowseHttpResult& r) {
            if (r.tlsNoCa)
                return "TLS VERIFY FAILED / no CA bundle";
            if (browseCurlTlsHandshake(r.curlExit))
                return "TLS HANDSHAKE/CONNECT ERROR";
            return "TLS VERIFY FAILED";
        };
        const auto headers = linkHeaders();
        // The first code plus three replacements. Select starts a fresh attempt.
        constexpr int kMaxPinCreates = 4;
        int createdCount = 0;
        while (still()) {
            if (createdCount >= kMaxPinCreates) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "SIGN IN FAILED";
                dirty_.store(true);
                logLine("plexui: link pin renewal cap");
                return;
            }
            ++createdCount;
            BrowseHttpResult created;
            if (hooks_.post)
                created = hooks_.post("https://plex.tv/api/v2/pins?strong=false",
                                      "application/x-www-form-urlencoded", "", headers, 4000, 12000,
                                      cancelFor(gen_.load()));
            if (!still())
                return;
            // A key press cancels the request. Back bumps linkGen_ and already returned.
            // Any other key is ignored: ask for the code again instead of failing the card.
            if (created.cancelled) {
                --createdCount;
                if (hooks_.playing && hooks_.playing())
                    parkLinkWhilePlaying(gen);
                continue;
            }
            const PlexPin pin = created.ok() ? parsePinResponse(created.body) : PlexPin{};
            if (created.tlsFailed) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = tlsStatus(created);
                dirty_.store(true);
                logLine(browseCurlTlsHandshake(created.curlExit)
                            ? "plexui: link tls handshake/connect error"
                            : "plexui: link tls verify failed");
                return;
            }
            if (!created.ok() || !pin.ok) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "SIGN IN FAILED";
                dirty_.store(true);
                logLine("plexui: link pin request failed");
                return;
            }
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(pin.expiresIn);
            {
                std::lock_guard<std::mutex> lk(mu_);
                linkCode_ = pin.code;
                linkSeconds_ = pin.expiresIn;
                status_.clear();
                dirty_.store(true);
            }
            if (hooks_.repaint)
                hooks_.repaint();
            logLine("plexui: link code ready");
            const std::string pollUrl = "https://plex.tv/api/v2/pins/" + pin.id;
            int nextPollMs = 0;
            int backoffMs = 2000;
            while (still() && std::chrono::steady_clock::now() < deadline) {
                if (nextPollMs <= 0) {
                    BrowseHttpResult polled;
                    if (hooks_.getHeaders)
                        polled = hooks_.getHeaders(pollUrl, headers, 4000, 8000,
                                                   cancelFor(gen_.load()));
                    if (!still())
                        return;
                    if (polled.cancelled) {
                        if (hooks_.playing && hooks_.playing())
                            parkLinkWhilePlaying(gen);
                        continue;
                    }
                    if (polled.tlsFailed) {
                        std::lock_guard<std::mutex> lk(mu_);
                        status_ = tlsStatus(polled);
                        dirty_.store(true);
                        logLine(browseCurlTlsHandshake(polled.curlExit)
                                    ? "plexui: link tls handshake/connect error"
                                    : "plexui: link tls verify failed");
                        return;
                    }
                    if (polled.status == 401) {
                        std::lock_guard<std::mutex> lk(mu_);
                        status_ = "SIGN IN FAILED";
                        dirty_.store(true);
                        logLine("plexui: link poll unauthorized");
                        return;
                    }
                    if (polled.status == 404 || polled.status == 410) {
                        logLine("plexui: link code expired");
                        break;
                    }
                    if (polled.ok()) {
                        const PlexPin now = parsePinResponse(polled.body);
                        if (pinAuthed(now) && !plexAccountTokenOk(now.authToken)) {
                            std::lock_guard<std::mutex> lk(mu_);
                            status_ = "SIGN IN FAILED";
                            dirty_.store(true);
                            logLine("plexui: link token rejected");
                            return;
                        }
                        if (pinAuthed(now)) {
                            bool stored = true;
                            if (cfg_.persistAccountToken)
                                stored = cfg_.persistAccountToken(now.authToken);
                            const char* file = std::getenv(kPlexUiTokenFileEnv);
                            if (file && *file)
                                stored = writeTokenFile(file, now.authToken) && stored;
                            if (!stored) {
                                std::lock_guard<std::mutex> lk(mu_);
                                status_ = "SIGN IN FAILED";
                                dirty_.store(true);
                                logLine("plexui: link token was not stored");
                                return;
                            }
                            {
                                std::lock_guard<std::mutex> lk(mu_);
                                if (cfg_.accountToken != now.authToken)
                                    clearActiveQueueLocked();
                                cfg_.accountToken = now.authToken;
                                linkCode_.clear();
                                status_.clear();
                            }
                            logLine("plexui: signed in");
                            post(BrowseOp::Open);
                            return;
                        }
                        backoffMs = 2000;
                        nextPollMs = 2000;
                    } else if (polled.status == 429 || polled.status >= 500) {
                        int wait = backoffMs;
                        if (polled.retryAfterSec >= 0)
                            wait = polled.retryAfterSec * 1000;
                        if (wait < 0)
                            wait = 0;
                        if (wait > 30000)
                            wait = 30000;
                        backoffMs = backoffMs < 15000 ? backoffMs * 2 : 30000;
                        nextPollMs = wait;
                        logLine("plexui: link poll backoff");
                    } else {
                        backoffMs = 2000;
                        nextPollMs = 2000;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                nextPollMs -= 200;
                if (!still())
                    return;
                const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                                                      deadline - std::chrono::steady_clock::now())
                                                      .count());
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    linkSeconds_ = std::max(0, left);
                    dirty_.store(true);
                }
            }
            if (still())
                logLine("plexui: link code expired");
        }
        });
    }
}

std::vector<GridCategoryChip> PlexUiController::buildGridChips(
    std::vector<GridCategoryChip> collections, std::vector<GridCategoryChip> genres) {
    std::vector<GridCategoryChip> out;
    GridCategoryChip all;
    all.kind = GridCategoryChip::Kind::All;
    all.title = "All";
    out.push_back(std::move(all));
    for (auto& c : collections)
        out.push_back(std::move(c));
    for (auto& g : genres)
        out.push_back(std::move(g));
    insertFilter43Chip(out);
    return out;
}

void PlexUiController::syncChipScroll() {
    if (gridChips_.empty())
        return;
    const int idx =
        std::max(0, std::min(static_cast<int>(gridChips_.size()) - 1, focus_.gridChip));
    float x = static_cast<float>(kSafeMargin());
    for (int i = 0; i < idx; ++i)
        x += measureCategoryChipWidth(renderer_.fonts(), gridChips_[static_cast<std::size_t>(i)].title) +
             kGridChipGap();
    const float chipW =
        measureCategoryChipWidth(renderer_.fonts(), gridChips_[static_cast<std::size_t>(idx)].title);
    const float left = x - static_cast<float>(gridChipScrollX_);
    const float right = left + chipW;
    if (left < static_cast<float>(kSafeMargin()))
        gridChipScrollX_ += static_cast<int>(left - static_cast<float>(kSafeMargin()));
    else if (right > static_cast<float>(kScreenW() - kSafeRight()))
        gridChipScrollX_ +=
            static_cast<int>(right - static_cast<float>(kScreenW() - kSafeRight()));
    if (gridChipScrollX_ < 0)
        gridChipScrollX_ = 0;
}

void PlexUiController::clearGridPageLocked() {
    gridTotalSize_ = -1;
    gridPageFull_ = false;
    gridPageBusy_ = false;
    ++gridPageGen_;
}

void PlexUiController::rememberGridPageLocked(int fetched, int totalSize, bool pageable) {
    ++gridPageGen_;
    gridPageBusy_ = false;
    if (!gridPageEnabled()) {
        gridTotalSize_ = -1;
        gridPageFull_ = false;
        return;
    }
    if (!pageable) {
        gridTotalSize_ = fetched;
        gridPageFull_ = false;
        return;
    }
    gridTotalSize_ = totalSize;
    gridPageFull_ = fetched >= kGridPageSize;
}

void PlexUiController::fetchAndAppendGridPage(PlexServer server, std::string sectionKey,
                                             std::string genreId, int start, int pageGen,
                                             int oldMaxRow, const BrowseCancelFn& cancel,
                                             char letter) {
    std::vector<PlexItem> more;
    int total = -1;
    std::string err;
    const bool ok = fetchSectionGridPage(hooks_.get, server, sectionKey, start, genreId, cancel, more,
                                         &total, err, letter);
    std::lock_guard<std::mutex> lk(mu_);
    gridPageBusy_ = false;
    const bool fresh = pageGen == gridPageGen_ && view_ == ViewId::LibraryGrid &&
                       gridSectionKey_ == sectionKey;
    if (!fresh)
        return;
    if (!ok) {
        if (!(cancel && cancel()))
            status_ = err.empty() ? "LOAD FAILED" : err;
        dirty_.store(true);
        return;
    }
    int added = 0;
#ifndef MUT_GRID_PAGE_NO_APPEND
    if (!filter43Enabled()) {
        std::unordered_set<std::string> have;
        have.reserve(grid_.size() + more.size());
        for (const auto& it : grid_) {
            if (!it.ratingKey.empty())
                have.insert(it.ratingKey);
        }
        for (auto& it : more) {
            if (!it.ratingKey.empty() && !have.insert(it.ratingKey).second)
                continue;
            grid_.push_back(std::move(it));
            ++added;
        }
    } else {
        if (gridFull_.empty())
            gridFull_ = grid_;
        std::unordered_set<std::string> have;
        have.reserve(gridFull_.size() + more.size());
        for (const auto& it : gridFull_) {
            if (!it.ratingKey.empty())
                have.insert(it.ratingKey);
        }
        for (auto& it : more) {
            if (!it.ratingKey.empty() && !have.insert(it.ratingKey).second)
                continue;
            gridFull_.push_back(std::move(it));
            ++added;
        }
        grid_ = libraryGridFiltered(gridFull_, filter43On_);
    }
#else
    (void)more;
#endif
    if (added == 0) {
        gridTotalSize_ = libraryLoadedCountLocked();
        gridPageFull_ = false;
    } else {
        if (total >= 0)
            gridTotalSize_ = total;
        gridPageFull_ = static_cast<int>(more.size()) >= kGridPageSize;
        const GridLayout gl = computeGridLayout(!gridChips_.empty());
        const int cols = std::max(1, gl.cols);
        const int newMax = grid_.empty() ? 0 : (static_cast<int>(grid_.size()) - 1) / cols;
        if (focus_.row == oldMaxRow)
            focus_.row = std::min(oldMaxRow + 1, newMax);
        ++dataGeneration_;
        markGridArtLoadStart();
        status_.clear();
    }
    syncGridScroll();
    dirty_.store(true);
}

bool PlexUiController::loadGridForChip(const GridCategoryChip& chip, const PlexServer& server,
                                       const std::string& sectionKey, const BrowseCancelFn& cancel,
                                       std::vector<PlexItem>& items, std::string& err,
                                       int* totalSize) {
    switch (chip.kind) {
    case GridCategoryChip::Kind::All:
        return fetchSectionGrid(hooks_.get, server, sectionKey, cancel, items, err, totalSize);
    case GridCategoryChip::Kind::Genre:
        return fetchSectionGridGenre(hooks_.get, server, sectionKey, chip.filterId, cancel, items,
                                     err, totalSize);
    case GridCategoryChip::Kind::Collection:
        if (totalSize)
            *totalSize = -1;
        return fetchCollectionChildren(hooks_.get, server, chip.filterId, cancel, items, err);
    case GridCategoryChip::Kind::Aspect43:
        err = "4:3";
        return false;
    }
    return false;
}

void PlexUiController::storeGridLocked(std::vector<PlexItem> items) {
    if (!filter43Enabled()) {
        grid_ = std::move(items);
        return;
    }
    gridFull_ = std::move(items);
    grid_ = libraryGridFiltered(gridFull_, filter43On_);
}

int PlexUiController::libraryLoadedCountLocked() const {
    if (filter43Enabled() && !gridFull_.empty())
        return static_cast<int>(gridFull_.size());
    return static_cast<int>(grid_.size());
}

} // namespace plexui
} // namespace misterplex
