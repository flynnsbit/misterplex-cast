#pragma once

#include "libmisterplex/browse_controller.hpp"
#include "plexui_render.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace misterplex {
namespace plexui {

class PlexUiController {
public:
    using HttpGetFn = BrowseController::HttpGetFn;
    struct Config {
        std::string accountToken;
        std::string fontDir;
        // Optional PLEX_HOME: server name, source title, or address. Empty picks the first server.
        std::string homeServer;
    };
    struct Hooks {
        HttpGetFn get;
        std::function<void()> repaint;
        std::function<bool()> playing;
        std::function<void()> stopPlayback;
        std::function<uint64_t(const BrowsePlayTicket&)> play;
        std::function<void(const std::string&)> log;
    };

    PlexUiController(Config cfg, Hooks hooks);
    ~PlexUiController();
    PlexUiController(const PlexUiController&) = delete;
    PlexUiController& operator=(const PlexUiController&) = delete;

    void start(const std::function<void()>& nameThread = {});
    void stop();

    void post(BrowseOp op);
    void postIngress(BrowseOp op, const char* src, int64_t recvMs);
    void postHorizontal(int delta, const char* src = "http", int64_t recvMs = 0);
    void onPlayStarted(uint64_t generation);

    bool visible() const;
    bool ownsSession() const;

    bool workerIdle();
    bool imagesBusy() const { return images_.busy(); }

    struct FrameSummary {
        std::string view;
        int servers = 0;
        int sections = 0;
        int hubs = 0;
        int hubItems = 0;
        int grid = 0;
        bool loading = false;
        std::string status;
        std::size_t imageBytes = 0;
    };
    FrameSummary frameSummary() const;
    bool waitForFrame(int timeoutMs);
    // True when copyFrame would produce a complete Plex TV surface (data loaded and, for
    // home/grid/detail, visible poster/backdrop art decoded). Used by paintIdle to avoid
    // presenting the idle chevron/black field over a live Plex session.
    bool paintReady();

    // The frame in the current render target's format at its geometry (default: R1 640x480 RGB24).
    bool copyFrame(std::vector<std::uint8_t>& frame);

    // OB port: draw for another target (the OB overlay canvas, BGRA8888). Re-lays out (grid
    // columns, sidebar / chip scroll, art sizes) and keeps the focused item. True when it changed;
    // the caller repaints (never from inside paintIdle). Thread-safe.
    bool setRenderTarget(int w, int h, PixelFormat fmt);
    PixelFormat renderFormat() const;

    void onR1FlipAck(int64_t plxhMs = 0, int64_t plxoMs = 0);

    void setShotPreferences(const std::string& serverNeedle, const std::string& sectionNeedle);
    bool shotFocusServer(const std::string& needle);
    bool shotFocusSection(const std::string& needle);
    bool shotFocusLibrary(const std::string& titleSlashSubtitle);
    bool shotFocusSidebarIndex(int index);

#ifdef MPX_PLEXUI_UNIT_TEST
    void testSeedHome(std::vector<PlexServer> servers) {
        std::lock_guard<std::mutex> lk(mu_);
        servers_ = std::move(servers);
        haveServer_ = !servers_.empty();
        server_ = haveServer_ ? servers_.front() : PlexServer{};
        view_ = ViewId::Home;
        visible_ = true;
        status_.clear();
    }
    void testArmPlaybackView(ViewId v) {
        std::lock_guard<std::mutex> lk(mu_);
        view_ = v;
        viewBeforePlay_ = v;
        visible_ = false;
        haveServer_ = true;
        ownsSession_ = true;
        if (servers_.empty()) {
            PlexServer s;
            s.name = "Box";
            s.baseUrl = "http://127.0.0.1:9";
            servers_.push_back(s);
            server_ = s;
        }
    }
    ViewId testView() const {
        std::lock_guard<std::mutex> lk(mu_);
        return view_;
    }
    void testHandleOpen() {
        Job j;
        j.op = BrowseOp::Open;
        j.gen = 1;
        handle(j);
    }
#endif

private:
    struct Job {
        BrowseOp op;
        uint64_t gen;
    };

    void run();
    void handle(const Job& j);
    void bumpAnim();
    void applyHorizontal(int delta);
    void scheduleImages();
    void syncGridScroll();
    void updateImageCacheBudget();
    void buildRenderState(RenderState& rs) const;
    void logLine(const std::string& s) const;
    void logSidebarFocus(const char* event) const;
    BrowseCancelFn cancelFor(uint64_t gen) const;
    bool startPlay(const PlexItem& item, int64_t offsetMs);
    void parseServerUri(const PlexServer& s, std::string& address, std::string& port,
                        std::string& protocol) const;

    Config cfg_;
    Hooks hooks_;
    Renderer renderer_;
    ImageCache images_;

    mutable std::mutex mu_;
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<Job> q_;
    // Set under qMu_ by onR1FlipAck so run() paints the next focus frame with no
    // locks held. A synchronous repaint here re-locks r1PaintMu_.
    bool animTick_ = false;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> busy_{false};
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> dirty_{false};

    PixelFormat fmt_ = PixelFormat::RGB24;
    bool visible_ = false;
    bool ownsSession_ = false;
    bool reopen_ = false;
    uint64_t browsePlayGen_ = 0;
    uint64_t lastStartedGen_ = 0;

    ViewId view_ = ViewId::ServerList;
    ViewId viewBeforePlay_ = ViewId::Home;   // restored when playback Back reopens the UI
    UiFocus focus_{};
    int focusAnim_ = 3;
    std::string status_;

    std::vector<PlexServer> servers_;
    int serverIndex_ = 0;
    PlexServer server_{};
    bool haveServer_ = false;
    std::vector<PlexSection> sections_;
    std::vector<SidebarLibraryEntry> sidebarEntries_;
    int sidebarScrollY_ = 0;
    std::vector<PlexHubRow> hubs_;
    std::unordered_map<std::string, int> hubItemServerIndex_;
    std::vector<PlexItem> grid_;
    PlexItem detail_;
    int gridSectionIndex_ = -1;
    std::string gridSectionTitle_;
    std::string gridSectionKey_;
    std::vector<GridCategoryChip> gridChips_;
    int gridActiveChip_ = 0;
    int gridChipScrollX_ = 0;
    int gridScrollRow_ = 0;
    std::string shotServerNeedle_;
    std::string shotSectionNeedle_;

    void syncChipScroll();
    bool loadGridForChip(const GridCategoryChip& chip, const PlexServer& server,
                         const std::string& sectionKey, const BrowseCancelFn& cancel,
                         std::vector<PlexItem>& items, std::string& err);
    static std::vector<GridCategoryChip> buildGridChips(std::vector<GridCategoryChip> collections,
                                                        std::vector<GridCategoryChip> genres);

    bool serverMatches(const PlexServer& s, const std::string& needle) const;
    int findServerIndex(const std::string& needle) const;
    int findSectionIndex(const std::string& needle) const;
    int findSidebarIndex(const std::string& titleSlashSubtitle) const;
    void syncSidebarScroll();
    bool loadStartupCatalog(const BrowseCancelFn& cancel);
    bool loadHomeHubs(const BrowseCancelFn& cancel);
    const PlexServer& serverForHubItem(const PlexItem& item) const;
    bool visibleArtReady() const;
    bool gridVisiblePostersCached() const;
    void markGridArtLoadStart();
    bool isHomeSurface() const;
    void coalescedImageRepaint();

    uint64_t gridArtLoadGen_ = 0;
    uint64_t gridArtLoggedGen_ = 0;
    std::chrono::steady_clock::time_point gridArtLoadStart_{};
    uint64_t imgViewGen_ = 0;
    uint64_t dataGeneration_ = 0;

    struct NavTrace {
        uint64_t id = 0;
        const char* src = "";
        char op[16]{};
        int64_t recvMs = 0;
        int64_t postMs = 0;
        int64_t stateMs = 0;
        int64_t renderStartMs = 0;
        int64_t renderEndMs = 0;
        int64_t plxhMs = 0;
        int64_t plxoMs = 0;
    };
    NavTrace activeNav_{};
    std::atomic<bool> navTraceOpen_{false};
    std::atomic<bool> animWaitAck_{false};

    std::atomic<bool> imgRepaintPending_{false};
    std::chrono::steady_clock::time_point lastImgRepaint_{};

    // Select into a library: don't close nav_visible on the LOADING frame.
    std::atomic<bool> navDeferVisible_{false};
    std::atomic<bool> navVisiblePending_{false};
    std::string navVisibleOp_;
    std::chrono::steady_clock::time_point navVisibleStart_{};

#ifdef MPX_PLEXUI_UNIT_TEST
public:
    void testSetLibraryGrid(PlexServer server, std::vector<PlexItem> grid,
                            std::vector<GridCategoryChip> chips);
    void testInsertImage(const ImageCacheKey& key);
    void testArmAnimAck();
    void testArmAnimAt(int frame);
    int testFocusAnim() const { return focusAnim_; }
    void testArmLibrarySelect();
    void testSetHome(std::vector<SidebarLibraryEntry> entries);
    int testSidebarFocus() const;
#endif
};

} // namespace plexui
} // namespace misterplex
