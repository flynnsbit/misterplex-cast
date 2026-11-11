#pragma once

// #228: parse an HTTP integer header value such as Retry-After seconds. Only digits are
// accepted, with surrounding spaces allowed. The value is read with strtol and clamped to
// INT_MAX. Anything else (an HTTP-date, a sign, junk such as "12abc", or empty) gives -1, so
// callers fall back to their own backoff.
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>

namespace misterplex {
inline int parseHeaderNonNegInt(const char* s) {
    if (!s)
        return -1;
    while (*s == ' ' || *s == '\t')
        ++s;
    if (!std::isdigit(static_cast<unsigned char>(*s)))
        return -1;
#ifdef MUT_HDRINT_ATOI
    return std::atoi(s);
#else
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    while (end && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        ++end;
    if (!end || *end != '\0')
        return -1;
    if (errno == ERANGE || v > INT_MAX)
        return INT_MAX;
    return static_cast<int>(v);
#endif
}
} // namespace misterplex
