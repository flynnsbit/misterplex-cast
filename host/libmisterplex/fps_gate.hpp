#pragma once

#include <cmath>
#include <cstring>

namespace misterplex {

enum class FpsGateMode { None, Mode240p, Mode480p, Mode480i };

struct FpsGateRow {
    FpsGateMode mode;
    const char* name;
};

// Shared fps gate mode table: 480p / 480i / 240p, one implementation.
inline const FpsGateRow kFpsGateModes[] = {
    {FpsGateMode::Mode240p, "240p"},
    {FpsGateMode::Mode480p, "480p"},
    {FpsGateMode::Mode480i, "480i"},
};

inline bool is480iEnv(const char* fpgaMode, const char* present480i) {
    if (fpgaMode && std::strcmp(fpgaMode, "480i") == 0)
        return true;
    if (!present480i || !present480i[0])
        return false;
    return present480i[0] == '1' || present480i[0] == 'y' || present480i[0] == 'Y' ||
           std::strcmp(present480i, "on") == 0 || std::strcmp(present480i, "ON") == 0 ||
           std::strcmp(present480i, "yes") == 0 || std::strcmp(present480i, "YES") == 0;
}

inline FpsGateMode fpsGateMode(int w, int h, const char* fpgaMode, const char* present480i) {
    if (is480iEnv(fpgaMode, present480i))
        return FpsGateMode::Mode480i;
    if (h == 240 && w > 0 && w < 640)
        return FpsGateMode::Mode240p;
    if (h == 480)
        return FpsGateMode::Mode480p;
    return FpsGateMode::None;
}

inline bool is240p(int w, int h) {
    return fpsGateMode(w, h, nullptr, nullptr) == FpsGateMode::Mode240p;
}
inline bool is480p(int w, int h, const char* fpgaMode, const char* present480i) {
    return fpsGateMode(w, h, fpgaMode, present480i) == FpsGateMode::Mode480p;
}
inline bool is480i(int w, int h, const char* fpgaMode, const char* present480i) {
    return fpsGateMode(w, h, fpgaMode, present480i) == FpsGateMode::Mode480i;
}

// Snap to the 24-family or 30-family when fpsGate is set.
// Keep 24/1, 24000/1001, 30/1, 30000/1001 (do not bucket 23.976 to 24/1).
// Halve 60/1 -> 30/1 and 60000/1001 -> 30000/1001.
// Other rates take nearest of {24/1, 30/1}.
// Returns true only when num/den changed.
inline bool snapFpsToStandard(int& num, int& den, bool fpsGate) {
    if (!fpsGate || num <= 0 || den <= 0)
        return false;
    const int oldNum = num;
    const int oldDen = den;
    if ((num == 24 && den == 1) || (num == 24000 && den == 1001) ||
        (num == 30 && den == 1) || (num == 30000 && den == 1001))
        return false;
    if (num == 60 && den == 1) {
        num = 30;
        den = 1;
        return true;
    }
    if (num == 60000 && den == 1001) {
        num = 30000;
        den = 1001;
        return true;
    }
    const double exact = static_cast<double>(num) / static_cast<double>(den);
    const double ntsc24 = 24000.0 / 1001.0;
    const double ntsc30 = 30000.0 / 1001.0;
    if (std::fabs(exact - ntsc24) <= 0.012) {
        num = 24000;
        den = 1001;
        return num != oldNum || den != oldDen;
    }
    if (std::fabs(exact - ntsc30) <= 0.012) {
        num = 30000;
        den = 1001;
        return num != oldNum || den != oldDen;
    }
    const double diff24 = std::fabs(exact - 24.0);
    const double diff30 = std::fabs(exact - 30.0);
    if (diff24 <= diff30) {
        num = 24;
        den = 1;
    } else {
        num = 30;
        den = 1;
    }
    return num != oldNum || den != oldDen;
}

} // namespace misterplex
