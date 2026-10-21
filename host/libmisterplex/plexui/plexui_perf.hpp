#pragma once

#include <chrono>
#include <cstdint>

namespace misterplex {
namespace plexui {

inline int64_t perfSteadyMs() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

} // namespace plexui
} // namespace misterplex
