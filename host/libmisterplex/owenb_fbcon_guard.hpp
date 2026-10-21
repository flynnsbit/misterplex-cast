#pragma once
// OWENB-P1B 0009 (T6 gap G15): keep the kernel's framebuffer console off fb0 while our overlay pages
// live there. h264ring_fb_map only stops the cursor blink (h264ring_linux.c:193-236); a kernel message,
// a getty on tty1 or MiSTer's F9 terminal would still draw text into the page the core is showing.
//
// While the OB overlay holds fb0 (OwenbPlayer's mapFb .. unmapFb: the idle session and the plays),
// fbcon is detached from the console layer -- "echo 0 > /sys/class/vtconsole/vtconN/bind" for the
// vtcon named "frame buffer device" (Documentation/fb/fbcon.rst §D) -- and attached again ("1") at the
// release, only the ones this guard detached. A marker file lists them, so a daemon that died holding
// fb0 gives the console back at its next start (restoreLeftover). MPX_OB_FBCON=keep: look, log, touch
// nothing (for an image that already boots with fbcon=map:1, docs/OWENB-IMAGE-FBCON.md).
// Paths are injectable (tests run on a temp tree, never /sys).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <dirent.h>

namespace misterplex::owenb {

enum class FbconPolicy { Unbind, Keep };
struct FbconConfig {
    FbconPolicy policy = FbconPolicy::Unbind;
    std::string vtRoot = "/sys/class/vtconsole";
    std::string marker = "/tmp/misterplex_ob_fbcon_unbound";
    std::string cmdline = "/proc/cmdline";
    std::function<void(const std::string&)> log;
};

class FbconGuard {
public:
    using Policy = FbconPolicy;
    using Config = FbconConfig;
    struct Vt {
        std::string dir;    // vtRoot/vtconN
        std::string name;   // e.g. "(M) frame buffer device"
        bool bound = false;
    };

    explicit FbconGuard(Config c = Config()) : c_(std::move(c)) {}

    static std::string readFile(const std::string& p) {
        std::string s;
        std::FILE* f = std::fopen(p.c_str(), "r");
        if (!f)
            return s;
        char buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            s.append(buf, n);
        std::fclose(f);
        while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        return s;
    }
    static bool writeFile(const std::string& p, const std::string& v) {
        std::FILE* f = std::fopen(p.c_str(), "w");
        if (!f)
            return false;
        const bool ok = std::fputs(v.c_str(), f) >= 0;
        return std::fclose(f) == 0 && ok;
    }
    // the vtcons that are fbcon ("frame buffer" in the name), in vtcon order
    std::vector<Vt> scan() const {
        std::vector<Vt> v;
        DIR* d = ::opendir(c_.vtRoot.c_str());
        if (!d)
            return v;
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n.rfind("vtcon", 0) != 0)
                continue;
            Vt t;
            t.dir = c_.vtRoot + "/" + n;
            t.name = readFile(t.dir + "/name");
            if (t.name.find("frame buffer") == std::string::npos)
                continue;
            t.bound = readFile(t.dir + "/bind") == "1";
            v.push_back(t);
        }
        ::closedir(d);
        return v;
    }
    // the image keeps fbcon off fb0 itself (fbcon=map:N with no fb0 in it, fbcon.rst §C.2)
    bool imageMapsFbconAway() const {
        const std::string cl = readFile(c_.cmdline);
        const size_t i = cl.find("fbcon=map:");
        if (i == std::string::npos)
            return false;
        const size_t a = i + 10, b = cl.find_first_of(" ,", a);
        const std::string m = cl.substr(a, b == std::string::npos ? std::string::npos : b - a);
        return !m.empty() && m.find('0') == std::string::npos;
    }

    // fb0 is about to carry our pages. Returns how many vtcons are detached (by us, now).
    int acquire() {
        if (held_)
            return int(mine_.size());
        held_ = true;
        mine_.clear();
        const std::vector<Vt> vts = scan();
        bool anyBound = false;
        for (const Vt& t : vts)
            anyBound = anyBound || t.bound;
        if (!anyBound) {
            logf(vts.empty() ? "fbcon: no framebuffer console on this image: fb0 is ours"
                             : "fbcon: the framebuffer console is already detached: fb0 is ours");
            return 0;
        }
        if (c_.policy == Policy::Keep) {
            logf("fbcon: MPX_OB_FBCON=keep: the framebuffer console stays on fb0 (text can overwrite the overlay)");
            return 0;
        }
        for (const Vt& t : vts) {
            if (!t.bound)
                continue;
            if (writeFile(t.dir + "/bind", "0") && readFile(t.dir + "/bind") != "1") {
                mine_.push_back(t.dir);
                logf("fbcon: " + t.name + " detached while the OB overlay holds fb0");
            } else {
                logf("fbcon: could not detach " + t.dir + " (text can overwrite the overlay)");
            }
        }
        writeMarker();
        return int(mine_.size());
    }
    // fb0 is released (unmapFb, a hop off OB, daemon exit): give the console back
    int release() {
        if (!held_)
            return 0;
        held_ = false;
        int n = 0;
#ifndef MUT_FBCON_NO_REBIND
        for (const std::string& d : mine_)
            if (writeFile(d + "/bind", "1"))
                ++n;
#endif
#ifdef MUT_FBCON_REBIND_ALL
        for (const Vt& t : scan())   // mutant: also attaches a console the user had detached
            if (writeFile(t.dir + "/bind", "1"))
                ++n;
#endif
        if (!mine_.empty())
            logf("fbcon: the framebuffer console attached again (" + std::to_string(n) + ")");
        mine_.clear();
        std::remove(c_.marker.c_str());
        return n;
    }
    // at daemon start: a previous daemon died holding fb0 -- attach what it detached
    int restoreLeftover() {
        const std::string m = readFile(c_.marker);
        if (m.empty())
            return 0;
        int n = 0;
        size_t a = 0;
        while (a < m.size()) {
            size_t b = m.find('\n', a);
            if (b == std::string::npos)
                b = m.size();
            const std::string d = m.substr(a, b - a);
            if (!d.empty() && d.rfind(c_.vtRoot + "/", 0) == 0 && writeFile(d + "/bind", "1"))
                ++n;
            a = b + 1;
        }
        std::remove(c_.marker.c_str());
        logf("fbcon: attached " + std::to_string(n) + " framebuffer console(s) a previous daemon left detached");
        return n;
    }
    bool held() const { return held_; }
    const std::vector<std::string>& detached() const { return mine_; }

private:
    void logf(const std::string& s) const {
        if (c_.log)
            c_.log(s);
    }
    void writeMarker() const {
        if (mine_.empty()) {
            std::remove(c_.marker.c_str());
            return;
        }
        std::string s;
        for (const std::string& d : mine_)
            s += d + "\n";
        writeFile(c_.marker, s);
    }
    Config c_;
    bool held_ = false;
    std::vector<std::string> mine_;
};

} // namespace misterplex::owenb
