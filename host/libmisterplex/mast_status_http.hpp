// GET /misterplex/mast: the MAST reload status, read-only (MAST R5b follow-up; review 01:28 and
// 03:52, release 04:05). Served by the existing companion HTTP server (no new port or listener),
// first after the header read, so it touches no other companion state. Binding spec:
//   - GET only, no parameters: another method on this path gives 405, a query or fragment 400;
//   - the body comes from the tmpfs reload marker alone (no DDR, devmem or SPI read), read with a
//     size cap (kStatusMarkerCap bytes; a larger file is invalid), never following a symlink and
//     never blocking (a FIFO or other non-regular file is invalid);
//   - no marker: 404, with cure_failed=0 in the body.
// The body is a fixed schema, text/plain, the same seven keys in the same order every time:
//   marker=present|absent|invalid
//   state=<requested|attempted|recovered|cure-failed|refused|load-failed, or empty>
//   time=<unix s from the marker, 0 when unknown>
//   reason=<text>
//   why=<text>
//   r6=<text>          (text: printable ASCII only, others '?', at most kStatusFieldMax chars)
//   cure_failed=0|1    (1 only for state=cure-failed)
// A marker with an unknown or missing state is invalid and echoes nothing; unknown marker keys are
// never echoed.
// Reading a reply (review 04:10 N1): HTTP 200 only means the endpoint answered; read marker=
// first. A file that is present but invalid (missing or unknown state, binary, over the cap, a
// symlink, a FIFO) also answers 200, with marker=invalid, empty fields and cure_failed=0: that is
// NOT "marker OK" and NOT "no failed cure", only "the marker cannot be read as a status".
#pragma once

#include "mast_reload.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace misterplex {
namespace mast {

constexpr const char* kStatusPath = "/misterplex/mast";
constexpr size_t kStatusMarkerCap = 1024;
constexpr size_t kStatusFieldMax = 160;

struct StatusReply {
    int code;
    std::string body;
};

inline bool knownMarkerState(const std::string& s) {
    return s == "requested" || s == "attempted" || s == "recovered" || s == "cure-failed" ||
           s == "refused" || s == "load-failed";
}

inline std::string statusField(const std::string& v) {
    std::string out;
    for (const char ch : v) {
        if (out.size() >= kStatusFieldMax)
            break;
        const unsigned char u = static_cast<unsigned char>(ch);
        out += (u >= 0x20 && u <= 0x7e) ? ch : '?';
    }
    return out;
}

struct StatusMarkerRead {
    bool present = false;  // something exists at the path
    bool valid = false;    // a regular file within the cap, read fully
    std::string text;
};

// At most kStatusMarkerCap + 1 bytes: one more than the cap marks the file invalid.
inline StatusMarkerRead readStatusMarker(const char* path) {
    StatusMarkerRead r;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        r.present = errno != ENOENT && errno != ENOTDIR;
        return r;
    }
    r.present = true;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return r;
    }
#ifdef MUT_MAST_STATUS_NO_CAP  // test-only mutant: the whole file is read, whatever its size
    const size_t cap = static_cast<size_t>(1) << 30;
#else
    const size_t cap = kStatusMarkerCap;
#endif
    char buf[256];
    bool ok = true;
    while (r.text.size() <= cap) {
        const size_t want = std::min(sizeof buf, cap + 1 - r.text.size());
        const ssize_t n = ::read(fd, buf, want);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (n == 0)
            break;
        r.text.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    r.valid = ok && r.text.size() <= cap;
    return r;
}

inline std::string statusBody(const char* path) {
    const StatusMarkerRead r = readStatusMarker(path);
    std::string marker = "absent";
    Marker m;
    if (r.present) {
        marker = "invalid";
        if (r.valid) {
            m = parseMarker(r.text);
            if (knownMarkerState(m.state))
                marker = "present";
        }
    }
    const bool show = marker == "present";
    return "marker=" + marker + "\nstate=" + (show ? m.state : std::string()) +
           "\ntime=" + std::to_string(show ? m.timeS : 0) +
           "\nreason=" + (show ? statusField(m.reason) : std::string()) +
           "\nwhy=" + (show ? statusField(m.why) : std::string()) +
           "\nr6=" + (show ? statusField(m.r6) : std::string()) +
           "\ncure_failed=" + (show && m.state == "cure-failed" ? "1" : "0") + "\n";
}

// The reply for one request, or nullopt when its path is not kStatusPath (any method): the
// companion then handles it as before. Only the request line is read.
inline std::optional<StatusReply> statusRequest(const std::string& req,
                                                const char* markerPath = kMarkerPath) {
    const std::string line = req.substr(0, req.find_first_of("\r\n"));
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos)
        return std::nullopt;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string target =
        line.substr(sp1 + 1, (sp2 == std::string::npos ? line.size() : sp2) - sp1 - 1);
    const size_t q = target.find_first_of("?#");
    if (target.substr(0, q) != kStatusPath)
        return std::nullopt;
#ifndef MUT_MAST_STATUS_ANY_VERB  // test-only mutant: any method is served
    if (line.compare(0, sp1, "GET") != 0)
        return StatusReply{405, "GET only\n"};
#endif
    if (q != std::string::npos)
        return StatusReply{400, "no parameters\n"};
    const std::string body = statusBody(markerPath);
    return StatusReply{body.rfind("marker=absent\n", 0) == 0 ? 404 : 200, body};
}

}  // namespace mast
}  // namespace misterplex
