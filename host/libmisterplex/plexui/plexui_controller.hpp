#pragma once

#include "libmisterplex/browse_controller.hpp"
#include "libmisterplex/mark_watched.hpp"
#include "libmisterplex/play_queue_write.hpp"
#include "plexui_data.hpp"
#include "plexui_home.hpp"
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
        // Stable X-Plex-Client-Identifier for this device (the daemon machine id).
        std::string clientId;
        // Writes the account token where the daemon keeps PLEX_TOKEN. Never logs it.
        std::function<bool(const std::string&)> persistAccountToken;
        // Persist UI_SIZE after the browse Y toggle (GH#46). Optional.
        std::function<bool(const char* sizeName)> persistUiSize;
        // Conf / local PMS rows shown even when plex.tv is not linked.
        std::vector<PlexServer> seedServers;
        // Optional LAN GDM probe. Empty / unset skips the network.
        std::function<std::vector<PlexServer>()> discoverLocal;
        // Saved Plex Home profile. Empty token means the account token is the
        // library token (single-user / no-Home / not switched).
        std::string homeUserId;
        std::string homeUserTitle;
        std::string homeUserToken;
        // Writes PLEX_HOME_ID / PLEX_HOME_TITLE / PLEX_HOME_TOKEN only.
        // Empty token clears them. Must not write PLEXTV_TOKEN or PLEX_TOKEN.
        std::function<bool(const HomeSession&)> persistHomeSession;
    };
    struct Hooks {
        HttpGetFn get;
        HttpPostFn post;
        HttpPostFn put;
        HttpHeaderGetFn getHeaders;
        std::function<void()> repaint;
        std::function<bool()> playing;
        std::function<void()> stopPlayback;
        std::function<uint64_t(const BrowsePlayTicket&)> play;
        std::function<void(const std::string&)> log;
        // Raw theme payload (MP3/AAC). Token is not in this vector. No play().
        std::function<void(std::vector<std::uint8_t>)> onThemeBytes;
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
    // Apply conf / toggle. Relays out browse geometry. Thread-safe.
    void applyUiSafeLayout(int marginPct, UiSize size);
    UiSize uiSize() const;
    int uiSafeMarginPct() const;

    void onR1FlipAck(int64_t plxhMs = 0, int64_t plxoMs = 0);

    // Manual next (+1) / previous (-1) while this UI owns a play queue.
    void postQueueStep(int delta);
    // Browse: Add to queue (append / create) or Play next (insert, or play if idle).
    void postQueueMark(QueueMark mark);
    // MARK_WATCHED on: mark the focused movie/episode. Off: the worker no-ops.
    void postMark(MarkWatchedOp op);
    bool queueHasNext() const;
    // End of an episode this UI started. True when an Up next card is showing
    // and the caller should not auto-advance on its own. endedRatingKey is the
    // item that just finished; a mismatch leaves the normal auto-next path alone.
    bool armUpNext(const std::string& endedRatingKey);

    void setShotPreferences(const std::string& serverNeedle, const std::string& sectionNeedle);
    bool shotFocusServer(const std::string& needle);
    bool shotFocusSection(const std::string& needle);
    bool shotFocusLibrary(const std::string& titleSlashSubtitle);
    bool shotFocusSidebarIndex(int index);

#ifdef MPX_PLEXUI_UNIT_TEST
    int testFindServerIndex(const std::string& needle) const { return findServerIndex(needle); }
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
    void testSetGridSection(int idx, const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        gridSectionIndex_ = idx;
        gridSectionKey_ = key;
    }
    int testGridSectionIndex() const {
        std::lock_guard<std::mutex> lk(mu_);
        return gridSectionIndex_;
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
    void testHandleSelect() {
        Job j;
        j.op = BrowseOp::Select;
        j.gen = 1;
        handle(j);
    }
    void testHandleBack() {
        Job j;
        j.op = BrowseOp::Back;
        j.gen = 1;
        handle(j);
    }
    void testStartLink() { startLinkSignIn(ViewId::ServerList, false); }
    void testToggleUiSize() {
        Job j;
        j.op = BrowseOp::ToggleUiSize;
        j.gen = 1;
        handle(j);
    }
#endif

private:
    struct Job {
        BrowseOp op;
        uint64_t gen;
        int queueDelta = 0;
        uint64_t upNextToken = 0;
        int queueMark = 0; // 1 append, 2 play next
        int markOp = 0;    // MarkWatchedOp, 0 = none
    };

    void run();
    void handle(const Job& j);
    void bumpAnim();
    void applyHorizontal(int delta);
    // Library grid LEFT-at-col-0: sidebar index for the open library (1+), or Home.
    int librarySidebarFocusLocked() const;
    void scheduleImages();
    void syncGridScroll();
    void updateImageCacheBudget();
    void buildRenderState(RenderState& rs) const;
    void logLine(const std::string& s) const;
    void logSidebarFocus(const char* event) const;
    BrowseCancelFn cancelFor(uint64_t gen) const;
    struct PlayQueueBind {
        std::string id;
        std::string version;
        std::string itemId;
        std::string containerKey;
    };
    bool startPlay(const PlexItem& item, int64_t offsetMs, const PlayQueueBind* queue = nullptr,
                   bool forceStart = false);
    bool playLeaf(const PlexItem& item, PlexServer srv, int64_t offsetMs, bool forceStart,
                  const BrowseCancelFn& cancel);
    bool focusedQueueLeafLocked(PlexItem& item) const;
    bool focusedMarkLeafLocked(PlexItem& item) const;
    void applyQueueMark(QueueMark mark, const BrowseCancelFn& cancel);
    void applyMark(MarkWatchedOp op, const BrowseCancelFn& cancel);
    void noteMarkAppliedLocked(MarkWatchedOp op, const std::string& ratingKey);
    void refreshQueueLineLocked();
    void stepActiveQueue(int delta);
    void syncSeasonScroll();
    void syncEpisodeScroll();
    void touchListsLocked() { ++listGeneration_; }
    void clearActiveQueueLocked();
    void bindActiveQueueLocked(PlexPlayQueue q, int index, const PlexServer& srv);
    bool queueBoundToServerLocked() const;
    void assignServerLocked(PlexServer next);
    void kickShowTheme(const PlexServer& srv, const PlexItem& item, const BrowseCancelFn& cancel);
    void cancelUpNextLocked();
    void joinOwnedThreadUnlocked(std::thread& th);
    void joinUpNextThread();
    void joinLinkThread();
    void startLinkSignIn(ViewId returnView, bool returnVisible);
    void ensureHomeUserRowLocked();
    void openHomeUsers();
    void beginHomeSwitch(const HomeUser& user, const std::string& pin);
    bool applyHomeSession(HomeSession session);
    // Sign-in retries are cancelled for the whole play. Wait here instead of spinning.
    void parkLinkWhilePlaying(uint64_t gen);
    std::vector<std::pair<std::string, std::string>> linkHeaders() const;
    void parseServerUri(const PlexServer& s, std::string& address, std::string& port,
                        std::string& protocol) const;

    Config cfg_;
    Hooks hooks_;
    Renderer renderer_;
    ImageCache images_;

    mutable std::mutex mu_;
    // Owns upNextThread_. stop() and armUpNext() both join it; never touch it unlocked.
    std::mutex helperMu_;
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
    bool serverPickerFromHome_ = false;      // Back from the picker returns to Home
    UiFocus focus_{};
    int focusAnim_ = 3;
    std::string status_;
    std::string lastLoadErr_;   // reviewer: last 401/403 library error, shown in status
    std::string queueLine_;

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
    // FILTER_4_3. The fetched page. Empty while the flag is off, and grid_ is
    // the only list. The toggle hides widescreen in grid_ without a new query.
    std::vector<PlexItem> gridFull_;
    bool filter43On_ = false;
    PlexItem detail_;
    std::string themeKickedKey_;
    int pickedSubtitleId_ = -1; // -1 = use resolve default; 0 = Off; >0 = stream id
    int pickedAudioId_ = -1;    // -1 = daemon default (language pref / PMS selected); >0 = stream id
    int gridSectionIndex_ = -1;
    std::string gridSectionTitle_;
    std::string gridSectionKey_;
    std::vector<GridCategoryChip> gridChips_;
    int gridActiveChip_ = 0;
    int gridChipScrollX_ = 0;
    int gridScrollRow_ = 0;
    // AZ_JUMP applied letter. 0 is the unfiltered grid (flag off stays here).
    char gridLetterFilter_ = 0;
    // GRID_PAGE. totalSize < 0 and a short last page means the loaded rows are the grid.
    int gridTotalSize_ = -1;
    bool gridPageFull_ = false;
    bool gridPageBusy_ = false;
    int gridPageGen_ = 0;
    std::string shotServerNeedle_;
    std::string shotSectionNeedle_;

    std::vector<PlexItem> seasons_;
    std::vector<PlexItem> episodes_;
    uint64_t listGeneration_ = 1;
    mutable RenderState frameRs_;
    int seasonScrollRow_ = 0;
    int episodeScrollRow_ = 0;
    std::string listTitle_;
    PlexPlayQueue activeQueue_{};
    int activeQueueIndex_ = -1;
    std::string queueMachineId_;
    std::string queueBaseUrl_;
    std::string queueToken_;
    bool queuePlayArmed_ = false;
    std::atomic<uint64_t> upNextToken_{0};
    int upNextSeconds_ = 0;
    std::string upNextArmedKey_;
    std::thread upNextThread_;

    std::string linkCode_;
    int linkSeconds_ = 0;
    ViewId linkReturn_ = ViewId::Home;
    bool linkReturnVisible_ = false;
    std::atomic<uint64_t> linkGen_{0};
    std::mutex linkWaitMu_;
    std::condition_variable linkWaitCv_;
    std::thread linkThread_;

    std::vector<HomeUser> homeUsers_;
    int homeSwitchIndex_ = -1;
    std::string homePin_ = "0000";
    int homePinSlot_ = 0;
    std::string homePinTitle_;

    void syncChipScroll();
    void storeGridLocked(std::vector<PlexItem> items);
    int libraryLoadedCountLocked() const;
    void clearGridPageLocked();
    void rememberGridPageLocked(int fetched, int totalSize, bool pageable);
    void fetchAndAppendGridPage(PlexServer server, std::string sectionKey, std::string genreId,
                                int start, int pageGen, int oldMaxRow, const BrowseCancelFn& cancel,
                                char letter = 0);
    bool loadGridForChip(const GridCategoryChip& chip, const PlexServer& server,
                         const std::string& sectionKey, const BrowseCancelFn& cancel,
                         std::vector<PlexItem>& items, std::string& err, int* totalSize = nullptr);
    static std::vector<GridCategoryChip> buildGridChips(std::vector<GridCategoryChip> collections,
                                                        std::vector<GridCategoryChip> genres);

    bool serverMatches(const PlexServer& s, const std::string& needle) const;
    int findServerIndex(const std::string& needle) const;
    int findSectionIndex(const std::string& needle) const;
    int findSidebarIndex(const std::string& titleSlashSubtitle) const;
    void syncSidebarScroll();
    // First sidebar stop that is a server or a library (1-based). Sign-in and
    // Switch user are skipped. 0 when the sidebar has neither.
    int firstSidebarNavFocusLocked() const;
    // Home content takes Up/Down only when some hub row has tiles.
    bool homeContentNavigableLocked() const;
    // Hubs empty: leave the yellow Home pill and focus the first server/library.
    void focusEmptyHomeSidebarLocked();
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
                            std::vector<GridCategoryChip> chips,
                            std::vector<SidebarLibraryEntry> sidebar = {},
                            int sectionIndex = -1);
    void testInsertImage(const ImageCacheKey& key);
    void testArmAnimAck();
    void testArmAnimAt(int frame);
    int testFocusAnim() const { return focusAnim_; }
    void testArmLibrarySelect();
    void testSetHome(std::vector<SidebarLibraryEntry> entries);
    void testSetHomeHubs(PlexServer server, std::vector<PlexHubRow> hubs,
                         std::vector<SidebarLibraryEntry> entries = {});
    std::vector<PlexHubRow> testHubs() const;
    PlexItem testDetail() const;
    int testFocusCol() const;
    int testFocusRow() const;
    // #220 review nit: the Home refresh path (filter, then hubs_ =) that keeps focus_, so a shrink can be tested.
    void testRefreshHomeHubsKeepFocus(std::vector<PlexHubRow> hubs);
    int testGridCount() const;
    std::string testGridTitle(int idx) const;
    bool testFilter43On() const;
    bool testGridOnLetters() const;
    int testGridLetterSlot() const;
    char testGridLetterFilter() const;
    // totalSize < 0 means unknown. activeChip < 0 leaves the current chip.
    void testArmGridPage(int totalSize, const std::string& sectionKey, int activeChip = -1);
    void testSetDetail(PlexServer server, PlexItem detail);
    void testKickShowTheme();
    void testSetDetailButton(int button);
    void testSetPickerRow(int row);
    int testDetailButton() const;
    int testPickedSubtitleId() const;
    int testPickedAudioId() const;
    int testPickerRow() const;
    void testSetServers(std::vector<PlexServer> servers);
    void testSetEpisodes(PlexServer server, std::vector<PlexItem> episodes);
    int testSidebarFocus() const;
    int testSidebarEntryCount() const;
    void testSeedTvStack();
    std::string testViewName() const;
    bool testVisible() const;
    int testQueueIndex() const;
    bool testQueueBound() const;
    std::string testLinkCode() const;
    std::string testStatus() const;
    std::string testQueueId() const;
    int testQueueDepth() const;
    void testSetFocusRow(int row);
    void testHandleInfo();
    void testHandlePlayNext();
    void testHandleMark(int op);
    int testEpisodeViewCount() const;
    void testStepNext();
    uint64_t testUpNextToken() const { return upNextToken_.load(); }
    int testUpNextSeconds() const;
    std::string testAccountToken() const;
    std::string testHomeToken() const;
    std::string testSidebarTitle(int index) const;
#endif
};

} // namespace plexui
} // namespace misterplex
