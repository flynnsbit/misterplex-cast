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
#include <cstring>

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

} // namespace misterplex
