#pragma once
// GH#9: misterplexd --version must print and exit before ProcessSingleton,
// FPGA, or companion. A hung version probe on lab left FPGA color bars and
// no :3005 (forbidden companion 4fdf97e0).

#include <cstdio>
#include <cstring>

namespace misterplex {

inline const char* daemonGitCommit() {
#ifdef MPX_GIT_COMMIT
    return MPX_GIT_COMMIT[0] ? MPX_GIT_COMMIT : "unknown";
#else
    return "unknown";
#endif
}

inline bool isDaemonVersionArg(const char* a) {
#ifdef MUT_VERSION_FALL_THROUGH
    (void)a;
    return false;
#else
    return a && (std::strcmp(a, "--version") == 0 || std::strcmp(a, "-V") == 0);
#endif
}

// 0 = handled (print and exit 0). -1 = not a version invocation.
inline int daemonVersionExit(int argc, char** argv, std::FILE* out) {
    if (argc < 2 || !isDaemonVersionArg(argv[1]))
        return -1;
    if (out)
        std::fprintf(out, "misterplexd %s\n", daemonGitCommit());
    return 0;
}

} // namespace misterplex
