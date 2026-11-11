#pragma once

#include <fstream>
#include <cstdint>
#include <cerrno>
#include <string>
#include "analog_raster.hpp"

namespace misterplex::service {
inline constexpr unsigned kWord = 87;
inline constexpr uint64_t kSelection = 1ull << 10;
inline constexpr uint64_t kCapability = 1ull << 11;
inline constexpr const char* kStatePath = "/tmp/misterplex-service-switch.state";
inline constexpr const char* kReadyPath = "/tmp/misterplex-service.ready";

inline std::string name(bool spelunkr) { return spelunkr ? "Spelunkr" : "Plex"; }
inline std::string notice(bool spelunkr) {
    return spelunkr ? "SWITCHING TO SPELUNKR" : "SWITCHING TO PLEX";
}
struct State {
    std::string phase, target;
    bool pending() const { return phase == "pending"; }
    std::string overlay() const {
        if (pending())
            return notice(target == "Spelunkr");
        if (phase == "error")
            return target == "Spelunkr" ? "SPELUNKR SWITCH FAILED" :
                target == "Plex" ? "PLEX SWITCH FAILED" : "SERVICE SWITCH STATE FAILED";
        return {};
    }
};
inline State readState(const std::string& path = kStatePath) {
    State s;
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return errno == ENOENT ? s : State{"error", ""};
    std::ifstream in(path);
    if (!(in >> s.phase >> s.target) ||
        (s.phase != "pending" && s.phase != "ready" && s.phase != "error") ||
        (s.target != "Plex" && s.target != "Spelunkr"))
        return {"error", ""};
    return s;
}
inline bool writeState(const State& s, const std::string& path = kStatePath) {
    return plexob::writeFileAtomic(path, s.phase + " " + s.target + "\n");
}
class FailureRecovery {
public:
    bool tick(bool failed, bool independentlyReady, int64_t nowUs) {
        if (!failed) {
            sinceUs_ = -1;
            return false;
        }
        if (sinceUs_ < 0)
            sinceUs_ = nowUs;
        return independentlyReady && nowUs - sinceUs_ >= 5000000;
    }
private:
    int64_t sinceUs_ = -1;
};
class PlexCfg {
public:
    bool observe(const std::string& path, bool spelunkr, bool selected, bool busy) {
        if (spelunkr)
            return restore(path);
        if (busy || selected)
            return true;
        bool big = false;
        std::string bytes;
        if (!plexob::readSmallFile(path, bytes, big) || bytes.size() < 4)
            return false;
        cached_ = std::move(bytes);
        return true;
    }
    bool save(const std::string& path) const {
        return cached_.size() >= 4 && plexob::writeFileAtomic(path + ".service-save", cached_);
    }
    static bool restore(const std::string& path) {
        bool big = false;
        std::string saved, current;
        if (!plexob::readSmallFile(path + ".service-save", saved, big) || saved.size() < 4)
            return false;
        if (!plexob::readSmallFile(path, current, big))
            return false;
        return current == saved || plexob::writeFileAtomic(path, saved);
    }
    static bool settings(const std::string& path, unsigned values[7]) {
        bool big = false;
        std::string bytes;
        const std::string saved = path + ".service-save";
        struct stat st {};
        if (::stat(saved.c_str(), &st) != 0) {
            if (errno != ENOENT || !plexob::readSmallFile(path, bytes, big))
                return false;
        } else if (!plexob::readSmallFile(saved, bytes, big)) {
            return false;
        }
        if (bytes.size() < 4)
            return false;
        uint32_t word = 0;
        for (unsigned i = 0; i < 4; ++i)
            word |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
        const unsigned shifts[] = {1, 3, 5, 7, 8, 11, 15};
        for (unsigned i = 0; i < 7; ++i)
            values[i] = (word >> shifts[i]) & (i == 3 ? 1u : 3u);
        return true;
    }
private:
    std::string cached_;
};
// A saved menu bit is not a request. First synchronize it with this process's
// actual mode, and wait for the ring echo before accepting deliberate changes.
class Selection {
public:
    enum class Action { None, Sync, Switch };
    Action tick(uint64_t word, bool active, bool busy, uint64_t generation) {
        if (!(word & kCapability))
            return Action::None;
        if (generation_ != generation) {
            generation_ = generation;
            synced_ = sent_ = false;
        }
        const bool selected = (word & kSelection) != 0;
        if (!synced_) {
            if (!sent_)
                return Action::Sync;
            if (selected == active)
                synced_ = true;
            return Action::None;
        }
        return !busy && selected != active ? Action::Switch : Action::None;
    }
    void syncSent(bool ok) { sent_ = ok; }
    void resync() { synced_ = sent_ = false; }
    bool synced() const { return synced_; }
private:
    uint64_t generation_ = UINT64_MAX;
    bool synced_ = false, sent_ = false;
};
} // namespace misterplex::service
