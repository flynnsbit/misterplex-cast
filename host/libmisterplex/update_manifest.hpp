// Self-update: version compare, release-manifest parse + verify, conf. Host-testable, no I/O.
// Manifest = text "key=value" lines, ends with a "sig=<hex>" line. The signed bytes are every
// byte before that line. Signature checking is an injected callback (fail closed if absent).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "update_sha256.hpp"

namespace mpx { namespace upd {

struct Version { char family = 0; std::vector<unsigned long> n; bool ok = false; };  // 'r' = rcN, 'd' = dotted

inline Version parseVersion(const std::string& in) {
    Version v; std::string s = in;
    if (!s.empty() && s[0] == 'v') s.erase(0, 1);
    if (s.compare(0, 2, "rc") == 0) { v.family = 'r'; s.erase(0, 2); } else v.family = 'd';
    if (s.empty() || s.size() > 32) return v;
    size_t start = 0;
    while (start <= s.size()) {
        size_t dot = s.find('.', start);
        std::string seg = s.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (seg.empty() || seg.size() > 6) return v;
        unsigned long x = 0;
        for (char c : seg) { if (c < '0' || c > '9') return v; x = x * 10 + unsigned(c - '0'); }
        v.n.push_back(x);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (v.family == 'r' && v.n.size() != 1) return v;
    v.ok = true;
    return v;
}

enum class Cmp { Older, Same, Newer, Incomparable };
// How does `candidate` relate to `current`?
inline Cmp compareVersion(const std::string& current, const std::string& candidate) {
    Version a = parseVersion(current), b = parseVersion(candidate);
    if (!a.ok || !b.ok || a.family != b.family) return Cmp::Incomparable;
    size_t m = a.n.size() > b.n.size() ? a.n.size() : b.n.size();
    for (size_t i = 0; i < m; i++) {
        unsigned long x = i < a.n.size() ? a.n[i] : 0, y = i < b.n.size() ? b.n[i] : 0;
#ifdef MUT_VERSION_LEXICAL
        x = (a.n.size() > i) ? a.n[i] % 10 : 0; y = (b.n.size() > i) ? b.n[i] % 10 : 0;
#endif
        if (y > x) return Cmp::Newer;
        if (y < x) return Cmp::Older;
    }
    return Cmp::Same;
}

struct Artifact { std::string role, file, sha256; uint64_t size = 0; };  // role: daemon|rbf|boot

struct Manifest {
    std::string version, commit, signedText, sigHex;
    std::vector<Artifact> artifacts;  // [0] is always the daemon
    const Artifact* find(const std::string& role) const {
        for (const auto& a : artifacts) if (a.role == role) return &a;
        return nullptr;
    }
};

enum class MStatus { Ok, Malformed, NoSig, BadSig, NoVerifier, Unsafe };

inline bool isHex(const std::string& s, size_t len) {
    if (s.size() != len) return false;
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool safeFileName(const std::string& f) {
    if (f.empty() || f.size() > 64 || f[0] == '.') return false;
    for (char c : f)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    return true;
}

using SigVerify = std::function<bool(const std::string& message, const std::string& sigHex)>;

// Parse and verify. A signature is ALWAYS required (the sha256 of each artifact is inside the signed text).
inline MStatus parseManifest(const std::string& text, const SigVerify& verify, Manifest& out) {
    out = Manifest();
    if (text.empty() || text.size() > 4096) return MStatus::Malformed;
    std::map<std::string, std::string> kv;
    size_t pos = 0, sigAt = std::string::npos;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) return MStatus::Malformed;  // every line ends in \n
        if (sigAt != std::string::npos) return MStatus::Malformed;  // sig= must be the last line
        std::string line = text.substr(pos, nl - pos);
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return MStatus::Malformed;
        std::string k = line.substr(0, eq);
        if (kv.count(k)) return MStatus::Malformed;  // duplicate key
        kv[k] = line.substr(eq + 1);
        if (k == "sig") sigAt = pos;
        pos = nl + 1;
    }
    static const char* known[] = {"format", "version", "commit", "file", "size", "sha256", "rbf_file", "rbf_size",
                                  "rbf_sha256", "boot_file", "boot_size", "boot_sha256", "sig"};
    for (const auto& e : kv) {
        bool ok = false; for (const char* k : known) if (e.first == k) ok = true;
        if (!ok) return MStatus::Malformed;
    }
    if (kv["format"] != "1") return MStatus::Malformed;
    out.version = kv["version"]; out.commit = kv["commit"];
    if (!parseVersion(out.version).ok) return MStatus::Malformed;
    if (out.commit.size() < 7 || !isHex(out.commit, out.commit.size()) || out.commit.size() > 40) return MStatus::Malformed;
    static const char* pre[3] = {"", "rbf_", "boot_"};
    static const char* role[3] = {"daemon", "rbf", "boot"};
    for (int r = 0; r < 3; r++) {
        std::string p = pre[r];
        int have = int(kv.count(p + "file")) + int(kv.count(p + "size")) + int(kv.count(p + "sha256"));
        if (have == 0 && r > 0) continue;
        if (have != 3) return MStatus::Malformed;  // daemon entry missing, or a partial aux entry
        Artifact a; a.role = role[r]; a.file = kv[p + "file"]; a.sha256 = kv[p + "sha256"];
        const std::string& sz = kv[p + "size"];
        if (!safeFileName(a.file)) return MStatus::Unsafe;
        if (!isHex(a.sha256, 64) || sz.empty() || sz.size() > 10) return MStatus::Malformed;
        for (char c : sz) { if (c < '0' || c > '9') return MStatus::Malformed; a.size = a.size * 10 + uint64_t(c - '0'); }
        if (a.size == 0) return MStatus::Malformed;
        out.artifacts.push_back(a);
    }
    if (sigAt == std::string::npos) return MStatus::NoSig;
    out.signedText = text.substr(0, sigAt); out.sigHex = kv["sig"];
    if (!isHex(out.sigHex, 128)) return MStatus::BadSig;
    if (!verify) return MStatus::NoVerifier;
#ifndef MUT_MANIFEST_NO_VERIFY
    if (!verify(out.signedText, out.sigHex)) return MStatus::BadSig;
#endif
    return MStatus::Ok;
}

// Conf: default OFF. Only the exact value "1" enables; UPDATE_URL must be https and shell-safe.
struct UpdateConf { bool enabled = false; std::string url; bool allowAux = false; uint64_t maxBytes = 24u << 20; };

inline UpdateConf parseUpdateConf(const std::map<std::string, std::string>& conf) {
    UpdateConf c;
    auto get = [&](const char* k) { auto it = conf.find(k); return it == conf.end() ? std::string() : it->second; };
#ifdef MUT_CONF_DEFAULT_ON
    c.enabled = get("UPDATE_ENABLE") != "0";
#else
    c.enabled = get("UPDATE_ENABLE") == "1";
#endif
    c.url = get("UPDATE_URL"); c.allowAux = get("UPDATE_ALLOW_AUX") == "1";
    if (c.url.compare(0, 8, "https://") != 0 || c.url.size() < 12 || c.url.size() > 512) c.enabled = false;
    for (char ch : c.url)
        if (ch <= ' ' || ch == '\'' || ch == '"' || ch == '\\' || ch == '`' || ch == '$') c.enabled = false;
    return c;
}

// Artifact URL: same directory as the manifest URL + the validated file name, so a manifest cannot
// redirect the download to another host.
inline std::string artifactUrl(const std::string& manifestUrl, const std::string& file) {
    size_t s = manifestUrl.rfind('/');
    if (s == std::string::npos || !safeFileName(file)) return std::string();
    return manifestUrl.substr(0, s + 1) + file;
}

}}  // namespace mpx::upd
