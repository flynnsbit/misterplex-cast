// The painter's OsdConfig: fb0 through h264ring_fb_map, with the fbcon guard around it. The same mapping the daemon
// uses (arm/misterplexd/media_player.cpp owenbOsdConfigFromEnv: pages inside [0x22000000, +8 MiB), never the idle
// framebuffer, never FbPresent); kept apart so the painter links no libav. Only tools/owenb_idle_card.cpp includes
// this (it links the ring objects that define h264ring_fb_map); the host test injects a fake mapFb instead.
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "h264ring.h"
#include "owenb_fbcon_guard.hpp"
#include "owenb_play.hpp"

namespace misterplex::owenb_idle_card {

inline owenb::OsdConfig fbOsdConfig() {
    owenb::OsdConfig c;
    c.enabled = true;
    c.allowDouble = true;
    owenb::FbconConfig gc;
    const char* keep = std::getenv("MPX_OB_FBCON");   // as the daemon: =keep only looks and logs
    gc.policy = keep && std::strcmp(keep, "keep") == 0 ? owenb::FbconPolicy::Keep : owenb::FbconPolicy::Unbind;
    gc.log = [](const std::string& line) { std::fprintf(stderr, "owenb_idle_card: %s\n", line.c_str()); };
    auto fbcon = std::make_shared<owenb::FbconGuard>(gc);
    fbcon->restoreLeftover();   // a painter or daemon that died holding fb0 left fbcon detached
    auto fb = std::make_shared<struct ::h264ring_fb>();
    c.mapFb = [fb, fbcon](owenb_osd::FbMap& m, std::string& err) {
        *fb = {};
        fb->fd = -1;
        fbcon->acquire();   // no console text over our page
        const int rc = h264ring_fb_map(fb.get());
        if (rc != 0) {
            fbcon->release();
            err = std::string("h264ring_fb_map: ") + std::strerror(rc < 0 ? -rc : rc);
            return false;
        }
        m.virt = static_cast<uint8_t*>(fb->virt);
        m.phys = fb->phys;
        m.len = fb->len;
        return true;
    };
    c.unmapFb = [fb, fbcon] {
        h264ring_fb_unmap(fb.get());
        fbcon->release();
    };
    return c;
}

} // namespace misterplex::owenb_idle_card
