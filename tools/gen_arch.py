#!/usr/bin/env python3
"""Generate docs/architecture.svg (rc5: PlexOB core + HPS cast stack).

Same visual language as the earlier hardware block diagram: grey ARM boxes,
orange DDR3 table, navy FPGA fabric frame, clock-domain fills, legend.
Every block and arrow is mapped to source in VERIFY.md.
"""
from html import escape

W, H = 2400, 3160
out = []
grad_n = [0]

C_SYS, C_MEM, C_DEC, C_VID, C_AUD, C_ARM = "#DFF5DF", "#FFE2C6", "#DCEBFF", "#EADCF8", "#FFF5C2", "#E6E6E6"
A_DDR, A_DATA, A_NET, A_CTL, A_VID, A_AUD, A_PLAN = "#D2691E", "#1F4E9E", "#000000", "#666666", "#7B2FBE", "#B8860B", "#B03030"


def t(x, y, s, size=16, bold=False, fill="#333333", anchor="middle"):
    w = ' font-weight="bold"' if bold else ""
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}"{w} fill="{fill}" text-anchor="{anchor}">{escape(s)}</text>')


def rect(x, y, w, h, fill, stroke="#333333", sw=1.6, rx=10, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def grad(a, b):
    grad_n[0] += 1
    gid = f"g{grad_n[0]}"
    out.append(f'<defs><linearGradient id="{gid}" x1="0" x2="1" y1="0" y2="0"><stop offset="0.5" stop-color="{a}"/>'
               f'<stop offset="0.5" stop-color="{b}"/></linearGradient></defs>')
    return f"url(#{gid})"


def block(x, y, w, h, title, lines=(), fill="#FFFFFF", sw=1.6, stroke="#333333", dash=None, tsize=21, lsize=16, align="middle"):
    rect(x, y, w, h, fill, stroke=stroke, sw=sw, dash=dash)
    n = len(lines)
    total = 26 + 21 * n
    y0 = y + (h - total) / 2 + 20
    cx = x + w / 2 if align == "middle" else x + 16
    t(cx, y0, title, tsize, True, "#7A1F1F" if dash else "#111111", align)
    for i, ln in enumerate(lines):
        t(cx, y0 + 26 + 21 * i, ln, lsize, False, "#7A1F1F" if dash else "#333333", align)


def group(x, y, w, h, title, fill, stroke, sw=2.2, tsize=21, sub=None):
    rect(x, y, w, h, fill, stroke=stroke, sw=sw, rx=16)
    t(x + 16, y + 28, title, tsize, True, "#222222", "start")
    if sub:
        t(x + 16, y + 54, sub, 18, False, "#333333", "start")


def head(x, y, col, d, size=20):
    # arrowhead at (x,y) pointing in direction d (R,L,U,D)
    s, hw = size, size * 0.54
    pts = {"R": [(x, y), (x - s, y - hw), (x - s, y + hw)], "L": [(x, y), (x + s, y - hw), (x + s, y + hw)],
           "D": [(x, y), (x - hw, y - s), (x + hw, y - s)], "U": [(x, y), (x - hw, y + s), (x + hw, y + s)]}[d]
    out.append('<polygon points="' + " ".join(f"{a:.1f},{b:.1f}" for a, b in pts) + f'" fill="{col}"/>')


def arrow(pts, col, sw=3.0, dash=None, both=False, hs=18):
    def dirn(a, b):
        (x1, y1), (x2, y2) = a, b
        if x1 == x2:
            return "D" if y2 > y1 else "U"
        return "R" if x2 > x1 else "L"
    p = list(pts)
    d_end = dirn(p[-2], p[-1])
    # shorten the line under the arrowheads
    def back(pt, d, k):
        x, y = pt
        return {"R": (x - k, y), "L": (x + k, y), "D": (x, y - k), "U": (x, y + k)}[d]
    q = p[:]
    q[-1] = back(p[-1], d_end, hs * 0.7)
    if both:
        d_start = dirn(p[1], p[0])
        q[0] = back(p[0], d_start, hs * 0.7)
    dd = f' stroke-dasharray="{dash}"' if dash else ""
    out.append('<polyline points="' + " ".join(f"{a:.1f},{b:.1f}" for a, b in q) +
               f'" fill="none" stroke="{col}" stroke-width="{sw}" stroke-linejoin="round"{dd}/>')
    head(*p[-1], col, d_end, hs)
    if both:
        head(*p[0], col, dirn(p[1], p[0]), hs)


labels = []


def label(x, y, lines, col, size=15, anchor="middle"):
    labels.append((x, y, lines if isinstance(lines, (list, tuple)) else [lines], col, size, anchor))


def flush_labels():
    for x, y, lines, col, size, anchor in labels:
        wmax = max(len(s) for s in lines) * size * 0.56 + 16
        h = len(lines) * (size + 5) + 8
        x0 = x - wmax / 2 if anchor == "middle" else x
        out.append(f'<rect x="{x0:.1f}" y="{y - h / 2:.1f}" width="{wmax:.1f}" height="{h:.1f}" rx="5" fill="white" '
                   f'fill-opacity="0.95" stroke="{col}" stroke-width="0.8"/>')
        for i, s in enumerate(lines):
            t(x0 + wmax / 2, y - h / 2 + 4 + (size + 5) * (i + 1) - 4, s, size, False, "#222222", "middle")


# ---------------------------------------------------------------- page
out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" font-family="DejaVu Sans, sans-serif">')
rect(0, 0, W, H, "white", stroke="none", sw=0, rx=0)
t(1200, 52, "MisterPlex: system block diagram (rc5 · PlexOB core)", 38, True, "#0b1d3a")
t(1200, 88, "Plex, AirPlay and YouTube casting on MiSTer DE10-Nano · ARM daemon feeds the PlexOB H.264 core through shared DDR3 (no SDRAM board) · "
            "Cyclone V 5CSEBA6 · 41,033 / 41,910 ALMs (97.9%)", 20, False, "#333333")

# ---------------------------------------------------------------- senders
group(30, 130, 330, 890, "Senders", "#FAFAFA", "#777777", tsize=22)
t(46, 180, "LAN / internet", 17, False, "#333333", "start")
S = dict(x=45, w=300)
block(45, 200, 300, 125, "Plex app / Web", ["casts to the player", "(GDM + companion)"], fill="#F2F2F2", stroke="#222222", sw=2, tsize=22)
block(45, 345, 300, 125, "Plex Media Server", ["library · HLS / https", "server list via plex.tv"], fill="#F2F2F2", stroke="#222222", sw=2, tsize=22)
block(45, 490, 300, 125, "iPhone / iPad / Mac", ["AirPlay video by URL", "(no screen mirroring)"], fill="#F2F2F2", stroke="#222222", sw=2, tsize=22)
block(45, 635, 300, 150, "YouTube app", ["cast or TV-code pairing", "streams from YouTube", "servers (https)"], fill="#F2F2F2", stroke="#222222", sw=2, tsize=22)
block(45, 830, 300, 125, "Pad / keyboard", ["on the MiSTer's USB", "(read by Main_MiSTer)"], fill="#F2F2F2", stroke="#222222", sw=2, tsize=22)

# ---------------------------------------------------------------- HPS
rect(380, 130, 1990, 890, "#F4F4F4", stroke="#777777", sw=2.2, rx=16)
t(396, 162, "MiSTer ARM / HPS (Linux)", 24, True, "#111111", "start")
rect(400, 176, 1475, 830, C_ARM, stroke="#333333", sw=1.6)
t(418, 208, "misterplexd · cast + playback daemon (one static ARM binary)", 23, True, "#111111", "start")
t(420, 244, "Cast receivers", 18, True, "#444444", "start")
t(915, 244, "Sessions + playback", 18, True, "#444444", "start")
t(1395, 244, "Core + system", 18, True, "#444444", "start")

# column 1: receivers
block(420, 260, 455, 120, "Plex companion", ["GDM discovery + HTTP player (:3005)", "play queue → Plex resolve → PMS stream"])
block(420, 410, 455, 140, "AirPlay receiver (UxPlay)", ["AirPlay works on the MiSTer by itself;", "a helper on a computer is optional",
                                                          "H.264 ≤ 720p30 passes straight through", "audio: RAOP + spectrum screen · no mirroring"], lsize=15)
block(420, 560, 455, 140, "YouTube receiver", ["Lounge channel over verified HTTPS", "DIAL + SSDP answered only while Lounge is up",
                                                 "TV-code pairing · reconnect backoff 1 → 30 s"], lsize=15)
block(420, 730, 455, 255, "yt-dlp resolver", ["1. progressive H.264 ≤ 720p, ≤ 30 fps", "2. DASH H.264 ≤ 720p, ≤ 30 fps",
                                                "    + m4a audio (two URLs)", "https formats only (no HLS)", "bundled; unpacked at first boot",
                                                "daily update (update_ytdlp.sh)"])
# column 2: sessions + playback
block(915, 260, 450, 120, "Session arbiter", ["one owner: Plex | AirPlay | YouTube", "new play stops old · Stop always stops"], lsize=15)
block(915, 410, 450, 190, "Player (libav, static)", ["demux HLS / https (OpenSSL TLS)", "video → H.264 access units (Annex B)",
                                                       "audio → 48 kHz s16 stereo PCM", "records written through h264ring", "(owenb hps/ client, MIT)"])
block(915, 630, 450, 140, "plexui controller", ["Browse Plex on the TV", "pad / keyboard from the core input region", "Back returns to home"])
block(915, 800, 450, 185, "OSD renderer", ["ready screen: Plex | AirPlay | YouTube", "(only cast types that are up)", "cast bar, toasts, error screens",
                                            "opaque library page over idle bars"])
# column 3: core + system
block(1395, 260, 460, 150, "Core gate", ["CORENAME must be PlexOB", "build ID read from ring word 88", "only the shipped PlexOB build plays;", "stock H264 + retired IDs refused"], lsize=15)
block(1395, 430, 460, 130, "Clock rescue", ["wrong clock at boot →", "RTC, then SNTP, then HTTPS Date"])
block(1395, 580, 460, 110, "OSD Send Home (virtual pad)", ["returns home · not yet checked on a TV"], fill="#FFF7F7", stroke=A_PLAN, sw=2.4, dash="10,6", tsize=20)
block(1395, 710, 460, 120, "DDR window (/dev/mem)", ["maps ring control page + 8 MiB ring", "never maps the core's idle FB"], lsize=15)

# right column
block(1910, 176, 440, 335, "Boot hook", ["user-startup.sh → misterplexd-boot-rc3.sh", "1. load PlexOB if another core runs",
                                         "2. wait ≤ 60 s for network, then start", "    (AirPlay registers with an address)",
                                         "3. name misterplex-<last 3 octets>", "    (a name you set is kept)", "4. stop cleanly on shutdown"],
      fill=C_ARM, lsize=15)
block(1910, 545, 440, 165, "Main_MiSTer", ["menu + core OSD options", "load_core via /dev/MiSTer_cmd", "grabs USB pads + keyboard"], fill=C_ARM, tsize=22)
block(1910, 745, 440, 130, "Linux framebuffer (fb0)", ["holds the OSD canvas", "shown by the core's overlay plane"], fill=C_ARM)

# ---------------------------------------------------------------- DDR table
DY = 1060
rect(380, DY, 1320, 330, "#FFF3E6", stroke=A_DDR, sw=3, rx=16)
t(396, DY + 32, "HPS DDR3 (shared with Linux) · FPGA side via f2sdram bridges", 24, True, "#111111", "start")
rows = [
    ("0x2200_0000 +8 MiB", ["Linux framebuffer (fb0): OSD canvas, double-buffered"]),
    ("0x3140_0000 (4 KiB)", ["Ring control block: core status 0–31 · program 32–39 (write index,",
                             "pause / volume, settings, rumble) · status 40–47 · input 48–71",
                             "overlay descriptor 72–79 · telemetry 80–87 (87 = Y/C) · build ID 88–95"]),
    ("0x3180_0000 +8 MiB", ["Record ring: H.264 access units + 48 kHz s16 PCM audio records"]),
    ("0x3200_0000 20 × 2 MiB", ["DPB: 20 picture stores (decoder writes, display reads)"]),
]
y = DY + 46
rect(400, y, 380, 36, "#F6D7B8", stroke="#C8A080", sw=1, rx=0)
rect(780, y, 900, 36, "#F6D7B8", stroke="#C8A080", sw=1, rx=0)
t(408, y + 23, "Byte address", 17, True, "#1a1a1a", "start")
t(788, y + 23, "Region (PlexOB memory map)", 17, True, "#1a1a1a", "start")
y += 36
for addr, lines in rows:
    h = 14 + 28 * len(lines)
    rect(400, y, 380, h, "white", stroke="#C8A080", sw=1, rx=0)
    rect(780, y, 900, h, "white", stroke="#C8A080", sw=1, rx=0)
    t(408, y + 26, addr, 17, True, "#1a1a1a", "start")
    for i, ln in enumerate(lines):
        t(788, y + 26 + 28 * i, ln, 17, False, "#1a1a1a", "start")
    y += h
DDR_BOTTOM = DY + 330
block(1740, DY, 610, 200, "Core memory ports", ["ram1 (DDRAM): record reader + status write-back,", "decoder DPB writes",
                                                     "ram2 (DDRAM2): display reader + overlay reads", "both on clk_mem 90 MHz"], fill="#FFFFFF", lsize=16)
block(1740, DY + 220, 610, 110, "Status heartbeat", ["core rewrites the status block every frame;", "daemon reads it (no session needed)"], fill="#FFFFFF", lsize=16)

# ---------------------------------------------------------------- FPGA
FY = 1480
rect(30, FY, 2340, 1035, "#FFFFFF", stroke="#1F3A68", sw=4, rx=16)
t(46, FY + 32, "FPGA fabric · Cyclone V 5CSEBA6 · PlexOB core (fork of owenb's H.264 decoder)", 23, True, "#111111", "start")
t(46, FY + 60, "Timing (worst of four corners): all clocks ≥ 0 ns except pll_hdmi setup −0.166 ns (bar −0.3 ns)", 19, False, "#333333", "start")
t(2350, FY + 32, "41,033 / 41,910 ALMs (97.9%) · fitter seed 10", 23, True, "#111111", "end")
t(2350, FY + 60, "worst hold +0.092 ns · all fit gates PASS", 19, False, "#333333", "end")
# framework lane
group(60, FY + 80, 2280, 150, "MiSTer framework (sys, stock)", "#F6FFF6", "#3C8D3C")
block(80, FY + 120, 1360, 95, "f2sdram bridges", ["ram1 + ram2 into HPS DDR3 · clk_mem 90 MHz"], fill=C_MEM)
block(1470, FY + 120, 420, 95, "PLLs", ["sys_pll 27 / 90 / 96.43 MHz · vid_pll 81 MHz"], fill="#FFFFFF", lsize=15)
block(1915, FY + 120, 405, 95, "pll_hdmi · pll_audio", ["HDMI pixel clock · 48 kHz audio"], fill="#FFFFFF", lsize=15)

LY = FY + 255  # lanes top
LH = 755
group(60, LY, 650, LH, "HPS interface", "#F5F9FF", "#3A6EA5")
group(750, LY, 650, LH, "H.264 decode", "#FFFAF3", "#C07030")
group(1440, LY, 530, LH, "Presentation + video output", "#FBF7FF", A_VID)
group(2005, LY, 355, LH, "Audio", "#FFFDF0", A_AUD)
BY = LY + 70
# lane 1
block(90, BY, 590, 140, "ring_rd · record reader", ["reads AU + audio records on ram1", "writes the status block back"], fill=grad(C_SYS, C_MEM))
block(90, BY + 170, 590, 120, "hps_io (in the core)", ["OSD options, pads, keys, mouse"], fill=C_SYS)
block(90, BY + 320, 590, 130, "ring_in · input mirror", ["pads / keys / OSD events → words 48–71", "the daemon reads them for the menus"], fill=C_SYS)
block(90, BY + 480, 590, 150, "ring_stat · status block", ["heartbeat + telemetry each frame", "word 87: Y/C mode · word 88: build ID"], fill=grad(C_SYS, C_MEM))
# lane 2
block(780, BY, 590, 140, "h264_top · front end", ["NAL / slice parsing, entropy decode", "clk_dec 96.43 MHz"], fill=C_DEC)
block(780, BY + 170, 590, 150, "recon_top · reconstruction", ["prediction + residual → pictures", "DPB on ram1 (ddr_mux2)"], fill=grad(C_DEC, C_MEM))
block(780, BY + 350, 590, 130, "recon_out + disp_ctl", ["output order · frame pacing", "on the raster's refresh grid"], fill=grad(C_DEC, C_SYS))
block(780, BY + 510, 590, 120, "idle_bars", ["colour bars when no stream plays"], fill=C_VID)
# lane 3
block(1470, BY, 470, 140, "disp_reader", ["DPB → 720×480 / 720×576 raster", "reads on ram2"], fill=grad(C_VID, C_MEM))
block(1470, BY + 160, 470, 110, "ovl_plane · OSD overlay", ["fb0 canvas over the picture"], fill=grad(C_VID, C_MEM))
block(1470, BY + 290, 470, 120, "vid_out · raster", ["Auto / Interlaced / Progressive /", "Native (HDMI) · clk_vid 81 MHz"], fill=C_VID, lsize=15)
block(1470, BY + 430, 470, 95, "ascal scaler (sys)", ["pll_hdmi · setup −0.166 ns"], fill=C_VID)
block(1470, BY + 545, 470, 115, "yc_out (sys) · Y/C encoder", ["S-Video / composite · in the build", "planned: not yet tested on a screen"],
      fill="#FFF7F7", stroke=A_PLAN, sw=2.4, dash="10,6", tsize=20, lsize=15)
# lane 4
block(2030, BY, 305, 170, "aud_play", ["PCM records", "from the ring", "paced to the", "display clock"], fill=C_AUD)
block(2030, BY + 230, 305, 120, "audio_out (sys)", ["48 kHz AUDIO_L/R", "ALSA mixer off"], fill=C_AUD)

# ---------------------------------------------------------------- outputs
OY = 2565
block(1290, OY, 430, 150, "HDMI", ["MiSTer scaler; mode from MiSTer.ini", "Plex tiers 480i / 480p / 720p (no 240p)", "video + audio"], fill="#F2F2F2", stroke="#222222", sw=2.4, tsize=24, lsize=15)
block(1760, OY, 430, 150, "Analog out", ["OSD: RGB / S-Video / Composite", "planned: Y/C bench test pending", "(15 kHz)"],
      fill="#FFF7F7", stroke=A_PLAN, sw=2.6, dash="10,6", tsize=24, lsize=15)

block(60, OY, 1190, 150, "", [], fill="#FFFFFF", stroke="#1F3A68", sw=2)
t(80, OY + 32, "Shipped PlexOB fit (Quartus, fitter seed 10)", 21, True, "#111111", "start")
for i, ln in enumerate(["Logic: 41,033 / 41,910 ALMs (97.9%) · input mirror (ring_in) kept in the netlist: 601 ALUTs / 1,026 registers",
                        "Setup slack: pll_hdmi −0.166 ns (bar −0.3 ns, TNS −2.403) · worst other clock +0.197 ns · hold ≥ +0.092 ns",
                        "Fmax: clk_dec 104.87 / 104.62 MHz (check 101.4) · clk_vid 91.6 / 89.4 MHz (check 86.0) · 100 °C / −40 °C",
                        "Gates: timing gate, hdmi_pll_check, fmax_check, lint_undriven, netlist_canary all PASS"]):
    t(80, OY + 60 + 22 * i, ln, 16, False, "#333333", "start")

# ---------------------------------------------------------------- arrows
# senders -> receivers (control)
arrow([(345, 292), (420, 292)], A_CTL, 2.6); label(382, 270, "cast", A_CTL, 13)
arrow([(345, 515), (420, 515)], A_CTL, 2.6)
arrow([(345, 650), (420, 650)], A_CTL, 2.6)
# network media into the player (black)
arrow([(345, 395), (905, 395), (905, 450), (915, 450)], A_NET, 3.0, hs=16)
arrow([(345, 560), (888, 560), (888, 545), (915, 545)], A_NET, 3.0, hs=16)
arrow([(345, 715), (896, 715), (896, 500), (915, 500)], A_NET, 3.0, hs=16)
label(640, 395, "HLS / https media", A_NET, 14)
label(640, 715, "progressive or DASH https", A_NET, 14)
# daemon internal
arrow([(875, 320), (915, 320)], A_CTL, 2.4, hs=14)
arrow([(647, 700), (647, 730)], A_CTL, 2.4, hs=14)
arrow([(1140, 380), (1140, 410)], A_CTL, 2.4, hs=14)
arrow([(1140, 630), (1140, 600)], A_CTL, 2.4, hs=14)
arrow([(1140, 770), (1140, 800)], A_CTL, 2.4, hs=14)
# boot hook
arrow([(1910, 300), (1875, 300)], A_CTL, 2.6, hs=16); label(1892, 278, "start / stop", A_CTL, 13)
arrow([(2130, 511), (2130, 545)], A_CTL, 2.6, hs=16); label(2200, 528, "load_core", A_CTL, 13)
# pad -> Main (USB) route below the HPS box
arrow([(345, 905), (366, 905), (366, 1040), (2362, 1040), (2362, 680), (2350, 680)], A_CTL, 2.4, hs=14)
label(700, 1040, "USB pads + keyboard → Main_MiSTer", A_CTL, 14)
# daemon <-> DDR
arrow([(1378, 505), (1378, 1060 + 0)], A_DDR, 4.0, hs=18)
label(1378, 990, ["AU + PCM", "records"], A_DDR, 14)
arrow([(1140, 985), (1140, 1060)], A_DDR, 3.4, hs=18)
label(1060, 1020, "OSD canvas", A_DDR, 14)
arrow([(1625, 1060), (1625, 830)], A_CTL, 2.6, hs=16)
label(1625, 950, ["status · input", "· build ID"], A_CTL, 14)
# DDR -> FPGA
arrow([(1300, DDR_BOTTOM), (1300, FY + 120)], A_DDR, 5.0, both=True, hs=22)
label(1300, (DDR_BOTTOM + FY) / 2, "f2sdram", A_DDR, 14)
# Main -> hps_io (HPS_BUS)
arrow([(2350, 630), (2385, 630), (2385, FY + 242), (725, FY + 242), (725, BY + 230), (680, BY + 230)], A_CTL, 2.4, hs=16)
label(2330, 1450, ["HPS_BUS:", "OSD + input"], A_CTL, 13)
# f2sdram -> lanes
arrow([(250, FY + 215), (250, BY)], A_DDR, 3.6, both=True, hs=18)
arrow([(740, FY + 215), (740, BY + 245), (780, BY + 245)], A_DDR, 3.6, both=True, hs=18)
arrow([(1428, FY + 215), (1428, BY + 60), (1470, BY + 60)], A_DDR, 3.6, hs=18)
arrow([(1428, BY + 60), (1428, BY + 215), (1470, BY + 215)], A_DDR, 3.6, hs=18)
# HPS interface internals
arrow([(385, BY + 290), (385, BY + 320)], A_CTL, 2.4, hs=14)
# bitstream into decoder (blue)
arrow([(680, BY + 70), (780, BY + 70)], A_DATA, 3.4, hs=18); label(730, BY + 40, "ES bytes", A_DATA, 13)
arrow([(1075, BY + 140), (1075, BY + 170)], A_DATA, 3.4, hs=14)
arrow([(1075, BY + 320), (1075, BY + 350)], A_DATA, 3.0, hs=14)
# disp_ctl -> disp_reader (which picture)
arrow([(1370, BY + 415), (1414, BY + 415), (1414, BY + 110), (1470, BY + 110)], A_CTL, 2.4, hs=14)
# idle bars -> vid_out
arrow([(1370, BY + 570), (1388, BY + 570), (1388, BY + 380), (1470, BY + 380)], A_VID, 2.6, hs=14)
# video chain
arrow([(1705, BY + 140), (1705, BY + 160)], A_VID, 3.0, hs=12)
arrow([(1705, BY + 270), (1705, BY + 290)], A_VID, 3.0, hs=12)
arrow([(1705, BY + 410), (1705, BY + 430)], A_VID, 3.0, hs=12)
arrow([(1940, BY + 350), (1958, BY + 350), (1958, BY + 600), (1940, BY + 600)], A_VID, 2.6, hs=14)
arrow([(1470, BY + 478), (1455, BY + 478), (1455, OY)], A_VID, 3.4, hs=18)
arrow([(1840, BY + 660), (1840, OY)], A_VID, 3.0, dash="11,7", hs=18)
# audio path
arrow([(500, BY), (500, BY - 22), (2180, BY - 22), (2180, BY)], A_AUD, 2.8, hs=14); label(1210, BY - 22, "PCM records (via the reader's engine)", A_AUD, 13)
arrow([(2180, BY + 170), (2180, BY + 230)], A_AUD, 3.0, hs=14)
arrow([(2180, BY + 350), (2180, OY - 30), (1700, OY - 30), (1700, OY)], A_AUD, 2.8, hs=16)
arrow([(2120, OY - 30), (2120, OY)], A_AUD, 2.8, dash="6,4", hs=14)

flush_labels()

# ---------------------------------------------------------------- legend
LGY = 2760
rect(40, LGY, 2320, 330, "#FFFFFF", stroke="#555555", sw=1.6, rx=16)
t(56, LGY + 29, "Legend", 21, True, "#222222", "start")
leg = [(C_DEC, "clk_dec 96.43 MHz: H.264 decoder · Fmax 104.87 / 104.62 MHz (100 °C / −40 °C) vs 101.4 MHz check"),
       (C_MEM, "clk_mem 90 MHz: DDR3 ports (ram1, ram2) and DDR-side engines"),
       (C_SYS, "clk_sys 27 MHz: HPS interface, display control, audio tick"),
       (C_VID, "clk_vid 81 MHz raster · Fmax 91.6 / 89.4 MHz vs 86.0 MHz check · pll_hdmi (scaler)"),
       (C_AUD, "audio (48 kHz)"),
       (C_ARM, "ARM / Linux software")]
for i, (c, s) in enumerate(leg):
    yy = LGY + 60 + 38 * i
    rect(70, yy, 70, 28, c, sw=1, rx=5)
    t(155, yy + 21, s, 18, False, "#1a1a1a", "start")
yy = LGY + 60 + 38 * len(leg)
rect(70, yy, 70, 28, grad(C_VID, C_MEM), sw=1, rx=5)
t(155, yy + 21, "two colours = block spans both clock domains", 18, False, "#1a1a1a", "start")
arrs = [(A_NET, "network media: HLS / https video + audio", None), (A_DDR, "DDR3 traffic (ARM /dev/mem, FPGA f2sdram)", None),
        (A_DATA, "H.264 data inside the FPGA", None), (A_CTL, "control, status, input, discovery", None),
        (A_VID, "video", None), (A_AUD, "audio", None), (A_PLAN, "planned / not yet proven on glass", "11,7")]
for i, (c, s, d) in enumerate(arrs):
    yy = LGY + 74 + 36 * i
    arrow([(1300, yy), (1380, yy)], c, 3.2, dash=d, hs=18)
    t(1400, yy + 7, s, 18, False, "#1a1a1a", "start")
t(1200, H - 30, "Source: MisterPlex rc5 tree (daemon, boot hook, PlexOB RTL) · fit + timing: Quartus reports of the shipped PlexOB core, seed 10 · "
                "see VERIFY.md", 19, True, "#222222")
out.append("</svg>")
open("docs/architecture.svg", "w").write("\n".join(out) + "\n")
print("ok", len(out))
