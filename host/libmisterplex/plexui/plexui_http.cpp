#include "plexui_http.hpp"

#include "libmisterplex/browse_fetch.hpp"

#include <unistd.h>

namespace misterplex {
namespace plexui {

namespace {

ProbeLogFn gProbeLog;

} // namespace

void setPlexuiProbeLog(ProbeLogFn log) { gProbeLog = std::move(log); }

void plexuiProbeLogLine(const std::string& line) {
    if (gProbeLog)
        gProbeLog(line);
}

std::string plexUiCaBundlePath() {
    const char* raw = std::getenv(kPlexUiCaBundleEnv);
    if (!raw || !*raw)
        return {};
    if (::access(raw, R_OK) != 0)
        return {};
    return raw;
}

std::string httpUrlHost(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return {};
    size_t i = scheme + 3;
    size_t end = url.find('/', i);
    if (end == std::string::npos)
        end = url.size();
    std::string hostport = url.substr(i, end - i);
    const size_t at = hostport.rfind('@');
    if (at != std::string::npos)
        hostport = hostport.substr(at + 1);
    const size_t colon = hostport.find(':');
    if (colon == std::string::npos)
        return hostport;
    return hostport.substr(0, colon);
}

HttpGetFn makePlexuiHttpGet() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, int connectMs, int timeoutMs,
                const BrowseCancelFn& cancel) {
        return browseHttpGet(url, connectMs, timeoutMs, cancel, "curl", "application/xml",
                             ca.empty() ? nullptr : ca.c_str());
    };
}

HttpGetFn makePlexuiImageHttpGet() {
    const std::string ca = plexUiCaBundlePath();
    return [ca](const std::string& url, int connectMs, int timeoutMs,
                const BrowseCancelFn& cancel) {
        return browseHttpGet(url, connectMs, timeoutMs, cancel, "curl", "*/*",
                             ca.empty() ? nullptr : ca.c_str());
    };
}

} // namespace plexui
} // namespace misterplex
