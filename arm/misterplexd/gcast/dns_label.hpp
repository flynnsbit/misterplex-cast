// GCAST: AirPlay/RAOP mDNS instance name as ONE DNS label.
// UxPlay's embedded mdnsd encodes "<name>._airplay._tcp.local" by splitting on '.', so a --name such as
// "MiSTerPlex-192.0.2.110" went out as several labels and avahi dropped the record (release, a lab MiSTer).
// Rules: bytes outside [A-Za-z0-9 _-] (dots included) become '-'; leading/trailing spaces and dashes
// are trimmed; empty -> "MiSTerPlex". The RAOP instance is "<12 hex>@<name>", so the name is capped at
// 63 - 13 = 50 bytes, which keeps both the _airplay (name) and _raop (hex@name) labels within 63 bytes.
#pragma once
#include <cstddef>
#include <string>

namespace misterplex::gcast {

constexpr std::size_t kDnsLabelMax = 63;
constexpr std::size_t kRaopPrefixBytes = 13;   // 12 hex digits of the device id + '@'
constexpr std::size_t kAirplayNameMax = kDnsLabelMax - kRaopPrefixBytes;

inline std::string sanitizeDnsLabel(const std::string& in, std::size_t maxBytes = kDnsLabelMax) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_' || c == ' ';
#ifdef MUT_DNSL_KEEP_DOTS
        out.push_back((ok || c == '.') ? static_cast<char>(c) : '-');
#else
        out.push_back(ok ? static_cast<char>(c) : '-');
#endif
    }
    const auto trimmable = [](char c) { return c == ' ' || c == '-'; };
    while (!out.empty() && trimmable(out.front()))
        out.erase(0, 1);
#ifndef MUT_DNSL_NO_CAP
    if (out.size() > maxBytes)
        out.resize(maxBytes);
#else
    (void)maxBytes;
#endif
    while (!out.empty() && trimmable(out.back()))
        out.pop_back();
    return out.empty() ? std::string("MiSTerPlex") : out;
}

// The instance name handed to dnssd_init (AirPlay and RAOP).
inline std::string airplayInstanceName(const std::string& name) { return sanitizeDnsLabel(name, kAirplayNameMax); }

} // namespace misterplex::gcast
