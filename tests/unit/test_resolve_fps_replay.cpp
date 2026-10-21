// DAEMON-FPS-SPEC-1002 T2: replay resolvePlayTarget against a fake PMS
// (tests/fixtures/fake_pms/fake_pms.py) and print the ResolveResult as JSON on stdout.
// The resolver's own log lines stay on stderr. Uses only APIs present at f6526d96 so
// the same file builds against the base and the patched plex_resolve.cpp.
//
//   test_resolve_fps_replay --base URL --key KEY [--token TOK] [--offset-ms N]
//       [--weak 240p|480p|720p|WxH] [--br KBPS] [--q N] [--decode WxH]
//       [--prefer-direct] [--no-weak-always] [--bank WxH] [--caps-build-id ID]
//
// --caps-build-id applies the player's build-ID gate (fpga_build_ids.hpp) to a
// CAPS build ID after the resolve, as fpgaThreadMain does after CAPS negotiation.
// A recognised build changes nothing. An unrecognised one prints the player's
// "ERROR FPGA H.264:" refusal on stderr and exits 3. Trees without the header
// (f6526d96) reject the option.
// --bank WxH adds the daemon's present-bank choice for that content size on true480
// glass (glassMax + presentBankDoorbell, as main.cpp doPlay does), so a conf check
// can tell the FPGA store (640x480 @ 0x300FF000) from the ARM-present R15 bank.
// The token is printed as REDACTED wherever it appears in the result.
#include "plex_resolve.hpp"
#include "libmisterplex/present_bank.hpp"
#if __has_include("libmisterplex/fpga_build_ids.hpp")
#include "libmisterplex/fpga_build_ids.hpp"
#define REPLAY_HAVE_BUILD_GATE 1
#endif

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string redact(std::string s, const std::string& token) {
    if (token.empty())
        return s;
    for (const std::string& needle : {token, misterplex::urlEncodeQuery(token)}) {
        size_t p = 0;
        while ((p = s.find(needle, p)) != std::string::npos) {
            s.replace(p, needle.size(), "REDACTED");
            p += 8;
        }
    }
    return s;
}

std::string json(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    return o + "\"";
}

int usage() {
    std::fprintf(stderr,
                 "usage: test_resolve_fps_replay --base URL --key KEY [--token TOK] "
                 "[--offset-ms N] [--weak NAME|WxH] [--br KBPS] [--q N] [--decode WxH] "
                 "[--prefer-direct] [--no-weak-always] [--bank WxH] [--caps-build-id ID]\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string base, key, token, weakName;
    long long offsetMs = 0;
    int br = -1, q = -1, decodeW = 0, decodeH = 0, bankW = 0, bankH = 0;
    bool preferDirect = false, weakAlways = true;
    [[maybe_unused]] bool haveBuildId = false;
    [[maybe_unused]] uint32_t buildId = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](std::string& out) {
            if (i + 1 >= argc)
                return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--base" && value(v)) {
            base = v;
        } else if (a == "--key" && value(v)) {
            key = v;
        } else if (a == "--token" && value(v)) {
            token = v;
        } else if (a == "--offset-ms" && value(v)) {
            offsetMs = std::strtoll(v.c_str(), nullptr, 10);
        } else if (a == "--weak" && value(v)) {
            weakName = v;
        } else if (a == "--br" && value(v)) {
            br = std::atoi(v.c_str());
        } else if (a == "--q" && value(v)) {
            q = std::atoi(v.c_str());
        } else if (a == "--decode" && value(v)) {
            if (std::sscanf(v.c_str(), "%dx%d", &decodeW, &decodeH) != 2)
                return usage();
        } else if (a == "--bank" && value(v)) {
            if (std::sscanf(v.c_str(), "%dx%d", &bankW, &bankH) != 2)
                return usage();
        } else if (a == "--prefer-direct") {
            preferDirect = true;
        } else if (a == "--no-weak-always") {
            weakAlways = false;
        } else if (a == "--caps-build-id" && value(v)) {
#ifdef REPLAY_HAVE_BUILD_GATE
            buildId = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 0));
            haveBuildId = true;
#else
            std::fprintf(stderr,
                         "test_resolve_fps_replay: --caps-build-id needs fpga_build_ids.hpp\n");
            return 2;
#endif
        } else {
            return usage();
        }
    }
    if (key.empty())
        return usage();

    misterplex::WeakLadder weak;
    if (!weakName.empty() && !misterplex::applyPlexTranscodeProfile(weakName, weak)) {
        std::fprintf(stderr, "test_resolve_fps_replay: unknown --weak %s\n", weakName.c_str());
        return 2;
    }
    if (br >= 0)
        weak.maxVideoBitrateKbps = br;
    if (q >= 0)
        weak.videoQuality = q;

    const misterplex::ResolveResult r = misterplex::resolvePlayTarget(
        key, base, token, offsetMs, weakAlways, weak, preferDirect, decodeW, decodeH);
    std::fflush(stderr);

    std::printf("{\n");
    std::printf("  \"ok\": %s,\n", r.ok ? "true" : "false");
    std::printf("  \"transcoded\": %s,\n", r.transcoded ? "true" : "false");
    std::printf("  \"playable\": %s,\n", json(redact(r.playable, token)).c_str());
    std::printf("  \"httpHeaders\": %s,\n", json(redact(r.httpHeaders, token)).c_str());
    std::printf("  \"title\": %s,\n", json(r.title).c_str());
    std::printf("  \"detail\": %s,\n", json(redact(r.detail, token)).c_str());
    std::printf("  \"durationMs\": %lld,\n", static_cast<long long>(r.durationMs));
    std::printf("  \"viewOffsetMs\": %lld,\n", static_cast<long long>(r.viewOffsetMs));
    std::printf("  \"ratingKey\": %s,\n", json(r.ratingKey).c_str());
    std::printf("  \"videoFrameRate\": %s,\n", json(r.videoFrameRate).c_str());
    std::printf("  \"frameRate\": %s,\n", json(r.frameRate).c_str());
    std::printf("  \"sourceFpsHint\": %d,\n", r.sourceFpsHint);
    std::printf("  \"fpsNum\": %d,\n", r.fpsNum);
    std::printf("  \"fpsDen\": %d,\n", r.fpsDen);
    std::printf("  \"mediaWidth\": %d,\n", r.mediaWidth);
    std::printf("  \"mediaHeight\": %d,\n", r.mediaHeight);
    std::printf("  \"hasAudio\": %s,\n", r.hasAudio ? "true" : "false");
    if (bankW > 0 && bankH > 0) {
        const auto bank =
            misterplex::glassMax(bankW, bankH, misterplex::LiveGlass::True480);
        std::printf("  \"bank\": {\"w\": %d, \"h\": %d, \"label\": %s, \"doorbell\": "
                    "\"0x%08X\", \"present480i\": %s},\n",
                    bank.width, bank.height, json(bank.label).c_str(),
                    static_cast<unsigned>(misterplex::presentBankDoorbell(bank.width,
                                                                          bank.height)),
                    misterplex::envFlagWanted("MPX_PRESENT_480I") ? "true" : "false");
    }
    std::printf("  \"sourceAspect\": {\"x\": %u, \"y\": %u, \"valid\": %s}\n",
                static_cast<unsigned>(r.sourceAspect.x), static_cast<unsigned>(r.sourceAspect.y),
                r.sourceAspect.valid ? "true" : "false");
    std::printf("}\n");
#ifdef REPLAY_HAVE_BUILD_GATE
    if (haveBuildId) {
        const std::string refusal = misterplex::fpgaBuildRefusal(buildId);
        if (!refusal.empty()) {
            std::fflush(stdout);
            std::fprintf(stderr, "ERROR FPGA H.264: %s\n", refusal.c_str());
            return 3;
        }
    }
#endif
    return 0;
}
