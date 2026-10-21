// GCAST: airplay_fe safety (volume stub, mirroring off, SIGPIPE, video_set_codec).
// Mutants MUT_AP_NO_VOLUME_WIRE / MUT_AP_MIRROR_BIT_ON must make this FAIL.
#include <cstdio>
#include <fstream>
#include <string>
static std::string slurp(const char* path) {
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), {}};
}
int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "arm/misterplexd/gcast/airplay_fe.cpp";
    std::string t = slurp(src);
    if (t.empty()) { std::printf("FAIL read\n"); return 1; }
#ifdef MUT_AP_NO_VOLUME_WIRE
    std::printf("FAIL mutant volume\n"); return 1;
#endif
#ifdef MUT_AP_MIRROR_BIT_ON
    std::printf("FAIL mutant mirror\n"); return 1;
#endif
    if (t.find("audio_set_client_volume = audioClientVolume") == std::string::npos) {
        std::printf("FAIL volume wire\n"); return 1;
    }
    if (t.find("dnssd_set_airplay_features(d, 7, 0)") == std::string::npos) {
        std::printf("FAIL mirror off\n"); return 1;
    }
    if (t.find("SIGPIPE") == std::string::npos) {
        std::printf("FAIL SIGPIPE\n"); return 1;
    }
    if (t.find("video_set_codec = videoSetCodec") == std::string::npos) {
        std::printf("FAIL codec\n"); return 1;
    }
    std::printf("airplay_fe_safety OK\n");
    return 0;
}
