// The MPX_H264_FILTER start-up line (review 17:17 (4)): its exact format, and that the value/source it prints
// is what the daemon's own readers see (main.cpp conf loop -> media_player getenv-or-"on" -> plex_resolve on|off).
#include "libmisterplex/h264_filter_setting.hpp"

#include <cstdio>
#include <cstdlib>
#include <regex>
#include <string>

using misterplex::effectiveH264Filter;
using misterplex::h264FilterLogLine;

namespace {
int fails = 0;
void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL h264_filter_line: %s\n", what.c_str());
        ++fails;
    }
}

struct Case {
    const char* conf;   // misterplex.conf value ("" = absent)
    const char* shell;  // shell env before the conf loop (nullptr = unset)
    const char* line;   // the exact start-up line
};
} // namespace

int main() {
    const Case cases[] = {
        {"", nullptr, "misterplexd: h264_filter=on src=default"},
        {"", "off", "misterplexd: h264_filter=off src=env"},
        {"", "on", "misterplexd: h264_filter=on src=env"},
        {"on", "off", "misterplexd: h264_filter=on src=conf"},
        {"off", nullptr, "misterplexd: h264_filter=off src=conf"},
        {"", "", "misterplexd: h264_filter= src=env INVALID (want on|off)"},
        {"maybe", nullptr, "misterplexd: h264_filter=maybe src=conf INVALID (want on|off)"},
        {"", "ON", "misterplexd: h264_filter=ON src=env INVALID (want on|off)"},
    };
    const std::regex shape(R"(^misterplexd: h264_filter=[^ ]* src=(conf|env|default)( INVALID \(want on\|off\))?$)");
    for (const auto& c : cases) {
        const auto s = effectiveH264Filter(c.conf, c.shell);
        const std::string line = h264FilterLogLine(s);
        const std::string tag = std::string("conf='") + c.conf + "' shell=" + (c.shell ? "'" + std::string(c.shell) + "'" : "unset");
        expect(line == c.line, tag + ": got [" + line + "] want [" + c.line + "]");
        expect(std::regex_match(line, shape), tag + ": [" + line + "] breaks the line format");
        // The daemon's own path: shell env, conf loop (non-empty conf wins), then the readers.
        if (c.shell)
            ::setenv("MPX_H264_FILTER", c.shell, 1);
        else
            ::unsetenv("MPX_H264_FILTER");
        if (*c.conf)
            ::setenv("MPX_H264_FILTER", c.conf, 1);
        const char* env = std::getenv("MPX_H264_FILTER");
        const std::string reader = env ? env : "on";                                       // media_player.cpp
        const bool resolveOk = !env || std::string(env) == "on" || std::string(env) == "off"; // plex_resolve.cpp
        expect(s.value == reader, tag + ": prints '" + s.value + "' but media_player reads '" + reader + "'");
        expect(s.valid == resolveOk, tag + ": validity differs from plex_resolve");
    }
    ::unsetenv("MPX_H264_FILTER");
    if (fails) {
        std::fprintf(stderr, "test_h264_filter_line: %d failure(s)\n", fails);
        return 1;
    }
    std::printf("test_h264_filter_line: OK (%zu cases)\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
