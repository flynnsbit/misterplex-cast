// Reachable-PMS fallback (playbar IPv6 lead). Plex Web can hand the cast target a PMS address
// the MiSTer cannot route to (buildhost's wifi IPv6 ULA). pick() decides whether the address Plex Web
// gave is usable and, when it is not, which IPv4/LAN base to use instead for BOTH the resolve and
// the PMS timeline posts. No network access of its own: the probe is injected, so the unit tests
// mock an unreachable ULA. The real probe is probePmsIdentity() (GET <base>/identity).
#pragma once

#include "plex_resolve.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace misterplex {

// true when `base` answers /identity (and carries machineId when one is given).
using PmsReachProbe =
    std::function<bool(const std::string& base, const std::string& machineId, int timeoutSec)>;

struct PmsFallbackInput {
    std::string selected;                   // what Plex Web handed over: URL, host, or label
    std::string port = "32400";             // port to pair with an IP-only candidate
    std::string machineId;                  // PMS machineIdentifier ("" when unknown)
    std::vector<std::string> resourceBases; // 1st: plex.tv local IPv4 connections (http://a.b.c.d:p)
    // Lazy plex.tv lookup, only called on a cache miss (one HTTPS round trip), results join resourceBases.
    std::function<std::vector<std::string>()> resourceFn;
    // Hosts the source IP must not be taken for: this box's own addresses, its default gateway, the ladder
    // proxy host (net_infra.hpp pmsInfraHosts). A peer in this list is the gateway/relay side, never the PMS.
    std::vector<std::string> infraHosts;
    std::string confBase;                   // 2nd: the configured PMS (PLEX_BASE / servers[0]), verified by /identity
    std::string peerAddress;                // 3rd, last resort: source IP of the cast/poll request. It is the
                                            // TCP peer, NOT necessarily the PMS: behind a NAT/gateway/relay it is
                                            // the gateway, which can forward :32400 and still answer /identity.
};

class PmsReachability {
public:
    static constexpr int kProbeTimeoutSec = 2;
    static constexpr int64_t kPositiveTtlMs = 5 * 60 * 1000;
    static constexpr int64_t kNegativeTtlMs = 30 * 1000;
    using NowMs = std::function<int64_t()>;

    explicit PmsReachability(PmsReachProbe probe, NowMs now = {})
        : probe_(std::move(probe)), now_(now ? std::move(now) : NowMs(&steadyMs)) {}

    // The base to use for this PMS. Returns in.selected unchanged when it needs no fallback (an
    // IPv4 or named host, or an IPv6 that a probe shows is reachable). Returns "" when a fallback
    // was needed and no candidate answered (the caller keeps its older conf mapping).
    std::string pick(const PmsFallbackInput& in, std::string* why = nullptr) {
        std::string v6;
        const bool isV6 = ipv6FromPmsHost(in.selected, v6);
        if (!isV6)
            return in.selected;
        const bool never = isUnreachableIpv6Host(in.selected);
        const std::string key = !in.machineId.empty() ? "id:" + in.machineId : "host:" + v6;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = cache_.find(key);
            if (it != cache_.end() && now_() < it->second.expiresMs) {
                if (why)
                    *why = "cached";
                return it->second.base;
            }
        }
        std::string chosen;
        std::string reason;
        if (!never && probeOnce("http://[" + v6 + "]:" + in.port, in.machineId)) {
            chosen = in.selected;
            reason = "global IPv6 answers";
        } else {
            PmsFallbackInput eff = in;
            if (in.resourceFn) {
                const auto more = in.resourceFn();
                eff.resourceBases.insert(eff.resourceBases.end(), more.begin(), more.end());
            }
            for (const auto& c : candidates(eff)) {
                if (probeOnce(c.first, in.machineId)) {
                    chosen = c.first;
                    reason = c.second;
                    break;
                }
            }
            if (chosen.empty())
                reason = "no candidate answered";
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            cache_[key] = {chosen, now_() + (chosen.empty() ? kNegativeTtlMs : kPositiveTtlMs)};
        }
        if (why)
            *why = reason;
        return chosen;
    }

    // #336 review: cached /identity id check for the #333 player-LAN base (same TTLs as pick()).
    bool identityMatches(const std::string& base, const std::string& machineId) {
#ifdef MUT_336_KEY_NO_ID
        const std::string key = "lan:" + normalizePlexBase(base);
#else
        // The id is part of the key: a result cached for one server id never answers for another.
        const std::string key = "lan:" + normalizePlexBase(base) + "#" + machineId;
#endif
#ifndef MUT_336_NO_CACHE
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = cache_.find(key);
            if (it != cache_.end() && now_() < it->second.expiresMs)
                return !it->second.base.empty();
        }
#endif
        const bool ok = probeOnce(base, machineId);
        std::lock_guard<std::mutex> lock(mu_);
        cache_[key] = {ok ? base : std::string(), now_() + (ok ? kPositiveTtlMs : kNegativeTtlMs)};
        return ok;
    }
    PmsReachProbe cachedProbe() {
        return [this](const std::string& b, const std::string& id, int) { return identityMatches(b, id); };
    }

    size_t probeCount() const {
        std::lock_guard<std::mutex> lock(mu_);
        return probes_;
    }
    void forget() {
        std::lock_guard<std::mutex> lock(mu_);
        cache_.clear();
    }

private:
    struct Entry {
        std::string base;
        int64_t expiresMs = 0;
    };

    static int64_t steadyMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool probeOnce(const std::string& base, const std::string& machineId) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++probes_;
        }
        return probe_ && probe_(base, machineId, kProbeTimeoutSec);
    }

    static bool dottedIpv4(const std::string& s) {
        unsigned char b[4];
        return inet_pton(AF_INET, s.c_str(), b) == 1;
    }

    // IPv4 candidates in the order: plex.tv connections, the configured PMS (conf), the request's
    // source IP last. The source IP is only the peer of the cast request (e.g. the lab gateway
    // 203.0.113.1 that forwards :32400 and answers /identity), so it must never beat the configured PMS.
    static std::vector<std::pair<std::string, std::string>> candidates(const PmsFallbackInput& in) {
        std::vector<std::pair<std::string, std::string>> out;
        auto add = [&out](const std::string& base, const char* why) {
            const std::string b = normalizePlexBase(base);
            std::string v6;
            if (b.empty() || ipv6FromPmsHost(b, v6))
                return;
            for (const auto& e : out)
                if (e.first == b)
                    return;
            out.push_back({b, why});
        };
        for (const auto& r : in.resourceBases)
            add(r, "plex.tv local IPv4 connection");
        add(in.confBase, "configured PMS (conf)");
        bool infra = false;
        for (const auto& h : in.infraHosts)
            if (h == in.peerAddress)
                infra = true;
        if (!infra && dottedIpv4(in.peerAddress) && in.peerAddress.rfind("127.", 0) != 0 &&
            in.peerAddress != "0.0.0.0")
            add("http://" + in.peerAddress + ":" + in.port, "request source IP");
        return out;
    }

    PmsReachProbe probe_;
    NowMs now_;
    mutable std::mutex mu_;
    std::map<std::string, Entry> cache_;
    size_t probes_ = 0;
};

// #333 (RELEASE-1103 glass, lab): the player-/24 gateway rule matches by address only, so a real
// PMS on the player's LAN (test PMS 203.0.113.1:32402, machineIdentifier in playMedia) was rewritten
// onto the conf default PMS ("cast base http://203.0.113.1:32402 -> reachable http://203.0.113.25:32401")
// and the cast ratingKey was resolved against the wrong server. Keep the cast base only when its
// /identity carries the cast machineIdentifier. No machine id is no proof (a NAT gateway can forward
// :32400 and answer /identity), so the old rule stands. The probe sends no token.
inline bool castLanPmsVerified(const std::string& selectedBase, const std::string& machineId,
                               const std::string& conf, const std::string& playerLan,
                               const PmsReachProbe& probe) {
#ifndef MUT_333_NO_ID_REQUIRED
    if (machineId.empty())
        return false;
#endif
    if (!probe)
        return false;
    const std::string n = normalizePlexBase(selectedBase);
    if (n.rfind("http://", 0) != 0)
        return false;
#ifndef MUT_333_ANY_HOST
    if (!castHostOnPlayerLan(n, playerLan, conf))
        return false;
#endif
#ifdef MUT_333_SKIP_PROBE
    return true;
#else
    return probe(n, machineId, PmsReachability::kProbeTimeoutSec);
#endif
}

// #344 glass / #333: an explicit cast base wins. A rewrite onto a different base (gateway rule,
// openable map) is allowed only when that target's /identity names the cast machineIdentifier.
// No machine id -> keep the cast base. The probe is untokened (no conf token on a mismatch).
// #347 glass (lab): the conf default (a third-party PMS) was probed on every miss. Now the target is
// probed only when it is already a known candidate for the cast server: targetKnownId (from the
// linked plex.tv roster, never from a probe) must equal the cast machine id. Otherwise: keep, no probe.
inline bool castRewriteVerified(const std::string& selectedBase, const std::string& target,
                                const std::string& machineId, const std::string& targetKnownId,
                                const PmsReachProbe& probe) {
    if (target.empty() || normalizePlexBase(target) == normalizePlexBase(selectedBase))
        return true;
#ifndef MUT_344D_NO_ID_REQUIRED
    if (machineId.empty() || !probe)
        return false;
#endif
#ifndef MUT_347C_PROBE_ANY_TARGET
    if (targetKnownId != machineId)   // machineId is non-empty here (checked above)
        return false;   // not a candidate for this cast server: no /identity request at all
#endif
#ifdef MUT_344D_SKIP_PROBE
    return true;
#else
    return probe ? probe(normalizePlexBase(target), machineId, PmsReachability::kProbeTimeoutSec) : true;
#endif
}

// #347 / #368: a kept cast base (no /identity switch) stays on the same host.
// ARM libav is linked with OpenSSL (MPX_HAVE_GCAST_TLS). https://name:port and
// https://a-b-c-d.<hash>.plex.direct:port keep TLS (SNI is the plex.direct label).
// https://dotted-IPv4:port still maps to http://IPv4:port: there is no name on the
// certificate. Never moves to a different host; a cross-server switch stays behind
// castRewriteVerified(). Unparseable / non-IPv4 plex.direct labels are unchanged.
inline std::string castKeptOpenableBase(const std::string& selectedBase) {
    std::string b = selectedBase;
    while (!b.empty() && (b.back() == '/' || b.back() == ' '))
        b.pop_back();
    const auto sep = b.find("://");
    if (sep == std::string::npos)
        return b;
    std::string scheme = b.substr(0, sep);
    for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string hp = b.substr(sep + 3);
    const auto slash = hp.find('/');
    if (slash != std::string::npos)
        hp = hp.substr(0, slash);
    if (hp.empty() || hp.front() == '[' || hp.find('@') != std::string::npos)
        return b;   // IPv6 literal / userinfo: not ours to touch
    std::string host = hp, port = "32400";
    const auto colon = hp.rfind(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = hp.substr(colon + 1);
    }
    if (host.empty() || port.empty())
        return b;
    std::string lower = host;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string pd = ".plex.direct";
    if (lower.size() > pd.size() && lower.compare(lower.size() - pd.size(), pd.size(), pd) == 0) {
        const std::string label = lower.substr(0, lower.find('.'));
        std::string ip = label;
        for (auto& c : ip) if (c == '-') c = '.';
        in_addr a{};
        if (std::count(label.begin(), label.end(), '-') == 3 && inet_pton(AF_INET, ip.c_str(), &a) == 1) {
#ifdef MUT_347_NO_PLEXDIRECT_MAP
            return "http://" + ip + ":" + port;
#else
            if (scheme == "https")
                return "https://" + host + ":" + port;
            return "http://" + ip + ":" + port;
#endif
        }
        return b;   // IPv6 / unknown label: keep (the IPv6 fallback path owns those)
    }
#ifndef MUT_347_NO_HTTPS_MAP
    if (scheme == "https") {
        in_addr a{};
        if (inet_pton(AF_INET, host.c_str(), &a) == 1)
            return "http://" + host + ":" + port;   // raw IP: no SNI name
        return "https://" + host + ":" + port;
    }
#else
    if (scheme == "https")
        return "http://" + host + ":" + port;
#endif
    return b;
}

// #347 glass (lab): the companion bound its PMS timeline (and main its provisional lastBase) through
// reachableTimelinePms(), whose address-only player-LAN gateway rule moved a cast from the test PMS
// 203.0.113.1:32402 onto the conf default (a third-party PMS) -> timeline traffic to that server.
// With an explicit cast address the timeline stays on the cast host: only the same-host
// https/plex.direct -> http map applies. No cast address -> the old conf fallback is unchanged.
// #351 review: one IPv6-literal test for cast hosts: bare, bracketed or schemed
// ("http://[fd00::5]:32400"). IPv4/DNS host[:port] is not.
inline bool isIpv6Literal(const std::string& address) {
    std::string a = address;
    const auto scheme = a.find("://");
    if (scheme != std::string::npos)
        a = a.substr(scheme + 3);
    if (!a.empty() && a[0] == '[')
        return true;
    a = a.substr(0, a.find('/'));
    return a.find(':') != a.rfind(':');
}

// #351 glass 2350ab1 FAIL: the caster's token went to the conf PMS (other machine id) for an IPv6
// cast target (bare: refused and cleared, so resolve fell to conf and the servers loop; bracketed:
// a stale "id:" pick() cache entry mapped it to conf). Every resolve send with the cast token is
// gated here: allowed to the cast's own IPv4/DNS host, else only after a FRESH (uncached)
// /identity probe names the cast machine id. No id: never.
inline bool castTokenTargetOk(const std::string& base, const std::string& castAddress,
                              const std::string& castToken, const std::string& tokenUsed,
                              const std::string& machineId, const PmsReachProbe& freshProbe) {
#ifdef MUT_351_TOKEN_GATE_OFF
    return true;
#endif
    if (castToken.empty() || tokenUsed != castToken)
        return true;   // conf token or no token: not the caster's
    const std::string bh = splitPlexHostPort(base, "").host;
    if (bh.empty())
        return false;
    if (!castAddress.empty() && !isIpv6Literal(castAddress) && splitPlexHostPort(castAddress, "").host == bh)
        return true;   // the cast's own server
#ifndef MUT_351_TOKEN_GATE_NO_ID
    if (machineId.empty())
        return false;
#endif
    return freshProbe && freshProbe(base, machineId, 2);
}

// Operator / plex_browse playMedia often repeats the conf PLEX_TOKEN. That is not a
// caster token: #351 must not withhold it from the conf PMS (empty machine id).
// A matching token on any other host still needs /identity.
inline bool confTokenOnConfHost(const std::string& reqToken, const std::string& confToken,
                                const std::string& base, const std::string& confBase) {
#ifdef MUT_351_CONF_TOKEN_FOREIGN
    (void)reqToken;
    (void)confToken;
    (void)base;
    (void)confBase;
    return true;
#else
    if (reqToken.empty() || confToken.empty() || reqToken != confToken)
        return false;
    const std::string bh = splitPlexHostPort(base, "").host;
    const std::string ch = splitPlexHostPort(confBase, "").host;
    return !bh.empty() && bh == ch;
#endif
}

// #351 glass f865f5a FAIL (c5/c6): a seek replays the companion's transport request, which can still
// carry the previous cast's address while the token is the new caster's. A seek always uses the
// server of the play it continues (lastPlay); after a refused target it has no cast server and
// no caster token (conf only), so castTokenTargetOk never sees a borrowed "own host".
template <class Req>
void pinSeekServer(Req& m, const Req& last) {
#ifndef MUT_355_SEEK_REJECTED_KEEPS_TOKEN
    if (m.targetRejected) {   // #355 B1: the companion carried a refused target; no cast server, no caster token
        m.address.clear();
        m.port.clear();
        m.protocol.clear();
        m.token.clear();
    }
#endif
    if (m.key.empty() || last.key != m.key)
        return;
#ifndef MUT_351_SEEK_KEEPS_ADDRESS
    m.address = last.address;
    m.port = last.port;
    m.protocol = last.protocol;
    m.serverMachineId = last.serverMachineId;
    m.targetRejected = last.targetRejected;
#endif
#ifndef MUT_351_SEEK_KEEPS_TOKEN
    if (last.targetRejected) {
        m.token.clear();
        return;
    }
#endif
    if (!last.token.empty())
        m.token = last.token;
}

// #351 review: PMS base sent with the cast token by window actions. Unverified and empty: nothing
// (never refilled with the conf default); else lastBase, or the conf default for non-cast sessions.
inline std::string windowTokenBase(const std::string& lastBase, bool castBaseUnverified,
                                   const std::string& defaultPms) {
#ifndef MUT_351_WINDOW_REFILL
    if (castBaseUnverified && lastBase.empty())
        return {};
#endif
    return lastBase.empty() ? defaultPms : lastBase;
}

inline PlexEndpoint castTimelinePms(const std::string& address, const std::string& port,
                                    const std::string& protocol, const std::string& lanFallback,
                                    const std::string& playerLan) {
#ifndef MUT_347C_TIMELINE_GATEWAY
    // #351: an IPv6 literal (playMedia ULA) is not openable from the IPv4-only MiSTer, so it is
    // never kept on the Timeline; it takes the old reachable/LAN fallback. IPv4, plex.direct and
    // DNS cast hosts are kept (#347).
    const bool ipv6Literal =
#ifdef MUT_351_KEEP_ULA
        false;
#else
        isIpv6Literal(address);
#endif
    if (!address.empty() && !ipv6Literal) {
        PlexEndpoint raw = splitPlexHostPort(address, port);
        if (!protocol.empty())
            raw.protocol = protocol;
        if (raw.protocol.empty())
            raw.protocol = "http";
        if (raw.port.empty())
            raw.port = port.empty() ? "32400" : port;
        if (raw.host.empty())
            return raw;
        const std::string open =
            castKeptOpenableBase(joinPlexHostPort(raw.protocol, raw.host, raw.port));
        PlexEndpoint out = splitPlexHostPort(open, raw.port);
        if (out.host.empty())
            return raw;
        if (out.protocol.empty())
            out.protocol = raw.protocol;
        if (out.port.empty())
            out.port = raw.port;
        return out;
    }
#endif
    return reachableTimelinePms(address, port, protocol, lanFallback, playerLan);
}

// #351 review (FPGA Bug, aa7ebfe): main's provisional lastBase (used with the cast token by queue/add,
// skip and mark-watched until the verified commit) must never be the conf default chosen without a
// machine-id check. An IPv6-literal cast host has no openable same-origin base, so the provisional
// base is empty until the resolve commits a machine-id-verified base.
inline std::string provisionalCastBase(const std::string& address, const std::string& port,
                                       const std::string& protocol, const std::string& lanFallback,
                                       const std::string& playerLan) {
    if (address.empty())
        return {};
#ifndef MUT_351_PROVISIONAL_FALLBACK
    if (isIpv6Literal(address))
        return {};
#endif
    const PlexEndpoint ep = castTimelinePms(address, port, protocol, lanFallback, playerLan);
    return joinPlexHostPort(ep.protocol, ep.host, ep.port);
}

// #347 glass 71e4b762 (lab, release conf): with a cast from another server playing, the status
// heartbeat probe (TCP connect to the conf default every 10 s) was the only thing still reaching the
// conf default. While a play is active whose base is a different host, skip that probe and keep the
// last state. No play, no base, or the same host as the conf default: probe as before.
inline bool heartbeatProbeAllowed(bool playActive, const std::string& activeBase, const std::string& confBase) {
#ifdef MUT_347D_PROBE_DURING_CAST
    return true;
#else
    if (!playActive || activeBase.empty() || confBase.empty())
        return true;
    const PlexEndpoint a = splitPlexHostPort(normalizePlexBase(activeBase), "32400");
    const PlexEndpoint c = splitPlexHostPort(normalizePlexBase(confBase), "32400");
    return a.host.empty() || a.host == c.host;
#endif
}

} // namespace misterplex
