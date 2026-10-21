// Home library list: sections + items, scroll, and the 624-wide idle-FB paint.
#include "libmisterplex/browse_library_list.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int fails = 0;

void expect(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "FAIL browse_library_list: %s\n", msg);
        ++fails;
    }
}

const std::uint8_t* px(const std::vector<std::uint8_t>& rgb, int stride, int x, int y) {
    return rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) +
           static_cast<std::size_t>(x) * 3u;
}

bool sameRgb(const std::uint8_t* p, const std::uint8_t rgb[3]) {
    return p[0] == rgb[0] && p[1] == rgb[1] && p[2] == rgb[2];
}

int firstSetBit(unsigned char bits) {
    for (int i = 0; i < 8; ++i) {
        if (bits & (0x80u >> i))
            return i;
    }
    return -1;
}

} // namespace

int main() {
    using namespace misterplex::librarylist;

    expect(!browseLibListEnabled(nullptr), "unset is off");
    expect(!browseLibListEnabled(""), "empty is off");
    expect(!browseLibListEnabled("0"), "0 is off");
    expect(!browseLibListEnabled("true"), "true is off");
    expect(browseLibListEnabled("1"), "1 is on");

    const char* sections =
        "<MediaContainer>"
        "<Directory key=\"2\" title=\"Movies\" type=\"movie\"/>"
        "<Directory key=\"/library/sections/5\" title=\"Shows\" type=\"show\"/>"
        "<Directory key=\"9\" title=\"\" type=\"artist\"/>"
        "</MediaContainer>";
    std::vector<std::string> got;
    const HomeLibraryFetch fetched = fetchHomeLibrary([&](const std::string& path) {
        got.push_back(path);
        if (path == "/library/sections")
            return std::string(sections);
        if (path == "/library/sections/2/all")
            return std::string("<MediaContainer>"
                               "<Video ratingKey=\"1\" title=\"BBB\"/>"
                               "<Video ratingKey=\"8\" title=\"A &amp; B\"/>"
                               "</MediaContainer>");
        if (path == "/library/sections/5/all")
            return std::string("<MediaContainer><Directory ratingKey=\"3\" title=\"Trek\"/></MediaContainer>");
        if (path == "/library/sections/9/all")
            return std::string();
        return std::string("UNEXPECTED");
    });
    expect(got.size() == 4, "four GETs");
    expect(got.size() > 0 && got[0] == "/library/sections", "sections first");
    expect(got.size() > 1 && got[1] == "/library/sections/2/all", "numeric key /all");
    expect(got.size() > 2 && got[2] == "/library/sections/5/all", "absolute key /all");
    expect(got.size() > 3 && got[3] == "/library/sections/9/all", "empty title still fetched");
    expect(fetched.sectionsOk, "sections ok");
    expect(fetched.list.rows.size() == 6, "3 sections + 3 titles");
    expect(fetched.list.rows[0].kind == ListKind::Section && fetched.list.rows[0].title == "Movies",
           "movies header");
    expect(fetched.list.rows[1].kind == ListKind::Item && fetched.list.rows[1].title == "BBB" &&
               fetched.list.rows[1].key == "1",
           "BBB item");
    expect(fetched.list.rows[2].title == "A & B", "entity decoded title");
    expect(fetched.list.rows[3].title == "Shows", "shows header");
    expect(fetched.list.rows[4].title == "Trek" && fetched.list.rows[4].kind == ListKind::Item, "show title");
    expect(fetched.list.rows[5].kind == ListKind::Section && fetched.list.rows[5].title == "9",
           "empty title falls back to key");
    expect(fetched.list.cursor == 1, "cursor starts on the first item");

    const HomeLibraryFetch failed = fetchHomeLibrary([](const std::string&) { return std::string(); });
    expect(!failed.sectionsOk && failed.list.status == "FETCH FAILED" && failed.list.rows.empty(),
           "empty body is FETCH FAILED");
    expect(failed.paths.size() == 1, "no item GET after a failed sections body");

    const HomeLibraryFetch none = fetchHomeLibrary(PathGetFn{});
    expect(none.list.status == "NO PLEX SERVER" && none.paths.empty(), "no fetch function");

    std::string many = "<MediaContainer>";
    for (int i = 0; i < kLibraryListMaxItemsPerSection + 5; ++i)
        many += "<Video ratingKey=\"" + std::to_string(i) + "\" title=\"T" + std::to_string(i) + "\"/>";
    many += "</MediaContainer>";
    std::vector<SectionBody> bodies{{"1", many}};
    const LibraryList capped =
        buildLibraryList("<MediaContainer><Directory key=\"1\" title=\"Movies\"/></MediaContainer>", bodies, true);
    int items = 0;
    for (const ListRow& row : capped.rows)
        if (row.kind == ListKind::Item)
            ++items;
    expect(items == kLibraryListMaxItemsPerSection && capped.status == "TRUNCATED", "item cap");

    LibraryList scroll;
    scroll.rows.push_back(ListRow{ListKind::Section, "Movies", "1", "1"});
    for (int i = 0; i < 30; ++i)
        scroll.rows.push_back(ListRow{ListKind::Item, "Title " + std::to_string(i), std::to_string(i), "1"});
    scroll.cursor = 0;
    expect(libraryListViewportTop(scroll) == 0, "top at the start");
    moveLibraryList(scroll, -3);
    expect(scroll.cursor == 0, "does not wrap upward");
    moveLibraryList(scroll, 20);
    expect(scroll.cursor == 20, "moves down");
    expect(libraryListViewportTop(scroll) > 0, "viewport follows the cursor");
    const int top = libraryListViewportTop(scroll);
    expect(scroll.cursor >= top && scroll.cursor < top + kLibraryListViewRows, "cursor stays on screen");
    moveLibraryList(scroll, 1000);
    expect(scroll.cursor == static_cast<int>(scroll.rows.size()) - 1, "does not wrap past the end");

    LibraryList page;
    page.header = "Library";
    page.rows.push_back(ListRow{ListKind::Section, "Movies", "2", "2"});
    page.rows.push_back(ListRow{ListKind::Item, "BBB", "1", "2"});
    page.cursor = 1;
    const std::vector<std::uint8_t> packed = renderLibraryListPacked(page);
    expect(packed.size() == static_cast<std::size_t>(kLibraryListStride) * kLibraryListH, "624x480 packed");
    int gx = -1, gy = -1;
    const auto& glyphA = misterplex::kFont8x16[static_cast<std::size_t>('B' - 0x20)];
    for (int row = 0; row < kGlyphH && gy < 0; ++row) {
        gx = firstSetBit(glyphA[static_cast<std::size_t>(row)]);
        if (gx >= 0)
            gy = row;
    }
    expect(gx >= 0, "font has a B pixel");
    const int textY = kLibraryListY0 + kLibraryListRowH + 4;
    const std::uint8_t* ink = px(packed, kLibraryListStride, kLibraryListTextX + gx, textY + gy);
    expect(sameRgb(ink, kInk), "selected BBB glyph is ink on the amber bar");
    const std::uint8_t* bar = px(packed, kLibraryListStride, 10, textY);
    expect(sameRgb(bar, kAmber), "selection bar is amber");

    std::vector<std::uint8_t> idle(static_cast<std::size_t>(1920) * 480, 0x5A);
    blitLibraryListIdle(page, idle.data(), 640, 480, 1920);
    bool marginBg = true;
    for (int y = 0; y < 480 && marginBg; ++y) {
        for (int x = kLibraryListW; x < 640; ++x) {
            if (!sameRgb(px(idle, 1920, x, y), kBg))
                marginBg = false;
        }
    }
    expect(marginBg, "columns 624..639 of the idle FB stay background");
    const std::uint8_t* shown = px(idle, 1920, kLibraryListTextX + gx, textY + gy);
    expect(sameRgb(shown, kInk), "BBB is in the idle FB");

    LibraryList late = scroll;
    late.cursor = 0;
    moveLibraryList(late, 24);
    const int lateTop = libraryListViewportTop(late);
    expect(lateTop > 0 && late.rows[static_cast<std::size_t>(lateTop)].title != "Title 0",
           "scrolled viewport leaves the first title");

    // Mailbox up/down/A/B. Prod cores emit SkipBack/SkipForward/PlayPause/Stop;
    // wide cores emit Up/Down/Back. A on an item is the playMedia key. B hides
    // the list so the idle painter draws the chevron.
    LibraryList nav;
    nav.rows.push_back(ListRow{ListKind::Section, "Movies", "2", "2"});
    nav.rows.push_back(ListRow{ListKind::Item, "BBB", "1", "2"});
    nav.rows.push_back(ListRow{ListKind::Item, "Trek", "3", "2"});
    nav.cursor = 1;
    bool vis = true;
    const BrowseListStep up = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::SkipBack);
    expect(up.effect == BrowseListEffect::Move && nav.cursor == 0 && vis, "SkipBack moves up onto the header");
    const BrowseListStep upClamp = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Up);
    expect(upClamp.effect == BrowseListEffect::Move && nav.cursor == 0, "Up does not wrap");
    const BrowseListStep down = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::SkipForward);
    expect(down.effect == BrowseListEffect::Move && nav.cursor == 1 && nav.rows[1].title == "BBB",
           "SkipForward moves down onto BBB");
    const BrowseListStep downWide = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Down);
    expect(downWide.effect == BrowseListEffect::Move && nav.cursor == 2, "Down moves to Trek");
    const BrowseListStep backItem = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Up);
    expect(backItem.effect == BrowseListEffect::Move && nav.cursor == 1, "back on BBB");

    const BrowseListStep play = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::PlayPause);
    expect(play.effect == BrowseListEffect::Play && play.playKey == "1" &&
               play.playMediaKey == "/library/metadata/1" &&
               play.playMediaKey == browseListPlayMediaKey(play.playKey) && !vis,
           "A on BBB is playMedia /library/metadata/1 and hides the list");
    expect(nav.cursor == 1, "A leaves the cursor on the item");

    expect(browseListPlayMediaKey("/library/metadata/2") == "/library/metadata/2" &&
               browseListPlayRatingKey("/library/metadata/2") == "2",
           "a metadata path is the play path and the rating key is its id");
    LibraryList pathItem;
    pathItem.rows.push_back(ListRow{ListKind::Item, "Already", "/library/metadata/9", "1"});
    pathItem.cursor = 0;
    vis = true;
    const BrowseListStep pathPlay =
        stepBrowseListInput(pathItem, vis, false, misterplex::PlaybackCommand::PlayPause);
    expect(pathPlay.effect == BrowseListEffect::Play && pathPlay.playKey == "/library/metadata/9" &&
               pathPlay.playMediaKey == "/library/metadata/9" && !vis,
           "A on an item whose key is already a metadata path plays that path");

    vis = true;
    nav.cursor = 0;
    const BrowseListStep headerA = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::PlayPause);
    expect(headerA.effect == BrowseListEffect::Hold && headerA.playKey.empty() && vis && nav.cursor == 0,
           "A on a section header does not play");

    nav.cursor = 2;
    const BrowseListStep bee = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Stop);
    expect(bee.effect == BrowseListEffect::IdleChevron && !vis, "B hides the list for the idle chevron");
    const BrowseListStep backHidden =
        stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Back);
    expect(backHidden.effect == BrowseListEffect::Ignore && !vis,
           "Back while hidden does not reopen");

    vis = true;
    const BrowseListStep backVis = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Back);
    expect(backVis.effect == BrowseListEffect::IdleChevron && !vis, "Back while shown is the chevron");

    vis = false;
    const BrowseListStep pauseWhilePlay =
        stepBrowseListInput(nav, vis, true, misterplex::PlaybackCommand::PlayPause);
    expect(pauseWhilePlay.effect == BrowseListEffect::Ignore, "A during playback stays a pause");
    const BrowseListStep stopWhilePlay =
        stepBrowseListInput(nav, vis, true, misterplex::PlaybackCommand::Stop);
    expect(stopWhilePlay.effect == BrowseListEffect::Ignore, "B during playback stays a stop");
    const BrowseListStep openIdle =
        stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Stop);
    expect(openIdle.effect == BrowseListEffect::Open, "B while idle and hidden opens the list");

    expect(libraryListOwnsInput(true, false, misterplex::PlaybackCommand::Down),
           "visible idle Down is list-owned");
    expect(!libraryListOwnsInput(true, true, misterplex::PlaybackCommand::SkipForward),
           "visible playing SkipForward stays skip");
    expect(!libraryListOwnsInput(true, true, misterplex::PlaybackCommand::PlayPause),
           "visible playing PlayPause stays pause");
    expect(!libraryListOwnsInput(true, true, misterplex::PlaybackCommand::Stop),
           "visible playing Stop stays stop");
    expect(libraryListOwnsInput(false, false, misterplex::PlaybackCommand::Stop),
           "hidden idle Stop still opens the list");

    // Daemon-shaped wrapper: OwnsInput false skips stepBrowseListInput, so a
    // missing call site (always-step) fails the playing cases.
    nav.cursor = 1;
    vis = true;
    BrowseListStep owned{};
    const int cursorBeforeSkip = nav.cursor;
    expect(!consumeBrowseListInput(nav, vis, true, misterplex::PlaybackCommand::SkipForward, owned),
           "daemon shape: visible playing SkipForward is not consumed");
    expect(nav.cursor == cursorBeforeSkip && vis && owned.effect == BrowseListEffect::Ignore,
           "daemon shape: playing skip does not move");
    owned = BrowseListStep{};
    expect(!consumeBrowseListInput(nav, vis, true, misterplex::PlaybackCommand::PlayPause, owned),
           "daemon shape: visible playing PlayPause is not consumed");
    expect(vis && owned.effect == BrowseListEffect::Ignore && owned.playKey.empty(),
           "daemon shape: playing PlayPause does not hide or play");
    owned = BrowseListStep{};
    expect(!consumeBrowseListInput(nav, vis, true, misterplex::PlaybackCommand::Stop, owned),
           "daemon shape: visible playing Stop is not consumed");
    expect(vis && owned.effect == BrowseListEffect::Ignore,
           "daemon shape: playing Stop leaves the list visible");
    owned = BrowseListStep{};
    expect(consumeBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Down, owned),
           "daemon shape: visible idle Down is consumed");
    expect(owned.effect == BrowseListEffect::Move && nav.cursor == 2 && vis,
           "daemon shape: idle Down moves");
    vis = false;
    owned = BrowseListStep{};
    expect(consumeBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::Stop, owned),
           "daemon shape: hidden idle Stop is consumed");
    expect(owned.effect == BrowseListEffect::Open, "daemon shape: idle Stop opens");

    {
        std::string here = __FILE__;
        auto slash = here.find_last_of('/');
        std::string unitDir = slash == std::string::npos ? std::string(".") : here.substr(0, slash);
        slash = unitDir.find_last_of('/');
        std::string testsDir = slash == std::string::npos ? unitDir : unitDir.substr(0, slash);
        slash = testsDir.find_last_of('/');
        std::string root = slash == std::string::npos ? testsDir : testsDir.substr(0, slash);
        std::ifstream in(root + "/arm/misterplexd/media_player.cpp");
        expect(static_cast<bool>(in), "media_player.cpp readable");
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto fn = src.find("MediaPlayer::libraryHandleInputList");
        expect(fn != std::string::npos, "libraryHandleInputList present");
        const auto nextFn = src.find("\nvoid MediaPlayer::", fn + 1);
        const std::string body =
            src.substr(fn, nextFn == std::string::npos ? std::string::npos : nextFn - fn);
        expect(body.find("consumeBrowseListInput") != std::string::npos,
               "libraryHandleInputList calls consumeBrowseListInput");
        expect(body.find("stepBrowseListInput") == std::string::npos,
               "libraryHandleInputList does not call stepBrowseListInput directly");
        const auto playFn = src.find("MediaPlayer::playInternal");
        expect(playFn != std::string::npos, "playInternal present");
        const auto playNext = src.find("\nvoid MediaPlayer::", playFn + 1);
        const std::string playBody =
            src.substr(playFn, playNext == std::string::npos ? std::string::npos : playNext - playFn);
        expect(playBody.find("libraryListVisible_ = false") != std::string::npos,
               "playInternal clears libraryListVisible_");
    }

    nav.rows.push_back(ListRow{ListKind::Item, "NoKey", "", "2"});
    nav.cursor = 3;
    vis = true;
    const BrowseListStep noKey = stepBrowseListInput(nav, vis, false, misterplex::PlaybackCommand::PlayPause);
    expect(noKey.effect == BrowseListEffect::Hold && vis && noKey.playKey.empty(), "A on a keyless item does not play");

    LibraryList paged;
    for (int i = 0; i < 40; ++i)
        paged.rows.push_back(ListRow{ListKind::Item, "T" + std::to_string(i), std::to_string(i), "2"});
    paged.cursor = 0;
    vis = true;
    const BrowseListStep pageStep =
        stepBrowseListInput(paged, vis, false, misterplex::PlaybackCommand::PageDown);
    expect(pageStep.effect == BrowseListEffect::Move && paged.cursor == kLibraryListViewRows, "page down");

    if (fails) {
        std::fprintf(stderr, "test_browse_library_list: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_browse_library_list: OK\n");
    return 0;
}
