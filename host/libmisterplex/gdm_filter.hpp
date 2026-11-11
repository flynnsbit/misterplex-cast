// Plex GDM (UDP 32412) reply filter.
//
// Defect (Sweep 114, parent-measured): companion gdmLoop replied to ANY datagram
// whose payload contained the substring "plex":
//
//   if (strstr(buf, "M-SEARCH") || strstr(buf, "plex")) sendto(peer, advertise);
//
// Our own advertise/response body is full of "plex" ("Content-Type: plex/...",
// "Protocol: plex", ...). On a host that receives its own UDP (common with
// broadcast + local delivery), the loop is:
//   advertise → recv own packet → match "plex" → sendto(self) → recv → …
// select() never blocks (socket always readable), voluntary ctxt switches stay
// near zero, and one core sits at ~100% system+user. Parent forensics on the
// unnamed GDM thread: nonvoluntary:voluntary ≈ 1936:1, syscall=running.
//
// Contract: reply ONLY to discovery probes (M-SEARCH), never to 200 OK
// advertisements or other plex-bearing traffic. Unit tests pin this so a
// "contains plex" catch-all cannot return.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace misterplex {

// True iff `buf[0,len)` is a GDM/SSDP-style discovery probe we should answer.
inline bool gdmShouldReply(const char* buf, size_t len) {
    if (!buf || len == 0)
        return false;

    // Scan for "M-SEARCH" case-insensitively without allocating.
    // Probes look like: "M-SEARCH * HTTP/1.1\r\n..." (Plex GDM / SSDP).
    static constexpr char kNeedle[] = "m-search";
    constexpr size_t kNlen = sizeof(kNeedle) - 1;
    if (len < kNlen)
        return false;

    for (size_t i = 0; i + kNlen <= len; ++i) {
        size_t j = 0;
        for (; j < kNlen; ++j) {
            const unsigned char c = static_cast<unsigned char>(buf[i + j]);
            const char lower = static_cast<char>(std::tolower(c));
            if (lower != kNeedle[j])
                break;
        }
        if (j == kNlen)
            return true;
    }
    return false;
}

// Convenience for NUL-terminated C strings (tests + call sites with buf[n]=0).
inline bool gdmShouldReplyCStr(const char* buf) {
    if (!buf)
        return false;
    return gdmShouldReply(buf, std::strlen(buf));
}

// Cast picker must not see us unless HTTP :port is listening.
// Chevron (FPGA idle) is not enough — Plex Web talks HTTP after GDM.
// RED twin: the gold/legacy companion advertised GDM as soon as gdmLoop
// started, even when bind :3005 had already failed (picker vanish).
inline bool gdmMayAdvertise(bool httpReady) { return httpReady; }

inline bool gdmMayReply(bool httpReady, const char* buf, size_t len) {
    return httpReady && gdmShouldReply(buf, len);
}

// F2190: GDM is L2-only (UDP 32412 broadcast). WAN PMS /clients stays size=0
// because we do not PUT /clients on a remote PLEX_BASE and we do not GDM-reply
// to a unicast WAN peer (203.0.113.47). Native LAN M-SEARCH still gets a reply.
inline bool gdmPeerIsL2Lan(const char* dotted) {
#ifdef MUT_GDM_WAN_REPLY
    (void)dotted;
    return true;
#else
    if (!dotted || !dotted[0])
        return false;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (std::sscanf(dotted, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    if (a == 10)
        return true;
    if (a == 192 && b == 168)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    if (a == 127)
        return true;
    if (a == 169 && b == 254)
        return true;
    return false;
#endif
}

inline bool gdmMayReplyToPeer(bool httpReady, const char* buf, size_t len, const char* peerDotted) {
    return gdmMayReply(httpReady, buf, len) && gdmPeerIsL2Lan(peerDotted);
}

// Exact pre-fix advertise policy (HTTP readiness ignored). Tests pin that
// the new gate disagrees with this when httpReady is false.
inline bool legacyGdmAdvertiseRegardlessOfHttp(bool /*httpReady*/) { return true; }

// Negative oracle used by tests: a body that is our advertise shape must NOT
// match. Kept as a pure string so the test does not need Companion.
inline constexpr const char* kGdmAdvertiseShape =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: plex/media-player\r\n"
    "Name: MiSTerPlex\r\n"
    "Port: 3005\r\n"
    "Product: MiSTerPlex\r\n"
    "Version: 0.2.0\r\n"
    "Protocol: plex\r\n"
    "Protocol-Version: 1\r\n"
    "Protocol-Capabilities: timeline,playback,navigation,mirror,playqueues\r\n"
    "Device-Class: stb\r\n"
    "Resource-Identifier: test-id\r\n"
    "\r\n";

// Dual-home GDM advertise (GH#23). PMS /clients records the UDP source IP.
// A box with two RFC1918 ifaces on different L2 networks must announce from
// each iface, or Plex Web on one subnet lists the other and the picker is
// one hop off.
inline constexpr uint16_t kGdmListenPort = 32412;
inline constexpr uint16_t kGdmClientRegisterPort = 32413;
inline constexpr const char* kGdmClientMulticast = "239.0.0.250";

struct GdmIface {
    std::string ip;
    std::string broadcast;
    bool up = false;
    bool loopback = false;
};

struct GdmDest {
    std::string src;
    std::string dst;
    uint16_t port = kGdmListenPort;
    bool hello = false;  // 32413 register uses HELLO; 32412 uses 200 OK
};

inline bool gdmIpLinkLocal(const std::string& ip) {
    return ip.size() >= 8 && ip.compare(0, 8, "169.254.") == 0;
}

inline bool gdmIpRfc1918(const std::string& ip) {
    if (ip.compare(0, 3, "10.") == 0)
        return true;
    if (ip.compare(0, 8, "192.168.") == 0)
        return true;
    if (ip.compare(0, 4, "172.") == 0)
        return true;
    return false;
}

inline std::vector<GdmDest> gdmAdvertiseDests(const std::vector<GdmIface>& ifs) {
    std::vector<GdmDest> out;
    for (const auto& i : ifs) {
        if (!i.up || i.loopback || i.ip.empty())
            continue;
        if (gdmIpLinkLocal(i.ip) || !gdmIpRfc1918(i.ip))
            continue;
        if (!i.broadcast.empty())
            out.push_back({i.ip, i.broadcast, kGdmListenPort, false});
        out.push_back({i.ip, kGdmClientMulticast, kGdmClientRegisterPort, true});
    }
    return out;
}

} // namespace misterplex
