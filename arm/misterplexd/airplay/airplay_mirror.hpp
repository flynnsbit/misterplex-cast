#pragma once
#include "airplay/ns.hpp"
// AirPlay screen-mirroring (SETUP type 110): host-testable framing, decrypt, Annex-B.
//
// Framing and AES-CTR keystream derivation are ported from UxPlay
// lib/raop_rtp_mirror.c and lib/mirror_buffer.c (FDH2/UxPlay 4e95a9e;
// original dsafa22 / fduncanh). LGPL-2.1-or-later; see packaging/licenses/uxplay/.
//
// Production: UxPlay's raop_rtp_mirror already opens the TCP listener and
// decrypts when video_set_codec returns 0. This module is the testable
// algorithm + the decoder-feed seam. Default off (AIRPLAY_MIRROR unset):
// the front end still answers 453 / streams:[].
//
// PTS: header bytes 8-15 are UxPlay video NTP (nanoseconds since boot, no
// 1900 epoch). Converted to 90 kHz when nonzero; otherwise kNoPts90k
// (arrival order / best effort). Glass pacing is unverified here.

#include "libmisterplex/h264_bitstream_transport.hpp"

#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace misterplex {
namespace airplay {

inline constexpr uint32_t kAirplayMirrorHeaderSize = 128;
inline constexpr uint32_t kAirplayMirrorMaxPayload = 2u * 1024u * 1024u;
inline constexpr int kAirplayMirrorType = 110;
inline constexpr int kAirplayMirrorSetupOk = 200;
inline constexpr int kAirplayMirrorSetupRefuse = 453;

enum class MirrorPayloadType : uint8_t {
    EncryptedAu = 0x00,   // H.264 AU, AES-CTR
    SpsPps = 0x01,        // unencrypted avcC SPS/PPS
    Heartbeat = 0x02,     // old-protocol keepalive
    StreamReport = 0x05,  // client FPS plist (ignored)
    Unknown = 0xff,
};

enum class MirrorSessionState : uint8_t {
    Idle = 0,
    Listening = 1,
    Mirroring = 2,
    Stopped = 3,
};

inline bool airplayMirrorFlagOn(const char* v) {
    if (!v || !*v)
        return false;
#ifdef MUT_AP_MIRROR_DEFAULT_ON
    return true;
#else
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || std::strcmp(v, "on") == 0 ||
           std::strcmp(v, "ON") == 0 || std::strcmp(v, "yes") == 0 || std::strcmp(v, "true") == 0;
#endif
}

// Env wins when set. Conf is AIRPLAY_MIRROR= from misterplex.conf. Default off.
inline bool airplayMirrorWanted(const char* env, const char* conf) {
#ifdef MUT_AP_MIRROR_IGNORE_OPTIN
    (void)env;
    (void)conf;
    return true;
#else
    if (env && *env)
        return airplayMirrorFlagOn(env);
    return airplayMirrorFlagOn(conf);
#endif
}

struct MirrorSetupReply {
    int status = kAirplayMirrorSetupRefuse;
    uint16_t dataPort = 0;
    int type = 0;
    bool emptyStreams = true;
};

inline MirrorSetupReply makeMirrorSetupReply(bool accept, uint16_t dataPort) {
    MirrorSetupReply r;
    if (!accept) {
        r.status = kAirplayMirrorSetupRefuse;
        r.emptyStreams = true;
        return r;
    }
    r.status = kAirplayMirrorSetupOk;
    r.dataPort = dataPort;
    r.type = kAirplayMirrorType;
    r.emptyStreams = false;
    return r;
}

// UxPlay / doubletake shape: {streams:[{dataPort:<port>,type:110}]} or {streams:[]}
inline std::string mirrorSetupReplyJson(const MirrorSetupReply& r) {
    if (r.emptyStreams)
        return "{\"streams\":[]}";
    char buf[80];
    std::snprintf(buf, sizeof buf, "{\"streams\":[{\"dataPort\":%u,\"type\":110}]}",
                  static_cast<unsigned>(r.dataPort));
    return buf;
}

inline uint32_t mirrorLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t mirrorLe64(const uint8_t* p) {
    return static_cast<uint64_t>(mirrorLe32(p)) | (static_cast<uint64_t>(mirrorLe32(p + 4)) << 32);
}

inline uint16_t mirrorBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline uint32_t mirrorBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct MirrorHeader {
    uint32_t payloadSize = 0;
    MirrorPayloadType type = MirrorPayloadType::Unknown;
    uint8_t option = 0;
    uint64_t ntpNs = 0;
    bool valid = false;
};

inline MirrorHeader parseMirrorHeader(const uint8_t* hdr, size_t len) {
    MirrorHeader h;
    if (!hdr || len < kAirplayMirrorHeaderSize)
        return h;
    h.payloadSize = mirrorLe32(hdr);
#ifdef MUT_AP_MIRROR_NO_CAP
    if (h.payloadSize == 0 && hdr[4] != 0x02 && hdr[4] != 0x05)
        return h;
#else
    if (h.payloadSize > kAirplayMirrorMaxPayload)
        return h;
#endif
    switch (hdr[4]) {
    case 0x00:
        h.type = MirrorPayloadType::EncryptedAu;
        break;
    case 0x01:
        h.type = MirrorPayloadType::SpsPps;
        break;
    case 0x02:
        h.type = MirrorPayloadType::Heartbeat;
        break;
    case 0x05:
        h.type = MirrorPayloadType::StreamReport;
        break;
    default:
        h.type = MirrorPayloadType::Unknown;
        break;
    }
    h.option = hdr[6];
    h.ntpNs = mirrorLe64(hdr + 8);
    h.valid = true;
    return h;
}

// UxPlay video NTP (ns) -> 90 kHz. 0 means "no stamp" (heartbeat / report).
inline int64_t mirrorNtpToPts90k(uint64_t ntpNs) {
    if (ntpNs == 0)
        return h264stream::kNoPts90k;
    const uint64_t sec = ntpNs / 1000000000ull;
    const uint64_t nsec = ntpNs % 1000000000ull;
    const uint64_t pts = sec * 90000ull + (nsec * 90ull) / 1000000000ull;
    if (pts > static_cast<uint64_t>(INT64_MAX))
        return h264stream::kNoPts90k;
    return static_cast<int64_t>(pts);
}

// --- compact SHA-512 and AES-128 (FIPS; used when OpenSSL headers are absent) ---
namespace mirror_crypto {

inline void sha512(const uint8_t* data, size_t len, uint8_t out[64]) {
    static const uint64_t k[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
    auto rotr = [](uint64_t x, unsigned n) { return (x >> n) | (x << (64 - n)); };
    uint64_t h[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                     0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                     0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    auto load = [](const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | p[i];
        return v;
    };
    auto store = [](uint8_t* p, uint64_t v) {
        for (int i = 7; i >= 0; --i) {
            p[i] = static_cast<uint8_t>(v);
            v >>= 8;
        }
    };
    auto compress = [&](const uint8_t* block) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = load(block + i * 8);
        for (int i = 16; i < 80; ++i) {
            const uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
            const uint64_t ch = (e & f) ^ ((~e) & g);
            const uint64_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };

    uint8_t block[128];
    size_t n = 0;
    const uint64_t bitLo = static_cast<uint64_t>(len) * 8ull;
    for (size_t i = 0; i < len; ++i) {
        block[n++] = data[i];
        if (n == 128) {
            compress(block);
            n = 0;
        }
    }
    block[n++] = 0x80;
    if (n > 112) {
        while (n < 128)
            block[n++] = 0;
        compress(block);
        n = 0;
    }
    while (n < 112)
        block[n++] = 0;
    for (int i = 0; i < 8; ++i)
        block[n++] = 0;
    store(block + 120, bitLo);
    compress(block);
    for (int i = 0; i < 8; ++i)
        store(out + i * 8, h[i]);
}

struct Aes128 {
    uint32_t rk[44]{};

    static uint32_t sub(uint32_t x) {
        static const uint8_t s[256] = {
            0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
            0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
            0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
            0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
            0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
            0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
            0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
            0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
            0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
            0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
            0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
            0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
            0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
            0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
            0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
            0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
        return (static_cast<uint32_t>(s[x & 0xff])) | (static_cast<uint32_t>(s[(x >> 8) & 0xff]) << 8) |
               (static_cast<uint32_t>(s[(x >> 16) & 0xff]) << 16) |
               (static_cast<uint32_t>(s[(x >> 24) & 0xff]) << 24);
    }

    void setKey(const uint8_t key[16]) {
        static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
        for (int i = 0; i < 4; ++i)
            rk[i] = static_cast<uint32_t>(key[4 * i]) | (static_cast<uint32_t>(key[4 * i + 1]) << 8) |
                    (static_cast<uint32_t>(key[4 * i + 2]) << 16) | (static_cast<uint32_t>(key[4 * i + 3]) << 24);
        for (int i = 4; i < 44; ++i) {
            uint32_t t = rk[i - 1];
            if ((i % 4) == 0) {
                t = sub((t >> 8) | (t << 24)) ^ rcon[i / 4 - 1];
            }
            rk[i] = rk[i - 4] ^ t;
        }
    }

    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        auto xtime = [](uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); };
        uint8_t s[16];
        std::memcpy(s, in, 16);
        auto add = [&](int round) {
            for (int i = 0; i < 4; ++i) {
                const uint32_t w = rk[round * 4 + i];
                s[4 * i] ^= static_cast<uint8_t>(w);
                s[4 * i + 1] ^= static_cast<uint8_t>(w >> 8);
                s[4 * i + 2] ^= static_cast<uint8_t>(w >> 16);
                s[4 * i + 3] ^= static_cast<uint8_t>(w >> 24);
            }
        };
        auto subBytes = [&]() {
            for (int i = 0; i < 16; ++i) {
                const uint32_t v = sub(s[i]);
                s[i] = static_cast<uint8_t>(v);
            }
        };
        auto shift = [&]() {
            uint8_t t;
            t = s[1];
            s[1] = s[5];
            s[5] = s[9];
            s[9] = s[13];
            s[13] = t;
            t = s[2];
            s[2] = s[10];
            s[10] = t;
            t = s[6];
            s[6] = s[14];
            s[14] = t;
            t = s[15];
            s[15] = s[11];
            s[11] = s[7];
            s[7] = s[3];
            s[3] = t;
        };
        auto mix = [&]() {
            for (int i = 0; i < 4; ++i) {
                uint8_t* c = s + 4 * i;
                const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
                c[0] = xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3;
                c[1] = a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3;
                c[2] = a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3;
                c[3] = xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3);
            }
        };
        add(0);
        for (int r = 1; r < 10; ++r) {
            subBytes();
            shift();
            mix();
            add(r);
        }
        subBytes();
        shift();
        add(10);
        std::memcpy(out, s, 16);
    }
};

}  // namespace mirror_crypto

// SHA-512("AirPlayStreamKey<id>" || aeskey) / SHA-512("AirPlayStreamIV<id>" || aeskey).
// aeskey is the FairPlay-unwrapped 16-byte key UxPlay already uses for RAOP audio.
inline bool deriveMirrorKeys(const uint8_t aeskey[16], uint64_t streamConnectionId, uint8_t key[16],
                             uint8_t iv[16]) {
    if (!aeskey || !key || !iv)
        return false;
    char label[48];
    const int kn = std::snprintf(label, sizeof label, "AirPlayStreamKey%llu",
                                 static_cast<unsigned long long>(streamConnectionId));
    if (kn <= 0 || kn >= static_cast<int>(sizeof label))
        return false;
    uint8_t buf[48 + 16];
    std::memcpy(buf, label, static_cast<size_t>(kn));
    std::memcpy(buf + kn, aeskey, 16);
    uint8_t hash[64];
    mirror_crypto::sha512(buf, static_cast<size_t>(kn) + 16, hash);
    std::memcpy(key, hash, 16);

    const int ivn = std::snprintf(label, sizeof label, "AirPlayStreamIV%llu",
                                  static_cast<unsigned long long>(streamConnectionId));
    if (ivn <= 0 || ivn >= static_cast<int>(sizeof label))
        return false;
    std::memcpy(buf, label, static_cast<size_t>(ivn));
    std::memcpy(buf + ivn, aeskey, 16);
    mirror_crypto::sha512(buf, static_cast<size_t>(ivn) + 16, hash);
    std::memcpy(iv, hash, 16);
    return true;
}

// UxPlay mirror_buffer_decrypt: AES-128-CTR with leftover keystream across packets.
class MirrorCtr {
public:
    bool init(const uint8_t aeskey[16], uint64_t streamConnectionId) {
        uint8_t key[16], iv[16];
        if (!deriveMirrorKeys(aeskey, streamConnectionId, key, iv))
            return false;
        aes_.setKey(key);
        std::memcpy(ctr_, iv, 16);
        nextDecryptCount_ = 0;
        std::memset(og_, 0, 16);
        ready_ = true;
        return true;
    }

    bool decrypt(const uint8_t* in, uint8_t* out, size_t len) {
        if (!ready_ || !in || !out)
            return false;
#ifdef MUT_AP_MIRROR_NO_CTR
        std::memcpy(out, in, len);
        return true;
#else
        size_t off = 0;
        if (nextDecryptCount_ > 0) {
            const int n = nextDecryptCount_;
            for (int i = 0; i < n && off < len; ++i, ++off)
                out[off] = static_cast<uint8_t>(in[off] ^ og_[static_cast<size_t>(16 - n + i)]);
        }
        const size_t remain = len - off;
        const size_t aligned = (remain / 16) * 16;
        if (aligned) {
            ctrXor(in + off, out + off, aligned);
            off += aligned;
        }
        nextDecryptCount_ = 0;
        const size_t rest = len - off;
        if (rest > 0) {
            uint8_t block[16];
            std::memset(block, 0, 16);
            std::memcpy(block, in + off, rest);
            ctrXor(block, og_, 16);
            std::memcpy(out + off, og_, rest);
            nextDecryptCount_ = static_cast<int>(16 - rest);
        }
        return true;
#endif
    }

    bool ready() const { return ready_; }

private:
    void incCtr() {
        for (int i = 15; i >= 0; --i) {
            if (++ctr_[i] != 0)
                break;
        }
    }
    void ctrXor(const uint8_t* in, uint8_t* out, size_t len) {
        uint8_t ks[16];
        size_t i = 0;
        while (i < len) {
            aes_.encryptBlock(ctr_, ks);
            incCtr();
            const size_t n = len - i < 16 ? len - i : 16;
            for (size_t j = 0; j < n; ++j)
                out[i + j] = static_cast<uint8_t>(in[i + j] ^ ks[j]);
            i += n;
        }
    }

    mirror_crypto::Aes128 aes_{};
    uint8_t ctr_[16]{};
    uint8_t og_[16]{};
    int nextDecryptCount_ = 0;
    bool ready_ = false;
};

inline bool annexBStart(const uint8_t* p, size_t n) {
    return n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1;
}

// avcC (type 1 payload) -> Annex-B SPS + PPS. Rejects truncated / oversized NALs.
inline bool avccToAnnexB(const uint8_t* payload, size_t len, std::vector<uint8_t>& out) {
    out.clear();
    if (!payload || len < 11)
        return false;
    const uint16_t spsSize = mirrorBe16(payload + 6);
    if (spsSize == 0 || static_cast<size_t>(8) + spsSize + 3 > len)
        return false;
    const uint16_t ppsSize = mirrorBe16(payload + spsSize + 9);
    if (ppsSize == 0 || static_cast<size_t>(11) + spsSize + ppsSize > len)
        return false;
    const uint8_t* sps = payload + 8;
    const uint8_t* pps = payload + spsSize + 11;
    if ((sps[0] & 0x80) || (pps[0] & 0x80))
        return false;
#ifdef MUT_AP_MIRROR_NO_ANNEXB
    out.assign(sps, sps + spsSize);
    out.insert(out.end(), pps, pps + ppsSize);
    return true;
#else
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out.reserve(static_cast<size_t>(8) + spsSize + ppsSize);
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), sps, sps + spsSize);
    out.insert(out.end(), sc, sc + 4);
    out.insert(out.end(), pps, pps + ppsSize);
    return true;
#endif
}

// Length-prefixed AU (4-byte BE sizes) -> Annex-B. Never over-reads.
inline bool lengthPrefixedToAnnexB(uint8_t* payload, size_t len, std::vector<uint8_t>& out,
                                   int* nalCount = nullptr) {
    out.clear();
    if (nalCount)
        *nalCount = 0;
    if (!payload || len == 0)
        return false;
#ifdef MUT_AP_MIRROR_NO_ANNEXB
    out.assign(payload, payload + len);
    if (nalCount)
        *nalCount = 1;
    return true;
#else
    size_t off = 0;
    int nals = 0;
    while (off + 4 <= len) {
        const uint32_t nsz = mirrorBe32(payload + off);
        if (nsz == 0 || off + 4 + nsz > len)
            return false;
        if (payload[off + 4] & 0x80)
            return false;
        payload[off] = 0;
        payload[off + 1] = 0;
        payload[off + 2] = 0;
        payload[off + 3] = 1;
        off += 4 + nsz;
        ++nals;
    }
    if (off != len)
        return false;
    out.assign(payload, payload + len);
    if (nalCount)
        *nalCount = nals;
    return true;
#endif
}

inline size_t annexBStartCodeLen(const uint8_t* p, size_t n, size_t i) {
    if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
        return 4;
    if (i + 3 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        return 3;
    return 0;
}

inline bool annexBContainsNalType(const uint8_t* p, size_t n, uint8_t type) {
    size_t i = 0;
    while (i + 3 < n) {
        const size_t sc = annexBStartCodeLen(p, n, i);
        if (!sc) {
            ++i;
            continue;
        }
        const size_t nal = i + sc;
        if (nal < n && (p[nal] & 0x1f) == type)
            return true;
        i = nal + 1;
    }
    return false;
}

inline bool annexBContainsIdr(const uint8_t* p, size_t n) {
    return annexBContainsNalType(p, n, 5);
}

// Copy in-band SPS (7) / PPS (8) including their start codes, last-wins.
inline void stashAnnexBSpsPps(const uint8_t* p, size_t n, std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) {
    size_t i = 0;
    while (i + 3 < n) {
        const size_t sc = annexBStartCodeLen(p, n, i);
        if (!sc) {
            ++i;
            continue;
        }
        const size_t nal = i + sc;
        if (nal >= n)
            break;
        size_t j = nal + 1;
        while (j < n && annexBStartCodeLen(p, n, j) == 0)
            ++j;
        const uint8_t t = p[nal] & 0x1f;
        if (t == 7)
            sps.assign(p + i, p + j);
        else if (t == 8)
            pps.assign(p + i, p + j);
        i = j;
    }
}

// Prepend cached SPS/PPS on session start and on IDR when the AU has no in-band SPS.
inline bool prependSpsPpsIfNeeded(const uint8_t* in, size_t n, const std::vector<uint8_t>& sps,
                                  const std::vector<uint8_t>& pps, bool sessionStartOrIdr,
                                  std::vector<uint8_t>& out) {
    out.clear();
    if (!sessionStartOrIdr || !in || n == 0 || sps.empty())
        return false;
    if (annexBContainsNalType(in, n, 7))
        return false;
    out.reserve(sps.size() + pps.size() + n);
    out.insert(out.end(), sps.begin(), sps.end());
    out.insert(out.end(), pps.begin(), pps.end());
    out.insert(out.end(), in, in + n);
    return true;
}

struct MirrorAu {
    std::vector<uint8_t> annexb;
    int64_t pts90k = h264stream::kNoPts90k;
    bool keyframe = false;
    MirrorPayloadType type = MirrorPayloadType::Unknown;
};

// One mirror session. SETUP accept -> Listening; first video -> Mirroring;
// TEARDOWN / disconnect -> Stopped then Idle (Ready).
class MirrorSession {
public:
    bool acceptSetup(uint16_t dataPort) {
        if (state_ == MirrorSessionState::Mirroring || state_ == MirrorSessionState::Listening)
            return false;
        state_ = MirrorSessionState::Listening;
        dataPort_ = dataPort;
        pendingSps_.clear();
        return true;
    }

    bool initKeys(const uint8_t aeskey[16], uint64_t streamConnectionId) {
        return ctr_.init(aeskey, streamConnectionId);
    }

    MirrorSetupReply setupReply() const {
        return makeMirrorSetupReply(state_ != MirrorSessionState::Idle &&
                                        state_ != MirrorSessionState::Stopped,
                                    dataPort_);
    }

    enum class Outcome : uint8_t { Ok, Heartbeat, Ignore, Error, NeedSetup };

    Outcome handlePacket(const uint8_t* header, size_t headerLen, const uint8_t* payload, size_t payloadLen,
                         MirrorAu& au) {
        au = MirrorAu{};
        if (state_ == MirrorSessionState::Idle || state_ == MirrorSessionState::Stopped)
            return Outcome::NeedSetup;
        const MirrorHeader h = parseMirrorHeader(header, headerLen);
        if (!h.valid)
            return Outcome::Error;
        if (payloadLen != h.payloadSize)
            return Outcome::Error;
#ifdef MUT_AP_MIRROR_IGNORE_TYPE
        (void)h;
#endif
        switch (h.type) {
        case MirrorPayloadType::Heartbeat:
        case MirrorPayloadType::StreamReport:
            return Outcome::Heartbeat;
        case MirrorPayloadType::SpsPps: {
            if (!avccToAnnexB(payload, payloadLen, pendingSps_))
                return Outcome::Error;
            resolutionChanges_++;
            return Outcome::Ignore;
        }
        case MirrorPayloadType::EncryptedAu: {
            if (payloadLen == 0)
                return Outcome::Error;
            std::vector<uint8_t> plain(payloadLen);
            if (!ctr_.ready() || !ctr_.decrypt(payload, plain.data(), payloadLen))
                return Outcome::Error;
            std::vector<uint8_t> body;
            int nals = 0;
            if (!lengthPrefixedToAnnexB(plain.data(), plain.size(), body, &nals))
                return Outcome::Error;
            if (!pendingSps_.empty()) {
                au.annexb.swap(pendingSps_);
                au.annexb.insert(au.annexb.end(), body.begin(), body.end());
                pendingSps_.clear();
            } else {
                au.annexb.swap(body);
            }
            au.pts90k = mirrorNtpToPts90k(h.ntpNs);
            au.keyframe = annexBContainsIdr(au.annexb.data(), au.annexb.size());
            au.type = MirrorPayloadType::EncryptedAu;
            state_ = MirrorSessionState::Mirroring;
            ++auCount_;
            return Outcome::Ok;
        }
        default:
            return Outcome::Error;
        }
    }

    void teardown() {
        state_ = MirrorSessionState::Stopped;
        pendingSps_.clear();
        dataPort_ = 0;
    }

    void resetToReady() {
        teardown();
        state_ = MirrorSessionState::Idle;
        auCount_ = 0;
        resolutionChanges_ = 0;
        ctr_ = MirrorCtr{};
    }

    MirrorSessionState state() const { return state_; }
    uint16_t dataPort() const { return dataPort_; }
    uint64_t auCount() const { return auCount_; }
    unsigned resolutionChanges() const { return resolutionChanges_; }

private:
    MirrorSessionState state_ = MirrorSessionState::Idle;
    uint16_t dataPort_ = 0;
    MirrorCtr ctr_{};
    std::vector<uint8_t> pendingSps_;
    uint64_t auCount_ = 0;
    unsigned resolutionChanges_ = 0;
};

// Testable TCP data listener (UxPlay opens the production one after SETUP 200).
class MirrorDataListener {
public:
    ~MirrorDataListener() { close(); }

    bool listen(uint16_t port = 0) {
        close();
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
        int one = 1;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0 ||
            ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(fd, 1) < 0) {
            ::close(fd);
            return false;
        }
        sockaddr_in got{};
        socklen_t gl = sizeof got;
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&got), &gl) != 0) {
            ::close(fd);
            return false;
        }
        fd_ = fd;
        port_ = ntohs(got.sin_port);
        return port_ != 0;
    }

    uint16_t port() const { return port_; }

    int acceptOnce(int timeoutMs) {
        if (fd_ < 0)
            return -1;
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, timeoutMs) <= 0)
            return -1;
        return ::accept(fd_, nullptr, nullptr);
    }

    void close() {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
        port_ = 0;
    }

private:
    int fd_ = -1;
    uint16_t port_ = 0;
};

// Narrow decoder-feed seam. Default producer is null: start() is false, no FPGA write.
// MediaPlayer::setAirplayMirrorProducer attaches the existing IAvRecordProducer
// (OwenbRing / FpgaBitstreamProducer). Existing Plex/gcast play never calls this.
class AirplayMirrorDecoderFeed {
public:
    void attach(h264stream::IAvRecordProducer* p) { prod_ = p; }
    h264stream::IAvRecordProducer* producer() const { return prod_; }

    bool start() {
        if (!prod_ || active_)
            return false;
        const auto r = prod_->begin(sessionId_);
        if (r != h264stream::ControlResult::Ok)
            return false;
        active_ = true;
        seq_ = 0;
        return true;
    }

    bool push(const uint8_t* annexb, size_t len, int64_t pts90k) {
        if (!prod_ || !active_ || !annexb || len == 0)
            return false;
        h264stream::AccessUnitView au;
        au.session_id = sessionId_;
        au.seq = seq_;
        au.annexb = annexb;
        au.len = len;
        au.pts90k = pts90k;
        au.low_latency = true;
        const auto r = prod_->pushAccessUnit(au);
        if (r != h264stream::PushResult::Ok)
            return false;
        ++seq_;
        return true;
    }

    void stop() {
        if (!prod_ || !active_)
            return;
        prod_->end(sessionId_);
        active_ = false;
        ++sessionId_;
        seq_ = 0;
    }

    bool active() const { return active_; }
    uint32_t seq() const { return seq_; }

private:
    h264stream::IAvRecordProducer* prod_ = nullptr;
    uint64_t sessionId_ = 1;
    uint32_t seq_ = 0;
    bool active_ = false;
};

// Host-test producer: copies AUs, implements the FPGA feed contract.
class RecordingAvProducer final : public h264stream::IAvRecordProducer {
public:
    h264stream::ControlResult begin(uint64_t session_id) override {
        if (active_)
            return h264stream::ControlResult::ActiveSession;
        session_id_ = session_id;
        seq_ = 0;
        active_ = true;
        aus_.clear();
        return h264stream::ControlResult::Ok;
    }
    h264stream::PushResult pushNal(const h264stream::NalView&) override {
        return h264stream::PushResult::Fatal;
    }
    h264stream::ControlResult flush(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        return h264stream::ControlResult::Ok;
    }
    h264stream::ControlResult end(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        active_ = false;
        return h264stream::ControlResult::Ok;
    }
    h264stream::ControlResult pause(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        return h264stream::ControlResult::Ok;
    }
    h264stream::ControlResult resume(uint64_t session_id) override {
        if (!active_ || session_id != session_id_)
            return h264stream::ControlResult::NoSession;
        return h264stream::ControlResult::Ok;
    }
    h264stream::Telemetry status() const override {
        h264stream::Telemetry t;
        t.session_id = session_id_;
        t.active = active_;
        t.producer_seq = seq_;
        t.nal_accepted = aus_.size();
        return t;
    }
    h264stream::PushResult pushAccessUnit(const h264stream::AccessUnitView& au) override {
        if (!active_ || au.session_id != session_id_ || !au.annexb || au.len == 0)
            return h264stream::PushResult::Fatal;
        if (au.seq != seq_)
            return h264stream::PushResult::Desync;
        MirrorAu rec;
        rec.annexb.assign(au.annexb, au.annexb + au.len);
        rec.pts90k = au.pts90k;
        rec.keyframe = annexBContainsIdr(au.annexb, au.len);
        aus_.push_back(std::move(rec));
        ++seq_;
        return h264stream::PushResult::Ok;
    }
    h264stream::PushResult pushPcm(const h264stream::PcmView&) override {
        return h264stream::PushResult::Fatal;
    }

    const std::vector<MirrorAu>& aus() const { return aus_; }
    bool active() const { return active_; }

private:
    std::vector<MirrorAu> aus_;
    uint64_t session_id_ = 0;
    uint32_t seq_ = 0;
    bool active_ = false;
};

}  // namespace airplay
}  // namespace misterplex
