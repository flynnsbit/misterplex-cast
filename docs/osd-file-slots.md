# OSD file slots

MiSTer `CONF_STR` file entries are `F<idx>,<ext>,<label>;`. The extension list is parsed in fixed 3-character chunks by `Main_MiSTer` (`ScanDirectory` copies three bytes at a time and advances `ext += 3`), so a four-character `.h264` suffix cannot match a single F slot. Plex uses:

- `F1,raw,RGB565 frame (320x240);`
- `F2,raw,s16le stereo PCM @48k;`
- `F3,264,H.264 annex-B elementary;`

`F` entries and `J1` controller labels are metadata; they do not consume `status[]` bits. The OSD v3 bit layout in `host/libmisterplex/osd_menu.hpp` is unchanged, and `v,6` does not need a bump for these fixes.
