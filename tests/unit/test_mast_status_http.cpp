// GET /misterplex/mast (MAST R5b follow-up; review 01:28/03:52, release 04:05):
// host/libmisterplex/mast_status_http.hpp, the read-only status the companion serves.
//  - marker present: 200, the fixed seven-key body, cure_failed=1 only for cure-failed;
//  - marker absent: 404, marker=absent and cure_failed=0;
//  - garbage marker (no or unknown state, binary bytes, over the size cap, a symlink, a FIFO):
//    200, marker=invalid, nothing of it echoed, cure_failed=0, and never blocking;
//  - fields are printable ASCII, capped; unknown marker keys never appear;
//  - another method on the path: 405; a query or fragment: 400; other paths are not ours.
// Built with -DMUT_MAST_STATUS_ANY_VERB or -DMUT_MAST_STATUS_NO_CAP, this test must FAIL.
#include "libmisterplex/mast_status_http.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace misterplex::mast;

namespace {

int fails = 0;
int rows = 0;

void expect(bool ok, const std::string& what) {
    ++rows;
    if (!ok) {
        ++fails;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

void put(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

const std::string kAbsent =
    "marker=absent\nstate=\ntime=0\nreason=\nwhy=\nr6=\ncure_failed=0\n";
const std::string kInvalid =
    "marker=invalid\nstate=\ntime=0\nreason=\nwhy=\nr6=\ncure_failed=0\n";

std::string get(const char* marker, int* code = nullptr) {
    const auto r = statusRequest("GET /misterplex/mast HTTP/1.1\r\nHost: x\r\n\r\n", marker);
    if (!r) {
        if (code)
            *code = -1;
        return "(not served)";
    }
    if (code)
        *code = r->code;
    return r->body;
}

}  // namespace

int main() {
    char dir[] = "/tmp/test_mast_status_http.XXXXXX";
    if (!mkdtemp(dir)) {
        std::perror("mkdtemp");
        return 2;
    }
    const std::string d = dir;
    const std::string mk = d + "/marker";
    int code = 0;

    // Absent: 404, cure_failed=0.
    expect(get(mk.c_str(), &code) == kAbsent && code == 404, "absent marker: 404 + absent body");

    // Present.
    put(mk, "state=cure-failed\ntime=1759650000\nreason=gate-open probe\n"
            "why=gate-silent:no CAPS reply after 5 probes\nr6=quiesced (consumed == written)\n");
    expect(get(mk.c_str(), &code) ==
                   "marker=present\nstate=cure-failed\ntime=1759650000\nreason=gate-open probe\n"
                   "why=gate-silent:no CAPS reply after 5 probes\nr6=quiesced (consumed == written)\n"
                   "cure_failed=1\n" &&
               code == 200,
           "cure-failed marker: 200, the fixed body, cure_failed=1");
    for (const char* s : {"requested", "attempted", "recovered", "refused", "load-failed"}) {
        put(mk, std::string("state=") + s + "\ntime=5\nreason=begin\n");
        const std::string b = get(mk.c_str(), &code);
        expect(code == 200 && b == std::string("marker=present\nstate=") + s +
                                       "\ntime=5\nreason=begin\nwhy=\nr6=\ncure_failed=0\n",
               std::string("state ") + s + ": 200, cure_failed=0: " + b);
    }
    // Unknown keys never appear; only the fixed schema.
    put(mk, "token=SECRET-XYZ\nstate=attempted\nX-Plex-Token=abc\ntime=7\nreason=r\nevil=1\n");
    {
        const std::string b = get(mk.c_str(), &code);
        expect(code == 200 && !has(b, "SECRET") && !has(b, "abc") && !has(b, "evil") &&
                   b == "marker=present\nstate=attempted\ntime=7\nreason=r\nwhy=\nr6=\ncure_failed=0\n",
               "unknown keys are not echoed: " + b);
    }
    // Fields: printable ASCII only, capped.
    put(mk, std::string("state=refused\ntime=1\nreason=a\rb\x01" "c\x7f" "d\xff" "e\nwhy=") +
                std::string(500, 'w') + "\n");
    {
        const std::string b = get(mk.c_str(), &code);
        expect(has(b, "\nreason=a?b?c?d?e\n") && has(b, "\nwhy=" + std::string(kStatusFieldMax, 'w') + "\n") &&
                   !has(b, std::string(kStatusFieldMax + 1, 'w')),
               "fields sanitized and capped: " + b);
    }

    // Garbage: invalid, nothing echoed, cure_failed=0.
    put(mk, std::string("\x00\x01\xfe garbage token=SECRET\n", 25));
    expect(get(mk.c_str(), &code) == kInvalid && code == 200, "binary garbage: invalid");
    put(mk, "reason=only a reason\nwhy=x\n");
    expect(get(mk.c_str(), &code) == kInvalid && code == 200, "no state line: invalid");
    put(mk, "state=hacked\ntime=1\nreason=x\n");
    expect(get(mk.c_str()) == kInvalid, "unknown state: invalid, not echoed");
    put(mk, "");
    expect(get(mk.c_str(), &code) == kInvalid && code == 200, "empty marker: invalid");
    // The size cap: exactly kStatusMarkerCap bytes is read; one more is invalid.
    {
        std::string head = "state=cure-failed\ntime=9\nreason=r\nwhy=";
        std::string atCap = head + std::string(kStatusMarkerCap - head.size() - 1, 'z') + "\n";
        put(mk, atCap);
        const std::string b = get(mk.c_str());
        expect(atCap.size() == kStatusMarkerCap && has(b, "marker=present\nstate=cure-failed\n") &&
                   has(b, "cure_failed=1\n"),
               "a marker of exactly the cap is read: " + b.substr(0, 60));
        put(mk, atCap + "z");
        expect(get(mk.c_str(), &code) == kInvalid && code == 200,
               "a marker one byte over the cap is invalid (size-capped read)");
    }
    // Not a regular file: a symlink is not followed, a FIFO never blocks the companion thread.
    {
        const std::string target = d + "/target";
        put(target, "state=cure-failed\ntime=1\nreason=r\n");
        ::unlink(mk.c_str());
        expect(::symlink(target.c_str(), mk.c_str()) == 0, "symlink made");
        expect(get(mk.c_str(), &code) == kInvalid && code == 200, "symlink marker: invalid, not followed");
        ::unlink(mk.c_str());
        expect(::mkfifo(mk.c_str(), 0600) == 0, "fifo made");
        const auto t0 = std::chrono::steady_clock::now();
        const std::string b = get(mk.c_str(), &code);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        expect(b == kInvalid && code == 200 && ms < 500,
               "fifo marker: invalid at once (" + std::to_string(ms) + " ms)");
        ::unlink(mk.c_str());
        ::unlink(target.c_str());
    }

    // Method and parameters (the path is ours; nothing is read).
    put(mk, "state=cure-failed\ntime=1\nreason=r\n");
    for (const char* m : {"POST", "HEAD", "PUT", "DELETE", "OPTIONS", "get", "GETX"}) {
        const auto r = statusRequest(std::string(m) + " /misterplex/mast HTTP/1.1\r\n\r\n", mk.c_str());
        expect(r && r->code == 405 && r->body == "GET only\n",
               std::string("method ") + m + ": 405 GET only");
    }
    for (const char* t : {"/misterplex/mast?x=1", "/misterplex/mast?", "/misterplex/mast#f",
                          "/misterplex/mast?X-Plex-Token=abc"}) {
        const auto r = statusRequest(std::string("GET ") + t + " HTTP/1.1\r\n\r\n", mk.c_str());
        expect(r && r->code == 400 && r->body == "no parameters\n",
               std::string("params ") + t + ": 400 no parameters");
    }
    // Other paths are not ours: the companion handles them as before.
    for (const char* q : {"GET /misterplex/mastx HTTP/1.1", "GET /misterplex/mast/ HTTP/1.1",
                          "GET /misterplex HTTP/1.1", "GET /resources HTTP/1.1",
                          "GET http://h/misterplex/mast HTTP/1.1", "GET", "", "\r\n"}) {
        expect(!statusRequest(std::string(q) + "\r\n\r\n", mk.c_str()),
               std::string("not ours: [") + q + "]");
    }
    // HTTP/1.0 and a bare request line are served.
    for (const char* q : {"GET /misterplex/mast HTTP/1.0", "GET /misterplex/mast"}) {
        const auto r = statusRequest(std::string(q) + "\r\n\r\n", mk.c_str());
        expect(r && r->code == 200 && has(r->body, "cure_failed=1\n"), std::string("served: ") + q);
    }

    ::unlink(mk.c_str());
    ::rmdir(dir);
    if (fails) {
        std::fprintf(stderr, "test_mast_status_http: %d of %d checks FAILED\n", fails, rows);
        return 1;
    }
    std::printf("test_mast_status_http: %d checks OK (present, absent, garbage, size cap, symlink, "
                "fifo, verbs 405, params 400, other paths untouched)\n",
                rows);
    return 0;
}
