// PLEX_BASE TCP reachability, for the status block's "Plex server" row
// (status_heartbeat.hpp H[2:1]). Numeric hosts only: a static ARM binary must not
// call getaddrinfo (glibc NSS abort, av_inproc_decode.cpp). A hostname base
// reports "-" (unknown). Used only on a statusBlock build row.
#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace misterplex {

struct PmsEndpoint {
    sockaddr_storage addr{};
    socklen_t len = 0;
    int family = 0;  // 0: not a numeric host (no probe)
    std::string label;
};

// "http://192.0.2.24:32400", "https://[fd00::1]:32400/x", "192.0.2.2".
inline PmsEndpoint parsePmsBase(const std::string& base) {
    PmsEndpoint ep;
    std::string h = base;
    int port = 32400;
    if (h.rfind("http://", 0) == 0) {
        h = h.substr(7);
    } else if (h.rfind("https://", 0) == 0) {
        h = h.substr(8);
        port = 443;
    }
    const size_t slash = h.find('/');
    if (slash != std::string::npos)
        h = h.substr(0, slash);
    std::string host = h;
    if (!h.empty() && h[0] == '[') {
        const size_t rb = h.find(']');
        if (rb == std::string::npos)
            return ep;
        host = h.substr(1, rb - 1);
        if (rb + 1 < h.size() && h[rb + 1] == ':')
            port = std::atoi(h.c_str() + rb + 2);
    } else {
        const size_t colon = h.find(':');
        if (colon != std::string::npos && h.find(':', colon + 1) == std::string::npos) {
            host = h.substr(0, colon);
            port = std::atoi(h.c_str() + colon + 1);
        }
    }
    ep.label = host + ":" + std::to_string(port);
    auto* a4 = reinterpret_cast<sockaddr_in*>(&ep.addr);
    auto* a6 = reinterpret_cast<sockaddr_in6*>(&ep.addr);
    if (inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1) {
        a4->sin_family = AF_INET;
        a4->sin_port = htons(static_cast<uint16_t>(port));
        ep.family = AF_INET;
        ep.len = sizeof(*a4);
    } else if (inet_pton(AF_INET6, host.c_str(), &a6->sin6_addr) == 1) {
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(static_cast<uint16_t>(port));
        ep.family = AF_INET6;
        ep.len = sizeof(*a6);
    }
    return ep;
}

// Non-blocking TCP connect within timeoutMs.
inline bool tcpReachable(const PmsEndpoint& ep, int timeoutMs) {
    if (!ep.family)
        return false;
    const int s = ::socket(ep.family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0)
        return false;
    bool ok = false;
    if (::connect(s, reinterpret_cast<const sockaddr*>(&ep.addr), ep.len) == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        pollfd p{s, POLLOUT, 0};
        if (::poll(&p, 1, timeoutMs) == 1) {
            int err = 0;
            socklen_t l = sizeof(err);
            ::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
            ok = err == 0;
        }
    }
    ::close(s);
    return ok;
}

}  // namespace misterplex
