#pragma once
// #355 (#351) B1 root fix: the caster token goes to a host only when /identity at that host named the
// token's cast machine id. The verified (token, host) pair is recorded here; the companion's
// serverHost_ / a transport request's address is never trusted for a token it did not arrive with.
#include "pms_reach.hpp"
#include "seek_reuse.hpp"

#include <mutex>
#include <string>

namespace misterplex {

enum class TokenSite { Resolve, Proxy, Seek, Skip, Timeline, TimelineUpdate, QueueAdd, MarkWatched };

inline bool tokenSiteOpenMutant(TokenSite s) {
    (void)s;
#ifdef MUT_355_OPEN_RESOLVE
    if (s == TokenSite::Resolve) return true;   // P3
#endif
#ifdef MUT_355_OPEN_PROXY
    if (s == TokenSite::Proxy) return true;     // P1
#endif
#ifdef MUT_355_OPEN_SKIP
    if (s == TokenSite::Skip) return true;      // S5
#endif
#ifdef MUT_355_OPEN_TIMELINE
    if (s == TokenSite::Timeline) return true;  // S4
#endif
#ifdef MUT_355_OPEN_TOKEN_UPDATE
    if (s == TokenSite::TimelineUpdate) return true;
#endif
    return false;
}

class CastTokenBinding {
public:
    // castAddress is the request's address: logged by callers, never trusted (MUT_355_TRUST_CAST_ADDRESS).
    bool allow(TokenSite site, const std::string& base, const std::string& castAddress,
               const std::string& castToken, const std::string& tokenUsed, const std::string& machineId,
               const PmsReachProbe& probe) {
        (void)castAddress;
        if (tokenSiteOpenMutant(site))
            return true;
        if (castToken.empty() || tokenUsed != castToken)
            return castTokenTargetOk(base, "", castToken, tokenUsed, machineId, probe);
        std::string bound;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!token_.empty() && token_ == castToken)
                bound = host_;
        }
#ifdef MUT_355_TRUST_CAST_ADDRESS
        bound = castAddress;   // the f865f5a/d708d49 shortcut
#endif
        const std::string bh = splitPlexHostPort(base, "").host;
        if (!bound.empty() && !bh.empty() && splitPlexHostPort(bound, "").host == bh)
            return castTokenTargetOk(base, bound, castToken, tokenUsed, machineId, probe);
        if (!castTokenTargetOk(base, "", castToken, tokenUsed, machineId, probe))
            return false;
        std::lock_guard<std::mutex> lock(mu_);
        token_ = castToken;    // verified by /identity at this host
        host_ = bh;
        return true;
    }
    void forget() {
        std::lock_guard<std::mutex> lock(mu_);
        token_.clear();
        host_.clear();
    }
    std::string boundHostFor(const std::string& token) const {
        std::lock_guard<std::mutex> lock(mu_);
        return token == token_ ? host_ : std::string();
    }

private:
    mutable std::mutex mu_;
    std::string token_;
    std::string host_;
};

// A playMedia with no usable target (refused, or tokened without address): drop the previous cast's
// base, its seek session and (refused) the stored token, so no later send reuses them.
template <class Req>
void stageCastWithoutTarget(const Req& req, std::string& lastBase, std::string& lastToken,
                            bool& castBaseUnverified, SeekReuseEntry& seekReuse) {
    if (!req.targetRejected && req.token.empty())
        return;
#ifndef MUT_355_KEEP_BASE
    lastBase.clear();                       // P2
#endif
    castBaseUnverified = true;
#ifndef MUT_355_KEEP_SEEK_REUSE
    seekReuse = SeekReuseEntry{};           // S2
#endif
#ifndef MUT_355_KEEP_TOKEN
    if (req.targetRejected)
        lastToken.clear();                  // S3
#endif
}

}  // namespace misterplex
