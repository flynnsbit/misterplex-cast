#pragma once

#include "plexob_yc.hpp"

#include "core_gate.hpp"

#include <cctype>
#include <fcntl.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace misterplex::plexob {

inline std::string trimIniLine(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline bool sectionNameMatches(const std::string& bracketed, const char* want) {
    if (bracketed.size() < 2 || bracketed.front() != '[' || bracketed.back() != ']')
        return false;
    const std::string name = trimIniLine(bracketed.substr(1, bracketed.size() - 2));
    if (!want || name.size() != std::strlen(want))
        return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) !=
            std::tolower(static_cast<unsigned char>(want[i])))
            return false;
    }
    return true;
}

inline std::string iniValueOnly(std::string val) {
    const auto semi = val.find(';');
    if (semi != std::string::npos)
        val = trimIniLine(val.substr(0, semi));
    const auto hash = val.find('#');
    if (hash != std::string::npos)
        val = trimIniLine(val.substr(0, hash));
    return val;
}

inline PlexObIniYc readPlexObIniYc(const std::string& path) {
    PlexObIniYc out;
    std::ifstream in(path);
    if (!in)
        return out;
    bool inSection = false;
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trimIniLine(line);
        if (t.empty() || t.front() == '#' || t.front() == ';')
            continue;
        if (t.front() == '[' && t.back() == ']') {
            inSection = sectionNameMatches(t, "PlexOB");
            out.foundSection = out.foundSection || inSection;
            continue;
        }
        if (!inSection || t.find('=') == std::string::npos)
            continue;
        const auto eq = t.find('=');
        const std::string key = trimIniLine(t.substr(0, eq));
        const std::string val = iniValueOnly(trimIniLine(t.substr(eq + 1)));
        if (key == "vga_mode") {
            out.vgaMode = val;
            out.hasVga = true;
        } else if (key == "ntsc_mode") {
            out.ntscMode = val;
            out.hasNtsc = true;
        } else if (key == "vga_scaler") {
            out.vgaScaler = val;
            out.hasScaler = true;
        }
    }
    return out;
}

inline bool lineStartsKey(const std::string& line, const char* key) {
    if (!key || !*key)
        return false;
    const std::string t = trimIniLine(line);
    return t.rfind(std::string(key) + "=", 0) == 0;
}

inline bool writeIniTextAtomic(const std::string& path, const std::string& rendered) {
    const std::string tmp = path + ".yc.tmp";
    std::remove(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < rendered.size()) {
        const ssize_t n = ::write(fd, rendered.data() + off, rendered.size() - off);
        if (n <= 0) {
            ::close(fd);
            std::remove(tmp.c_str());
            return false;
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        std::remove(tmp.c_str());
        return false;
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        const int dirfd = ::open(path.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd >= 0) {
            (void)::fsync(dirfd);
            ::close(dirfd);
        }
    }
    return true;
}

inline std::string renderIniLines(const std::vector<std::string>& lines) {
    std::string rendered;
    for (const auto& l : lines) {
        rendered.append(l);
        rendered.push_back('\n');
    }
    return rendered;
}

// Drop vga_mode, ntsc_mode and vga_scaler from [PlexOB] so RGB inherits the
// global [MiSTer] vga_mode. An empty [PlexOB] section is removed. A missing
// file is already inherited.
inline bool removePlexObYcKeys(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        return true;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);

    std::vector<std::string> out;
    bool inTarget = false;
    std::vector<std::string> section;
    bool sectionHeader = false;
    std::string header;
    auto flush = [&] {
        if (!sectionHeader)
            return;
        bool any = false;
        for (const auto& l : section) {
            if (!trimIniLine(l).empty())
                any = true;
        }
        if (any) {
            out.push_back(header);
            out.insert(out.end(), section.begin(), section.end());
        }
        section.clear();
        sectionHeader = false;
    };
    for (const auto& raw : lines) {
        const std::string t = trimIniLine(raw);
        const bool isSection = !t.empty() && t.front() == '[' && t.back() == ']';
        if (isSection) {
            flush();
            inTarget = sectionNameMatches(t, "PlexOB");
            if (inTarget) {
                sectionHeader = true;
                header = raw;
            } else {
                out.push_back(raw);
            }
            continue;
        }
        if (inTarget) {
            if (lineStartsKey(raw, "vga_mode") || lineStartsKey(raw, "ntsc_mode") ||
                lineStartsKey(raw, "vga_scaler"))
                continue;
            section.push_back(raw);
            continue;
        }
        out.push_back(raw);
    }
    flush();
    return writeIniTextAtomic(path, renderIniLines(out));
}

// tmp + fsync + rename + directory fsync. Pins vga_scaler=0 for svideo/cvbs.
// RGB removes the [PlexOB] keys instead of writing vga_mode=rgb.
inline bool writePlexObIni(const std::string& path, const std::string& vgaMode,
                           const std::string& ntscMode) {
    if (vgaMode == "rgb")
        return removePlexObYcKeys(path);
    if (!vgaModeValid(vgaMode) || !ntscModeValid(ntscMode))
        return false;
    const bool pinScaler = ycModeWantsScalerOff(vgaMode);
    std::ifstream in(path);
    std::vector<std::string> lines;
    std::string line;
    if (in) {
        while (std::getline(in, line))
            lines.push_back(line);
    }

    std::vector<std::string> out;
    bool inTarget = false;
    bool foundTarget = false;
    bool sawVga = false;
    bool sawNtsc = false;
    bool sawScaler = false;
    auto emitMissing = [&] {
        if (!inTarget)
            return;
        if (!sawVga)
            out.push_back("vga_mode=" + vgaMode);
        if (!sawNtsc)
            out.push_back("ntsc_mode=" + ntscMode);
        if (pinScaler && !sawScaler)
            out.push_back("vga_scaler=0");
    };

    for (const auto& raw : lines) {
        const std::string t = trimIniLine(raw);
        const bool isSection = !t.empty() && t.front() == '[' && t.back() == ']';
        if (isSection) {
            emitMissing();
            inTarget = sectionNameMatches(t, "PlexOB");
            foundTarget = foundTarget || inTarget;
            sawVga = false;
            sawNtsc = false;
            sawScaler = false;
            out.push_back(raw);
            continue;
        }
        if (inTarget && lineStartsKey(raw, "vga_mode")) {
            out.push_back("vga_mode=" + vgaMode);
            sawVga = true;
            continue;
        }
        if (inTarget && lineStartsKey(raw, "ntsc_mode")) {
            out.push_back("ntsc_mode=" + ntscMode);
            sawNtsc = true;
            continue;
        }
        if (inTarget && pinScaler && lineStartsKey(raw, "vga_scaler")) {
            out.push_back("vga_scaler=0");
            sawScaler = true;
            continue;
        }
        out.push_back(raw);
    }
    emitMissing();
    if (!foundTarget) {
        if (!out.empty() && !trimIniLine(out.back()).empty())
            out.push_back("");
        out.push_back("[PlexOB]");
        out.push_back("vga_mode=" + vgaMode);
        out.push_back("ntsc_mode=" + ntscMode);
        if (pinScaler)
            out.push_back("vga_scaler=0");
    }

    return writeIniTextAtomic(path, renderIniLines(out));
}

// OB idle/session poll. Requires the real gate (owenbOpen, legacy open false)
// and reads word 87 through the OwenbCtrlReader the gate already uses. A failed
// read, a non-Y/C build, or a live play does not write. Reconcile runs once
// when a saved CFG nibble has matched word 87 with the OSD closed for
// kYcStableUs, and again only when that nibble changes. The reload itself is
// the caller's job (its own worker). The marker is not written here.
inline YcPollResult pollYcOnOwenbGate(const CoreGate& gate, const OwenbCtrlReader& reader,
                                      YcTickState& st, const YcPollFiles& files, bool playing,
                                      int64_t nowUs) {
    YcPollResult out;
    if (!gate.owenbOpen() || gate.open()) {
        st.window = false;
        st.decided = false;
        st.needReload = false;
        return out;
    }
    if (!reader.read)
        return out;
    const uint64_t w87 = reader.read(kRingWordYcMode);
    const uint64_t liveW = reader.read(kRingWordLive);
    out.readWord = true;
    const uint32_t build = gate.buildId();
    out.buildId = build;
    if (!isPlexObYcBuild(build))
        return out;
    const uint32_t live = static_cast<uint32_t>(w87) & kYcRingLoMask;
    const bool settled = st.decided && st.decidedValue == live && !st.inFlight && !st.needReload;
    if (settled)
        return out;
    if (st.inFlight)
        return out;

    const bool osdOpen = ycOsdOpenFromLiveWord(liveW);
    const YcCfgNibble cfg = readPlexObCfgCached(st, files.cfgPath);
    const bool armed = cfg.present && (cfg.nibble & kYcRingLoMask) == live && !osdOpen;
    if (!armed) {
        st.window = false;
        return out;
    }
    if (!st.window || st.windowValue != live) {
        st.window = true;
        st.windowValue = live;
        st.windowSinceUs = nowUs;
        return out;
    }
    if (nowUs - st.windowSinceUs < kYcStableUs)
        return out;
    if (playing)
        return out;

    if (!st.decided || st.decidedValue != live) {
        const PlexObIniYc ini = readPlexObIniYc(files.iniPath);
        const YcReloadMarker marker = readYcReloadMarker(files.markerPath);
        const unsigned analog = analogFromRingLo(live);
        const unsigned ntsc = ntscFromRingLo(live);
        const bool iniOk =
            analog == 0 ? iniInheritsAnalog(ini) : iniMatchesOsdYc(ini, analog, ntsc);
        if (!iniOk) {
            if (!writePlexObIni(files.iniPath, ycVgaModeFromAnalogOutput(analog),
                                ycNtscModeFromColor(ntsc)))
                return out;
            out.wroteIni = analog != 0;
            out.removedKeys = analog == 0;
        }
        st.decided = true;
        st.decidedValue = live;
        st.attempts = 0;
        st.capLogged = false;
        // A matching ini means Main already programmed yc_out at core start.
        // The marker only suppresses a reload after we ourselves rewrote the ini.
        st.needReload = !iniOk && !markerCovers(marker, build, live);
        st.nextAttemptUs = nowUs;
        if (!st.needReload)
            return out;
    }
    if (!st.needReload)
        return out;
    if (st.attempts >= kYcReloadAttemptCap) {
        if (!st.capLogged)
            out.gaveUp = true;
        st.capLogged = true;
        st.needReload = false;
        return out;
    }
    if (nowUs < st.nextAttemptUs)
        return out;
    ++st.attempts;
    st.inFlight = true;
    st.nextAttemptUs = nowUs + kYcStableUs;
    out.wantReload = true;
    out.analog = analogFromRingLo(live);
    out.ntsc = ntscFromRingLo(live);
    out.value = live;
    out.buildId = build;
    return out;
}

}  // namespace misterplex::plexob
