// Self-update engine: stage, verify, atomic swap with .prev kept, journal, boot recovery, rollback.
// All I/O goes through the Fs interface so the host tests can inject a crash at every operation.
// Invariant: the target file (e.g. misterplexd) is always a whole old or whole new file, never absent or partial,
// because a swap is "copy old to .prev" followed by ONE atomic rename of .new over the target.
// Never touches RBF or boot hook files unless the signed manifest lists them, conf allows it, and sha256 matches.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "update_manifest.hpp"

namespace mpx { namespace upd {

struct Fs {
    virtual ~Fs() {}
    virtual bool exists(const std::string& p) = 0;
    virtual bool read(const std::string& p, std::string& out) = 0;
    virtual bool writeSync(const std::string& p, const std::string& data) = 0;  // create/truncate + fsync; false on ENOSPC/IO
    virtual bool rename(const std::string& from, const std::string& to) = 0;    // atomic replace
    virtual bool remove(const std::string& p) = 0;
    virtual bool freeBytes(const std::string& p, uint64_t& out) = 0;
};

struct Paths {
    std::string daemon, rbf, boot;  // absolute target paths; rbf/boot empty = not configured
    std::string journal, marker;    // journal in the install dir; marker on tmpfs
    const std::string& target(const std::string& role) const { return role == "rbf" ? rbf : role == "boot" ? boot : daemon; }
};

enum class Result { Swapped, Disabled, Busy, Playing, ManifestFetch, ManifestBad, SigBad, NotNewer, Downgrade,
                    Incomparable, AuxNotAllowed, AuxNotConfigured, Oversize, DiskFull, Truncated, HashMismatch,
                    FetchFailed, IoError };
enum class FetchStatus { Ok, Failed, TooBig };
using Fetch = std::function<FetchStatus(const std::string& url, uint64_t maxBytes, std::string& body)>;

inline std::string newPath(const std::string& p) { return p + ".new"; }
inline std::string prevPath(const std::string& p) { return p + ".prev"; }
inline std::string tmpPath(const std::string& p) { return p + ".tmp"; }

struct JEntry { std::string role, path, newSha, oldSha; };
struct Journal { std::string state, version; int attempts = 0; std::vector<JEntry> items; bool ok = false; };

inline std::string journalText(const Journal& j) {
    std::string s = "state=" + j.state + "\nversion=" + j.version + "\nattempts=" + std::to_string(j.attempts) +
                    "\nn=" + std::to_string(j.items.size()) + "\n";
    for (size_t i = 0; i < j.items.size(); i++)
        s += "a" + std::to_string(i) + "=" + j.items[i].role + "|" + j.items[i].path + "|" + j.items[i].newSha + "|" +
             j.items[i].oldSha + "\n";
    return s + "end=1\n";
}

inline Journal parseJournal(const std::string& t) {
    Journal j; size_t pos = 0;
    std::vector<std::string> lines;
    while (pos < t.size()) {
        size_t nl = t.find('\n', pos); if (nl == std::string::npos) return j;
        lines.push_back(t.substr(pos, nl - pos)); pos = nl + 1;
    }
    if (lines.size() < 5) return j;
    auto val = [&](size_t i, const std::string& k, std::string& out) {
        std::string p = k + "="; if (lines[i].compare(0, p.size(), p) != 0) return false;
        out = lines[i].substr(p.size()); return true; };
    std::string a, nn;
    if (!val(0, "state", j.state) || !val(1, "version", j.version) || !val(2, "attempts", a) || !val(3, "n", nn)) return j;
    if (j.state != "STAGED" && j.state != "PENDING") return j;
    if (a.empty() || a.size() > 3 || nn.size() != 1 || nn[0] < '1' || nn[0] > '3') return j;
    for (char c : a) if (c < '0' || c > '9') return j;
    j.attempts = std::stoi(a);
    int n = nn[0] - '0';
    if (lines.size() != size_t(n) + 5 || lines.back() != "end=1") return j;
    for (int i = 0; i < n; i++) {
        std::string v; if (!val(4 + i, "a" + std::to_string(i), v)) return j;
        size_t p1 = v.find('|'), p2 = p1 == std::string::npos ? p1 : v.find('|', p1 + 1),
               p3 = p2 == std::string::npos ? p2 : v.find('|', p2 + 1);
        if (p3 == std::string::npos) return j;
        JEntry e; e.role = v.substr(0, p1); e.path = v.substr(p1 + 1, p2 - p1 - 1);
        e.newSha = v.substr(p2 + 1, p3 - p2 - 1); e.oldSha = v.substr(p3 + 1);
        if (e.path.empty() || !isHex(e.newSha, 64) || !isHex(e.oldSha, 64)) return j;
        j.items.push_back(e);
    }
    j.ok = true;
    return j;
}

// Atomic journal write: temp + fsync + rename.
inline bool writeJournal(Fs& fs, const Paths& P, const Journal& j) {
    return fs.writeSync(tmpPath(P.journal), journalText(j)) && fs.rename(tmpPath(P.journal), P.journal);
}

inline void cleanStray(Fs& fs, const Paths& P) {
    const std::string* t[3] = {&P.daemon, &P.rbf, &P.boot};
    for (const std::string* p : t) if (!p->empty()) { fs.remove(newPath(*p)); fs.remove(tmpPath(*p)); }
    fs.remove(tmpPath(P.journal));
}

// Restore every artifact the journal says may have been swapped. Idempotent: safe to re-run after a crash.
// Returns false (journal kept) if a needed .prev is missing or does not match the recorded old hash.
// `restored` is set when at least one target file was put back.
inline bool rollback(Fs& fs, const Paths& P, const Journal& j, bool& restored) {
    restored = false;
    for (size_t k = j.items.size(); k-- > 0;) {
        const JEntry& e = j.items[k];
        std::string cur;
        bool have = fs.exists(e.path) && fs.read(e.path, cur);
        if (have && sha256Hex(cur) == e.oldSha) continue;  // already the old file
        std::string prev;
        if (!fs.exists(prevPath(e.path)) || !fs.read(prevPath(e.path), prev) || sha256Hex(prev) != e.oldSha) return false;
        if (!fs.writeSync(tmpPath(e.path), prev) || !fs.rename(tmpPath(e.path), e.path)) return false;
        restored = true;
    }
    for (const JEntry& e : j.items) { fs.remove(prevPath(e.path)); }
    cleanStray(fs, P);
    return true;
}

enum class Boot { Normal, Committed, RunPending, RolledBack, RollbackFailed };
constexpr int kMaxUnhealthyBoots = 2;

inline bool markerMatches(Fs& fs, const Paths& P, const std::string& version) {
    std::string m;
#ifdef MUT_MARKER_ANY
    return fs.exists(P.marker);
#endif
    return fs.exists(P.marker) && fs.read(P.marker, m) && m == version + "\n";
}

// Run first thing at daemon start (and by any supervisor). Decides commit / keep trying / roll back.
inline Boot recoverOnBoot(Fs& fs, const Paths& P) {
    if (!fs.exists(P.journal)) { cleanStray(fs, P); return Boot::Normal; }
    std::string text; Journal j;
    if (fs.read(P.journal, text)) j = parseJournal(text);
    if (!j.ok) {  // corrupt journal: never leave the daemon missing
        cleanStray(fs, P);
        bool restored = false;
        if (!fs.exists(P.daemon) && fs.exists(prevPath(P.daemon))) {
            restored = fs.rename(prevPath(P.daemon), P.daemon);
        }
        fs.remove(P.journal);
        return restored ? Boot::RolledBack : Boot::Normal;
    }
    bool restored = false;
    if (j.state == "PENDING") {
        if (markerMatches(fs, P, j.version)) { fs.remove(P.journal); return Boot::Committed; }
#ifndef MUT_NO_ROLLBACK_ON_UNHEALTHY
        if (j.attempts >= kMaxUnhealthyBoots) {
#else
        if (false) {
#endif
            if (!rollback(fs, P, j, restored)) return Boot::RollbackFailed;
            fs.remove(P.journal);
            return Boot::RolledBack;
        }
        j.attempts++;
        if (!writeJournal(fs, P, j)) return Boot::RunPending;  // cannot count; still safe: files are whole
        return Boot::RunPending;
    }
    if (!rollback(fs, P, j, restored)) return Boot::RollbackFailed;  // STAGED: crashed mid-swap
    fs.remove(P.journal);
    return restored ? Boot::RolledBack : Boot::Normal;
}

// ---- health marker (daemon side) and supervisor decision (init-wrapper side) ----
inline bool healthDue(unsigned uptimeSec, bool companionAnswering, unsigned delaySec) {
    return uptimeSec >= delaySec && companionAnswering;
}
// The running new daemon calls this once healthy: marker first, then the journal goes away (= committed).
inline bool commitHealthy(Fs& fs, const Paths& P, const std::string& version) {
    if (!fs.writeSync(P.marker, version + "\n")) return false;
    std::string text;
    if (fs.exists(P.journal) && fs.read(P.journal, text)) {
        Journal j = parseJournal(text);
        if (j.ok && j.state == "PENDING" && j.version == version) fs.remove(P.journal);
    }
    return true;
}
enum class Sup { Wait, Commit, Rollback };
inline Sup supervisorDecide(const Journal& j, bool markerOk, unsigned elapsedSec, unsigned timeoutSec = 60) {
    if (!j.ok || j.state != "PENDING") return Sup::Wait;
    if (markerOk) return Sup::Commit;
    return elapsedSec >= timeoutSec ? Sup::Rollback : Sup::Wait;
}

struct Ctx {
    UpdateConf conf; std::string current; bool forceDowngrade = false;  // force only from a console, never the button
    std::function<bool()> playing; Fetch fetch; SigVerify verify;
};

// ---- the update itself: gates, fetch, verify, stage, journal, swap ----
inline Result runUpdate(Fs& fs, const Paths& P, const Ctx& c) {
#ifndef MUT_UPDATE_IGNORES_ENABLE
    if (!c.conf.enabled) return Result::Disabled;
#endif
#ifndef MUT_UPDATE_WHILE_PLAYING
    if (c.playing && c.playing()) return Result::Playing;
#endif
    if (fs.exists(P.journal)) return Result::Busy;
    std::string mtext;
    if (!c.fetch || c.fetch(c.conf.url, 4096, mtext) != FetchStatus::Ok) return Result::ManifestFetch;
    Manifest m;
    MStatus ms = parseManifest(mtext, c.verify, m);
    if (ms == MStatus::BadSig || ms == MStatus::NoSig || ms == MStatus::NoVerifier) return Result::SigBad;
    if (ms != MStatus::Ok) return Result::ManifestBad;
    Cmp cmp = compareVersion(c.current, m.version);
    if (cmp == Cmp::Incomparable) return Result::Incomparable;
    if (cmp == Cmp::Same) return Result::NotNewer;
#ifndef MUT_ALLOW_DOWNGRADE
    if (cmp == Cmp::Older && !c.forceDowngrade) return Result::Downgrade;
#endif
    for (const Artifact& a : m.artifacts) {
        if (a.role == "daemon") continue;
#ifndef MUT_AUX_UNGATED
        if (!c.conf.allowAux) return Result::AuxNotAllowed;
#endif
        if (P.target(a.role).empty()) return Result::AuxNotConfigured;
    }
    Journal j; j.state = "STAGED"; j.version = m.version;
    std::vector<std::string> bodies;
    for (const Artifact& a : m.artifacts) {
        const std::string& path = P.target(a.role);
#ifndef MUT_NO_SIZE_CAP
        if (a.size > c.conf.maxBytes) return Result::Oversize;
#endif
        uint64_t freeB = 0;
        if (!fs.freeBytes(path, freeB) || freeB < a.size * 2 + 4096) return Result::DiskFull;
        std::string cur, body;
        if (!fs.exists(path) || !fs.read(path, cur)) return Result::IoError;  // never "install" over a missing target
        FetchStatus fst = c.fetch(artifactUrl(c.conf.url, a.file), c.conf.maxBytes, body);
        if (fst == FetchStatus::TooBig) return Result::Oversize;
        if (fst != FetchStatus::Ok) return Result::FetchFailed;
        if (body.size() > a.size) return Result::Oversize;
#ifndef MUT_ACCEPT_TRUNCATED
        if (body.size() < a.size) return Result::Truncated;
#endif
#ifndef MUT_SKIP_HASH
        if (sha256Hex(body) != a.sha256) return Result::HashMismatch;
#endif
        j.items.push_back({a.role, path, a.sha256, sha256Hex(cur)});
        bodies.push_back(body);
    }
    // stage every file, read it back and hash it again (catches short or corrupt writes)
    for (size_t i = 0; i < j.items.size(); i++) {
        const std::string np = newPath(j.items[i].path);
        std::string back;
        if (!fs.writeSync(np, bodies[i])) { cleanStray(fs, P); return Result::DiskFull; }
        if (!fs.read(np, back) || sha256Hex(back) != j.items[i].newSha) { cleanStray(fs, P); return Result::HashMismatch; }
    }
    fs.remove(P.marker);
    if (!writeJournal(fs, P, j)) { cleanStray(fs, P); fs.remove(P.journal); return Result::IoError; }
    for (size_t i = 0; i < j.items.size(); i++) {
        const std::string& p = j.items[i].path;
#ifndef MUT_NO_PREV
        std::string old;
        if (!fs.read(p, old) || !fs.writeSync(tmpPath(p), old) || !fs.rename(tmpPath(p), prevPath(p))) { recoverOnBoot(fs, P); return Result::IoError; }
#endif
#ifdef MUT_SWAP_NONATOMIC
        fs.remove(p);
#endif
        if (!fs.rename(newPath(p), p)) { recoverOnBoot(fs, P); return Result::IoError; }
    }
    j.state = "PENDING"; j.attempts = 0;
    if (!writeJournal(fs, P, j)) return Result::IoError;  // journal stays STAGED: boot recovery rolls back
    return Result::Swapped;
}

}}  // namespace mpx::upd
