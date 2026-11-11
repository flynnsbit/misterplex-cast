// POSIX implementation of the self-update Fs: write + fsync, atomic rename + directory fsync, statvfs.
// Files are created 0755 (daemon and boot hook must stay executable; FAT ignores the mode bits anyway).
#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include "update_swap.hpp"

namespace mpx { namespace upd {

struct RealFs : Fs {
    static std::string dirOf(const std::string& p) {
        size_t s = p.rfind('/');
        return s == std::string::npos ? "." : (s == 0 ? "/" : p.substr(0, s));
    }
    static void syncDir(const std::string& p) {
        int d = ::open(dirOf(p).c_str(), O_RDONLY);
        if (d >= 0) { ::fsync(d); ::close(d); }  // best effort: FAT/some FUSE refuse it
    }
    bool exists(const std::string& p) override { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
    bool read(const std::string& p, std::string& out) override {
        FILE* f = ::fopen(p.c_str(), "rb");
        if (!f) return false;
        out.clear(); char buf[8192]; size_t n;
        while ((n = ::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        bool ok = !::ferror(f); ::fclose(f); return ok;
    }
    bool writeSync(const std::string& p, const std::string& data) override {
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (fd < 0) return false;
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(fd, data.data() + off, data.size() - off);
            if (n < 0) { if (errno == EINTR) continue; ::close(fd); ::unlink(p.c_str()); return false; }
            off += size_t(n);
        }
        bool ok = ::fsync(fd) == 0;
        ok = (::close(fd) == 0) && ok;
        if (!ok) ::unlink(p.c_str());
        return ok;
    }
    bool rename(const std::string& a, const std::string& b) override {
        if (::rename(a.c_str(), b.c_str()) != 0) return false;
        syncDir(b); return true;
    }
    bool remove(const std::string& p) override { bool r = ::unlink(p.c_str()) == 0; if (r) syncDir(p); return r || errno == ENOENT; }
    bool freeBytes(const std::string& p, uint64_t& out) override {
        struct statvfs v;
        if (::statvfs(dirOf(p).c_str(), &v) != 0) return false;
        out = uint64_t(v.f_bavail) * uint64_t(v.f_frsize); return true;
    }
};

}}  // namespace mpx::upd
