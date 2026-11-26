#include "libmisterplex/browse_shell_controller.hpp"

#include "libmisterplex/plexui/plexui_data.hpp"
#include "libmisterplex/plexui/plexui_http.hpp"
#include "libmisterplex/plexui/grid_page.hpp"
#include "third_party/stb_image.h"
#include "third_party/stb_image_resize2.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace misterplex {
namespace {

BrowseTitleRow itemToRow(const plexui::PlexItem& it) {
    BrowseTitleRow t;
    t.ratingKey = it.ratingKey;
    t.key = it.key;
    t.title = it.title;
    t.year = it.year;
    t.type = it.type;
    t.summary = it.summary;
    t.thumb = it.thumb;
    t.runtimeMs = it.runtimeMs;
    t.viewOffsetMs = it.viewOffsetMs;
    t.viewCount = it.viewCount;
    if (it.runtimeMs > 0 && it.viewOffsetMs > 0)
        t.progress = std::min(1.f, static_cast<float>(it.viewOffsetMs) / static_cast<float>(it.runtimeMs));
    return t;
}

BrowseServerRow serverToRow(const plexui::PlexServer& s) {
    BrowseServerRow r;
    r.id = !s.machineIdentifier.empty() ? s.machineIdentifier : s.clientIdentifier;
    r.name = s.name;
    r.locality = s.offline ? "local" : (plexui::serverLinkLabel(s) == "relay" ? "remote" : plexui::serverLinkLabel(s));
    if (r.locality == "Connecting..." || r.locality.empty())
        r.locality = "local";
    if (r.locality == "Offline")
        r.locality = "local";
    r.host = s.baseUrl;
    const auto slash = r.host.find("://");
    if (slash != std::string::npos) {
        auto host = r.host.substr(slash + 3);
        const auto cut = host.find('/');
        if (cut != std::string::npos)
            host = host.substr(0, cut);
        r.host = host;
    }
    if (s.offline) {
        r.status = "—";
        r.ok = false;
    } else if (s.baseUrl.empty()) {
        r.status = "…";
        r.ok = false;
    } else {
        r.status = "OK";
        r.ok = true;
    }
    r.baseUrl = s.baseUrl;
    r.token = s.accessToken;
    r.connectionBases = plexui::rankedConnectionBases(s);
    return r;
}

bool movieOrShow(const plexui::PlexSection& s) { return s.type == "movie" || s.type == "show"; }

std::string urlNoToken(const std::string& base, const std::string& path) {
    std::string u = base;
    if (!path.empty() && path[0] != '/')
        u.push_back('/');
    u += path;
    return u;
}

} // namespace

BrowseShellController::BrowseShellController(Config cfg, Hooks hooks)
    : cfg_(std::move(cfg)), hooks_(std::move(hooks)),
      disk_(cfg_.cacheDir.empty() ? browseShellDir() : cfg_.cacheDir, browseShellDiskCacheBytes()),
      ram_(browseShellRamCacheBytes()) {
    if (!hooks_.get)
        hooks_.get = plexui::makePlexuiHttpGet();
    std::string err;
    const bool fontLoaded = font_.load(cfg_.fontDir, err);
    if ((!fontLoaded || font_.usingFallback()) && hooks_.log)
        hooks_.log("browse-shell: " + err);
    relayoutLocked();
    model_.w = canvasW_;
    model_.h = canvasH_;
    model_.view = browseDefaultView(layout_);
    model_.search.keys = browseSearchKeys(layout_);
    model_.search.query.reserve(64);
    for (auto& line : msLines_)
        line.reserve(160);
    prepareFontsLocked();
}

BrowseShellController::~BrowseShellController() { stop(); }

void BrowseShellController::start(const std::function<void()>& nameThread) {
    std::lock_guard<std::mutex> lk(qMu_);
    if (worker_.joinable() || stopping_)
        return;
    worker_ = std::thread([this, nameThread] {
        if (nameThread)
            nameThread();
        run();
    });
}

void BrowseShellController::stop() {
    stopping_ = true;
    ++gen_;
    qCv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
        worker_.join();
    plexui::plexUiApiCacheDrain();
}

void BrowseShellController::post(BrowseOp op) { postIngress(op, "http", 0); }

void BrowseShellController::postHorizontal(int delta, const char* src, int64_t recvMs) {
    postIngress(delta > 0 ? BrowseOp::Right : BrowseOp::Left, src, recvMs);
}

void BrowseShellController::postSearch() {
    Job j;
    j.search = true;
    j.recvMs = browseShellNowMs();
    j.action = "search_key";
    enqueue(j);
}

void BrowseShellController::postText(char ch) {
    Job j;
    j.text = ch;
    j.recvMs = browseShellNowMs();
    j.action = "search_key";
    enqueue(j);
}

void BrowseShellController::postIngress(BrowseOp op, const char* src, int64_t recvMs) {
    Job j;
    j.op = op;
    j.recvMs = recvMs > 0 ? recvMs : browseShellNowMs();
    j.action = browseNavActionName(op);
    j.src = src && *src ? src : "http";
    if (op == BrowseOp::Select)
        j.action = "keypress";
    enqueue(j);
}

void BrowseShellController::enqueue(Job j) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        j.cancelLoading = model_.screen == BrowseShellScreen::Loading && j.op == BrowseOp::Back &&
                          !j.search && !j.text;
        if (model_.screen == BrowseShellScreen::Loading &&
            (j.search || j.text ||
             (j.op != BrowseOp::Back && j.op != BrowseOp::Open && j.op != BrowseOp::StopAndReopen)))
            return;
    }
    bool full = false;
    {
        std::lock_guard<std::mutex> lk(qMu_);
        if (stopping_)
            return;
        full = qSize_ == q_.size();
        if (!full) {
            j.gen = ++gen_;
            q_[(qHead_ + qSize_++) % q_.size()] = j;
        }
    }
    if (full)
        logLine("browse-shell: input queue full; key rejected");
    else
        qCv_.notify_one();
}

// Caller holds mu_ and model_ is not Search. Saves searchOrigin_ for Back.
void BrowseShellController::enterSearchLocked() {
    if (!model_.searchResults) {
        searchOrigin_ = std::move(model_);
        model_ = {};
        model_.w = canvasW_;
        model_.h = canvasH_;
        model_.view = searchOrigin_.view;
    }
    if (searchOrigin_.screen == BrowseShellScreen::Servers && !searchOrigin_.servers.empty()) {
        const auto& row = searchOrigin_.servers[static_cast<std::size_t>(
            browseClampIndex(searchOrigin_.focus, static_cast<int>(searchOrigin_.servers.size())))];
        server_.baseUrl = row.baseUrl;
        server_.accessToken = row.token;
        server_.machineIdentifier = row.id;
        server_.name = row.name;
    }
    model_.searchResults = false;
    model_.header = "SEARCH";
    model_.headerRight.clear();
    model_.search.keys = browseSearchKeys(layout_);
    model_.search.query.reserve(64);
    if (searchOrigin_.screen == BrowseShellScreen::Titles)
        model_.search.sectionId = section_.key;
    else
        model_.search.sectionId.clear();
    model_.search.scopeLabel = model_.search.sectionId.empty() ? "All libraries" : section_.title;
    model_.screen = BrowseShellScreen::Search;
    model_.search.region = BrowseSearchUi::Region::Field;
}

void BrowseShellController::requestRepaint(bool contentChanged) {
    if (contentChanged) {
        std::lock_guard<std::mutex> lk(mu_);
        ++contentEpoch_;
    }
    if (hooks_.repaint)
        hooks_.repaint();
}

void BrowseShellController::thumbRepaint(std::string_view ratingKey, int w, int h) {
    bool dirty = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& L = layout_;
        BrowseRect rect;
        if (model_.screen == BrowseShellScreen::Titles && model_.view == BrowseViewMode::Grid &&
            w == L.tileW && h == L.tileH) {
            const int end = std::min(static_cast<int>(model_.titles.size()),
                                    model_.scroll + browseViewportCount(L, model_.view));
            for (int i = model_.scroll; i < end; ++i) {
                if (model_.titles[static_cast<std::size_t>(i)].ratingKey != ratingKey)
                    continue;
                const auto tile = static_cast<std::size_t>(i - model_.scroll);
                const int x = L.tileX[tile], y = L.tileY[tile];
                rect = {x - L.focusRing, y - L.focusRing, x + w + L.focusRing, y + h + L.focusRing};
                dirty = true;
                break;
            }
        } else if (model_.screen == BrowseShellScreen::Titles && model_.view == BrowseViewMode::List &&
                   w == L.posterW && h == L.posterH && !model_.titles.empty() &&
                   model_.titles[static_cast<std::size_t>(model_.focus)].ratingKey == ratingKey) {
            const int x = canvasW_ - L.margin - L.posterW, y = L.headerH + 8;
            rect = {x - 1, y - 1, x + w + 1, y + h + 1};
            dirty = true;
        } else if (model_.screen == BrowseShellScreen::Detail && model_.detail.item.ratingKey == ratingKey &&
                   w == L.posterW * 2 / 3 && h == L.posterH * 2 / 3) {
            const int y = L.headerH + 4 + (L.compact ? 4 : 10);
            rect = {L.margin - 1, y - 1, L.margin + w + 1, y + h + 1};
            dirty = true;
        }
        if (dirty) {
            if (artDamageSize_ < artDamage_.size())
                artDamage_[artDamageSize_++] = rect;
            else
                ++contentEpoch_;
        }
    }
    if (dirty)
        requestRepaint(false);
}

void BrowseShellController::onPlayStarted(uint64_t) {
    ++gen_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        visible_ = false;
    }
    ram_.dropAll();
}

bool BrowseShellController::visible() const {
    std::lock_guard<std::mutex> lk(mu_);
    return visible_;
}

bool BrowseShellController::ownsSession() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ownsSession_;
}

bool BrowseShellController::searchOpen() const {
    std::lock_guard<std::mutex> lk(mu_);
    return visible_ && (model_.screen == BrowseShellScreen::Search || model_.searchResults);
}

bool BrowseShellController::paintReady() {
    std::lock_guard<std::mutex> lk(mu_);
    return visible_;
}

bool BrowseShellController::workerIdle() {
    std::lock_guard<std::mutex> lk(qMu_);
    return !qSize_ && !busy_.load();
}

BrowseRenderModel BrowseShellController::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return model_;
}

std::vector<std::string> BrowseShellController::browseMsLines() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(msSize_);
    for (std::size_t i = 0; i < msSize_; ++i)
        out.push_back(msLines_[(msHead_ + i) % msLines_.size()]);
    return out;
}

bool BrowseShellController::copyFrame(std::vector<std::uint8_t>& frame) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!visible_)
        return false;
    if (navTraceOpen_.load() && !activeNav_.renderStartMs)
        activeNav_.renderStartMs = browseShellNowMs();
    const int w = canvasW_, h = canvasH_;
    const auto& L = layout_;
    const auto& m = model_;
    const BrowsePaintThumbs thumbs{&ram_, server_.machineIdentifier};
    const auto rgbBytes = static_cast<std::size_t>(w) * h * 3;
    rgbFrame_.resize(rgbBytes);
    BrowseRect damage[32];
    int count = 0;
    auto add = [&](int x0, int y0, int x1, int y1) {
        damage[count++] = {std::max(0, x0), std::max(0, y0), std::min(w, x1), std::min(h, y1)};
    };
    const int top = L.headerH + 4;
    if (paintedEpoch_ != contentEpoch_ || painted_.screen != m.screen || painted_.view != m.view) {
        add(0, 0, w, h);
    } else if (painted_.scroll != m.scroll) {
        add(0, top, w, h);
    } else if (m.screen == BrowseShellScreen::Titles && m.view == BrowseViewMode::Grid &&
               painted_.focus != m.focus) {
        for (int focus : {painted_.focus, m.focus}) {
            const int i = focus - m.scroll;
            if (i < 0 || i >= browseViewportCount(L, m.view))
                continue;
            const int x = L.tileX[static_cast<std::size_t>(i)];
            const int y = L.tileY[static_cast<std::size_t>(i)];
            add(x - L.focusRing, y - L.focusRing, x + L.tileW + L.focusRing,
                y + L.tileH + (L.titleUnder ? L.fontSm + 5 : 0) + L.focusRing);
        }
        if (L.compact)
            add(0, h - L.footerH, w, h);
    } else if ((m.screen == BrowseShellScreen::Servers || m.screen == BrowseShellScreen::Libraries ||
                m.screen == BrowseShellScreen::Titles) && painted_.focus != m.focus) {
        const int right = m.screen == BrowseShellScreen::Titles && L.posterW > 0 ?
                          w - L.margin - L.posterW - L.margin / 2 : w - L.margin;
        for (int focus : {painted_.focus, m.focus}) {
            const int row = focus - m.scroll;
            if (row < 0 || row >= L.listRows)
                continue;
            const int y = L.rowY[static_cast<std::size_t>(row)];
            add(L.margin, y, right, y + L.rowH);
        }
        if (m.screen == BrowseShellScreen::Titles && L.posterW > 0)
            add(right, top, w, h - L.footerH);
    } else if (m.screen == BrowseShellScreen::Search) {
        const int fieldY = top + 4, fieldH = L.rowH + (L.compact ? 2 : 4);
        const bool regionChanged = painted_.region != m.search.region;
        if (painted_.querySize != m.search.query.size() ||
            std::memcmp(painted_.query.data(), m.search.query.data(), painted_.querySize) != 0 || regionChanged)
            add(L.margin - 1, fieldY - 1, w - L.margin + 1, fieldY + fieldH + 1);
        if (painted_.kbIndex != m.search.kbIndex || regionChanged)
            for (int index : {painted_.kbIndex, m.search.kbIndex}) {
                if (index < 0 || index >= static_cast<int>(L.keyX.size()))
                    continue;
                const int x = L.keyX[static_cast<std::size_t>(index)];
                const int y = L.keyY[static_cast<std::size_t>(index)];
                add(x - 1, y - 1, x + L.kbCell + 1, y + L.kbCell + 1);
            }
        const int resultsY = L.searchResultsY;
        if (painted_.resultScroll != m.search.resultScroll)
            add(0, resultsY, w, h - L.footerH);
        else if (painted_.resultFocus != m.search.resultFocus || regionChanged)
            for (int focus : {painted_.resultFocus, m.search.resultFocus}) {
                const int y = resultsY + (focus - m.search.resultScroll) * L.rowH;
                add(L.margin, y, w - L.margin, y + L.rowH);
            }
    } else if (m.screen == BrowseShellScreen::Detail && painted_.button != m.detail.button) {
        add(0, h - L.footerH - L.btnH - (L.compact ? 4 : 10) - 1, w, h - L.footerH);
    }
    for (std::size_t i = 0; i < artDamageSize_; ++i)
        add(artDamage_[i].x0, artDamage_[i].y0, artDamage_[i].x1, artDamage_[i].y1);
    artDamageSize_ = 0;
    if (fmt_ != plexui::PixelFormat::RGB24)
        packedFrame_.resize(static_cast<std::size_t>(w) * h * 4);
    for (int r = 0; r < count; ++r) {
        const auto& rect = damage[r];
        paintBrowseShell(m, L, font_, rgbFrame_.data(), w, h, &thumbs, &rect);
        if (fmt_ != plexui::PixelFormat::RGB24)
            for (int y = rect.y0; y < rect.y1; ++y)
                for (int x = rect.x0; x < rect.x1; ++x) {
                    const auto i = static_cast<std::size_t>(y) * w + x;
                    packedFrame_[i * 4] = rgbFrame_[i * 3 + 2];
                    packedFrame_[i * 4 + 1] = rgbFrame_[i * 3 + 1];
                    packedFrame_[i * 4 + 2] = rgbFrame_[i * 3];
                    packedFrame_[i * 4 + 3] = 255;
                }
    }
    paintedEpoch_ = contentEpoch_;
    painted_ = {m.screen, m.view, m.focus, m.scroll, m.search.kbIndex,
                m.search.resultFocus, m.search.resultScroll};
    painted_.region = m.search.region;
    painted_.button = m.detail.button;
    painted_.querySize = std::min(m.search.query.size(), painted_.query.size());
    std::memcpy(painted_.query.data(), m.search.query.data(), painted_.querySize);
    lastDamage_.count = 0;
    lastDamage_.full = (count <= 0);
    for (int r = 0; r < count && lastDamage_.count < static_cast<int>(lastDamage_.rects.size()); ++r) {
        const auto& rect = damage[r];
        if (rect.x0 <= 0 && rect.y0 <= 0 && rect.x1 >= w && rect.y1 >= h) {
            lastDamage_.full = true;
            lastDamage_.count = 0;
            break;
        }
        lastDamage_.rects[lastDamage_.count++] = rect;
    }
    const auto& pixels = fmt_ == plexui::PixelFormat::RGB24 ? rgbFrame_ : packedFrame_;
    const int bpp = fmt_ == plexui::PixelFormat::RGB24 ? 3 : 4;
#ifdef MUT_NAV_FULL_FRAME_COPY
    frame.resize(pixels.size());
    std::memcpy(frame.data(), pixels.data(), pixels.size());
#else
    if (frame.size() != pixels.size() || lastDamage_.full || lastDamage_.count <= 0) {
        frame.resize(pixels.size());
        std::memcpy(frame.data(), pixels.data(), pixels.size());
    } else {
        // Full dirty rows, not x-clipped: writeDirtyRows blits whole rows from this
        // dest onto the live page. Ping-pong dest is zeros besides these rows; an
        // x-clipped copy left 28 px of 0 in HDMI margins (review 6a2ed6a5).
        const std::size_t nbytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(bpp);
        for (int r = 0; r < lastDamage_.count; ++r) {
            const auto& rect = lastDamage_.rects[r];
            const int y0 = std::max(0, rect.y0), y1 = std::min(h, rect.y1);
            for (int y = y0; y < y1; ++y) {
                const std::size_t off = static_cast<std::size_t>(y) * nbytes;
                std::memcpy(frame.data() + off, pixels.data() + off, nbytes);
            }
        }
    }
#endif
    ++renderedFrame_;
    if (navTraceOpen_.load())
        activeNav_.renderEndMs = browseShellNowMs();
    return true;
}

bool BrowseShellController::setRenderTarget(int w, int h, plexui::PixelFormat fmt, bool interlaced) {
    std::lock_guard<std::mutex> lk(mu_);
    w = std::max(160, std::min(1920, w));
    h = std::max(120, std::min(1080, h));
    const bool changed = w != canvasW_ || h != canvasH_ || fmt != fmt_ || interlaced != interlaced_;
    if (!changed)
        return false;
    canvasW_ = w;
    canvasH_ = h;
    fmt_ = fmt;
    interlaced_ = interlaced;
    relayoutLocked();
    ++contentEpoch_;
    model_.w = canvasW_;
    model_.h = canvasH_;
    if (model_.titles.empty())
        model_.view = browseDefaultView(layout_);
    if (interlaced_)
        model_.view = BrowseViewMode::List;
    browseEnsureFocusVisible(model_, layout_);
    model_.search.keys = browseSearchKeys(layout_);
    prepareFontsLocked();
    return changed;
}

plexui::PixelFormat BrowseShellController::renderFormat() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_;
}

void BrowseShellController::applyUiSafeLayout(int marginPct, UiSize size) {
    std::lock_guard<std::mutex> lk(mu_);
    marginPct = std::max(0, std::min(10, marginPct));
    if (marginPct == marginPct_ && size == uiSize_)
        return;
    marginPct_ = marginPct;
    uiSize_ = size;
    relayoutLocked();
    ++contentEpoch_;
    browseEnsureFocusVisible(model_, layout_);
    prepareFontsLocked();
}

void BrowseShellController::prepareTitleLocked(const BrowseTitleRow& t) {
    font_.measureText(t.title, layout_.fontMd, false);
    font_.measureText(t.title, layout_.fontSm, false);
    font_.measureText(t.title, layout_.fontXl, true);
    font_.measureText(t.year, layout_.fontSm, false);
    font_.measureText(t.type, layout_.fontSm, false);
    font_.measureText(t.summary, layout_.fontMd, false);
}

void BrowseShellController::prepareFontsLocked() {
    const std::string_view common =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~\xE2\x80\xA6\xE2\x80\x94\xC2\xB7";
    for (int size : {layout_.fontSm, layout_.fontMd, layout_.fontLg, layout_.fontXl,
                     std::max(8, layout_.fontSm - 1), std::max(10, layout_.tileW / 4),
                     std::max(10, layout_.posterW / 4), std::max(10, (layout_.posterW * 2 / 3) / 4)}) {
        font_.measureText(common, size, false);
        font_.measureText(common, size, true);
    }
    for (const auto* s : {&model_.header, &model_.libraryName})
        font_.measureText(*s, layout_.fontLg, true);
    for (const auto* s : {&model_.headerRight, &model_.serverName, &model_.footer, &model_.status,
                         &model_.search.scopeLabel})
        font_.measureText(*s, layout_.fontSm, false);
    for (const auto& s : model_.servers) {
        font_.measureText(s.name, layout_.fontMd, false);
        font_.measureText(s.locality, layout_.fontSm, false);
        font_.measureText(s.host, layout_.fontSm, false);
        font_.measureText(s.status, layout_.fontSm, false);
    }
    for (const auto& l : model_.libraries) {
        font_.measureText(l.title, layout_.fontMd, false);
        font_.measureText(l.title, layout_.fontSm, false);
        font_.measureText(l.title, layout_.fontLg, true);
    }
    for (const auto& t : model_.titles)
        prepareTitleLocked(t);
    for (const auto& t : model_.search.results)
        prepareTitleLocked(t);
    prepareTitleLocked(model_.detail.item);
}

void BrowseShellController::relayoutLocked() {
    layout_ = browseLayoutFor(canvasW_, canvasH_, interlaced_);
    layout_.margin = std::max(layout_.margin, std::min(canvasW_, canvasH_) * marginPct_ / 100);
    if (uiSize_ == UiSize::Large) {
        layout_.fontSm += 2;
        layout_.fontMd += 2;
        layout_.fontLg += 2;
        layout_.fontXl += 2;
        layout_.rowH += 4;
        layout_.listRows = std::min(layout_.listRows, std::max(1,
            (canvasH_ - layout_.headerH - layout_.footerH - 8) / layout_.rowH));
    }
    browseBakeLayout(layout_);
}

void BrowseShellController::run() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(qMu_);
            if (thumbRetryAt_ > 0)
                qCv_.wait_until(lk, std::chrono::steady_clock::time_point(
                    std::chrono::milliseconds(thumbRetryAt_)), [this] { return stopping_ || qSize_; });
            else
                qCv_.wait(lk, [this] { return stopping_ || qSize_; });
            if (stopping_)
                return;
            if (qSize_) {
                j = q_[qHead_];
                qHead_ = (qHead_ + 1) % q_.size();
                --qSize_;
            } else {
                j.gen = gen_.load();
                j.recvMs = -1;
            }
            busy_ = true;
        }
        if (j.recvMs >= 0)
            handle(j);
        const uint64_t g = gen_.load();
        {
            std::lock_guard<std::mutex> lk(qMu_);
            if (qSize_) {
                busy_ = false;
                continue;
            }
        }
        if (serversRefreshPending_.load()) {
            refreshServersFromNetwork(g);
            {
                std::lock_guard<std::mutex> lk(qMu_);
                if (qSize_) {
                    busy_ = false;
                    continue;
                }
            }
        }
        // Visible posters first (review #3), then catalog pages, then thumbs
        // for the pages that just arrived / the next viewport.
        if (!cancelFor(g)())
            prefetchThumbs(g);
        while (!cancelFor(g)() && prefetchNext(g)) {}
        if (!cancelFor(g)())
            prefetchThumbs(g);
        busy_ = false;
    }
}

void BrowseShellController::handle(const Job& j) {
    if (stopping_)
        return;
    const int64_t t0 = j.recvMs > 0 ? j.recvMs : browseShellNowMs();
    if (j.cancelLoading) {
        requestRepaint();
        logMs("keypress", t0);
        return;
    }
    if (j.search) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            visible_ = true;
            if (model_.screen == BrowseShellScreen::Search)
                return;
            enterSearchLocked();
        }
        requestRepaint();
        logMs("search_key", t0);
        return;
    }
    if (j.text) {
        if (j.text == '\n') {
            requestRepaint(false);
            logMs("search_key", t0);
            runSearch(j.gen, true);
            requestRepaint();
            return;
        }
        bool fetch = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (model_.screen != BrowseShellScreen::Search) {
                // Typing opens Search like the Search key: save searchOrigin_ so
                // Back returns to the screen we came from (review #5).
#ifdef MUT_BROWSE_TEXT_NO_ORIGIN
                model_.screen = BrowseShellScreen::Search;
                model_.search.keys = browseSearchKeys(layout_);
                model_.search.query.reserve(64);
#else
                enterSearchLocked();
#endif
                ++contentEpoch_;
            }
            const auto oldResults = model_.search.results.size();
            browseSearchType(model_.search, j.text, t0);
            if (oldResults != model_.search.results.size())
                ++contentEpoch_;
            fetch = model_.search.pending;
        }
        requestRepaint(false);
        logMs("search_key", t0);
        if (fetch)
            runSearch(j.gen);
        requestRepaint(false);
        return;
    }
    if (j.op == BrowseOp::StopAndReopen) {
        if (hooks_.playing && hooks_.playing() && hooks_.stopPlayback)
            hooks_.stopPlayback();
        {
            std::lock_guard<std::mutex> lk(mu_);
            visible_ = true;
            ownsSession_ = false;
        }
        requestRepaint();
        logMs("keypress", t0);
        return;
    }
    handleNav(j.op, t0, j.action, j.src);
}

void BrowseShellController::beginNavTrace(const char* src, int64_t recvMs) {
    NavTrace nt{};
    nt.id = ++navSeq_;
    nt.src = src && *src ? src : "ring_in";
    nt.recvMs = recvMs;
    nt.postMs = browseShellNowMs();
    {
        std::lock_guard<std::mutex> lk(mu_);
        activeNav_ = nt;
    }
    navTraceOpen_.store(true);
}

void BrowseShellController::logNavTrace(int64_t plxhMs, int64_t plxoMs) {
#ifndef MUT_NAV_NO_SHELL_NAVTRACE
    if (!navTraceOpen_.exchange(false))
        return;
    NavTrace nt;
    {
        std::lock_guard<std::mutex> lk(mu_);
        nt = activeNav_;
    }
    const int64_t total = plxoMs > 0 ? plxoMs - nt.recvMs : browseShellNowMs() - nt.recvMs;
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "plexui: navtrace id=%llu src=%s recv=%lld post=%lld state=%lld "
                  "render_start=%lld render_end=%lld plxh=%lld plxo=%lld total=%lld",
                  static_cast<unsigned long long>(nt.id), nt.src, static_cast<long long>(nt.recvMs),
                  static_cast<long long>(nt.postMs), static_cast<long long>(nt.stateMs),
                  static_cast<long long>(nt.renderStartMs), static_cast<long long>(nt.renderEndMs),
                  static_cast<long long>(plxhMs), static_cast<long long>(plxoMs),
                  static_cast<long long>(total));
    logLine(buf);
#else
    (void)plxhMs;
    (void)plxoMs;
    navTraceOpen_.store(false);
#endif
}

void BrowseShellController::handleNav(BrowseOp op, int64_t recvMs, const char* action, const char* src) {
    beginNavTrace(src, recvMs);
    BrowseShellNav nav;
    bool needServers = false, needLibs = false, needTitles = false, needSearch = false, needPlay = false;
    bool needChildren = false;
    bool timingLogged = false;
    int pageTarget = -1;
    BrowseTitleRow playItem;
    int64_t playOff = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if ((op == BrowseOp::Back || op == BrowseOp::Open) &&
            !cursorKey_.empty() && model_.screen == BrowseShellScreen::Titles &&
            !model_.childList && !model_.searchResults) {
            auto& saved = cursors_[cursorKey_];
            saved = std::move(model_);
            model_ = {};
            model_.w = canvasW_;
            model_.h = canvasH_;
            model_.screen = BrowseShellScreen::Titles;
            model_.view = saved.view;
            model_.servers = std::move(saved.servers);
            model_.libraries = std::move(saved.libraries);
            model_.serverFocus = saved.serverFocus;
            model_.libraryFocus = saved.libraryFocus;
            model_.serverName = saved.serverName;
        }
        if (op == BrowseOp::Back && model_.screen == BrowseShellScreen::Servers)
            visible_ = false;
        if (op == BrowseOp::Back && (model_.screen == BrowseShellScreen::Search ||
                                    (model_.screen == BrowseShellScreen::Titles && model_.searchResults))) {
            model_ = std::move(searchOrigin_);
            model_.search.query.clear();
            model_.search.pending = false;
        } else if (op == BrowseOp::Back && model_.screen == BrowseShellScreen::Titles &&
                   model_.childList && !parents_.empty()) {
            model_ = std::move(parents_.back());
            parents_.pop_back();
        } else {
        if (model_.screen == BrowseShellScreen::Titles && !model_.childList && !model_.searchResults) {
            int delta = 0;
            if (op == BrowseOp::PageDown)
                delta = browsePageDelta(layout_, model_.view);
            else if (op == BrowseOp::Down)
                delta = model_.view == BrowseViewMode::Grid ? layout_.gridCols : 1;
            else if (op == BrowseOp::Right && model_.view == BrowseViewMode::Grid)
                delta = 1;
            const int target = browseClampIndex(model_.focus + delta, model_.totalTitles);
            if (delta > 0 && target >= static_cast<int>(model_.titles.size()))
                pageTarget = target;
        }
        if (op == BrowseOp::Open) {
            visible_ = true;
            if (hooks_.playing && hooks_.playing() && hooks_.stopPlayback) {
                // stop outside lock
                needServers = true;
            } else {
                needServers = true;
            }
        }
        const auto oldResults = model_.search.results.size();
        browseApplyNav(model_, layout_, op, recvMs, &nav);
        if (oldResults != model_.search.results.size())
            ++contentEpoch_;
        needServers = needServers || nav.wantServers;
        needLibs = nav.wantLibraries;
        needTitles = nav.wantTitles;
        needSearch = nav.wantSearchFetch;
        if (model_.screen == BrowseShellScreen::Search &&
            model_.search.region == BrowseSearchUi::Region::Field &&
            searchOrigin_.screen == BrowseShellScreen::Titles &&
            (op == BrowseOp::Left || op == BrowseOp::Right)) {
            auto& s = model_.search;
            s.sectionId = s.sectionId.empty() ? section_.key : "";
            s.scopeLabel = s.sectionId.empty() ? "All libraries" : section_.title;
            s.pending = !s.query.empty();
            s.lastChangeMs = recvMs;
            needSearch = s.pending;
            ++contentEpoch_;
        }
        needPlay = nav.wantPlay;
        needChildren = nav.wantChildren;
        if (needChildren)
            playItem = model_.screen == BrowseShellScreen::Search
                           ? model_.search.results[static_cast<std::size_t>(nav.playIndex)]
                           : model_.titles[static_cast<std::size_t>(nav.playIndex)];
        if (needPlay) {
            if (model_.screen == BrowseShellScreen::Detail) {
                playItem = model_.detail.item;
                if (model_.detail.button == 0 && model_.detail.inProgress)
                    playOff = playItem.viewOffsetMs;
                else
                    playOff = 0;
            } else if (model_.screen == BrowseShellScreen::Search) {
                if (nav.playIndex >= 0 && nav.playIndex < static_cast<int>(model_.search.results.size()))
                    playItem = model_.search.results[static_cast<std::size_t>(nav.playIndex)];
            } else if (nav.playIndex >= 0 && nav.playIndex < static_cast<int>(model_.titles.size())) {
                playItem = model_.titles[static_cast<std::size_t>(nav.playIndex)];
            }
        }
        model_.generation = gen_.load();
        model_.fetchCount = fetchCount_.load();
        }
        if (navTraceOpen_.load())
            activeNav_.stateMs = browseShellNowMs();
    }
    if (op == BrowseOp::Open && hooks_.playing && hooks_.playing() && hooks_.stopPlayback)
        hooks_.stopPlayback();

    const uint64_t g = gen_.load();
    const char* act = action ? action : "keypress";
    if (needPlay) {
        act = "title_play";
        startPlay(playItem, playOff);
        logMs(act, recvMs);
        return;
    }
    if (pageTarget >= 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            model_.screen = BrowseShellScreen::Loading;
            model_.loadingPct = 30;
            model_.status = "next titles";
        }
        requestRepaint();
        while (!cancelFor(g)()) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (static_cast<int>(model_.titles.size()) > pageTarget)
                    break;
            }
            if (!prefetchNext(g, pageTarget))
                break;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            model_.screen = BrowseShellScreen::Titles;
            if (!cancelFor(g)()) {
                model_.focus = browseClampIndex(pageTarget, static_cast<int>(model_.titles.size()));
                browseEnsureFocusVisible(model_, layout_);
                if (model_.focus == pageTarget)
                    model_.status.clear();
            }
        }
    } else if (needServers)
        openServers(g, recvMs);
    else if (needLibs)
        openLibraries(g);
    else if (needTitles)
        openTitles(g);
    else if (needChildren)
        openChildren(g, playItem);
    else if (needSearch) {
        bool empty;
        {
            std::lock_guard<std::mutex> lk(mu_);
            empty = model_.search.query.empty();
        }
        if (empty) {
            std::lock_guard<std::mutex> lk(mu_);
            model_ = std::move(searchOrigin_);
            ++contentEpoch_;
        } else {
            requestRepaint(false);
            logMs("search_key", recvMs);
            timingLogged = true;
            runSearch(g, nav.wantSearchCommit);
        }
    }

    requestRepaint(needServers || needLibs || needTitles || needChildren || pageTarget >= 0 ||
                   op == BrowseOp::Back || op == BrowseOp::Open);
    if (needTitles || needLibs)
        act = "server_titles";
    if (!needServers && !timingLogged)
        logMs(act, recvMs);
}

void BrowseShellController::openServers(uint64_t gen, int64_t recvMs) {
    (void)gen;
    std::vector<BrowseServerRow> cached;
    std::string lastUsed;
    std::string json;
    if (disk_.loadServersJson(json))
        cached = browseServersFromJson(json, lastUsed);

    std::vector<BrowseServerRow> seeds;
    auto localSeeds = cfg_.seedServers;
    for (const auto& s : localSeeds)
        seeds.push_back(serverToRow(s));
    for (auto& row : cached) {
        for (const auto& seed : seeds) {
            if (seed.id == row.id || seed.baseUrl == row.baseUrl)
                row.token = seed.token;
        }
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        visible_ = true;
        if (!cached.empty()) {
            browseSortServers(cached);
            model_.servers = cached;
            model_.screen = BrowseShellScreen::Servers;
            model_.header = "SERVERS";
            model_.headerRight = "[Search]";
        } else if (!seeds.empty()) {
            model_.servers = seeds;
            model_.screen = BrowseShellScreen::Servers;
        } else {
            model_.screen = BrowseShellScreen::Loading;
            model_.loadingPct = 10;
            model_.status = "servers";
        }
        prepareFontsLocked();
    }
    requestRepaint();
    logMs("keypress", recvMs);
    // Return so queued ring_in keys run before the plex.tv / identity probe
    // (glass 379146af: post 4-49 s after recv while this worker sat in
    // resolveServerBases). refreshServersFromNetwork runs once the queue drains.
    serversRefreshPending_.store(true);
}

void BrowseShellController::refreshServersFromNetwork(uint64_t gen) {
    if (!serversRefreshPending_.exchange(false))
        return;
    const BrowseCancelFn stop = [this] { return stopping_.load(); };
    const BrowseCancelFn probeCancel = cancelFor(gen);
    std::vector<BrowseServerRow> cached;
    std::string lastUsed;
    std::string json;
    if (disk_.loadServersJson(json))
        cached = browseServersFromJson(json, lastUsed);
    auto localSeeds = cfg_.seedServers;
    std::vector<BrowseServerRow> seeds;
    for (const auto& s : localSeeds)
        seeds.push_back(serverToRow(s));

    if (stop())
        return;
    if (localSeeds.empty() && cached.empty() && cfg_.discoverLocal)
        localSeeds = cfg_.discoverLocal();

    std::vector<plexui::PlexServer> account;
    if (hooks_.get && !cfg_.accountToken.empty()) {
        fetchCount_.fetch_add(1);
        const auto r = hooks_.get(plexui::appendToken(
            "https://plex.tv/api/resources?includeHttps=1&includeRelay=1", cfg_.accountToken),
            2000, 6000, stop);
        if (stop()) {
            std::lock_guard<std::mutex> lk(mu_);
            if (model_.screen == BrowseShellScreen::Loading)
                model_.screen = BrowseShellScreen::Servers;
            return;
        }
        if (!r.ok()) {
            logLine("browse-shell: resources fetch failed status=" + std::to_string(r.status));
            std::lock_guard<std::mutex> lk(mu_);
            model_.screen = BrowseShellScreen::Servers;
            model_.status = "Server refresh failed; using cached servers";
            return;
        }
        account = plexui::parsePlexResources(r.body);
        plexui::pickServerConnections(account);
        // #331: prefer mergeSeedsWithAccount so tokenless/account rows do not steal a seed token by host:port.
        account = plexui::mergeSeedsWithAccount(localSeeds, account);
#ifndef MUT_BROWSE_NO_PROBE
        // Identity probe of unreachable hosts is what queued ring_in for tens of
        // seconds (glass 379146af). Yield it when a newer key has arrived; still
        // apply the resources rows so Select can use the fresh token (P1-1).
        if (hooks_.get && !probeCancel())
            plexui::resolveServerBases(hooks_.get, account, probeCancel);
#endif
    } else {
        account = localSeeds;
#ifndef MUT_BROWSE_NO_PROBE
        if (hooks_.get && !account.empty() && !probeCancel())
            plexui::resolveServerBases(hooks_.get, account, probeCancel);
#endif
    }
    if (stop())
        return;
    std::vector<BrowseServerRow> rows;
    for (const auto& s : account)
        rows.push_back(serverToRow(s));
    if (rows.empty())
        rows = seeds;
    if (!lastUsed.empty()) {
        lastUsedId_ = lastUsed;
        for (auto& r : rows)
            r.lastUsed = r.id == lastUsedId_;
    }
    browseSortServers(rows);
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string focusedId;
        if (!model_.servers.empty())
            focusedId = model_.servers[static_cast<std::size_t>(
                browseClampIndex(model_.focus, static_cast<int>(model_.servers.size())))].id;
        // Glass 63332105: refresh finished after TITLE ENTER and reset screen=Servers /
        // visible=true, so play never left navigation. Keep Libraries/Titles/play.
        const bool stay =
#ifndef MUT_BROWSE_TITLE_PLAY_NOPICTURE
            !visible_ || model_.screen == BrowseShellScreen::Libraries ||
            model_.screen == BrowseShellScreen::Titles ||
            model_.screen == BrowseShellScreen::Detail ||
            model_.screen == BrowseShellScreen::Search;
#else
            false;
#endif
        model_.servers = rows;
        if (!stay) {
            model_.screen = BrowseShellScreen::Servers;
            model_.loadingPct = 100;
            model_.focus = 0;
            for (std::size_t i = 0; i < rows.size(); ++i)
                if (rows[i].id == focusedId)
                    model_.focus = static_cast<int>(i);
#ifndef MUT_BROWSE_TITLE_PLAY_NOPICTURE
            browseSnapFocusToUsableServer(model_);
#endif
            browseEnsureFocusVisible(model_, layout_);
            model_.status.clear();
            visible_ = true;
            if (rows.empty())
                model_.status = "No servers. Configure PLEX_SERVERS or sign in with the legacy UI.";
        }
        prepareFontsLocked();
    }
    if (!disk_.saveServersJson(browseServersToJson(rows, lastUsedId_)))
        logLine("browse-shell: cannot save server cache");
    requestRepaint();
}

void BrowseShellController::openLibraries(uint64_t gen) {
    plexui::PlexServer srv;
    std::vector<std::string> alts;
    [[maybe_unused]] int serverFocus = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (model_.servers.empty())
            return;
        int i = browseClampIndex(model_.focus, static_cast<int>(model_.servers.size()));
#ifndef MUT_BROWSE_TITLE_PLAY_NOPICTURE
        if (!browseServerRowUsable(model_.servers[static_cast<std::size_t>(i)])) {
            const int pick = browseFirstUsableServerIndex(model_.servers);
            if (pick >= 0) {
                i = pick;
                model_.focus = pick;
                browseEnsureFocusVisible(model_, layout_);
            }
        }
#endif
        const auto& row = model_.servers[static_cast<std::size_t>(i)];
        if (row.token.empty() && !cfg_.accountToken.empty()) {
            model_.status = "Server credentials unavailable; reopen Browse to refresh";
            ++contentEpoch_;
            return;
        }
        if (!browseServerRowUsable(row)) {
#ifndef MUT_BROWSE_TITLE_PLAY_NOPICTURE
            model_.status = row.name.empty() ? "Server not reachable" : (row.name + ": no reachable address");
            ++contentEpoch_;
#endif
            return;
        }
        srv.name = row.name;
        srv.baseUrl = row.baseUrl;
        srv.accessToken = row.token;
        srv.machineIdentifier = row.id;
        alts = row.connectionBases;
        serverFocus = i;
        lastUsedId_ = row.id;
        model_.serverFocus = i;
        model_.screen = BrowseShellScreen::Loading;
        model_.loadingPct = 20;
        model_.status = "libraries";
        model_.serverName = row.name;
        visible_ = true;
    }
    requestRepaint();
    const BrowseCancelFn cancel = cancelFor(gen);
    fetchCount_.fetch_add(1);
    std::vector<plexui::PlexSection> sections;
    std::string err;
    bool ok = hooks_.get && plexui::fetchLibrarySections(hooks_.get, srv, cancel, sections, err);
#ifndef MUT_BROWSE_NO_SELECT_RETRY
    // Cheap (b): probe may keep a base that later fails /library/sections. Retry remaining
    // connections (incl. relay) on transport/no-response, then persist the winner.
    if (!ok && hooks_.get && !cancel() && plexui::isLibrarySectionsTransportError(err)) {
        logLine("browse-shell: libraries failed server=" + srv.name + " base=" + srv.baseUrl +
                " status=" + err + " (will retry alts)");
        for (const auto& base : alts) {
            if (base.empty() || base == srv.baseUrl)
                continue;
            if (cancel())
                break;
            plexui::PlexServer trySrv = srv;
            trySrv.baseUrl = base;
            std::string err2;
            std::vector<plexui::PlexSection> sections2;
            fetchCount_.fetch_add(1);
            if (plexui::fetchLibrarySections(hooks_.get, trySrv, cancel, sections2, err2)) {
                srv = std::move(trySrv);
                sections = std::move(sections2);
                err.clear();
                ok = true;
                plexui::rememberServerBase(srv.machineIdentifier, srv.baseUrl);
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    if (serverFocus >= 0 &&
                        serverFocus < static_cast<int>(model_.servers.size()) &&
                        model_.servers[static_cast<std::size_t>(serverFocus)].id == srv.machineIdentifier) {
                        auto& row = model_.servers[static_cast<std::size_t>(serverFocus)];
                        row.baseUrl = srv.baseUrl;
                        row.host = srv.baseUrl;
                        const auto slash = row.host.find("://");
                        if (slash != std::string::npos) {
                            auto host = row.host.substr(slash + 3);
                            const auto cut = host.find('/');
                            if (cut != std::string::npos)
                                host = host.substr(0, cut);
                            row.host = host;
                        }
                        row.status = "OK";
                        row.ok = true;
                    }
                }
                logLine("browse-shell: libraries retry ok server=" + srv.name + " base=" + srv.baseUrl);
                break;
            }
            if (!cancel()) {
                logLine("browse-shell: libraries failed server=" + srv.name + " base=" + base +
                        " status=" + err2);
                err = err2;
            }
        }
    }
#endif
    if (!ok) {
        if (!cancel()) {
            logLine("browse-shell: libraries failed server=" + srv.name + " base=" + srv.baseUrl +
                    " status=" + err);
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (cancel()) {
            model_.status = "";
        } else if (!err.empty()) {
            model_.status = err;
        } else {
            model_.status = srv.name + ": no reachable address";
        }
        model_.screen = BrowseShellScreen::Servers;
        return;
    }
    std::vector<BrowseLibraryRow> libs;
    for (const auto& s : sections) {
        if (!movieOrShow(s))
            continue;
        BrowseLibraryRow r;
        r.key = s.key;
        r.title = s.title;
        r.type = s.type;
        libs.push_back(std::move(r));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cancel()) {
            model_.screen = BrowseShellScreen::Servers;
            return;
        }
        server_ = srv;
        failedThumbs_.clear();
        thumbRetryAt_ = 0;
        model_.libraries = std::move(libs);
        model_.screen = BrowseShellScreen::Libraries;
        model_.header = "LIBRARIES";
        model_.headerRight = srv.name;
        model_.focus = 0;
        model_.scroll = 0;
        model_.loadingPct = 100;
        model_.status = model_.libraries.empty() ? "No movie or TV libraries" : "";
        prepareFontsLocked();
    }
    if (!disk_.saveServersJson(browseServersToJson(snapshot().servers, lastUsedId_)))
        logLine("browse-shell: cannot save last-used server");
}

void BrowseShellController::openTitles(uint64_t gen) {
    plexui::PlexServer srv;
    plexui::PlexSection sec;
    BrowseShellLayout L;
    BrowseViewMode view;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (model_.libraries.empty())
            return;
        const int i = browseClampIndex(model_.focus, static_cast<int>(model_.libraries.size()));
        const auto& lib = model_.libraries[static_cast<std::size_t>(i)];
        model_.libraryFocus = i;
        sec.key = lib.key;
        sec.title = lib.title;
        sec.type = lib.type;
        srv = server_;
        L = layout_;
        view = model_.view;
        cursorKey_ = srv.machineIdentifier + "|" + lib.key;
        const auto saved = cursors_.find(cursorKey_);
        parents_.clear();
        if (saved != cursors_.end()) {
            saved->second.servers = std::move(model_.servers);
            saved->second.libraries = std::move(model_.libraries);
            model_ = std::move(saved->second);
            cursors_.erase(saved);
            model_.w = canvasW_;
            model_.h = canvasH_;
            if (interlaced_)
                model_.view = BrowseViewMode::List;
            browseEnsureFocusVisible(model_, layout_);
            prepareFontsLocked();
            section_ = sec;
            return;
        }
        model_.screen = BrowseShellScreen::Loading;
        model_.loadingPct = 30;
        model_.libraryName = lib.title;
        model_.status = "titles";
    }
    requestRepaint();
    const BrowseCancelFn cancel = cancelFor(gen);
    if (!hooks_.get || srv.baseUrl.empty()) {
        std::lock_guard<std::mutex> lk(mu_);
        model_.screen = BrowseShellScreen::Libraries;
        return;
    }
    const int vp = browseViewportCount(L, view);
    const int firstN = std::max(60, vp * 2);
    std::string path = "/library/sections/" + sec.key +
                       "/all?sort=titleSort&X-Plex-Container-Start=0&X-Plex-Container-Size=" +
                       std::to_string(firstN);
    fetchCount_.fetch_add(1);
    const auto r0 = hooks_.get(plexui::appendToken(urlNoToken(srv.baseUrl, path), srv.accessToken), 4000, 20000,
                               cancel);
    if (cancel() || !r0.ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        model_.screen = BrowseShellScreen::Libraries;
        model_.status = cancel() ? "" : "titles failed";
        return;
    }
    int total = gridPageTotalSize(r0.body);
    auto items = plexui::parseLibraryAll(r0.body);
#ifdef MUT_BROWSE_BLOCKING_CATALOG
    const int want = browsePrefetchCount(vp, total);
    int start = static_cast<int>(items.size());
    while (start < want && (total < 0 || start < total) && !cancel()) {
        const int chunk = std::min(200, want - start);
        path = "/library/sections/" + sec.key +
               "/all?sort=titleSort&X-Plex-Container-Start=" + std::to_string(start) +
               "&X-Plex-Container-Size=" + std::to_string(chunk);
        fetchCount_.fetch_add(1);
        const auto r = hooks_.get(plexui::appendToken(urlNoToken(srv.baseUrl, path), srv.accessToken), 4000, 20000,
                                  cancel);
        auto more = r.ok() ? plexui::parseLibraryAll(r.body) : std::vector<plexui::PlexItem>{};
        if (more.empty())
            break;
        start += static_cast<int>(more.size());
        items.insert(items.end(), more.begin(), more.end());
    }
#endif
    // Review #3: only the first page blocks the Titles screen. The rest of the
    // catalog (<= kBrowseShellPrefetchAllMax) pages in from run() after the
    // visible posters, so the worker is free for thumbnails and keys.
    std::vector<BrowseTitleRow> titles;
    titles.reserve(items.size());
    for (const auto& it : items)
        titles.push_back(itemToRow(it));
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cancel()) {
            model_.screen = BrowseShellScreen::Libraries;
            return;
        }
        section_ = sec;
        model_.titles = std::move(titles);
        model_.totalTitles = total > 0 ? total : static_cast<int>(model_.titles.size());
        model_.screen = BrowseShellScreen::Titles;
        model_.header.clear();
        model_.headerRight = std::to_string(model_.totalTitles) + " titles";
        model_.focus = 0;
        model_.scroll = 0;
        model_.loadingPct = 100;
        model_.status = model_.titles.empty() ? "No titles" : "";
        model_.childList = false;
        model_.searchResults = false;
        prepareFontsLocked();
    }
}

void BrowseShellController::openChildren(uint64_t gen, const BrowseTitleRow& item) {
    BrowseRenderModel parent;
    plexui::PlexServer srv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        parent = model_;
        srv = server_;
        model_.screen = BrowseShellScreen::Loading;
        model_.loadingPct = 30;
        model_.status = item.type == "show" ? "seasons" : "episodes";
    }
    requestRepaint();
    const auto cancel = cancelFor(gen);
    std::vector<plexui::PlexItem> items;
    std::string err;
    ++fetchCount_;
    const bool ok = plexui::fetchMetadataChildren(hooks_.get, srv, item.ratingKey, cancel, items, err);
    std::lock_guard<std::mutex> lk(mu_);
    if (!ok || cancel()) {
        model_ = std::move(parent);
        if (!cancel())
            model_.status = err.empty() ? "TV titles failed" : err;
        return;
    }
    parents_.push_back(std::move(parent));
    model_.titles.clear();
    for (const auto& it : items)
        model_.titles.push_back(itemToRow(it));
    ++contentEpoch_;
    model_.screen = BrowseShellScreen::Titles;
    model_.childList = true;
    model_.view = BrowseViewMode::List;
    model_.libraryName = item.title;
    model_.header.clear();
    model_.headerRight.clear();
    model_.totalTitles = static_cast<int>(items.size());
    model_.focus = model_.scroll = 0;
    model_.status = items.empty() ? "No seasons or episodes" : "";
    prepareFontsLocked();
}

bool BrowseShellController::prefetchNext(uint64_t gen, int target) {
    plexui::PlexServer srv;
    std::string sectionKey;
    int start;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_ || (model_.screen != BrowseShellScreen::Titles &&
                         !(target >= 0 && model_.screen == BrowseShellScreen::Loading)) || model_.childList ||
            model_.searchResults || model_.titles.size() >= static_cast<std::size_t>(model_.totalTitles))
            return false;
        start = static_cast<int>(model_.titles.size());
        // Keep two viewports ahead; catalogs <= kBrowseShellPrefetchAllMax fill
        // completely in the background (after the visible posters).
        const int vp = browseViewportCount(layout_, model_.view);
        if (target < 0 && start > model_.focus + std::max(60, 2 * vp) &&
            start >= browsePrefetchCount(vp, model_.totalTitles))
            return false;
        srv = server_;
        sectionKey = section_.key;
    }
    const auto cancel = cancelFor(gen);
    ++fetchCount_;
    const std::string path = "/library/sections/" + sectionKey +
        "/all?sort=titleSort&X-Plex-Container-Start=" + std::to_string(start) +
        "&X-Plex-Container-Size=200";
    const auto r = hooks_.get(plexui::appendToken(urlNoToken(srv.baseUrl, path), srv.accessToken),
                              2000, 8000, cancel);
    if (cancel())
        return false;
    auto items = r.ok() ? plexui::parseLibraryAll(r.body) : std::vector<plexui::PlexItem>{};
    if (!r.ok() || items.empty()) {
        logLine("browse-shell: next title page failed status=" + std::to_string(r.status));
        {
            std::lock_guard<std::mutex> lk(mu_);
            model_.status = "Next title page unavailable; retry by navigating";
            ++contentEpoch_;
        }
        requestRepaint(false);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (cancel())
            return false;
        for (const auto& it : items) {
            auto row = itemToRow(it);
            prepareTitleLocked(row);
            model_.titles.push_back(std::move(row));
        }
        // Background catalog pages below the screen must not force a full
        // repaint of the next nav frame; only bump when the page is on screen.
        if (start < model_.scroll + browseViewportCount(layout_, model_.view) + 1)
            ++contentEpoch_;
    }
    return true;
}

void BrowseShellController::prefetchThumbs(uint64_t gen) {
    thumbRetryAt_ = 0;
    BrowseShellLayout L;
    plexui::PlexServer srv;
    BrowseShellScreen screen;
    BrowseViewMode view;
    int tw, th, start, count, visibleEnd;
    struct Want {
        int index = 0;
        BrowseTitleRow row;
        std::string key;
        BrowseHttpResult r;
    };
    std::vector<Want> want;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!visible_ || (model_.screen != BrowseShellScreen::Titles &&
                          model_.screen != BrowseShellScreen::Detail))
            return;
        L = layout_;
        srv = server_;
        screen = model_.screen;
        view = model_.view;
        tw = view == BrowseViewMode::Grid ? L.tileW : L.posterW;
        th = view == BrowseViewMode::Grid ? L.tileH : L.posterH;
        start = model_.scroll;
        count = 2 * browseViewportCount(L, view);
        if (screen == BrowseShellScreen::Detail) {
            start = 0;
            count = 1;
            tw = L.posterW * 2 / 3;
            th = L.posterH * 2 / 3;
        } else if (view == BrowseViewMode::List)
            start = model_.focus;
        if (screen != BrowseShellScreen::Detail)
            count = std::max(0, std::min(count, static_cast<int>(model_.titles.size()) - start));
        visibleEnd = start + browseViewportCount(L, view);
        if (tw <= 0 || th <= 0 || srv.baseUrl.empty())
            return;
        // Ordered by index: the visible viewport comes first, lookahead after.
        for (int i = start; i < start + count; ++i) {
            const auto& t = screen == BrowseShellScreen::Detail ? model_.detail.item :
                            model_.titles[static_cast<std::size_t>(i)];
            if (t.thumb.empty() || t.ratingKey.empty())
                continue;
            Want w;
            w.index = i;
            w.row = t;
            w.key = browseThumbKey(srv.machineIdentifier, t.ratingKey, tw, th);
            want.push_back(std::move(w));
        }
    }
    const auto cancel = cancelFor(gen);
    auto stillCurrent = [&] {
        std::lock_guard<std::mutex> lk(mu_);
        return model_.screen == screen && model_.view == view && !cancel();
    };
    auto retry = [&](const std::string& key, bool permanent) {
        const auto at = browseShellNowMs() + (permanent ? 60000 : 1000);
        if (failedThumbs_.size() >= 128) {
            // P3 N2: drop nearest-deadline entry (earliest retry), not arbitrary begin().
            auto victim = failedThumbs_.begin();
            for (auto it = failedThumbs_.begin(); it != failedThumbs_.end(); ++it) {
                if (it->second < victim->second)
                    victim = it;
            }
            failedThumbs_.erase(victim);
        }
        failedThumbs_[key] = at;
        if (!thumbRetryAt_ || at < thumbRetryAt_)
            thumbRetryAt_ = at;
    };
    auto publish = [&](const Want& w, BrowseThumb&& thumb) {
        ram_.put(w.key, std::move(thumb));
        if (w.index < visibleEnd)
            thumbRepaint(w.row.ratingKey, tw, th);
    };
    // RAM / back-off / disk first: no network, cheap, in index order.
    std::vector<Want> net;
    for (auto& w : want) {
        if (cancel())
            return;
        if (ram_.contains(w.key))
            continue;
        const auto failed = failedThumbs_.find(w.key);
        if (failed != failedThumbs_.end()) {
            if (failed->second > browseShellNowMs()) {
                if (!thumbRetryAt_ || failed->second < thumbRetryAt_)
                    thumbRetryAt_ = failed->second;
                continue;
            }
            failedThumbs_.erase(failed);
        }
        BrowseThumb thumb;
        if (disk_.loadThumb(srv.machineIdentifier, w.row.ratingKey, tw, th, thumb)) {
            publish(w, std::move(thumb));
            continue;
        }
        net.push_back(std::move(w));
    }
    // Network: waves of kBrowseShellThumbConcurrency GETs in flight (curl
    // processes; bounded for the ARM board), visible tiles in the first waves.
#ifdef MUT_BROWSE_THUMBS_SERIAL
    const std::size_t par = 1;
#else
    const std::size_t par = static_cast<std::size_t>(kBrowseShellThumbConcurrency);
#endif
    for (std::size_t w0 = 0; w0 < net.size(); w0 += par) {
        if (!stillCurrent())
            return;
        const std::size_t w1 = std::min(net.size(), w0 + par);
        auto fetch = [&](std::size_t k) {
            net[k].r = hooks_.get(plexui::photoTranscodeUrl(srv.baseUrl, net[k].row.thumb, srv.accessToken, tw, th),
                                  1000, 4000, cancel);
        };
        std::vector<std::thread> helpers;
        helpers.reserve(w1 - w0);
        for (std::size_t k = w0 + 1; k < w1; ++k) {
            fetchCount_.fetch_add(1);
            helpers.emplace_back(fetch, k);
        }
        fetchCount_.fetch_add(1);
        fetch(w0);
        for (auto& t : helpers)
            t.join();
        if (cancel())
            return;
        for (std::size_t k = w0; k < w1; ++k) {
            const auto& r = net[k].r;
            const std::string& key = net[k].key;
            int iw = 0, ih = 0, comp = 0;
            const auto* data = reinterpret_cast<const stbi_uc*>(r.body.data());
            const int len = static_cast<int>(r.body.size());
            if (!r.ok() || !stbi_info_from_memory(data, len, &iw, &ih, &comp) ||
                iw <= 0 || ih <= 0 || static_cast<int64_t>(iw) * ih > 512LL * 768) {
                retry(key, r.ok() || (r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429));
                logLine("browse-shell: thumbnail unavailable or wrong dimensions");
                continue;
            }
            stbi_uc* decoded = stbi_load_from_memory(data, len, &iw, &ih, &comp, 3);
            if (!decoded) {
                retry(key, true);
                logLine("browse-shell: thumbnail decode failed");
                continue;
            }
            BrowseThumb thumb;
            thumb.w = tw;
            thumb.h = th;
            thumb.rgb.resize(static_cast<std::size_t>(tw) * th * 3u);
            if (iw == tw && ih == th)
                std::memcpy(thumb.rgb.data(), decoded, thumb.rgb.size());
            else if (!stbir_resize_uint8_linear(decoded, iw, ih, 0, thumb.rgb.data(), tw, th, 0, STBIR_RGB)) {
                stbi_image_free(decoded);
                retry(key, true);
                logLine("browse-shell: thumbnail resize failed");
                continue;
            }
            stbi_image_free(decoded);
            if (cancel())
                return;
            if (!disk_.saveThumb(srv.machineIdentifier, net[k].row.ratingKey, tw, th, thumb))
                logLine("browse-shell: cannot save thumbnail cache");
            publish(net[k], std::move(thumb));
        }
    }
}

void BrowseShellController::runSearch(uint64_t gen, bool commit) {
    std::string q, sectionId, base, token;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (model_.search.query.empty() || server_.baseUrl.empty())
            return;
        q = model_.search.query;
        sectionId = model_.search.sectionId;
        base = server_.baseUrl;
        token = server_.accessToken;
        if (commit && !q.empty()) {
            model_.search.pending = true;
            model_.search.lastChangeMs = browseShellNowMs() - kBrowseShellSearchDebounceMs;
        }
        if (q.empty() || base.empty())
            return;
        model_.screen = BrowseShellScreen::Search;
    }
    const BrowseCancelFn cancel = cancelFor(gen);
    if (cancel())
        return;
    for (;;) {
        BrowseSearchUi s;
        {
            std::lock_guard<std::mutex> lk(mu_);
            s = model_.search;
        }
        if (s.query.empty() || !s.pending)
            return;
        const int64_t now = browseShellNowMs();
        if (browseSearchShouldFetch(s, now)) {
            q = s.query;
            sectionId = s.sectionId;
            break;
        }
        const int64_t wait = kBrowseShellSearchDebounceMs - (now - s.lastChangeMs);
        if (wait > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(wait, 50)));
        if (cancel())
            return;
    }
    if (q.empty() || cancel())
        return;
    std::string path = "/hubs/search?query=" + plexui::urlEncode(q) + "&limit=50";
    if (!sectionId.empty())
        path += "&sectionId=" + plexui::urlEncode(sectionId);
    fetchCount_.fetch_add(1);
    const auto r = hooks_.get(plexui::appendToken(urlNoToken(base, path), token), 4000, 12000, cancel);
    if (cancel())
        return;
    if (!r.ok()) {
        std::lock_guard<std::mutex> lk(mu_);
        model_.status = "Search failed";
        model_.search.pending = false;
        ++contentEpoch_;
        return;
    }
    auto items = plexui::parseLibraryAll(r.body);
    std::vector<BrowseTitleRow> rows;
    for (const auto& it : items) {
        if (it.type == "movie" || it.type == "show" || it.type == "episode")
            rows.push_back(itemToRow(it));
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        model_.search.results = std::move(rows);
        ++contentEpoch_;
        prepareFontsLocked();
        model_.search.pending = false;
        model_.search.resultFocus = 0;
        model_.search.resultScroll = 0;
        if (!commit)
            return;
        model_.titles = model_.search.results;
        model_.totalTitles = static_cast<int>(model_.titles.size());
        model_.screen = BrowseShellScreen::Titles;
        model_.searchResults = true;
        model_.childList = false;
        model_.libraryName = "Search: " + q;
        model_.header.clear();
        model_.headerRight.clear();
        model_.focus = 0;
        model_.scroll = 0;
        model_.status = model_.titles.empty() ? "No search results" : "";
    }
}

bool BrowseShellController::startPlay(const BrowseTitleRow& item, int64_t offsetMs) {
    if (!hooks_.play || item.ratingKey.empty()) {
        logLine("browse-shell: play unavailable or missing rating key");
        return false;
    }
    plexui::PlexServer srv;
    {
        std::lock_guard<std::mutex> lk(mu_);
        srv = server_;
        ram_.dropAll();
    }
    BrowsePlayTicket t;
    t.ratingKey = item.ratingKey;
    t.key = item.key.empty() ? ("/library/metadata/" + item.ratingKey) : item.key;
    t.title = item.title;
    t.source = "pms";
    t.mpxServer = "browse-shell";
    parseServerUri(srv, t.address, t.port, t.protocol);
    t.token = srv.accessToken;
    t.machineIdentifier = srv.machineIdentifier;
    if (offsetMs > 0) {
        t.offsetPresent = true;
        t.offsetMs = offsetMs;
    }
    const uint64_t pg = hooks_.play(t);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pg) {
            browsePlayGen_ = pg;
            ownsSession_ = true;
            visible_ = false;
        } else
            model_.status = "Playback could not start";
    }
    if (!pg)
        requestRepaint();
    return pg != 0;
}

void BrowseShellController::logLine(const std::string& s) const {
    if (hooks_.log)
        hooks_.log(s);
}

void BrowseShellController::logMs(const char* action, int64_t t0) {
    thread_local std::string line;
    if (line.capacity() < 160)
        line.reserve(160);
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (renderedFrame_ > flushedFrame_ && std::strcmp(action, "title_play") != 0) {
            if (pendingSize_ < pendingMs_.size()) {
                pendingMs_[(pendingHead_ + pendingSize_++) % pendingMs_.size()] = {action, t0, renderedFrame_};
                return;
            }
            overflow = true;
        }
        const int64_t end = renderedFrame_ && flushedMs_ >= t0 &&
                            std::strcmp(action, "title_play") != 0 ? flushedMs_ : browseShellNowMs();
        char buf[160];
        std::snprintf(buf, sizeof buf, "BROWSE_MS action=%s ms=%lld", action,
                      static_cast<long long>(std::max<int64_t>(0, end - t0)));
        line.assign(buf);
        msLines_[(msHead_ + msSize_) % msLines_.size()] = line;
        if (msSize_ < msLines_.size())
            ++msSize_;
        else
            msHead_ = (msHead_ + 1) % msLines_.size();
    }
    if (overflow)
        logLine("browse-shell: frame timing queue full; reporting without flush acknowledgement");
    logLine(line);
}

void BrowseShellController::onFrameFlushed(int64_t flushedMs, int64_t presentDurMs) {
    int64_t plxo = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        flushedFrame_ = renderedFrame_;
        flushedMs_ = flushedMs > 0 ? flushedMs : browseShellNowMs();
        plxo = flushedMs_;
    }
    logNavTrace(presentDurMs, plxo);
    thread_local std::string line;
    if (line.capacity() < 160)
        line.reserve(160);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!pendingSize_ || pendingMs_[pendingHead_].frame > flushedFrame_)
                break;
            const auto pending = pendingMs_[pendingHead_];
            pendingHead_ = (pendingHead_ + 1) % pendingMs_.size();
            --pendingSize_;
            char buf[160];
            std::snprintf(buf, sizeof buf, "BROWSE_MS action=%s ms=%lld", pending.action,
                          static_cast<long long>(std::max<int64_t>(0, flushedMs_ - pending.receivedMs)));
            line.assign(buf);
            msLines_[(msHead_ + msSize_) % msLines_.size()] = line;
            if (msSize_ < msLines_.size())
                ++msSize_;
            else
                msHead_ = (msHead_ + 1) % msLines_.size();
        }
        logLine(line);
    }
}

BrowseCancelFn BrowseShellController::cancelFor(uint64_t gen) const {
    return [this, gen] { return stopping_.load() || gen_.load() != gen; };
}

void BrowseShellController::parseServerUri(const plexui::PlexServer& s, std::string& address,
                                           std::string& port, std::string& protocol) const {
    protocol = "http";
    address.clear();
    port.clear();
    const std::string& u = s.baseUrl;
    const auto sep = u.find("://");
    if (sep == std::string::npos)
        return;
    protocol = u.substr(0, sep);
    std::string host = u.substr(sep + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos)
        host = host.substr(0, slash);
    const auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(']') == std::string::npos) {
        address = host.substr(0, colon);
        port = host.substr(colon + 1);
    } else {
        address = host;
        port = protocol == "https" ? "443" : "32400";
    }
}

} // namespace misterplex
