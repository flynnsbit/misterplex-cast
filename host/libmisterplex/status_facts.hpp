#pragma once
// Phone status page (frontier item 3): the REAL facts behind GET /misterplex/status. Until this header the daemon's
// provider returned placeholders (md5 "unknown", title "-", helper "unknown"). Pure + injectable so the host test can
// prove the page carries real values; the daemon wires the sources (exe path, title getter, helper probes).
// Nothing here reads a token or a PMS URL; the page's own field() still caps, redacts and escapes every value.
#include "libmisterplex/phone_status_page.hpp"
#include "libmisterplex/present_bank.hpp"

#include <functional>
#include <string>

namespace misterplex {
namespace phonestatus {

// "up" / "down" / "off". A configured remote helper wins (up or down by its probe); with none configured the
// on-card loopback helper is "up" when it answers, else "off" (standalone has no helper, which is not a fault).
inline const char* helperText(bool remoteConfigured, bool remoteOk, bool localOk) {
#ifdef MUT_SF_HELPER_UNKNOWN
    (void)remoteConfigured; (void)remoteOk; (void)localOk;
    return "unknown";
#endif
    if (remoteConfigured)
        return remoteOk ? "up" : "down";
    return localOk ? "up" : "off";
}

// 32 lowercase hex of the running binary, or "unknown" when it cannot be read (never an empty cell).
inline std::string md5OrUnknown(const char* path) {
#ifdef MUT_SF_MD5_PLACEHOLDER
    (void)path;
    return "unknown";
#endif
    const std::string h = md5HexOfFile(path);
    return h.size() == 32 ? h : std::string("unknown");
}

struct Facts {
    std::string version = "dev", commit = "unknown";
    std::string exePath = "/proc/self/exe";
    std::function<std::string()> title;          // current playback title ("" when idle)
    std::function<const char*()> helper;        // already helperText()
};

// The provider body. The md5 of the running daemon is computed once per call site (the daemon caches it).
inline Status build(const Facts& f, const std::string& cachedMd5) {
    Status st;
    st.version = f.version;
    st.commit = f.commit;
    st.md5 = cachedMd5.empty() ? md5OrUnknown(f.exePath.c_str()) : cachedMd5;
    const std::string t = f.title ? f.title() : std::string();
#ifdef MUT_SF_TITLE_PLACEHOLDER
    st.title = "-";
#else
    st.title = t.empty() ? "-" : t;
#endif
    st.helper = f.helper ? f.helper() : "unknown";
    return st;
}

}  // namespace phonestatus
}  // namespace misterplex
