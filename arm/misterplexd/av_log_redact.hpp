#pragma once
// libav's default log prints the open URL. A transcode start URL carries
// X-Plex-Token. Every av_log line is redacted before it reaches stderr.

#include "log_redact.hpp"

#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" {
#include <libavutil/log.h>
}

namespace misterplex {

inline void redactingAvLog(void*, int level, const char* fmt, va_list ap) {
    if (level > av_log_get_level())
        return;
    char buf[4096];
    va_list aq;
    va_copy(aq, ap);
    const int n = std::vsnprintf(buf, sizeof buf, fmt ? fmt : "", aq);
    va_end(aq);
    std::string line;
    if (n > 0)
        line.assign(buf, buf + (n < static_cast<int>(sizeof buf) ? n : static_cast<int>(sizeof buf) - 1));
    line = redactUrlTokens(std::move(line));
    std::fputs(line.c_str(), stderr);
    if (line.empty() || line.back() != '\n')
        std::fputc('\n', stderr);
}

inline void installRedactingAvLog() {
    av_log_set_callback(redactingAvLog);
}

} // namespace misterplex
