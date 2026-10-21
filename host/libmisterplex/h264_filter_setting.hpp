#pragma once
// The effective MPX_H264_FILTER and where it came from, for ONE daemon start-up line (review 17:17 (4)).
// Precedence matches main.cpp's conf loop (misterplex.conf wins over the shell env with setenv) and the readers:
// media_player.cpp uses getenv("MPX_H264_FILTER") or "on"; plex_resolve.cpp refuses an FPGA play for any value
// that is not exactly "on" or "off".
#include <string>

namespace misterplex {

struct H264FilterSetting {
    std::string value;   // what the readers see
    std::string source;  // "conf", "env" or "default"
    bool valid = false;  // "on" or "off"; anything else makes FPGA plays refuse
};

// confValue: misterplex.conf's MPX_H264_FILTER ("" when absent; the conf loop ignores an empty value).
// shellValue: the process env before the conf loop (nullptr when unset; "" stays an explicit, invalid value).
inline H264FilterSetting effectiveH264Filter(const std::string& confValue, const char* shellValue) {
    H264FilterSetting s;
    if (!confValue.empty()) {
        s.value = confValue;
        s.source = "conf";
    } else if (shellValue) {
        s.value = shellValue;
        s.source = "env";
    } else {
        s.value = "on";
        s.source = "default";
    }
    s.valid = s.value == "on" || s.value == "off";
    return s;
}

// "misterplexd: h264_filter=<value> src=<conf|env|default>", plus " INVALID (want on|off)" when FPGA plays refuse.
inline std::string h264FilterLogLine(const H264FilterSetting& s) {
    std::string line = "misterplexd: h264_filter=" + s.value + " src=" + s.source;
    if (!s.valid)
        line += " INVALID (want on|off)";
    return line;
}

} // namespace misterplex
