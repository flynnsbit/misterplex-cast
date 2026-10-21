#pragma once

#include "plexui_data.hpp"

#include <functional>
#include <string>

namespace misterplex {
namespace plexui {

constexpr const char* kPlexUiCaBundleEnv = "MPX_CA_BUNDLE";

// Readable CA bundle path from MPX_CA_BUNDLE, or empty (plexui uses curl -k).
std::string plexUiCaBundlePath();

std::string httpUrlHost(const std::string& url);

HttpGetFn makePlexuiHttpGet();
HttpGetFn makePlexuiImageHttpGet();

using ProbeLogFn = std::function<void(const std::string&)>;
void setPlexuiProbeLog(ProbeLogFn log);
void plexuiProbeLogLine(const std::string& line);

} // namespace plexui
} // namespace misterplex
