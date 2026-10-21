#include "plexui_disk_cache.hpp"

#define QOI_IMPLEMENTATION
#include "third_party/qoi.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace misterplex {
namespace plexui {

namespace {

struct Sha1Ctx {
    std::uint32_t state[5]{};
    std::uint32_t count[2]{};
    std::uint8_t buffer[64]{};
};

inline std::uint32_t rol(std::uint32_t v, int bits) {
    return (v << bits) | (v >> (32 - bits));
}

void sha1Transform(std::uint32_t state[5], const std::uint8_t block[64]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const std::uint32_t temp = rol(a, 5) + f + e + k + w[static_cast<std::size_t>(i)];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1Init(Sha1Ctx& ctx) {
    ctx.state[0] = 0x67452301;
    ctx.state[1] = 0xEFCDAB89;
    ctx.state[2] = 0x98BADCFE;
    ctx.state[3] = 0x10325476;
    ctx.state[4] = 0xC3D2E1F0;
    ctx.count[0] = ctx.count[1] = 0;
}

void sha1Update(Sha1Ctx& ctx, const std::uint8_t* data, std::size_t len) {
    std::size_t i = 0;
    std::uint32_t j = (ctx.count[0] >> 3) & 63;
    if ((ctx.count[0] += static_cast<std::uint32_t>(len << 3)) < static_cast<std::uint32_t>(len << 3))
        ctx.count[1]++;
    ctx.count[1] += static_cast<std::uint32_t>(len >> 29);
    const std::size_t partLen = 64 - j;
    if (len >= partLen) {
        std::memcpy(&ctx.buffer[j], &data[0], partLen);
        sha1Transform(ctx.state, ctx.buffer);
        for (i = partLen; i + 63 < len; i += 64)
            sha1Transform(ctx.state, &data[i]);
        j = 0;
    } else
        i = 0;
    std::memcpy(&ctx.buffer[j], &data[i], len - i);
}

void sha1Final(Sha1Ctx& ctx, std::uint8_t digest[20]) {
    std::uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i)
        finalcount[i] = static_cast<std::uint8_t>((ctx.count[(i >= 4) ? 0 : 1] >>
                                                   ((3 - (i & 3)) * 8)) &
                                                  0xff);
    sha1Update(ctx, reinterpret_cast<const std::uint8_t*>("\200"), 1);
    std::uint8_t zero = 0;
    while ((ctx.count[0] & 504) != 448)
        sha1Update(ctx, &zero, 1);
    sha1Update(ctx, finalcount, 8);
    for (int i = 0; i < 20; ++i)
        digest[i] = static_cast<std::uint8_t>(
            (ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xff);
}

std::size_t envSizeMb(const char* name, std::size_t defaultMb) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw)
        return defaultMb;
    const int v = std::atoi(raw);
    return v > 0 ? static_cast<std::size_t>(v) : defaultMb;
}

} // namespace

std::string PlexUiDiskCache::sha1Hex(const std::string& s) {
    Sha1Ctx ctx;
    sha1Init(ctx);
    sha1Update(ctx, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    std::uint8_t digest[20];
    sha1Final(ctx, digest);
    static const char* hex = "0123456789abcdef";
    std::string out(40, '0');
    for (int i = 0; i < 20; ++i) {
        out[static_cast<std::size_t>(i) * 2] = hex[digest[i] >> 4];
        out[static_cast<std::size_t>(i) * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

std::size_t readMemAvailableKb() {
    std::ifstream in("/proc/meminfo");
    if (!in)
        return 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 12, "MemAvailable") == 0) {
            std::size_t kb = 0;
            if (std::sscanf(line.c_str(), "MemAvailable: %zu kB", &kb) == 1)
                return kb;
        }
    }
    return 0;
}

std::string PlexUiDiskCache::filePathFor(const std::string& dir, const std::string& digest) {
    return dir + "/" + digest + ".qoi";
}

PlexUiDiskCache::PlexUiDiskCache() {
    const char* raw = std::getenv("MPX_PLEXUI_DISK_CACHE");
    if (raw && *raw)
        dir_ = raw;
    maxBytes_ = envSizeMb("MPX_PLEXUI_DISK_CACHE_MB", 200) * 1024u * 1024u;
}

PlexUiDiskCache::~PlexUiDiskCache() { stop(); }

void PlexUiDiskCache::start() {
    if (dir_.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    evictToCap();
    stopping_ = false;
    if (!writer_.joinable())
        writer_ = std::thread([this] { writerLoop(); });
}

void PlexUiDiskCache::stop() {
    stopping_ = true;
    if (writer_.joinable())
        writer_.join();
}

bool PlexUiDiskCache::loadRgba(const std::string& cacheKey, int w, int h,
                               std::vector<std::uint8_t>& rgbaOut) {
    if (dir_.empty() || w <= 0 || h <= 0)
        return false;
    const std::string digest = sha1Hex(cacheKey);
    const std::string path = filePathFor(dir_, digest);
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz <= 0)
        return false;
    std::vector<std::uint8_t> qoi(static_cast<std::size_t>(sz));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(qoi.data()), sz);
    if (!in)
        return false;
    qoi_desc desc{};
    void* px = qoi_decode(qoi.data(), qoi.size(), &desc, 4);
    if (!px || desc.width != static_cast<unsigned>(w) || desc.height != static_cast<unsigned>(h)) {
        if (px)
            QOI_FREE(px);
        return false;
    }
    rgbaOut.resize(static_cast<std::size_t>(w) * h * 4);
    std::memcpy(rgbaOut.data(), px, rgbaOut.size());
    QOI_FREE(px);
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());
    return true;
}

void PlexUiDiskCache::queueWrite(const std::string& cacheKey, int w, int h,
                                 const std::uint8_t* rgba, std::size_t rgbaBytes) {
    const std::size_t need = static_cast<std::size_t>(w) * h * 4u;
    if (dir_.empty() || !rgba || w <= 0 || h <= 0 || rgbaBytes < need)
        return;
    DiskCacheWriteJob job;
    job.keyDigest = sha1Hex(cacheKey);
    job.w = w;
    job.h = h;
    job.rgba.assign(rgba, rgba + rgbaBytes);
    std::lock_guard<std::mutex> lk(mu_);
    if (writeQ_.size() >= writeQMax_)
        return;
    queuedBytes_ += job.rgba.size();
    writeQ_.push_back(std::move(job));
}

std::size_t PlexUiDiskCache::queuedBytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queuedBytes_;
}

bool PlexUiDiskCache::writeFileAtomic(const std::string& path,
                                      const std::vector<std::uint8_t>& qoi) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(qoi.data()),
                  static_cast<std::streamsize>(qoi.size()));
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::size_t PlexUiDiskCache::dirBytesOnDisk() const {
    if (dir_.empty())
        return 0;
    std::size_t total = 0;
    std::error_code ec;
    for (const auto& ent : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec)
            break;
        if (!ent.is_regular_file())
            continue;
        const auto name = ent.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".qoi") != 0)
            continue;
        total += static_cast<std::size_t>(ent.file_size(ec));
    }
    return total;
}

void PlexUiDiskCache::evictToCap() {
    if (dir_.empty() || maxBytes_ == 0)
        return;
    struct Entry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        std::uintmax_t size;
    };
    std::vector<Entry> files;
    std::error_code ec;
    for (const auto& ent : std::filesystem::directory_iterator(dir_, ec)) {
        if (ec)
            return;
        if (!ent.is_regular_file())
            continue;
        const auto name = ent.path().filename().string();
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".qoi") != 0)
            continue;
        Entry e;
        e.path = ent.path();
        e.mtime = ent.last_write_time(ec);
        e.size = ent.file_size(ec);
        files.push_back(std::move(e));
    }
    auto total = dirBytesOnDisk();
    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    for (const auto& f : files) {
        if (total <= maxBytes_)
            break;
        std::filesystem::remove(f.path, ec);
        total -= static_cast<std::size_t>(f.size);
    }
}

void PlexUiDiskCache::writerLoop() {
    for (;;) {
        DiskCacheWriteJob job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (stopping_ && writeQ_.empty())
                return;
            if (writeQ_.empty()) {
                lk.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            job = std::move(writeQ_.front());
            writeQ_.erase(writeQ_.begin());
        }
        struct QBytes {   // 0012: the copy counts until this job's RGBA is freed
            PlexUiDiskCache* c; std::size_t n;
            ~QBytes() { std::lock_guard<std::mutex> g(c->mu_); c->queuedBytes_ -= std::min(n, c->queuedBytes_); }
        } qbytes{this, job.rgba.size()};
        qoi_desc desc{};
        desc.width = static_cast<unsigned>(job.w);
        desc.height = static_cast<unsigned>(job.h);
        desc.channels = 4;
        desc.colorspace = QOI_SRGB;
        int qoiLen = 0;
        void* encoded = qoi_encode(job.rgba.data(), &desc, &qoiLen);
        if (!encoded || qoiLen <= 0)
            continue;
        std::vector<std::uint8_t> qoiBuf(static_cast<std::size_t>(qoiLen));
        std::memcpy(qoiBuf.data(), encoded, static_cast<std::size_t>(qoiLen));
        QOI_FREE(encoded);
        const std::string path = filePathFor(dir_, job.keyDigest);
        if (writeFileAtomic(path, qoiBuf)) {
            std::lock_guard<std::mutex> lk(mu_);
            evictToCap();
        }
    }
}

} // namespace plexui
} // namespace misterplex
