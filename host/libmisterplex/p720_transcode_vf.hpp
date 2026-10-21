#pragma once
// 720p24 product: PMS universal already emits videoResolution=1280x720.
// Crop+scale of that stream on dual-A9 is a 14 unique lock (Star Trek 40868).
// 480p still crops/scales. Local identity files use a different skip.

#include <cstdlib>
#include <cstring>
#include <string>

namespace misterplex {

inline bool isPlex720pBankSize(int w, int h) { return w == 1280 && h == 720; }

inline bool isUniversalTranscodeUrl(const std::string& url) {
    return url.find("transcode/universal") != std::string::npos ||
           url.find("/video/:/transcode/") != std::string::npos;
}

// Skip FFmpeg crop/scale only when coded size is measured 1280×720.
// URL videoResolution=1280x720 is NOT enough: live 40868 PMS often delivers
// 640×480 anyway and skip-vf packs it into the 720p bank (green bands).
inline bool skipRedundant720pTranscodeVf(const std::string& url, int bankW, int bankH,
                                         int codedW = 0, int codedH = 0) {
    (void)url;
    if (!isPlex720pBankSize(bankW, bankH))
        return false;
    return codedW == 1280 && codedH == 720;
}

// Skip-vf coded size is the probed start.mp4 WxH, never library Media@width.
// 40868 source is 1440×1080 HEVC; passing that as coded kept crop+scale and
// locked unique ~14. Probe fail → 0,0 → no skip (640×480 pack / green bands).
inline int skipVfProbedCodedDim(int probed, int librarySource) {
    (void)librarySource;
    return probed > 0 ? probed : 0;
}

// 4:3 Trek at 720-tall (960×720) is already 24p scanlines. Pad into the 1280×720
// bank; do not scale 640×480 up (that is the 14 unique lock).
inline bool transcodePadOnly720p(int codedW, int codedH, int bankW, int bankH) {
    return bankW == 1280 && bankH == 720 && codedH == 720 && codedW >= 640 &&
           codedW < 1280;
}

inline bool metadataKeyIsFarpoint40868(const std::string& key) {
    return key.find("/library/metadata/40868") != std::string::npos ||
           key == "40868" || key == "/library/metadata/40868";
}

// Library metadata (rk 40868) must spawn PMS universal 1280×720, never the
// identity cache file. Tests drive this shipped policy.
inline bool libraryMetadataMustUsePmsUniversal(const std::string& key) {
    return key.find("/library/metadata/") != std::string::npos ||
           key.find("library/metadata/") != std::string::npos ||
           metadataKeyIsFarpoint40868(key);
}

inline bool playableIsPmsUniversal720p(const std::string& playable) {
    return isUniversalTranscodeUrl(playable) &&
           playable.find("videoResolution=1280x720") != std::string::npos;
}

inline bool playableIsFarpointIdentityCache(const std::string& playable) {
    return playable.find("farpoint_1280x720.mp4") != std::string::npos;
}

// Library rk 40868 must spawn PMS universal, never a local cache/file playable.
inline bool libraryKeyMustNotSpawnLocalFile(const std::string& key,
                                            const std::string& playable) {
    if (!libraryMetadataMustUsePmsUniversal(key))
        return false;
    if (playableIsFarpointIdentityCache(playable))
        return true;
    return playable.rfind("/media/fat/", 0) == 0;
}

// Live PMS universal (40868 ~91 min) never exits. Prefetch+waitPid 20s
// killed the x86 transcode (wait_ok=0) and ffmpeg short-read the truncated
// TS saved as .mp4 (frames=0). 480p streams the HTTP URL; 720p matches.
inline bool liveUniversalMustStreamHttp(const std::string& url) {
    return isUniversalTranscodeUrl(url);
}

// Prefetch-to-EOF is only for a finite identity HTTP body. Live universal
// must stream. farpoint_1280x720.mp4 cache is never a prefetch source.
inline bool inprocPrefetchHttpIdentity720p(const std::string& url, int bankW, int bankH,
                                           int codedW, int codedH) {
    (void)codedW;
    (void)codedH;
    if (playableIsFarpointIdentityCache(url))
        return false;
    if (!isPlex720pBankSize(bankW, bankH))
        return false;
    if (liveUniversalMustStreamHttp(url))
        return false;
    return false;
}

// Parse `ffmpeg -i` / ffprobe stderr for a local 40868 cache. RED if 640x480
// is accepted as a 720p identity file.
inline bool parseFfmpegIdentify(const char* text, int& w, int& h, int& fpsNum, int& fpsDen,
                                int& darX, int& darY) {
    w = h = fpsNum = fpsDen = darX = darY = 0;
    if (!text || !*text)
        return false;
    const char* video = std::strstr(text, "Video:");
    if (!video)
        video = std::strstr(text, "video:");
    if (!video)
        return false;
    const char* dim = video;
    while (*dim) {
        if (dim[0] >= '1' && dim[0] <= '9') {
            char* end = nullptr;
            const long ww = std::strtol(dim, &end, 10);
            if (end && *end == 'x' && end[1] >= '1' && end[1] <= '9') {
                char* endh = nullptr;
                const long hh = std::strtol(end + 1, &endh, 10);
                if (ww >= 16 && hh >= 16 && ww <= 7680 && hh <= 4320) {
                    w = static_cast<int>(ww);
                    h = static_cast<int>(hh);
                    dim = endh;
                    break;
                }
            }
        }
        ++dim;
    }
    if (w <= 0 || h <= 0)
        return false;
    const char* dar = std::strstr(video, "DAR ");
    if (dar) {
        dar += 4;
        char* e1 = nullptr;
        const long ax = std::strtol(dar, &e1, 10);
        if (e1 && *e1 == ':' && e1[1] >= '1') {
            const long ay = std::strtol(e1 + 1, nullptr, 10);
            if (ax > 0 && ay > 0) {
                darX = static_cast<int>(ax);
                darY = static_cast<int>(ay);
            }
        }
    }
    if (darX <= 0 || darY <= 0) {
        if (w * 9 == h * 16) {
            darX = 16;
            darY = 9;
        } else if (w * 3 == h * 4) {
            darX = 4;
            darY = 3;
        } else {
            darX = w;
            darY = h;
        }
    }
    const char* fps = std::strstr(video, " fps");
    if (fps && fps > video) {
        const char* p = fps;
        while (p > video && (p[-1] == '.' || (p[-1] >= '0' && p[-1] <= '9')))
            --p;
        const double v = std::atof(p);
        if (v > 23.5 && v < 24.2) {
            fpsNum = 24000;
            fpsDen = 1001;
        } else if (v > 23.9 && v < 24.1) {
            fpsNum = 24;
            fpsDen = 1;
        } else if (v > 29.9 && v < 30.1) {
            fpsNum = 30000;
            fpsDen = 1001;
        } else if (v >= 1.0) {
            fpsNum = static_cast<int>(v + 0.5);
            fpsDen = 1;
        }
    }
    if (fpsNum <= 0) {
        fpsNum = 24000;
        fpsDen = 1001;
    }
    return true;
}

inline bool localFileIsTrue720p24(int w, int h) { return w == 1280 && h == 720; }

} // namespace misterplex
