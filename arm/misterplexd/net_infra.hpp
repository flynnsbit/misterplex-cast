// Addresses that are never "the PMS the cast came from": this box's own interface addresses, its default
// gateway, and the ladder proxy host. A request's source IP that equals one of these is the gateway/relay/
// proxy side of the connection, not the PMS, even when it forwards :32400 and answers /identity.
#pragma once

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace misterplex {

// /proc/net/route text -> dotted default-gateway IPv4 ("" when none). Gateway column is a little-endian hex.
inline std::string defaultGatewayFromProcNetRoute(const std::string& text) {
    std::istringstream is(text);
    std::string line;
    std::getline(is, line); // header
    while (std::getline(is, line)) {
        char iface[64] = {0};
        unsigned long dst = 1, gw = 0, flags = 0;
        if (std::sscanf(line.c_str(), "%63s %lx %lx %lx", iface, &dst, &gw, &flags) != 4)
            continue;
        if (dst != 0 || !(flags & 0x2) || gw == 0) // default route, RTF_GATEWAY
            continue;
        struct in_addr a;
        a.s_addr = static_cast<uint32_t>(gw);
        char buf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &a, buf, sizeof buf))
            return buf;
    }
    return {};
}

// "http://host:port", "host:port" or "host" -> host (bracketed IPv6 kept without brackets).
inline std::string hostOfBase(std::string s) {
    const auto sch = s.find("://");
    if (sch != std::string::npos)
        s = s.substr(sch + 3);
    const auto slash = s.find('/');
    if (slash != std::string::npos)
        s = s.substr(0, slash);
    if (!s.empty() && s[0] == '[') {
        const auto e = s.find(']');
        return e == std::string::npos ? s : s.substr(1, e - 1);
    }
    const auto colon = s.find(':');
    if (colon != std::string::npos && s.find(':', colon + 1) == std::string::npos)
        s = s.substr(0, colon);
    return s;
}

// Local interface IPv4 addresses (getifaddrs).
inline std::vector<std::string> localIpv4Addresses() {
    std::vector<std::string> out;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return out;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        char buf[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(p->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf))
            out.push_back(buf);
    }
    freeifaddrs(ifa);
    return out;
}

inline std::string readWholeFile(const char* path) {
    std::string s;
    if (FILE* f = std::fopen(path, "r")) {
        char b[1024];
        size_t n;
        while ((n = std::fread(b, 1, sizeof b, f)) > 0)
            s.append(b, n);
        std::fclose(f);
    }
    return s;
}

// Hosts a request source IP must not be taken for the PMS: gateway, local interfaces, proxy host.
inline std::vector<std::string> pmsInfraHosts(const std::string& proxyBase = std::string()) {
    std::vector<std::string> out = localIpv4Addresses();
    const std::string gw = defaultGatewayFromProcNetRoute(readWholeFile("/proc/net/route"));
    if (!gw.empty())
        out.push_back(gw);
    const std::string ph = hostOfBase(proxyBase);
    if (!ph.empty())
        out.push_back(ph);
    return out;
}

} // namespace misterplex
