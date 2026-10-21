#include "plexui_controller.hpp"

#include "libmisterplex/browse_fetch.hpp"
#include "libmisterplex/browse_v2.hpp"
#include "plexui_data.hpp"
#include "plexui_http.hpp"
#include "plexui_perf.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <thread>

namespace misterplex {
namespace plexui {

PlexUiController::PlexUiController(Config cfg, Hooks hooks)
    : cfg_(std::move(cfg)), hooks_(std::move(hooks)) {
    setPlexuiProbeLog([this](const std::string& line) { logLine(line); });
    if (!hooks_.get)
        hooks_.get = makePlexuiHttpGet();
    std::string err;
    if (!renderer_.initFonts(cfg_.fontDir, err) && hooks_.log)
        hooks_.log("plexui: font load failed: " + err);
    renderer_.setImageCache(&images_);
}

PlexUiController::~PlexUiController() { stop(); }

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
    qCv_.notify_all();
    images_.stop();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        worker_.join();
}

void PlexUiController::postHorizontal(int delta, const char* src, int64_t recvMs) {
    postIngress(delta > 0 ? BrowseOp::Right : BrowseOp::Left, src, recvMs);
}

void PlexUiController::applyHorizontal(int delta) {
    if (!visible_)
        return;
    if (view_ == ViewId::Home) {
        if (focus_.sidebar == 0 && delta < 0 && !sidebarEntries_.empty()) {
            focus_.sidebar = 1;
            syncSidebarScroll();
        } else if (focus_.sidebar > 0 && delta > 0 && focus_.col == 0) {
            focus_.sidebar = 0;
            syncSidebarScroll();
        } else if (focus_.sidebar == 0) {
            focus_.col = std::max(0, focus_.col + delta);
            if (!hubs_.empty() && focus_.row < static_cast<int>(hubs_.size())) {
                const int maxCol =
                    static_cast<int>(hubs_[static_cast<std::size_t>(focus_.row)].items.size()) - 1;
                if (maxCol >= 0)
                    focus_.col = std::min(maxCol, focus_.col);
            }
        }
    } else if (view_ == ViewId::LibraryGrid) {
        if (focus_.gridOnChips) {
            if (gridChips_.empty())
                return;
            focus_.gridChip =
                std::max(0, std::min(static_cast<int>(gridChips_.size()) - 1, focus_.gridChip + delta));
            syncChipScroll();
        } else {
            focus_.col = std::max(0, std::min(kGridCols() - 1, focus_.col + delta));
        }
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
    ownsSession_ = generation != 0 && generation == browsePlayGen_;
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
    if (s.view == "detail")
        return true;
    return false;
}

} // namespace

bool PlexUiController::workerIdle() {
    if (busy_.load())
        return false;
    std::lock_guard<std::mutex> lk(qMu_);
    return q_.empty();
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
                const bool wantsArt =
                    s.view == "home" || s.view == "grid" || s.view == "detail";
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
    const bool wantsArt = s.view == "home" || s.view == "grid" || s.view == "detail";
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
    if (newCols != oldCols && !focus_.gridOnChips) {   // keep the focused grid item
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
    RenderState rs;
    buildRenderState(rs);
    Renderer::RenderPerf rp;
    renderer_.render(rs, frame, &rp, fmt_);
    images_.setLayerBytes(renderer_.layerCacheBytes());   // 0012: layer tier of the global budget
    const int64_t renderEnd = perfSteadyMs();
    if (navTraceOpen_.load())
        activeNav_.renderEndMs = renderEnd;
    // NEVER call hooks_.repaint() here: copyFrame runs inside the daemon's paintIdle (player
    // mutex held) with mu_ held; re-entering paintIdle self-deadlocked on a lab MiSTer (pass 3).
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
    const int idx = focus_.sidebar;
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

bool PlexUiController::startPlay(const PlexItem& item, int64_t offsetMs) {
    if (!hooks_.play || !haveServer_)
        return false;
    BrowsePlayTicket t;
    t.ratingKey = item.ratingKey;
    t.key = item.key.empty() ? ("/library/metadata/" + item.ratingKey) : item.key;
    t.title = item.title;
    t.source = "pms";
    t.mpxServer = "plexui";
    parseServerUri(server_, t.address, t.port, t.protocol);
    t.token = server_.accessToken;
    t.machineIdentifier = server_.machineIdentifier;
    if (offsetMs > 0) {
        t.offsetPresent = true;
        t.offsetMs = offsetMs;
    }
    const uint64_t pg = hooks_.play(t);
    if (pg) {
        std::lock_guard<std::mutex> lk(mu_);
        browsePlayGen_ = pg;
        ownsSession_ = true;
        viewBeforePlay_ = view_;
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
        if (!focus_.gridOnChips && fidx >= 0 && fidx < static_cast<int>(grid_.size())) {
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
    rs.gridSectionTitle = gridSectionTitle_;
    rs.gridItemCount = static_cast<int>(grid_.size());
    rs.gridChips = gridChips_;
    rs.gridActiveChip = gridActiveChip_;
    rs.gridChipScrollX = gridChipScrollX_;
    rs.gridScrollRow = gridScrollRow_;
    rs.dataGeneration = dataGeneration_;
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
        } else if (view_ == ViewId::LibraryGrid && !focus_.gridOnChips) {
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
    const std::string n = lowerCopy(s.name);
    return n.find("native test") != std::string::npos ||
           n.find("node-worker") != std::string::npos ||
           n.find("misterplex native") != std::string::npos;
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
    const std::string& want = !needle.empty() ? needle : cfg_.homeServer;
    if (!want.empty()) {
        for (size_t i = 0; i < servers_.size(); ++i) {
            if (serverMatches(servers_[i], want))
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

void PlexUiController::syncSidebarScroll() {
    const int listTop = P(64);
    const int listBottom = kScreenH() - kSafeMargin();
    const int visibleH = listBottom - listTop;
    const int rowH = kSidebarRowH();
    const int focusRow = focus_.sidebar;
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

const PlexServer& PlexUiController::serverForHubItem(const PlexItem& item) const {
    const auto it = hubItemServerIndex_.find(item.ratingKey);
    if (it != hubItemServerIndex_.end() && it->second >= 0 &&
        it->second < static_cast<int>(servers_.size()))
        return servers_[static_cast<std::size_t>(it->second)];
    return server_;
}

bool PlexUiController::loadStartupCatalog(const BrowseCancelFn& cancel) {
    const auto perSec = fetchAllServerSectionsParallel(hooks_.get, servers_, cancel);
    if (cancel())
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    buildSidebarEntries(servers_, perSec, sidebarEntries_);
    const int hi = pickHomeServerIndex(servers_, cfg_.homeServer);
    if (hi >= 0) {
        serverIndex_ = hi;
        server_ = servers_[static_cast<std::size_t>(hi)];
        haveServer_ = true;
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
    if (!fetchServerHome(hooks_.get, srv, cancel, sec, hubs, err))
        return false;
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
    hubs_ = std::move(hubs);
    ++dataGeneration_;
    status_.clear();
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
                                          std::vector<GridCategoryChip> chips) {
    std::lock_guard<std::mutex> lk(mu_);
    visible_ = true;
    haveServer_ = true;
    server_ = std::move(server);
    view_ = ViewId::LibraryGrid;
    grid_ = std::move(grid);
    gridChips_ = std::move(chips);
    gridActiveChip_ = 0;
    gridSectionKey_ = "/library/sections/1";
    gridSectionTitle_ = "Movies";
    status_.clear();
    focus_ = {};
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
    server_ = s;
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

int PlexUiController::testSidebarFocus() const {
    std::lock_guard<std::mutex> lk(mu_);
    return focus_.sidebar;
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
        const int dw = P(150), dh = P(225);
        const GridLayout gl = computeGridLayout(true);
        const bool poster = cachedThumb(server_, detail_, dw, dh) ||
                            cachedThumb(server_, detail_, gl.posterW, gl.posterH) ||
                            cachedThumb(server_, detail_, kHubPosterW(), kHubPosterH());
        if (!detail_.thumb.empty() && !poster)
            return false;
        return cachedArt(server_, detail_);
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

    if (j.op == BrowseOp::Open) {
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
            // Send Home (BrowseOp::Open) lands on Home even when the UI is
            // already showing a deeper view. Hidden or reopen_ still reset.
            if (haveServer_ && (browseFromHidden || view_ != ViewId::Home || reopen_)) {
                view_ = ViewId::Home;
                focus_.sidebar = 0;
                status_.clear();
                gridSectionIndex_ = -1;
                gridSectionKey_.clear();
                gridChips_.clear();
                gridActiveChip_ = 0;
                gridChipScrollX_ = 0;
                focus_.gridOnChips = false;
                focus_.gridChip = 0;
                reopen_ = false;
                dirty_.store(true);
                if (!browseFromHidden)
                    return;
            }
        }
        if (servers_.empty()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "LOADING...";
            }
            dirty_.store(true);
            // 0011: the list fills in as servers resolve; a dead one turns "Offline" instead of blocking.
            auto list = fetchPlexServersProgressive(
                hooks_.get, cfg_.accountToken, cancel, [this](const std::vector<PlexServer>& snap) {
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        servers_ = snap;
                        ++dataGeneration_;
                        dirty_.store(true);
                    }
                    if (hooks_.repaint)
                        hooks_.repaint();
                });
            if (cancel())
                return;
            std::lock_guard<std::mutex> lk(mu_);
            servers_ = std::move(list);
            ++dataGeneration_;
            if (servers_.empty()) {
                status_ = "NO SERVERS";
                dirty_.store(true);
            } else {
                status_ = "LOADING...";
                dirty_.store(true);
            }
        } else {
            std::lock_guard<std::mutex> lk(mu_);
            visible_ = true;
            dirty_.store(true);
        }
        bool needCatalog = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            needCatalog = !servers_.empty() && sidebarEntries_.empty();
        }
        if (needCatalog) {
            if (loadStartupCatalog(cancel) && !cancel()) {
                if (loadHomeHubs(cancel)) {
                    dirty_.store(true);
                } else if (!cancel()) {
                    std::lock_guard<std::mutex> lk(mu_);
                    status_ = "LOAD FAILED";
                    dirty_.store(true);
                }
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
        if (needHubs && !cancel()) {
            if (!loadHomeHubs(cancel) && !cancel()) {
                std::lock_guard<std::mutex> lk(mu_);
                status_ = "LOAD FAILED";
                dirty_.store(true);
            }
        }
    }

    if (j.op == BrowseOp::Back) {
        std::lock_guard<std::mutex> lk(mu_);
        if (view_ == ViewId::Detail) {
            view_ = gridSectionIndex_ >= 0 ? ViewId::LibraryGrid : ViewId::Home;
            bumpAnim();
            dirty_.store(true);
            return;
        }
        if (view_ == ViewId::LibraryGrid) {
            view_ = ViewId::Home;
            gridSectionIndex_ = -1;
            gridSectionKey_.clear();
            gridChips_.clear();
            gridActiveChip_ = 0;
            gridChipScrollX_ = 0;
            focus_.gridOnChips = false;
            focus_.gridChip = 0;
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

    if (j.op == BrowseOp::StopAndReopen) {
        ViewId back = ViewId::Home;
        {
            std::lock_guard<std::mutex> lk(mu_);
            back = viewBeforePlay_;
        }
        if (hooks_.stopPlayback)
            hooks_.stopPlayback();
        handle({BrowseOp::Open, j.gen});
        if (back == ViewId::Detail || back == ViewId::LibraryGrid) {
            std::lock_guard<std::mutex> lk(mu_);
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
        if (view_ == ViewId::Home)
            logSidebarFocus(j.op == BrowseOp::Left ? "left" : "right");
        return;
    }

    if (j.op == BrowseOp::Up || j.op == BrowseOp::Down || j.op == BrowseOp::PageUp ||
        j.op == BrowseOp::PageDown) {
        const int dir = (j.op == BrowseOp::Down || j.op == BrowseOp::PageDown) ? 1 : -1;
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_)
            return;
        if (view_ == ViewId::ServerList) {
            focus_.row = std::max(0, std::min(static_cast<int>(servers_.size()) - 1,
                                                focus_.row + dir));
        } else if (view_ == ViewId::Home) {
            if (focus_.sidebar > 0)
                focus_.sidebar = std::max(
                    0, std::min(static_cast<int>(sidebarEntries_.size()), focus_.sidebar + dir));
            else
                focus_.row = std::max(0, focus_.row + dir);
            syncSidebarScroll();
            logSidebarFocus(dir > 0 ? "down" : "up");
        } else if (view_ == ViewId::LibraryGrid) {
            if (focus_.gridOnChips) {
                if (dir > 0) {
                    focus_.gridOnChips = false;
                    focus_.row = 0;
                }
            } else if (dir < 0 && focus_.row == 0) {
                focus_.gridOnChips = true;
                focus_.gridChip = gridActiveChip_;
                syncChipScroll();
            } else {
                focus_.row = std::max(0, focus_.row + dir);
                syncGridScroll();
            }
        } else if (view_ == ViewId::Detail) {
            focus_.detailButton = std::max(0, std::min(1, focus_.detailButton + dir));
        }
        bumpAnim();
        dirty_.store(true);
        return;
    }

    if (j.op == BrowseOp::Select || j.op == BrowseOp::Info) {
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
                server_ = servers_[static_cast<std::size_t>(serverIndex_)];
                haveServer_ = true;
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

        if (view_ == ViewId::Home) {
            if (focus_.sidebar > 0) {
                const int si = focus_.sidebar - 1;
                const auto& ent = sidebarEntries_[static_cast<std::size_t>(si)];
                const std::string key = ent.sectionKey;
                PlexServer srv = servers_[static_cast<std::size_t>(ent.serverIndex)];
                server_ = srv;
                serverIndex_ = ent.serverIndex;
                haveServer_ = true;
                gridSectionIndex_ = si;
                gridSectionTitle_ = ent.title;
                gridSectionKey_ = key;
                logSidebarFocus("select");
                grid_.clear();
                status_ = "LOADING...";
                navDeferVisible_.store(true);
                dirty_.store(true);
                lk.unlock();
                // XML is parsed in the fetch helpers, with mu_ released, then published below.
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
                if (!fetchSectionCollections(hooks_.get, srv, key, cancel, collections, err)) {
                    if (cancel())
                        stopLoading();
                    else {
                        std::lock_guard<std::mutex> lk2(mu_);
                        status_ = err;
                        dirty_.store(true);
                        navDeferVisible_.store(false);
                    }
                    return;
                }
                if (cancel()) {
                    stopLoading();
                    return;
                }
                if (!fetchSectionGenres(hooks_.get, srv, key, cancel, genres, err)) {
                    if (cancel())
                        stopLoading();
                    else {
                        std::lock_guard<std::mutex> lk2(mu_);
                        status_ = err;
                        dirty_.store(true);
                        navDeferVisible_.store(false);
                    }
                    return;
                }
                if (cancel()) {
                    stopLoading();
                    return;
                }
                const auto chips = buildGridChips(std::move(collections), std::move(genres));
                const bool ok = fetchSectionGrid(hooks_.get, srv, key, cancel, items, err);
                if (ok) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    gridChips_ = chips;
                    gridActiveChip_ = 0;
                    gridChipScrollX_ = 0;
                    grid_ = std::move(items);
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
                    server_ = srv;
                    for (size_t i = 0; i < servers_.size(); ++i) {
                        if (servers_[i].machineIdentifier == srv.machineIdentifier) {
                            serverIndex_ = static_cast<int>(i);
                            break;
                        }
                    }
                    detail_ = std::move(pick);
                    view_ = ViewId::Detail;
                    focus_.detailButton = 0;
                    dirty_.store(true);
                }
            }
            return;
        }

        if (view_ == ViewId::LibraryGrid) {
            if (focus_.gridOnChips) {
                const int idx = focus_.gridChip;
                if (idx < 0 || idx >= static_cast<int>(gridChips_.size()))
                    return;
                if (idx == gridActiveChip_)
                    return;
                PlexServer srv = server_;
                const std::string sectionKey = gridSectionKey_;
                const GridCategoryChip chip = gridChips_[static_cast<std::size_t>(idx)];
                gridActiveChip_ = idx;
                status_ = "LOADING...";
                dirty_.store(true);
                lk.unlock();
                std::vector<PlexItem> items;
                std::string err;
                if (loadGridForChip(chip, srv, sectionKey, cancel, items, err)) {
                    std::lock_guard<std::mutex> lk2(mu_);
                    grid_ = std::move(items);
                    ++dataGeneration_;
                    focus_.row = 0;
                    focus_.col = 0;
                    gridScrollRow_ = 0;
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
                view_ = ViewId::Detail;
                focus_.detailButton = 0;
                dirty_.store(true);
            }
            return;
        }

        if (view_ == ViewId::Detail) {
            int64_t off = 0;
            if (focus_.detailButton == 0 && detail_.viewOffsetMs > 0)
                off = detail_.viewOffsetMs;
            else if (focus_.detailButton == 1)
                off = 0;
            // Call the play hook with mu_ released: doPlay -> onPlayStarted takes mu_.
            const PlexItem item = detail_;
            lk.unlock();
            const bool started = startPlay(item, off);
            lk.lock();
            if (started)
                visible_ = false;
            dirty_.store(true);
            return;
        }
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

bool PlexUiController::loadGridForChip(const GridCategoryChip& chip, const PlexServer& server,
                                       const std::string& sectionKey, const BrowseCancelFn& cancel,
                                       std::vector<PlexItem>& items, std::string& err) {
    switch (chip.kind) {
    case GridCategoryChip::Kind::All:
        return fetchSectionGrid(hooks_.get, server, sectionKey, cancel, items, err);
    case GridCategoryChip::Kind::Genre:
        return fetchSectionGridGenre(hooks_.get, server, sectionKey, chip.filterId, cancel, items,
                                     err);
    case GridCategoryChip::Kind::Collection:
        return fetchCollectionChildren(hooks_.get, server, chip.filterId, cancel, items, err);
    }
    return false;
}

} // namespace plexui
} // namespace misterplex
