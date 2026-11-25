#pragma once
// Single browse worker, generation cancel, cache-first servers, atomic RenderModel.

#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/browse_shell.hpp"
#include "libmisterplex/browse_shell_cache.hpp"
#include "libmisterplex/browse_shell_layout.hpp"
#include "libmisterplex/browse_shell_model.hpp"
#include "libmisterplex/browse_shell_paint.hpp"
#include "libmisterplex/plexui/plexui_font.hpp"
#include "libmisterplex/plexui/plexui_render.hpp"
#include "libmisterplex/plexui/plexui_types.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

namespace misterplex {

class BrowseShellController {
public:
    using HttpGetFn = BrowseController::HttpGetFn;
    struct Config {
        std::string accountToken;
        std::string fontDir;
        std::string clientId;
        std::vector<plexui::PlexServer> seedServers;
        std::function<std::vector<plexui::PlexServer>()> discoverLocal;
        std::string cacheDir;
    };
    struct Hooks {
        HttpGetFn get;
        std::function<void()> repaint;
        std::function<bool()> playing;
        std::function<void()> stopPlayback;
        std::function<uint64_t(const BrowsePlayTicket&)> play;
        std::function<void(const std::string&)> log;
    };

    BrowseShellController(Config cfg, Hooks hooks);
    ~BrowseShellController();
    BrowseShellController(const BrowseShellController&) = delete;
    BrowseShellController& operator=(const BrowseShellController&) = delete;

    void start(const std::function<void()>& nameThread = {});
    void stop();

    void post(BrowseOp op);
    void postIngress(BrowseOp op, const char* src, int64_t recvMs);
    void postHorizontal(int delta, const char* src = "http", int64_t recvMs = 0);
    void postSearch();
    void postText(char ch);
    void onPlayStarted(uint64_t generation);
    void onFrameFlushed(int64_t flushedMs = 0, int64_t presentDurMs = 0);

    bool visible() const;
    bool ownsSession() const;
    bool paintReady();
    bool workerIdle();
    bool searchOpen() const;

    bool copyFrame(std::vector<std::uint8_t>& frame);
    const FrameDamage& lastFrameDamage() const { return lastDamage_; }
    bool setRenderTarget(int w, int h, plexui::PixelFormat fmt, bool interlaced = false);
    plexui::PixelFormat renderFormat() const;
    void applyUiSafeLayout(int marginPct, UiSize size);
    UiSize uiSize() const { std::lock_guard<std::mutex> lk(mu_); return uiSize_; }
    int uiSafeMarginPct() const { std::lock_guard<std::mutex> lk(mu_); return marginPct_; }

    BrowseRenderModel snapshot() const;
    int fetchCount() const { return fetchCount_.load(); }
    std::vector<std::string> browseMsLines() const;

#ifdef MPX_BROWSE_SHELL_UNIT_TEST
    void testSeed(BrowseRenderModel m) {
        std::lock_guard<std::mutex> lk(mu_);
        model_ = std::move(m);
        visible_ = true;
        layout_ = browseLayoutFor(model_.w, model_.h);
        model_.search.query.reserve(64);
        ++contentEpoch_;
        prepareFontsLocked();
    }
    void testHandle(BrowseOp op) { handleNav(op, browseShellNowMs(), browseNavActionName(op), "ring_in"); }
    BrowseRamCache& testRam() { return ram_; }
    void testThumbRepaint(std::string_view ratingKey, int w, int h) { thumbRepaint(ratingKey, w, h); }
#endif

private:
    struct Job {
        BrowseOp op = BrowseOp::Open;
        uint64_t gen = 0;
        int64_t recvMs = 0;
        char text = 0;
        bool search = false;
        bool cancelLoading = false;
        const char* action = "keypress";
        const char* src = "http";
    };

    void run();
    void enqueue(Job j);
    void requestRepaint(bool contentChanged = true);
    void thumbRepaint(std::string_view ratingKey, int w, int h);
    void handle(const Job& j);
    void relayoutLocked();
    void prepareFontsLocked();
    void prepareTitleLocked(const BrowseTitleRow& title);
    void handleNav(BrowseOp op, int64_t recvMs, const char* action, const char* src = "ring_in");
    void beginNavTrace(const char* src, int64_t recvMs);
    void logNavTrace(int64_t plxhMs, int64_t plxoMs);
    void openServers(uint64_t gen, int64_t recvMs);
    void refreshServersFromNetwork(uint64_t gen);
    void openLibraries(uint64_t gen);
    void enterSearchLocked();
    void openTitles(uint64_t gen);
    void openChildren(uint64_t gen, const BrowseTitleRow& item);
    bool prefetchNext(uint64_t gen, int target = -1);
    void prefetchThumbs(uint64_t gen);
    void runSearch(uint64_t gen, bool commit = false);
    bool startPlay(const BrowseTitleRow& item, int64_t offsetMs);
    void logLine(const std::string& s) const;
    void logMs(const char* action, int64_t t0);
    BrowseCancelFn cancelFor(uint64_t gen) const;
    void parseServerUri(const plexui::PlexServer& s, std::string& address, std::string& port,
                        std::string& protocol) const;

    Config cfg_;
    Hooks hooks_;
    plexui::FontSet font_;
    BrowseDiskCache disk_;
    BrowseRamCache ram_;

    mutable std::mutex mu_;
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::array<Job, 256> q_{};
    std::size_t qHead_ = 0, qSize_ = 0;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> busy_{false};
    std::atomic<uint64_t> gen_{0};
    std::atomic<int> fetchCount_{0};
    std::atomic<bool> serversRefreshPending_{false};

    BrowseRenderModel model_;
    BrowseShellLayout layout_ = browseLayout480();
    plexui::PixelFormat fmt_ = plexui::PixelFormat::RGB24;
    int canvasW_ = 640;
    int canvasH_ = 480;
    bool interlaced_ = false;
    bool visible_ = false;
    bool ownsSession_ = false;
    uint64_t browsePlayGen_ = 0;
    UiSize uiSize_ = UiSize::Normal;
    int marginPct_ = 5;
    std::string lastUsedId_;
    plexui::PlexServer server_;
    plexui::PlexSection section_;
    std::array<std::string, 64> msLines_;
    std::size_t msHead_ = 0, msSize_ = 0;
    std::string cursorKey_;
    std::unordered_map<std::string, BrowseRenderModel> cursors_;
    std::vector<BrowseRenderModel> parents_;
    BrowseRenderModel searchOrigin_;
    std::unordered_map<std::string, int64_t> failedThumbs_;
    int64_t thumbRetryAt_ = 0;
    struct PendingMs {
        const char* action = "keypress";
        int64_t receivedMs = 0;
        uint64_t frame = 0;
    };
    uint64_t renderedFrame_ = 0;
    uint64_t flushedFrame_ = 0;
    int64_t flushedMs_ = 0;
    std::array<PendingMs, 256> pendingMs_{};
    std::size_t pendingHead_ = 0, pendingSize_ = 0;
    std::vector<std::uint8_t> rgbFrame_, packedFrame_;
    uint64_t contentEpoch_ = 1, paintedEpoch_ = 0;
    struct PaintedState {
        BrowseShellScreen screen = BrowseShellScreen::Servers;
        BrowseViewMode view = BrowseViewMode::List;
        int focus = 0, scroll = 0, kbIndex = 0, resultFocus = 0, resultScroll = 0;
        BrowseSearchUi::Region region = BrowseSearchUi::Region::Keyboard;
        int button = 0;
        std::size_t querySize = 0;
        std::array<char, 64> query{};
    } painted_;
    std::array<BrowseRect, 21> artDamage_{};
    std::size_t artDamageSize_ = 0;
    FrameDamage lastDamage_{};

    // G2 three-clock split on the browse-shell path (lab product default).
    // plexui: navtrace was only emitted on the old Plex TV worker.
    struct NavTrace {
        uint64_t id = 0;
        const char* src = "";
        int64_t recvMs = 0;
        int64_t postMs = 0;
        int64_t stateMs = 0;
        int64_t renderStartMs = 0;
        int64_t renderEndMs = 0;
    };
    NavTrace activeNav_{};
    std::atomic<bool> navTraceOpen_{false};
    std::atomic<uint64_t> navSeq_{0};
};

} // namespace misterplex
