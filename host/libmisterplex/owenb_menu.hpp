#pragma once
// OWENB-P1B 0010 (T6 gap G2): the menu panel's items from the daemon's state, and where an OB input
// goes. The panel opens from the core's OSD action 1 "Program menu" (h264ring.h:390-394) or Menu
// (Start / Y / M / Tab, owenb_input.hpp). Keyboard P (PS/2 0x4D) is that panel on Ready idle
// only, when IDLE_MENU_KEY is on. Its commands are the daemon's own transport and library
// commands (PlaybackCommand, input_mailbox.hpp:36-49), dispatched as the legacy input path does
// (MediaPlayer::dispatchPlaybackInput): nothing here touches playback by itself.
//
// Routing (obRoute), first match:
//   1. the panel is open: every navigation is the panel's (Back / Menu close it);
//   2. Menu while playing or while the library is already up: open the panel;
//   3. idle Menu (F12 Program / Start / M / Tab) with a library: Browse, no panel;
//   4. the library page is up (local or remote host, same keys): d-pad moves, Select/Space
//      selects, Back goes back (browse_nav.hpp). Left/Right are not back/select. Legacy list:
//      SkipBack/SkipForward move, PlayPause opens, Stop pops (media_player libraryHandleInput);
//   5. a play: Select/Space = play/pause, Left/Right = skip back/forward; Home never stops a play;
//   6. idle: Select/Down/Home/Info (pad X) open the library (when there is one).
//      F1 is Home (owenb_input.hpp). Playing Info stays SkipForward.

#include "chapters.hpp"
#include "in_play_options.hpp"
#include "input_mailbox.hpp"
#include "owenb_input.hpp"
#include "owenb_osd_draw.hpp"
#include "picture_14_9.hpp"
#include "remember_picture.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace misterplex::owenb {

// Ready-card browse keys. Wording is cast-dev PR #47 ("Press Start or Program
// to browse Plex") plus pad X and keyboard F1. media_player.cpp is namespace
// misterplex: use owenb::obIdleBrowseHintLine, never a global :: qualifier.
#ifdef MUT_BH_OLD_WORDING
inline constexpr const char* kObIdleBrowseHint = "Press Start or Program to browse Plex";   // mutant: pre-fix wording
#else
inline constexpr const char* kObIdleBrowseHint = "Press X, F1, Start or Program";
#endif

inline std::string obIdleBrowseHintLine(const std::string& readyLine) {
    std::string s = readyLine.empty() ? std::string("Ready to cast") : readyLine;
    s.push_back('\n');
    s += kObIdleBrowseHint;
    return s;
}

struct ObMenuState {
    bool playing = false, paused = false;
    bool libraryAvailable = false;   // a browser is configured (MPX_BROWSE_V2 or the PMS list)
    bool libraryVisible = false;
    bool libraryLegacy = false;      // the pre-V2 PMS list: commands 1-5 only (library_browser.hpp; media_player
                                     // libraryHandleInput: PlayPause opens, Stop pops, SkipBack/Forward move)
    std::string title;               // the play's title
    int64_t positionMs = -1, durationMs = 0;
    std::string castName;            // the companion's name (what Plex apps show)
    std::string pmsLabel;
    int pmsState = -1;               // 1 reachable, 0 unreachable, -1 not probed
    int skipBackS = 10, skipForwardS = 30;
    bool queueUi = false;      // Plex TV browse can mark the PMS play queue
    bool queueHasNext = false; // a bound queue has an item after the current one
    bool markWatched = false;  // MARK_WATCHED on: mark / unmark / drop Continue Watching
    // SETSTREAMS on: Subtitles / Audio rows while a play has those streams.
    // Off (the default) ignores the row vectors, so the panel matches the tip.
    bool setStreams = false;
    // PICTURE_14_9 on: a per-play 14:9 / full row. Off: the row is absent.
    bool picture149 = false;
    bool picture149On = false;
    // IN_PLAY_OPTIONS on: an Options page while a title is playing.
    // Off (the default): the row is absent and Play from start is not offered.
    bool inPlayOptions = false;
    int inPlayW = 0;
    int inPlayH = 0;
    // REMEMBER_PICTURE on: one Picture row that cycles letterbox / 14:9 / zoom
    // 4:3 / stretch / fit-crop. Off: that row is absent. pictureMode -1 shows
    // Letterbox (the tip default) until a choice is restored or selected.
    bool rememberPicture = false;
    int pictureMode = -1;
    // CHAPTERS on and this play has PMS chapter markers. Off (the default)
    // ignores chapterRows, so the panel matches the tip.
    bool chapters = false;
    struct ObChapterRow {
        int64_t startMs = 0;
        std::string label;
        bool current = false;
    };
    std::vector<ObChapterRow> chapterRows;
    struct ObStreamRow {
        int id = 0;
        std::string label;
        bool current = false;
    };
    std::vector<ObStreamRow> subtitleRows;
    std::vector<ObStreamRow> audioRows;
    // Skip Intro / Up Next. Select and Back are remapped only while this is set.
    bool markerUiVisible = false;
    int64_t markerShownAtMs = -1;
    int64_t inputNowMs = -1;
    // SPELUNKR=1 TV on the owenb core: pad Up/Down zap (or move the guide cursor).
    bool spelunkrTv = false;
    bool spelunkrGuideOpen = false;
};

enum class ObMenuCmd : uint8_t {
    None, PlayPause, Stop, SkipBack, SkipForward, Library, Close, QueueAppend, QueuePlayNext, SkipToNext,
    MarkWatched, MarkUnwatched, MarkDropContinue, Subtitles, Audio, PickSubtitle, PickAudio, PickerBack,
    SafeArea, Picture149, PictureMode, Chapters, ChapterPrev, ChapterNext, PickChapter, Options, PlayFromStart
};

struct ObMenuItem {
    std::string label;
    ObMenuCmd cmd = ObMenuCmd::None;
    bool selectable = true;
    int streamId = 0;     // PickSubtitle / PickAudio
    bool current = false; // the stream that is open now, or the chapter that contains position
    int64_t chapterMs = -1; // PickChapter: PMS startTimeOffset
};

// A Program-menu stream choice. audio is false for a subtitle pick (id 0 = off).
struct ObStreamPick {
    bool valid = false;
    bool audio = false;
    int streamId = 0;
};

// A Program-menu chapter seek. startMs is the marker's startTimeOffset.
struct ObChapterPick {
    bool valid = false;
    int64_t startMs = -1;
};

// Play from start. restart is set only when IN_PLAY_OPTIONS accepted the row.
struct ObInPlayPick {
    bool restart = false;
};

// A Program-menu picture-mode choice. mode is PictureMode.
struct ObPicturePick {
    bool valid = false;
    int mode = -1;
};

inline std::string obClock(int64_t ms) {
    if (ms < 0)
        ms = 0;
    const int64_t s = ms / 1000, h = s / 3600, m = s / 60 % 60, ss = s % 60;
    char b[32];
    if (h)
        std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)ss);
    else
        std::snprintf(b, sizeof b, "%lld:%02lld", (long long)m, (long long)ss);
    return b;
}

inline std::string obChapterMenuLabel(int64_t startMs, const std::string& title, bool current) {
    std::string name = title;
    if (name.size() > 48)
        name.resize(48);
    std::string label = obClock(startMs) + "  " + (name.empty() ? std::string("Chapter") : name);
    if (current)
        label += "  *";
    return label;
}

inline void obPushChapterItems(std::vector<ObMenuItem>& v, const ObMenuState& s) {
#ifdef MUT_CHAPTERS_MENU_WHEN_OFF
    const bool on = s.playing && !s.chapterRows.empty();
    (void)s.chapters;
#else
    const bool on = s.playing && s.chapters && !s.chapterRows.empty();
#endif
    if (!on)
        return;
    v.push_back({"Previous chapter", ObMenuCmd::ChapterPrev, true});
    v.push_back({"Next chapter", ObMenuCmd::ChapterNext, true});
    v.push_back({"Chapters", ObMenuCmd::Chapters, true});
}

inline std::vector<ObMenuItem> obChapterPickerItems(const ObMenuState& s) {
    std::vector<ObMenuItem> v;
    v.push_back({"Chapters", ObMenuCmd::None, false});
    for (const ObMenuState::ObChapterRow& r : s.chapterRows) {
        ObMenuItem it;
        it.label = r.label;
        it.cmd = ObMenuCmd::PickChapter;
        it.selectable = true;
        it.chapterMs = r.startMs;
        it.current = r.current;
        v.push_back(std::move(it));
    }
    v.push_back({"Back", ObMenuCmd::PickerBack, true});
    return v;
}

inline std::vector<chapters::Chapter> obChaptersOf(const ObMenuState& s) {
    std::vector<chapters::Chapter> ch;
    ch.reserve(s.chapterRows.size());
    for (const ObMenuState::ObChapterRow& r : s.chapterRows) {
        chapters::Chapter c;
        c.startMs = r.startMs;
        ch.push_back(c);
    }
    return ch;
}

inline void obPushStreamItems(std::vector<ObMenuItem>& v, const ObMenuState& s) {
#ifdef MUT_SETSTREAMS_OFF_STILL_PICKS
    const bool on = true;
    (void)s.setStreams;
#else
    const bool on = s.setStreams;
#endif
    if (!s.playing || !on)
        return;
    if (!s.subtitleRows.empty())
        v.push_back({"Subtitles", ObMenuCmd::Subtitles, true});
    if (!s.audioRows.empty())
        v.push_back({"Audio", ObMenuCmd::Audio, true});
}

inline std::vector<ObMenuItem> obStreamPickerItems(const ObMenuState& s, bool audio) {
    std::vector<ObMenuItem> v;
    v.push_back({audio ? "Audio" : "Subtitles", ObMenuCmd::None, false});
    const std::vector<ObMenuState::ObStreamRow>& rows = audio ? s.audioRows : s.subtitleRows;
    const ObMenuCmd cmd = audio ? ObMenuCmd::PickAudio : ObMenuCmd::PickSubtitle;
    for (const ObMenuState::ObStreamRow& r : rows) {
        ObMenuItem it;
        it.label = r.label;
        it.cmd = cmd;
        it.selectable = true;
        it.streamId = r.id;
        it.current = r.current;
        v.push_back(std::move(it));
    }
    v.push_back({"Back", ObMenuCmd::PickerBack, true});
    return v;
}

inline const char* picture149MenuLabel(bool on) { return on ? "Picture: 14:9" : "Picture: full"; }

inline void obPushPicture149(std::vector<ObMenuItem>& v, const ObMenuState& s) {
#ifdef MUT_PICTURE149_MENU_WHEN_OFF
    if (!s.playing)
        return;
#else
    if (!s.playing || !s.picture149)
        return;
#endif
    v.push_back({picture149MenuLabel(s.picture149On), ObMenuCmd::Picture149, true});
}

inline void obPushInPlay(std::vector<ObMenuItem>& v, const ObMenuState& s) {
#ifdef MUT_INPLAY_MENU_WHEN_OFF
    if (!s.playing)
        return;
#else
    if (!s.playing || !s.inPlayOptions)
        return;
#endif
    v.push_back({"Options", ObMenuCmd::Options, true});
}

inline std::vector<ObMenuItem> obInPlayItems(const ObMenuState& s) {
    std::vector<ObMenuItem> v;
    v.push_back({"Options", ObMenuCmd::None, false});
    const std::string info = inPlayInfoText(s.inPlayOptions, s.inPlayW, s.inPlayH);
    if (!info.empty())
        v.push_back({info, ObMenuCmd::None, false});
    v.push_back({"Play from start", ObMenuCmd::PlayFromStart, true});
    v.push_back({"Back", ObMenuCmd::PickerBack, true});
    return v;
}

inline void obPushPictureMode(std::vector<ObMenuItem>& v, const ObMenuState& s) {
#ifdef MUT_REMEMBER_PICTURE_MENU_WHEN_OFF
    if (!s.playing)
        return;
    (void)s.rememberPicture;
#else
    if (!s.playing || !s.rememberPicture)
        return;
#endif
    const PictureMode shown = pictureModeShown(s.pictureMode);
    v.push_back({std::string("Picture: ") + pictureModeLabel(shown), ObMenuCmd::PictureMode, true});
}

inline void obPushMarkItems(std::vector<ObMenuItem>& v, bool on) {
#ifdef MUT_MW_MENU_WHEN_OFF
    (void)on;
    on = true;
#endif
    if (!on)
        return;
    v.push_back({"Mark watched", ObMenuCmd::MarkWatched, true});
    v.push_back({"Mark unwatched", ObMenuCmd::MarkUnwatched, true});
    v.push_back({"Remove from Continue Watching", ObMenuCmd::MarkDropContinue, true});
}

inline std::vector<ObMenuItem> obMenuItems(const ObMenuState& s) {
    std::vector<ObMenuItem> v;
    if (s.playing) {
        std::string now = s.title.empty() ? "Playing" : s.title;
        if (s.positionMs >= 0)   // the position only: the bar under the panel has the duration
            now += "  " + obClock(s.positionMs);
        v.push_back({now, ObMenuCmd::None, false});
        v.push_back({s.paused ? "Resume" : "Pause", ObMenuCmd::PlayPause, true});
        v.push_back({"Back " + std::to_string(s.skipBackS) + " s", ObMenuCmd::SkipBack, true});
        v.push_back({"Forward " + std::to_string(s.skipForwardS) + " s", ObMenuCmd::SkipForward, true});
        if (s.queueHasNext)
            v.push_back({"Skip to next", ObMenuCmd::SkipToNext, true});
        obPushChapterItems(v, s);
        obPushStreamItems(v, s);
        obPushPictureMode(v, s);
        if (!s.rememberPicture)
            obPushPicture149(v, s);
        obPushInPlay(v, s);
        v.push_back({s.spelunkrTv ? "Exit TV" : "Stop", ObMenuCmd::Stop, true});
        obPushMarkItems(v, s.markWatched);
        if (s.libraryAvailable)
            v.push_back({"Library (stops the play)", ObMenuCmd::Library, true});
    } else {
      if (s.spelunkrTv)
        v.push_back({"Exit TV", ObMenuCmd::Stop, true});
      if (s.libraryAvailable)
        v.push_back({"Browse the library", ObMenuCmd::Library, true});
      if (s.queueUi && s.libraryVisible) {
        v.push_back({"Add to queue", ObMenuCmd::QueueAppend, true});
        v.push_back({"Play next", ObMenuCmd::QueuePlayNext, true});
        obPushMarkItems(v, s.markWatched);
      }
        if (!s.castName.empty())   // the name Plex apps cast to
            v.push_back({s.castName, ObMenuCmd::None, false});
    }
    if (s.pmsState == 1 || s.pmsState == 0)   // the main loop's PMS probe (the server's name is on its error screen)
        v.push_back({s.pmsState == 1 ? "Server online" : "Server offline", ObMenuCmd::None, false});
    v.push_back({"Safe area", ObMenuCmd::SafeArea, true});
    v.push_back({"Close", ObMenuCmd::Close, true});
    return v;
}

enum class ObMenuPage : uint8_t { Main, Subtitles, Audio, Chapters, Options };

// The panel: items rebuilt from the state on every refresh (a pause, the position, the server),
// the cursor kept on the same command; it rests only on selectable rows.
// Subtitles / Audio replace the list until Back. A stream row closes the panel and stores a pick.
class ObProgramMenu {
public:
    bool isOpen() const { return open_; }
    void open(const ObMenuState& s) {
        open_ = true;
        page_ = ObMenuPage::Main;
        pick_ = {};
        chapterPick_ = {};
        state_ = s;
        items_ = obMenuItems(s);
        sel_ = -1;
        moveTo(0, 1);
    }
    void close() {
        open_ = false;
        page_ = ObMenuPage::Main;
        pick_ = {};
        chapterPick_ = {};
        picturePick_ = {};
    }
    void refresh(const ObMenuState& s) {
        if (!open_)
            return;
#ifdef MUT_MENU_STALE
        (void)s;   // mutant: the panel keeps the items it opened with (Pause after a pause)
#else
        state_ = s;
        if ((page_ == ObMenuPage::Subtitles || page_ == ObMenuPage::Audio) && (!s.setStreams || !s.playing))
            page_ = ObMenuPage::Main;
        if (page_ == ObMenuPage::Chapters && (!s.chapters || !s.playing || s.chapterRows.empty()))
            page_ = ObMenuPage::Main;
        if (page_ == ObMenuPage::Options && (!s.inPlayOptions || !s.playing))
            page_ = ObMenuPage::Main;
        const ObMenuCmd keep = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)].cmd : ObMenuCmd::None;
        const int keepId = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)].streamId : 0;
        const int64_t keepMs = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)].chapterMs : -1;
        items_ = itemsForPage();
        sel_ = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (!items_[i].selectable || items_[i].cmd != keep)
                continue;
            if (page_ == ObMenuPage::Chapters && items_[i].chapterMs != keepMs)
                continue;
            if (page_ != ObMenuPage::Main && page_ != ObMenuPage::Chapters && items_[i].streamId != keepId)
                continue;
            sel_ = int(i);
            break;
        }
        if (sel_ < 0)
            moveTo(0, 1);
#endif
    }
    // a navigation while open: the command chosen (the panel closes), or None.
    // Subtitles / Audio stay open and swap in the stream list. Back from that list
    // returns to the main rows.
    ObMenuCmd nav(ObNav n) { return nav(n, state_); }
    ObMenuCmd nav(ObNav n, const ObMenuState& s) {
        if (!open_)
            return ObMenuCmd::None;
        state_ = s;
        pick_ = {};
        chapterPick_ = {};
        picturePick_ = {};
        switch (n) {
        case ObNav::Up:
        case ObNav::Left:
            moveTo(sel_ - 1, -1);
            return ObMenuCmd::None;
        case ObNav::Down:
        case ObNav::Right:
            moveTo(sel_ + 1, 1);
            return ObMenuCmd::None;
        case ObNav::Select:
        case ObNav::PlayPause:
            return selectRow();
        case ObNav::Back:
            if (page_ != ObMenuPage::Main) {
                showMain(pagePrefer());
                return ObMenuCmd::None;
            }
            open_ = false;
            return ObMenuCmd::Close;
        case ObNav::Menu:
        case ObNav::Aux:
        case ObNav::Home:
            open_ = false;
            page_ = ObMenuPage::Main;
            return ObMenuCmd::Close;
        default:
            return ObMenuCmd::None;
        }
    }
    ObStreamPick takeStreamPick() {
        const ObStreamPick p = pick_;
        pick_ = {};
        return p;
    }
    ObChapterPick takeChapterPick() {
        const ObChapterPick p = chapterPick_;
        chapterPick_ = {};
        return p;
    }
    ObInPlayPick takeInPlayPick() {
        const ObInPlayPick p = inPlayPick_;
        inPlayPick_ = {};
        return p;
    }
    ObPicturePick takePicturePick() {
        const ObPicturePick p = picturePick_;
        picturePick_ = {};
        return p;
    }
    owenb_osd::Menu view() const {
        owenb_osd::Menu m;
        m.open = open_;
        m.title = "MiSTerPlex";
        for (const ObMenuItem& it : items_) {
            m.items.push_back(it.label);
            m.dim.push_back(it.selectable ? 0 : 1);
        }
        m.selected = sel_;
        return m;
    }
    const std::vector<ObMenuItem>& items() const { return items_; }
    int selected() const { return sel_; }
    ObMenuPage page() const { return page_; }

private:
    std::vector<ObMenuItem> itemsForPage() const {
        if (page_ == ObMenuPage::Subtitles)
            return obStreamPickerItems(state_, false);
        if (page_ == ObMenuPage::Audio)
            return obStreamPickerItems(state_, true);
        if (page_ == ObMenuPage::Chapters)
            return obChapterPickerItems(state_);
        if (page_ == ObMenuPage::Options)
            return obInPlayItems(state_);
        return obMenuItems(state_);
    }
    ObMenuCmd pagePrefer() const {
        if (page_ == ObMenuPage::Audio)
            return ObMenuCmd::Audio;
        if (page_ == ObMenuPage::Chapters)
            return ObMenuCmd::Chapters;
        if (page_ == ObMenuPage::Options)
            return ObMenuCmd::Options;
        return ObMenuCmd::Subtitles;
    }
    void showMain(ObMenuCmd prefer) {
        page_ = ObMenuPage::Main;
        items_ = obMenuItems(state_);
        sel_ = -1;
        if (prefer != ObMenuCmd::None) {
            for (size_t i = 0; i < items_.size(); ++i) {
                if (items_[i].selectable && items_[i].cmd == prefer) {
                    sel_ = int(i);
                    return;
                }
            }
        }
        moveTo(0, 1);
    }
    void showPicker(bool audio) {
        page_ = audio ? ObMenuPage::Audio : ObMenuPage::Subtitles;
        items_ = obStreamPickerItems(state_, audio);
        sel_ = -1;
        int land = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].selectable && items_[i].current) {
                land = int(i);
                break;
            }
        }
        moveTo(land < 0 ? 0 : land, 1);
    }
    void showChapters() {
        page_ = ObMenuPage::Chapters;
        items_ = obChapterPickerItems(state_);
        sel_ = -1;
        int land = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].selectable && items_[i].current) {
                land = int(i);
                break;
            }
        }
        moveTo(land < 0 ? 0 : land, 1);
    }
    void showOptions() {
        page_ = ObMenuPage::Options;
        items_ = obInPlayItems(state_);
        sel_ = -1;
        moveTo(0, 1);
    }
    ObMenuCmd selectRow() {
        const ObMenuItem it = sel_ >= 0 && sel_ < int(items_.size()) ? items_[size_t(sel_)] : ObMenuItem{};
        if (page_ == ObMenuPage::Main && (it.cmd == ObMenuCmd::Subtitles || it.cmd == ObMenuCmd::Audio)) {
            showPicker(it.cmd == ObMenuCmd::Audio);
            return ObMenuCmd::None;
        }
        if (page_ == ObMenuPage::Main && it.cmd == ObMenuCmd::Chapters) {
            if (!state_.chapters || state_.chapterRows.empty())
                return ObMenuCmd::None;
            showChapters();
            return ObMenuCmd::None;
        }
        if (page_ == ObMenuPage::Main && it.cmd == ObMenuCmd::Options) {
#ifdef MUT_INPLAY_NO_PAGE
            return ObMenuCmd::None;
#else
            if (!state_.inPlayOptions)
                return ObMenuCmd::None;
            showOptions();
            return ObMenuCmd::None;
#endif
        }
        if (it.cmd == ObMenuCmd::PlayFromStart) {
            if (inPlayAcceptRestart(state_.inPlayOptions, true))
                inPlayPick_.restart = true;
            open_ = false;
            page_ = ObMenuPage::Main;
            return ObMenuCmd::None;
        }
        if (it.cmd == ObMenuCmd::ChapterPrev || it.cmd == ObMenuCmd::ChapterNext) {
            if (!state_.chapters)
                return ObMenuCmd::None;
            const std::vector<chapters::Chapter> ch = obChaptersOf(state_);
            const int64_t target = it.cmd == ObMenuCmd::ChapterNext
                                       ? chapters::nextChapterStart(ch, state_.positionMs)
                                       : chapters::prevChapterStart(ch, state_.positionMs);
            if (target < 0)
                return ObMenuCmd::None;
#ifndef MUT_CHAPTERS_IGNORE_PICK
            chapterPick_.valid = true;
            chapterPick_.startMs = target;
#endif
            open_ = false;
            page_ = ObMenuPage::Main;
            return ObMenuCmd::None;
        }
        if (page_ == ObMenuPage::Chapters && it.cmd == ObMenuCmd::PickChapter) {
#ifndef MUT_CHAPTERS_IGNORE_PICK
            if (state_.chapters && it.chapterMs >= 0) {
                chapterPick_.valid = true;
                chapterPick_.startMs = it.chapterMs;
            }
#endif
            open_ = false;
            page_ = ObMenuPage::Main;
            return ObMenuCmd::None;
        }
        if (it.cmd == ObMenuCmd::PickerBack) {
            showMain(pagePrefer());
            return ObMenuCmd::None;
        }
        if (it.cmd == ObMenuCmd::Picture149) {
            picture149TogglePlay();
            state_.picture149On = picture149Active();
            showMain(ObMenuCmd::Picture149);
            return ObMenuCmd::None;
        }
        if (it.cmd == ObMenuCmd::PictureMode) {
#ifdef MUT_REMEMBER_PICTURE_MENU_WHEN_OFF
            const bool allow = state_.playing;
#else
            const bool allow = state_.playing && state_.rememberPicture;
#endif
            if (!allow)
                return ObMenuCmd::None;
            const int next = static_cast<int>(pictureModeCycle(pictureModeShown(state_.pictureMode)));
            state_.pictureMode = next;
            pictureSessionMode().store(next);
            picturePick_.valid = true;
            picturePick_.mode = next;
            showMain(ObMenuCmd::PictureMode);
            return ObMenuCmd::None;
        }
        if (page_ != ObMenuPage::Main && (it.cmd == ObMenuCmd::PickSubtitle || it.cmd == ObMenuCmd::PickAudio)) {
#ifndef MUT_SETSTREAMS_IGNORE_PICK
            if (state_.setStreams) {
                pick_.valid = true;
                pick_.audio = it.cmd == ObMenuCmd::PickAudio;
                pick_.streamId = it.streamId;
            }
#endif
            open_ = false;
            page_ = ObMenuPage::Main;
            return it.cmd;
        }
        if (it.cmd != ObMenuCmd::None) {
            open_ = false;
            page_ = ObMenuPage::Main;
        }
        return it.cmd;
    }
    void moveTo(int from, int dir) {   // the next selectable row from `from` in `dir`, wrapping
        const int n = int(items_.size());
        if (n == 0)
            return;
        int i = ((from % n) + n) % n;
        for (int k = 0; k < n; ++k, i = (i + dir + n) % n)
            if (items_[size_t(i)].selectable) {
                sel_ = i;
                return;
            }
    }
    bool open_ = false;
    ObMenuPage page_ = ObMenuPage::Main;
    ObMenuState state_{};
    ObStreamPick pick_{};
    ObChapterPick chapterPick_{};
    ObInPlayPick inPlayPick_{};
    ObPicturePick picturePick_{};
    std::vector<ObMenuItem> items_;
    int sel_ = -1;
};

inline PlaybackCommand obMenuCommand(ObMenuCmd c) {
    switch (c) {
    case ObMenuCmd::PlayPause: return PlaybackCommand::PlayPause;
    case ObMenuCmd::Stop: return PlaybackCommand::Stop;
    case ObMenuCmd::SkipBack: return PlaybackCommand::SkipBack;
    case ObMenuCmd::SkipForward: return PlaybackCommand::SkipForward;
    case ObMenuCmd::Library: return PlaybackCommand::Browse;
    case ObMenuCmd::QueueAppend: return PlaybackCommand::Info;
    case ObMenuCmd::QueuePlayNext: return PlaybackCommand::QueuePlayNext;
    case ObMenuCmd::SkipToNext: return PlaybackCommand::PageDown;
    case ObMenuCmd::MarkWatched: return PlaybackCommand::MarkWatched;
    case ObMenuCmd::MarkUnwatched: return PlaybackCommand::MarkUnwatched;
    case ObMenuCmd::MarkDropContinue: return PlaybackCommand::MarkDropContinue;
    case ObMenuCmd::Subtitles:
    case ObMenuCmd::Audio:
    case ObMenuCmd::PickSubtitle:
    case ObMenuCmd::PickAudio:
    case ObMenuCmd::PickerBack:
    case ObMenuCmd::SafeArea:
    case ObMenuCmd::Picture149:
    case ObMenuCmd::PictureMode:
    case ObMenuCmd::Chapters:
    case ObMenuCmd::ChapterPrev:
    case ObMenuCmd::ChapterNext:
    case ObMenuCmd::PickChapter:
    case ObMenuCmd::Options:
    case ObMenuCmd::PlayFromStart:
        return PlaybackCommand::None;
    default: return PlaybackCommand::None;
    }
}

// Where a navigation goes when the panel is not open (rule 1 is the caller's: ObProgramMenu::nav).
enum class ObRouteKind : uint8_t { None, OpenMenu, Command };
struct ObRoute {
    ObRouteKind kind = ObRouteKind::None;
    PlaybackCommand cmd = PlaybackCommand::None;
};

inline ObRoute obRoute(ObNav n, const ObMenuState& s) {
    auto cmd = [](PlaybackCommand c) {
        ObRoute r;
        r.kind = ObRouteKind::Command;
        r.cmd = c;
        return r;
    };
#ifdef MUT_ROUTE_INFO_BROWSE_ALWAYS
    if (n == ObNav::Info && s.libraryAvailable) return cmd(PlaybackCommand::Browse);   // mutant: X browses in a play / library
#endif
    if (s.spelunkrTv) {
      if (n == ObNav::Aux)
        return {ObRouteKind::OpenMenu, PlaybackCommand::None};
      if (n == ObNav::Menu || n == ObNav::Home || n == ObNav::Info ||
          n == ObNav::Back)
        return cmd(PlaybackCommand::GuideToggle);
      if (n == ObNav::Up || n == ObNav::PageUp)
        return cmd(PlaybackCommand::ChannelUp);
      if (n == ObNav::Down || n == ObNav::PageDown)
        return cmd(PlaybackCommand::ChannelDown);
      if (n == ObNav::Select || n == ObNav::PlayPause)
        return cmd((s.spelunkrGuideOpen || !s.playing)
                       ? PlaybackCommand::GuideTune
                       : PlaybackCommand::GuideToggle);  // GH#411: never Plex pause (frozen frame / CH 8)
      return {};
    }
    if (n == ObNav::Menu || (n == ObNav::Aux && !s.playing)) { // Aux: pad Y, the menu off a play
        // Idle Program / Start / F12-after-OSD: skip the "Browse the library"
        // panel. Y (Aux) still opens the panel so transport stays one button
        // away. Keyboard P is that Aux only on Ready idle when IDLE_MENU_KEY is
        // on (obIdleMenuKey). P is not a key in a play or in PlexUI.
        if (n == ObNav::Menu && !s.playing && !s.libraryVisible && s.libraryAvailable)
            return cmd(PlaybackCommand::Browse);
        ObRoute r;
        r.kind = ObRouteKind::OpenMenu;
        return r;
    }
    if (s.libraryVisible && s.libraryLegacy) {
        switch (n) {
        case ObNav::Up:
        case ObNav::PageUp:
#ifndef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Left:   // d-pad moves; Left is not back
#endif
            return cmd(PlaybackCommand::SkipBack);
        case ObNav::Down:
        case ObNav::PageDown:
#ifndef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Right:
#endif
            return cmd(PlaybackCommand::SkipForward);
        case ObNav::Select:
        case ObNav::PlayPause:
#ifdef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Right:   // mutant: Right selects, the pre-#121 list
#endif
            return cmd(PlaybackCommand::PlayPause);
        case ObNav::Back:
        case ObNav::Home:
#ifdef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Left:
#endif
            return cmd(PlaybackCommand::Stop);
        default: return {};
        }
    }
    if (s.libraryVisible) {   // local and remote rows share this (browse_controller.hpp)
        switch (n) {
        case ObNav::Up:
#ifndef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Left:
#endif
            return cmd(PlaybackCommand::Up);
        case ObNav::Down:
#ifndef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Right:
#endif
            return cmd(PlaybackCommand::Down);
        case ObNav::PageUp: return cmd(PlaybackCommand::PageUp);
        case ObNav::PageDown: return cmd(PlaybackCommand::PageDown);
        case ObNav::Info: return cmd(PlaybackCommand::Info);
        case ObNav::Select:
        case ObNav::PlayPause:
#ifdef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Right:
#endif
            return cmd(PlaybackCommand::PlayPause);
        case ObNav::Back:
        case ObNav::Home:
#ifdef MUT_RC1037_DPAD_ACTIVATE
        case ObNav::Left:
#endif
            return cmd(PlaybackCommand::Back);
        default: return {};
        }
    }
    if (s.playing && s.markerUiVisible) {
        if (n == ObNav::Select) {
            // A pause that was already in flight must not become a skip.
            if (s.markerShownAtMs >= 0 && s.inputNowMs >= 0 && s.inputNowMs - s.markerShownAtMs < 500)
                return {};
            return cmd(PlaybackCommand::MarkerActivate);
        }
        if (n == ObNav::Back)
            return cmd(PlaybackCommand::MarkerDismiss);
    }
    if (s.playing) {
        switch (n) {
        case ObNav::Select:
        case ObNav::PlayPause: return cmd(PlaybackCommand::PlayPause);
        case ObNav::Left: return cmd(PlaybackCommand::SkipBack);
        case ObNav::Right: return cmd(PlaybackCommand::SkipForward);
        case ObNav::PageDown:
            if (s.queueHasNext)
                return cmd(PlaybackCommand::PageDown);
            return {};
#ifndef MUT_ROUTE_PLAY_NO_TRANSPORT
        // v2-bugfix ( lab bug 3): the RBF's J line puts its transport on pad bits 4-7 (the Plex cores'
        // "J1,Play/Pause,Stop,Skip Fwd,Skip Back"; owenb's A,B,X,Y): B / Esc Stop, X Skip Fwd, Y Skip Back.
        // Home and Up still never stop a play.
        case ObNav::Back:
            if (s.spelunkrTv)
                return {};   // GH#411: Stop would flash Ready to cast
            return cmd(PlaybackCommand::Stop);
#ifdef MUT_ROUTE_INFO_STOPS_PLAY
        case ObNav::Info: return cmd(PlaybackCommand::Stop);   // mutant: X throws the play away
#else
        case ObNav::Info: return cmd(PlaybackCommand::SkipForward);
#endif
        case ObNav::Aux: return cmd(PlaybackCommand::SkipBack);
#endif
#ifdef MUT_ROUTE_HOME_STOPS_PLAY
        case ObNav::Home: return cmd(PlaybackCommand::Browse);   // mutant: Home throws the play away
#endif
        default: return {};   // Home, Back, Up/Down: never stop a play by accident (the menu has Stop)
        }
    }
    // Idle only: pad X (Info) opens Plex browse like A / Select / Home. Every branch above returns first, so X stays
    // Skip Fwd in a play and Info inside the library (no re-open loop).
    if (!s.spelunkrTv && s.libraryAvailable &&
        (n == ObNav::Select || n == ObNav::Down || n == ObNav::Home || n == ObNav::Info))
        return cmd(PlaybackCommand::Browse);
    return {};
}

// The panel and the routing together: one navigation -> the command to dispatch (None: nothing),
// menuChanged when the panel's view must be published again (UiBus::setMenu).
struct ObOverscanEvent {
    bool changed = false;
    bool saved = false;
    OverscanEdges edges{};
    owenb_osd::OverscanView view{};
};

class ObInputRouter {
public:
    void noteOverscanSeed(int leftPct, int rightPct, int topPct, int bottomPct) {
        seedL_ = clampOverscanPct(leftPct);
        seedR_ = clampOverscanPct(rightPct);
        seedT_ = clampOverscanPct(topPct);
        seedB_ = clampOverscanPct(bottomPct);
    }
    bool overscanOpen() const { return overscan_.open; }
    ObOverscanEvent takeOverscanEvent() {
        ObOverscanEvent e;
        if (!pendingOv_)
            return e;
        e.changed = true;
        e.saved = savedOv_;
        e.edges = overscanEdges();
        e.view.open = overscan_.open;
        e.view.focus = static_cast<int>(overscan_.focus);
        e.view.left = overscan_.left;
        e.view.right = overscan_.right;
        e.view.top = overscan_.top;
        e.view.bottom = overscan_.bottom;
        pendingOv_ = false;
        savedOv_ = false;
        return e;
    }
    PlaybackCommand handle(ObNav n, const ObMenuState& s, bool& menuChanged) {
        menuChanged = false;
        if (overscan_.open) {
            stepOverscan(n);
            return PlaybackCommand::None;
        }
        if (menu_.isOpen()) {
            const ObMenuCmd c = menu_.nav(n, s);
            menuChanged = true;
            if (c == ObMenuCmd::SafeArea)
                openOverscan();
            return obMenuCommand(c);
        }
        const ObRoute r = obRoute(n, s);
        if (r.kind == ObRouteKind::OpenMenu) {
            menu_.open(s);
            menuChanged = true;
            return PlaybackCommand::None;
        }
        return r.kind == ObRouteKind::Command ? r.cmd : PlaybackCommand::None;
    }
    // each tick while the panel is open: true when its view changed (the position, a pause, the server)
    bool refresh(const ObMenuState& s) {
        if (!menu_.isOpen())
            return false;
        const std::vector<ObMenuItem> before = menu_.items();
        const int sel = menu_.selected();
        menu_.refresh(s);
        if (sel != menu_.selected() || before.size() != menu_.items().size())
            return true;
        for (size_t i = 0; i < before.size(); ++i)
            if (before[i].label != menu_.items()[i].label)
                return true;
        return false;
    }
    void close() { menu_.close(); }
    ObStreamPick takeStreamPick() { return menu_.takeStreamPick(); }
    ObChapterPick takeChapterPick() { return menu_.takeChapterPick(); }
    ObInPlayPick takeInPlayPick() { return menu_.takeInPlayPick(); }
    ObPicturePick takePicturePick() { return menu_.takePicturePick(); }
    const ObProgramMenu& menu() const { return menu_; }

private:
    void openOverscan() {
        overscan_ = beginOverscanCal(overscanEdges(), seedL_, seedR_, seedT_, seedB_);
        pendingOv_ = true;
        savedOv_ = false;
    }
    void publishOverscan(bool saved) {
        pendingOv_ = true;
        savedOv_ = saved;
    }
    void stepOverscan(ObNav n) {
        switch (n) {
        case ObNav::Left: overscanNudge(overscan_, -1); publishOverscan(false); break;
        case ObNav::Right: overscanNudge(overscan_, 1); publishOverscan(false); break;
        case ObNav::Up: overscanCycle(overscan_, -1); publishOverscan(false); break;
        case ObNav::Down: overscanCycle(overscan_, 1); publishOverscan(false); break;
        case ObNav::Select:
        case ObNav::PlayPause:
#ifndef MUT_OVERSCAN_NO_SAVE
            setOverscanEdges(edgesOf(overscan_));
            savedOv_ = true;
#else
            savedOv_ = false;
#endif
            overscan_.open = false;
            pendingOv_ = true;
            break;
        case ObNav::Back:
        case ObNav::Menu:
        case ObNav::Home:
        case ObNav::Aux:
#ifdef MUT_OVERSCAN_SAVE_ON_CANCEL
            setOverscanEdges(edgesOf(overscan_));
            savedOv_ = true;
#else
            savedOv_ = false;
#endif
            overscan_.open = false;
            pendingOv_ = true;
            break;
        default:
            break;
        }
    }
    ObProgramMenu menu_;
    OverscanCal overscan_{};
    bool pendingOv_ = false;
    bool savedOv_ = false;
    int seedL_ = 10, seedR_ = 10, seedT_ = 10, seedB_ = 10;
};

} // namespace misterplex::owenb
