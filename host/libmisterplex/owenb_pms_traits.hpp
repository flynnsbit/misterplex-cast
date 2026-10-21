#pragma once
// BUG1 (V2-BUGS-1017 #1, lab 7AE78D80): what each PMS's transcoder emitted for the PlexOB core, so the next FIRST
// request to that server is sized for the ARM's CAVLC->CABAC rewrite instead of failing after ~3 s.
//
// Keyed by the PMS machineIdentifier (playMedia's). After a transcoded OB play the daemon records the output's
// entropy: CAVLC at R kbps with rewrite load L % (the session's measured window), or CABAC. Newest wins; an
// entry older than kTtlSec (30 days) is forgotten; CABAC clears the entry (the full ladder again).
// firstRequestKbps() = min(ladder, R * 60 / L), floor 1500, for a stored CAVLC entry with L > 60; else the
// ladder. Persisted as lines "id cavlc kbps load epoch" (MPX_OB_PMS_TRAITS, default
// /media/fat/misterplex/ob_pms_traits). Never a Plex client profile: only maxVideoBitrate moves.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace misterplex {

class ObPmsTraits {
public:
    static constexpr int64_t kTtlSec = 30LL * 24 * 3600;
    static constexpr int kFloorKbps = 1500;
    static constexpr int kTargetLoadPct = 60;
    struct Entry {
        bool cavlc = false;
        int kbps = 0;
        int loadPct = 0;
        int64_t epoch = 0;
    };

    // One observation; true when the stored state changed enough to save (new / cleared, load moved >= 5).
    bool record(const std::string& id, bool cavlc, int kbps, int loadPct, int64_t nowSec) {
        if (id.empty())
            return false;
        auto it = m_.find(id);
        if (!cavlc) {
#ifndef MUT_OB_TRAITS_CABAC_KEEPS
            if (it == m_.end())
                return false;
            m_.erase(it);
            return true;
#else
            return false;
#endif
        }
        if (kbps <= 0 || loadPct <= 0)
            return false;
        const bool changed = it == m_.end() || std::abs(it->second.loadPct - loadPct) >= 5 ||
                             std::abs(it->second.kbps - kbps) * 10 >= std::max(1, it->second.kbps);
        m_[id] = Entry{true, kbps, loadPct, nowSec};   // newest wins
        return changed;
    }

    // The FIRST request's maxVideoBitrate for this server: the ladder, or less after a stored CAVLC entry.
    int firstRequestKbps(const std::string& id, int ladderKbps, int64_t nowSec) const {
        auto it = m_.find(id);
        if (id.empty() || it == m_.end() || !it->second.cavlc)
            return ladderKbps;
#ifndef MUT_OB_TRAITS_NO_TTL
        if (nowSec - it->second.epoch > kTtlSec)
            return ladderKbps;
#endif
        const Entry& e = it->second;
        if (e.loadPct <= kTargetLoadPct)
            return ladderKbps;   // it kept up: no reason to ask less
        int want = static_cast<int>(int64_t(e.kbps) * kTargetLoadPct / e.loadPct);
#ifndef MUT_OB_TRAITS_NO_FLOOR
        want = std::max(want, kFloorKbps);
#endif
        return ladderKbps > 0 ? std::min(ladderKbps, want) : want;
    }
    const Entry* find(const std::string& id) const {
        auto it = m_.find(id);
        return it == m_.end() ? nullptr : &it->second;
    }

    std::string serialize(int64_t nowSec) const {
        std::ostringstream o;
        for (const auto& [id, e] : m_)
            if (nowSec - e.epoch <= kTtlSec)
                o << id << ' ' << (e.cavlc ? 1 : 0) << ' ' << e.kbps << ' ' << e.loadPct << ' ' << e.epoch << '\n';
        return o.str();
    }
    void parse(const std::string& text) {
        m_.clear();
        std::istringstream in(text);
        std::string id;
        int c = 0, k = 0, l = 0;
        long long ep = 0;
        while (in >> id >> c >> k >> l >> ep)
            if (k > 0 && l > 0)
                m_[id] = Entry{c != 0, k, l, static_cast<int64_t>(ep)};
    }
    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f)
            return false;
        std::stringstream ss;
        ss << f.rdbuf();
        parse(ss.str());
        return true;
    }
    bool save(const std::string& path, int64_t nowSec) const {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f)
                return false;
            f << serialize(nowSec);
            if (!f)
                return false;
        }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }

private:
    std::map<std::string, Entry> m_;
};

}  // namespace misterplex
