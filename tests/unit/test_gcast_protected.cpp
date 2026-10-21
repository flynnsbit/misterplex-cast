// FIELD-1018 F4: protected (FairPlay / Apple TV+) AirPlay sources are refused up front with an on-screen message.
#include "gcast/protected_src.hpp"
#include <cstdio>
using namespace misterplex::gcast;
static int bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); bad++; } } while (0)
int main() {
    // the exact a lab MiSTer URL (mpxd-apaudio-1043 / mpxd-ytap-1045)
    const std::string tvplus = "https://play-edge.itunes.apple.com/WebObjects/MZPlayLocal.woa/hls/subscription/playlist.m3u8"
                               "?cc=US&svcId=tvs.vds.4105&a=6805996567&isExternal=true";
    CHECK(protectedUrl(tvplus));
    CHECK(castSourceVerdict(tvplus, false, false) == CastSourceVerdict::Protected);
    CHECK(protectedUrl("skd://itunes.apple.com/P000000000/s1/e1"));
    // ordinary sources stay playable
    for (const char* u : {"http://127.0.0.1:32400/video.mp4", "https://devstreaming-cdn.apple.com/videos/bipbop/master.m3u8",
                          "mlhls://localhost/master.m3u8", "http://192.0.2.110:7002/raop.wav", "yt:QVv3mQzlrN0"})
        CHECK(!protectedUrl(u) && castSourceVerdict(u, true, true) == CastSourceVerdict::Play);
    // playlists
    CHECK(protectedPlaylist("#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"));
    CHECK(protectedPlaylist("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"data:abc\"\n#EXTINF:6,\na.ts\n"));
    CHECK(protectedPlaylist("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES-CTR,URI=\"https://k\"\n"));
    CHECK(!protectedPlaylist("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"https://k/key\"\n#EXTINF:6,\na.ts\n"));
    CHECK(!protectedPlaylist("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n# SAMPLE-AES in a comment only\n"));
    CHECK(castSourceVerdict("http://h/x.m3u8", false, false, "#EXT-X-KEY:METHOD=SAMPLE-AES\n") == CastSourceVerdict::Protected);
    // probe failure on HLS = fail fast (not a 30 s open timeout); non-HLS probe failure still tries to play
    CHECK(castSourceVerdict("https://cdn.example/live/index.m3u8", true, false) == CastSourceVerdict::Unplayable);
    CHECK(castSourceVerdict("http://h/movie.mp4", true, false) == CastSourceVerdict::Play);
    CHECK(std::string(kProtectedMsg) == "Protected content - can't play");
    std::printf("%s (%d failures)\n", bad ? "FAIL" : "protected_src OK", bad);
    return bad ? 1 : 0;
}
