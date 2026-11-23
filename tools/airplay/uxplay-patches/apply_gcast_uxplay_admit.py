#!/usr/bin/env python3
"""AirPlay /play returns the admit status, and /info advertises a 720p display.

Idempotent. Applied by scripts/build_arm_airplay.sh onto UxPlay lib/.
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])

raop_h = root / "lib" / "raop.h"
ht = raop_h.read_text()
old_cb = "    void  (*on_video_play) (void *cls, const char *location, const float start_position);"
new_cb = "    int   (*on_video_play) (void *cls, const char *location, const float start_position); /* 0 play, 400/415 refuse */"
if old_cb in ht:
    raop_h.write_text(ht.replace(old_cb, new_cb, 1))
    print("patched on_video_play return")
elif "0 play, 400/415 refuse" in ht:
    print("on_video_play return already patched")
else:
    print("WARN: on_video_play typedef not found", file=sys.stderr)
    sys.exit(1)

handlers = root / "lib" / "http_handlers.h"
hh = handlers.read_text()
helper = """
/* GCAST: /play admit. Nonzero is an HTTP error the sender surfaces. 0 plays. */
static void gcast_video_admit(raop_t *raop, http_response_t *response, const char *location, float start) {
    int admit = 0;
    if (raop && raop->callbacks.on_video_play)
        admit = raop->callbacks.on_video_play(raop->callbacks.cls, location, start);
    if (admit == 0)
        return;
    int status = admit == 400 ? 400 : 415;
    http_response_init(response, "HTTP/1.1", status,
                       status == 400 ? "Bad Request" : "Unsupported Media Type");
    http_response_set_disconnect(response, 1);
}

"""
if "gcast_video_admit" not in hh:
    needle = "static void\nhttp_handler_server_info("
    if needle not in hh:
        print("WARN: http_handler_server_info not found", file=sys.stderr)
        sys.exit(1)
    hh = hh.replace(needle, helper + needle, 1)
    print("patched gcast_video_admit helper")
else:
    print("gcast_video_admit already present")

calls = [
    (
        """            raop->callbacks.on_video_play(raop->callbacks.cls,
                                                get_playback_location(airplay_video),
                                                get_start_position_seconds(airplay_video));""",
        """            gcast_video_admit(raop, response, get_playback_location(airplay_video),
                              get_start_position_seconds(airplay_video));""",
    ),
    (
        """        raop->callbacks.on_video_play(raop->callbacks.cls,
                                      get_playback_location(airplay_video),
                                      resume_pos > start_pos ? resume_pos : start_pos);""",
        """        gcast_video_admit(raop, response, get_playback_location(airplay_video),
                          resume_pos > start_pos ? resume_pos : start_pos);""",
    ),
    (
        """        raop->callbacks.on_video_play(raop->callbacks.cls,
                                      get_playback_location(airplay_video),
                                      start_position_seconds);""",
        """        gcast_video_admit(raop, response, get_playback_location(airplay_video),
                          start_position_seconds);""",
    ),
]
for old, new in calls:
    if old in hh:
        hh = hh.replace(old, new, 1)
        print("patched one on_video_play call")
    elif new in hh:
        print("on_video_play call already patched")
    else:
        print("WARN: on_video_play call site not found", file=sys.stderr)
        sys.exit(1)
handlers.write_text(hh)

info = root / "lib" / "raop_handlers.h"
it = info.read_text()
old_feat = "    plist_t displays_0_features = plist_new_uint(14);"
new_feat = (
    "    /* gcast: 2 is 720p only. 14 (2|4|8) is the stock mask UxPlay sends with a 1080p display. */\n"
    "    plist_t displays_0_features = plist_new_uint(2);"
)
if old_feat in it:
    info.write_text(it.replace(old_feat, new_feat, 1))
    print("patched display features to 720p")
elif "plist_new_uint(2);" in it and "720p only" in it:
    print("display features already 720p")
else:
    print("WARN: display features site not found", file=sys.stderr)
    sys.exit(1)
