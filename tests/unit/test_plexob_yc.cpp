// Y/C on the real OB gate. The poll reads word 87 through OwenbCtrlReader after
// owenbGateRingProbe has opened the gate (owenbOpen, legacy open stays false).
// A stand-in plan struct is not the path under test.
#include "libmisterplex/core_gate.hpp"
#include "libmisterplex/core_id_select.hpp"
#include "libmisterplex/owenb_core_identity.hpp"
#include "libmisterplex/plexob_ini.hpp"
#include "libmisterplex/plexob_yc.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

using misterplex::CoreGate;
using misterplex::CoreGateEvent;
using misterplex::CoreObservation;
using misterplex::OwenbCtrlPageMap;
using misterplex::OwenbCtrlReader;
using misterplex::kFpgaMgrModeUser;
using misterplex::owenbGateRingProbe;
using misterplex::owenb_id::kCtrlBytes;
using misterplex::owenb_id::kCtrlPhys;
using misterplex::owenb_id::kMagic;
using misterplex::owenb_id::kMpxBuildIdTag;
using misterplex::owenb_id::kMpxBuildIdWord;
using misterplex::owenb_id::kRingBytes;
using misterplex::owenb_id::kVersion;
using misterplex::plexob::YcPollFiles;
using misterplex::plexob::YcPollResult;
using misterplex::plexob::YcTickState;
using misterplex::plexob::kPlexObYcBuildId;
using misterplex::plexob::kRingWordLive;
using misterplex::plexob::kRingWordYcMode;
using misterplex::plexob::kYcOsdOpenBit;
using misterplex::plexob::kYcReloadAttemptCap;
using misterplex::plexob::kYcStableUs;
using misterplex::plexob::pollYcOnOwenbGate;
using misterplex::plexob::kPlexObConfVersion;
using misterplex::plexob::plexObCfgFileName;
using misterplex::plexob::plexObCfgPath;
using misterplex::plexob::plexObCfgPathForVersion;
using misterplex::plexob::readPlexObCfgCached;
using misterplex::plexob::readPlexObIniYc;
using misterplex::plexob::ycReloadStampOk;
using misterplex::plexob::writeYcReloadMarker;
using misterplex::plexob::ycNoteReloadFinished;

namespace {

int fails = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++fails;
    }
}

struct Rig {
    CoreGate g;
    CoreObservation o;
    int64_t now = 10000000;
    int64_t mt = 2000;
    Rig() {
        g.resetForTest(false);
        o.fabric.known = true;
        o.fabric.mode = kFpgaMgrModeUser;
        o.main.pid = 100;
        o.main.startTicks = 1;
        o.mainStartWallNs = 1000;
        o.bootWallNs = 0;
        o.coreName = {true, "PlexOB", ++mt};
        o.rbfName = {true, "PlexOB.rbf", ++mt};
    }
    bool toProbe(int n = 40) {
        for (int i = 0; i < n; ++i) {
            now += 700000;
            o.nowUs = now;
            if (g.observe(o) == CoreGateEvent::ProbeWanted)
                return true;
        }
        return false;
    }
};

void put64(int fd, uint64_t off, uint64_t v) { check(::pwrite(fd, &v, 8, static_cast<off_t>(off)) == 8, "pwrite"); }

void writeCfg(const std::string& path, uint32_t nibble) {
    const uint32_t status = (nibble & 0xFu) << 20;
    uint8_t b[4] = {static_cast<uint8_t>(status), static_cast<uint8_t>(status >> 8),
                    static_cast<uint8_t>(status >> 16), static_cast<uint8_t>(status >> 24)};
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char*>(b), 4);
}

void seedIni(const std::string& path, const char* body) {
    std::ofstream out(path, std::ios::trunc);
    out << body;
}

struct PollAcc {
    YcTickState st;
    int reloads = 0;
    int writes = 0;
    int removes = 0;
    YcPollResult last{};
};

void step(PollAcc& a, CoreGate& g, const OwenbCtrlReader& rd, const YcPollFiles& files, int64_t now,
          bool playing) {
    a.last = pollYcOnOwenbGate(g, rd, a.st, files, playing, now);
    if (a.last.wroteIni)
        ++a.writes;
    if (a.last.removedKeys)
        ++a.removes;
    if (a.last.wantReload)
        ++a.reloads;
}

std::string readFile(const std::string& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string bodyOf(const std::string& src, const char* head) {
    const size_t a = src.find(head);
    if (a == std::string::npos)
        return {};
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}

}  // namespace

int main() {
    check(kRingWordYcMode == 87u, "Y/C ring word is 87");
    check(kRingWordYcMode != 82u, "Y/C does not use the rumble word");
    check(misterplex::plexob::packYcValue(1, 2) == 0x9u, "svideo + PAL-M packs to 0x9");
    check(misterplex::plexob::analogFromRingLo(0x319u) == 1u &&
              misterplex::plexob::ntscFromRingLo(0x319u) == 2u,
          "bits above [3:0] are ignored");
    check(kPlexObConfVersion == 1, "CONF_STR version constant is 1");
    check(plexObCfgFileName(kPlexObConfVersion) == "PlexOB_v1.CFG", "v1 file name");
    check(plexObCfgFileName(2) == "PlexOB_v2.CFG", "a later CONF_STR version changes the file name");
    check(plexObCfgPath() == "/media/fat/config/PlexOB_v1.CFG", "daemon reads PlexOB_v1.CFG");
    check(plexObCfgPath() == plexObCfgPathForVersion(kPlexObConfVersion),
          "the daemon path is derived from the CONF_STR version");
    const uint64_t goodStamp = (static_cast<uint64_t>(kMpxBuildIdTag) << 32) | kPlexObYcBuildId;
    const uint64_t t10Stamp = (static_cast<uint64_t>(kMpxBuildIdTag) << 32) | 0x0B593F92u;
    const uint64_t yc11a = (static_cast<uint64_t>(0x4D505842u) << 32) | 0x0B59C11Au;
    const uint64_t dead119 = (static_cast<uint64_t>(0x4D505842u) << 32) | 0x0B59C119u;
    check(kMpxBuildIdTag == 0x4D505842u, "word 88 tag is MPXB");
    check(kPlexObYcBuildId == 0x0B59C11Au, "Y/C BUILD_ID is 0x0B59C11A");
    check(ycReloadStampOk(goodStamp), "word 88 MPXB/0x0B59C11A is accepted");
    check(ycReloadStampOk(yc11a), "word 88 4D505842/0B59C11A is accepted");
    check(!ycReloadStampOk(dead119), "word 88 4D505842/0B59C119 is refused");
    check(!ycReloadStampOk(t10Stamp), "T10 0x0B593F92 is refused");
    check(!ycReloadStampOk(kPlexObYcBuildId), "an untagged id is refused");
    check(!ycReloadStampOk(0), "an empty word 88 is refused");

    const std::string dir = "build/plexob-yc-test";
    check(std::system(("mkdir -p " + dir).c_str()) == 0, "mkdir");
    const std::string ini = dir + "/MiSTer.ini";
    const std::string marker = dir + "/ob_yc_reload";
    const std::string cfg = dir + "/PlexOB_v1.CFG";
    std::remove(marker.c_str());
    YcPollFiles files{ini, marker, cfg};

    char path[] = "/tmp/test_plexob_yc_mem.XXXXXX";
    const int fd = ::mkstemp(path);
    check(fd >= 0, "sparse ctrl file");
    if (fd < 0)
        return 1;
    check(::ftruncate(fd, static_cast<off_t>(kCtrlPhys + kCtrlBytes)) == 0, "truncate");
    put64(fd, kCtrlPhys + 0, kMagic);
    put64(fd, kCtrlPhys + 8, kVersion);
    put64(fd, kCtrlPhys + 16, static_cast<uint64_t>(kRingBytes) << 32);
    put64(fd, kCtrlPhys + 24, 40);
    put64(fd, kCtrlPhys + kMpxBuildIdWord * 8,
          (static_cast<uint64_t>(kMpxBuildIdTag) << 32) | kPlexObYcBuildId);
    put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 0x319);
    put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordLive) * 8, 0);

    std::atomic<bool> stop{false};
    std::thread beat([&] {
        uint64_t hb = 41;
        while (!stop.load()) {
            const ssize_t nw = ::pwrite(fd, &hb, 8, static_cast<off_t>(kCtrlPhys + 24));
            if (nw != 8)
                break;
            ++hb;
            ::usleep(2000);
        }
    });

    Rig rig;
    check(rig.toProbe(), "gate reaches the ring probe");
    check(owenbGateRingProbe(rig.g, path, 200, rig.now, false) == CoreGateEvent::Opened,
          "stamped 0x0B59C11A opens the OB gate");
    check(rig.g.owenbOpen() && !rig.g.open(), "OB open, legacy closed");
    check(rig.g.buildId() == kPlexObYcBuildId, "gate build id is the Y/C stamp");

    OwenbCtrlPageMap map;
    check(map.open(path) == 0 && map.mapped(), "ctrl page maps");
    const OwenbCtrlReader rd = map.reader();
    check(rd.read && rd.read(kRingWordYcMode) == 0x319ull, "reader sees word 87");

    // Legacy open must not take the Y/C path, even with a live word 87.
    {
        CoreGate legacy;
        legacy.resetForTest(true);
        check(legacy.open() && !legacy.owenbOpen(), "legacy gate");
        YcTickState st;
        writeCfg(cfg, 0x9);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        const YcPollResult r = pollYcOnOwenbGate(legacy, rd, st, files, false, 0);
        check(!r.readWord && !r.wantReload && !r.wroteIni, "legacy open does not read word 87");
    }

    // Unsaved S-Video (live 1, CFG still RGB) for many seconds: no write, no reload.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 1);
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordLive) * 8, 0);
        writeCfg(cfg, 0);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        std::remove(marker.c_str());
        PollAcc a;
        int64_t t = 1000000;
        for (int i = 0; i < 100; ++i) {
            t += 100000;
            step(a, rig.g, rd, files, t, false);
        }
        check(a.reloads == 0 && a.writes == 0 && a.removes == 0, "unsaved svideo does not reload");
        check(readPlexObIniYc(ini).vgaMode.empty(), "unsaved svideo did not write the ini");
    }

    // Saved value, but the OSD stays open: still nothing.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 1);
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordLive) * 8, 1ull << kYcOsdOpenBit);
        writeCfg(cfg, 1);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        PollAcc a;
        int64_t t = 1000000;
        for (int i = 0; i < 100; ++i) {
            t += 100000;
            step(a, rig.g, rd, files, t, false);
        }
        check(a.reloads == 0 && a.writes == 0, "OSD open does not reload");
    }

    // CFG matches, OSD closes, stable for 3s: one write and one reload. A second
    // poll while the reload is in flight does not issue another.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordLive) * 8, 0);
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 0x319);
        writeCfg(cfg, 0x9);
        seedIni(ini, "[Plex]\nvideo_mode=8\nvideo_mode_ntsc=8\nvideo_mode_pal=8\n");
        std::remove(marker.c_str());
        PollAcc a;
        int64_t t = 5000000;
        step(a, rig.g, rd, files, t, false);
        step(a, rig.g, rd, files, t + kYcStableUs - 1, false);
        check(a.reloads == 0 && a.writes == 0, "inside the stability window: no reload");
        step(a, rig.g, rd, files, t + kYcStableUs, false);
        check(a.reloads == 1 && a.writes == 1, "stable saved svideo reloads once");
        check(a.last.readWord, "the plan ran after the word-87 read");
        const auto written = readPlexObIniYc(ini);
        check(written.vgaMode == "svideo" && written.ntscMode == "2" && written.vgaScaler == "0",
              "word 87 0x319 writes svideo / PAL-M and pins the scaler");
        step(a, rig.g, rd, files, t + kYcStableUs + 100000, false);
        check(a.reloads == 1, "in-flight reload is not issued twice");
        check(writeYcReloadMarker(marker, kPlexObYcBuildId, 0x9), "marker after confirmed reload");
        ycNoteReloadFinished(a.st, true, t + kYcStableUs + 200000);
        step(a, rig.g, rd, files, t + kYcStableUs + 300000, false);
        check(a.reloads == 1 && a.writes == 1, "confirmed reload is not repeated");

        // Core comes back RGB while the saved CFG is still S-Video: not a second reload.
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 0);
        for (int i = 0; i < 50; ++i)
            step(a, rig.g, rd, files, t + kYcStableUs + 1000000 + i * 200000, false);
        check(a.reloads == 1 && a.writes == 1, "live revert against a saved svideo CFG does not double-reload");
        check(readPlexObIniYc(ini).vgaMode == "svideo", "the svideo ini survives the mismatched live word");
    }

    // Revert to the CFG nibble before the window expires: zero reloads, not two.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 1);
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordLive) * 8, 0);
        writeCfg(cfg, 1);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        std::remove(marker.c_str());
        PollAcc a;
        const int64_t t0 = 8000000;
        step(a, rig.g, rd, files, t0, false);
        step(a, rig.g, rd, files, t0 + 2000000, false);
        check(a.reloads == 0, "window still open at 2s");
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 0);
        writeCfg(cfg, 0);
        for (int i = 0; i < 40; ++i)
            step(a, rig.g, rd, files, t0 + 2000000 + (i + 1) * 200000, false);
        check(a.reloads == 0 && a.writes == 0 && a.removes == 0, "revert before the window: no reload");
        check(readPlexObIniYc(ini).foundSection == false, "revert did not write [PlexOB]");
    }

    // RGB removes the [PlexOB] keys and leaves [Plex] video_mode alone.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 0);
        writeCfg(cfg, 0);
        seedIni(ini,
                "[Plex]\n"
                "video_mode=8\n"
                "\n"
                "[PlexOB]\n"
                "vga_mode=rgb\n"
                "ntsc_mode=0\n"
                "vga_scaler=1\n");
        std::remove(marker.c_str());
        PollAcc a;
        const int64_t t0 = 20000000;
        step(a, rig.g, rd, files, t0, false);
        step(a, rig.g, rd, files, t0 + kYcStableUs, false);
        check(a.removes == 1 && a.writes == 0 && a.reloads == 1, "RGB removes keys and reloads once");
        const std::string text = readFile(ini);
        check(text.find("vga_mode=") == std::string::npos, "RGB does not write vga_mode=rgb");
        check(text.find("video_mode=8") != std::string::npos, "[Plex] video_mode stays");
        check(readPlexObIniYc(ini).hasVga == false && readPlexObIniYc(ini).hasNtsc == false &&
                  readPlexObIniYc(ini).hasScaler == false,
              "the three [PlexOB] keys are gone");
    }

    // A failed reload is retried, then capped. The marker stays absent until success.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 1);
        writeCfg(cfg, 1);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        std::remove(marker.c_str());
        PollAcc a;
        int64_t t = 40000000;
        step(a, rig.g, rd, files, t, false);
        step(a, rig.g, rd, files, t + kYcStableUs, false);
        check(a.reloads == 1, "first attempt");
        for (unsigned n = 1; n < kYcReloadAttemptCap; ++n) {
            ycNoteReloadFinished(a.st, false, t + kYcStableUs + static_cast<int64_t>(n) * kYcStableUs);
            step(a, rig.g, rd, files,
                 t + kYcStableUs + static_cast<int64_t>(n) * kYcStableUs + kYcStableUs, false);
        }
        check(a.reloads == static_cast<int>(kYcReloadAttemptCap), "retries until the cap");
        ycNoteReloadFinished(a.st, false, t + 100000000);
        const int before = a.reloads;
        step(a, rig.g, rd, files, t + 100000000 + kYcStableUs, false);
        check(a.reloads == before && a.last.gaveUp, "the cap refuses another reload");
        check(!std::ifstream(marker), "a failed reload does not leave a marker");
    }

    // During play the stable value does not reload. The next idle tick does.
    {
        put64(fd, kCtrlPhys + static_cast<uint64_t>(kRingWordYcMode) * 8, 2);
        writeCfg(cfg, 2);
        seedIni(ini, "[Plex]\nvideo_mode=8\n");
        std::remove(marker.c_str());
        PollAcc a;
        const int64_t t0 = 60000000;
        step(a, rig.g, rd, files, t0, true);
        step(a, rig.g, rd, files, t0 + kYcStableUs + 1000000, true);
        check(a.reloads == 0 && a.writes == 0, "a live play does not reload");
        step(a, rig.g, rd, files, t0 + kYcStableUs + 1100000, false);
        check(a.reloads == 1 && a.writes == 1, "the idle tick after the window reloads once");
    }

    auto prod = [](const std::string& p) { return p == "/media/fat/PlexOB.rbf"; };
    auto util = [](const std::string& p) { return p == "/media/fat/_Utility/PlexOB.rbf"; };
    auto both = [](const std::string& p) {
        return p == "/media/fat/PlexOB.rbf" || p == "/media/fat/_Utility/PlexOB.rbf";
    };
    auto none = [](const std::string&) { return false; };
    auto yes = [](const std::string&) { return true; };
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", prod) == "load_core /media/fat/PlexOB.rbf",
          "name-only PlexOB resolves to the prod RBF");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", both) == "load_core /media/fat/PlexOB.rbf",
          "prod wins over _Utility");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", util) ==
              "load_core /media/fat/_Utility/PlexOB.rbf",
          "utility is the fallback when prod is absent");
    check(misterplex::plexObReloadLoadCoreLine("PlexOB", none).empty(), "a missing RBF resolves empty");
    check(misterplex::plexObReloadLoadCoreLine("/media/fat/PlexOB.rbf", yes) ==
              "load_core /media/fat/PlexOB.rbf",
          "an existing full path is reloaded as itself");
    check(misterplex::plexObReloadLoadCoreLine("/media/fat/PlexOB.rbf", none).empty(),
          "a missing full path is refused");
    check(misterplex::plexObReloadResolvePath("PlexOB", none).empty(), "resolve refuses a missing file");

    const std::string src = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/media_player.cpp");
    check(src.find("noteYcFromRing") == std::string::npos, "the legacy OSD poll no longer reads Y/C");
    const std::string tick = bodyOf(src, "void MediaPlayer::coreGateTick() {");
    const std::string yc = bodyOf(src, "void MediaPlayer::ycOwenbTick() {");
    const std::string reload = bodyOf(src, "bool MediaPlayer::reloadCoreForYcMode(");
    check(tick.find("ycOwenbTick();") != std::string::npos, "coreGateTick runs the Y/C tick");
    check(yc.find("owenbOpen()") != std::string::npos && yc.find("pollYcOnOwenbGate") != std::string::npos,
          "the tick is gated on owenbOpen and uses the poll");
    check(yc.find("enqueueYcReload") != std::string::npos, "the tick does not reload on the caller thread");
    const size_t resolveAt = reload.find("plexObReloadResolvePath");
    const size_t menuAt = reload.find("menu.rbf");
    check(resolveAt != std::string::npos && menuAt != std::string::npos && resolveAt < menuAt,
          "the RBF is resolved before the menu bounce");
    const size_t playBeforeMenu = reload.find("playing_.load()", resolveAt);
    check(playBeforeMenu != std::string::npos && playBeforeMenu < menuAt,
          "playing_ is re-checked after resolve and before menu.rbf");
    const size_t coreWrite = reload.find("writeMisterCmdLineWithTimeout");
    const size_t playBeforeCore = reload.find("playing_.load()", menuAt);
    check(playBeforeCore != std::string::npos && coreWrite != std::string::npos && playBeforeCore < coreWrite,
          "playing_ is re-checked before the core load");
    check(reload.find("ycReloadStampOk") != std::string::npos, "reload refuses a BUILD_ID other than 0x0B59C11A");
    check(reload.find("word88") != std::string::npos, "reload logs the word 88 stamp with the path");
    check(yc.find("OwenbCtrlPageMap map") == std::string::npos && yc.find("ycMap_") != std::string::npos,
          "the tick keeps one control-page map");
    check(yc.find("plexObCfgPath()") != std::string::npos, "the tick reads the versioned CFG path");

    {
        const std::string cachePath = dir + "/cache.CFG";
        auto setMtime = [](const std::string& path, time_t sec) {
            struct timespec ts[2] = {};
            ts[0].tv_sec = sec;
            ts[1].tv_sec = sec;
            check(::utimensat(AT_FDCWD, path.c_str(), ts, 0) == 0, "utimensat");
        };
        writeCfg(cachePath, 1);
        setMtime(cachePath, 1000);
        YcTickState cache;
        const auto first = readPlexObCfgCached(cache, cachePath);
        check(first.present && first.nibble == 1u, "cfg cache reads the saved nibble");
        const auto held = readPlexObCfgCached(cache, cachePath);
        check(held.present && held.nibble == 1u, "cfg cache holds the nibble while stat is unchanged");
        writeCfg(cachePath, 2);
        {
            std::ofstream extra(cachePath, std::ios::binary | std::ios::app);
            extra.put('\xab');
        }
        setMtime(cachePath, 1000);
        const auto sized = readPlexObCfgCached(cache, cachePath);
        check(sized.present && sized.nibble == 2u,
              "cfg cache re-reads when size changes even if mtime does not");
        ::usleep(20000);
        writeCfg(cachePath, 1);
        setMtime(cachePath, 1000);
        const auto retimed = readPlexObCfgCached(cache, cachePath);
        check(retimed.present && retimed.nibble == 1u,
              "cfg cache re-reads when ctime changes even if mtime does not");
        std::remove(cachePath.c_str());
        YcTickState missing;
        const auto gone = readPlexObCfgCached(missing, cachePath);
        check(!gone.present, "a missing CFG stays not-present");
        const auto still = readPlexObCfgCached(missing, cachePath);
        check(!still.present, "a still-missing CFG stays cached as not-present");
    }

    stop.store(true);
    beat.join();
    map.close();
    ::close(fd);
    ::unlink(path);

    if (fails) {
        std::fprintf(stderr, "test_plexob_yc: %d FAIL\n", fails);
        return 1;
    }
    std::puts("test_plexob_yc: OK");
    return 0;
}
