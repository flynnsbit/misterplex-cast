#!/usr/bin/env python3
"""AirPlay /play: map X's twitterav://video-s.twimg.com/... to https://. Idempotent.

lab 2026-11-10 19:14 CT: after a swipe in the X app every /play arrived as
    Content-Location: twitterav://video-s.twimg.com/amplify_video/.../pl/....m3u8
and stock UxPlay 4e95a9e logged "Content-Location has unsupported form",
answered 400 and reset the connection, so nothing played. The CDN serves the
same path over https. Only that exact host is rewritten (gcast_twitterav.h);
every other twitterav:// host still takes the stock reject path.

The rewrite runs right after Content-Location is read and before the
"/master.m3u8" suffix test and the http/https branch. Applied by
scripts/build_arm_airplay.sh after apply_gcast_uxplay_handoff.py.
Mutant: GCAST_MUT_TWITTERAV=1 leaves the tree stock.
"""
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
MARK = "GCAST twitterav: rewrite"
FUNC_ANCHOR = "static void\nhttp_handler_play("
CALL_ANCHOR = '    /* we support HLS playlists if the playback location is terminated by "/master.m3u8"'
CALL = (
    "    /* " + MARK + " twitterav://video-s.twimg.com/ to https:// (gcast_twitterav.h) */\n"
    "    {\n"
    "        int gcast_tw = gcast_twitterav_rewrite(playback_location);\n"
    "        if (gcast_tw > 0)\n"
    "            logger_log(raop->logger, LOGGER_INFO, \"GCAST twitterav location rewritten to https\");\n"
    "        else if (gcast_tw < 0) {\n"
    "            logger_log(raop->logger, LOGGER_ERR, \"GCAST twitterav location refused (host is not video-s.twimg.com)\");\n"
    "            /* browse-lane #627 P3-1: let the daemon refuse it (415 + format notice on the TV) instead of a silent 400 */\n"
    "            int gcast_st = 0;\n"
    "            if (raop->callbacks.on_video_play)\n"
    "                gcast_st = raop->callbacks.on_video_play(raop->callbacks.cls, playback_location, 0.0f);\n"
    "            if (gcast_st == 0)\n"
    "                goto play_error;\n"
    "            http_response_init(response, \"HTTP/1.1\", gcast_st == 400 ? 400 : 415,\n"
    "                               gcast_st == 400 ? \"Bad Request\" : \"Unsupported Media Type\");\n"
    "            http_response_set_disconnect(response, 1);\n"
    "            plist_mem_free(playback_location);\n"
    "            if (req_root_node)\n"
    "                plist_free(req_root_node);\n"
    "            return;\n"
    "        }\n"
    "    }\n"
)


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: apply_gcast_uxplay_twitterav.py UXPLAY_SRC", file=sys.stderr)
        return 2
    if os.environ.get("GCAST_MUT_TWITTERAV") == "1":
        print("MUTANT: twitterav rewrite not applied")
        return 0
    handlers = pathlib.Path(sys.argv[1]) / "lib" / "http_handlers.h"
    t = handlers.read_text()
    if MARK in t:
        print("twitterav rewrite already patched")
        return 0
    func = (HERE / "gcast_twitterav.h").read_text()
    if FUNC_ANCHOR not in t:
        print("WARN: http_handler_play definition not found", file=sys.stderr)
        return 1
    if t.count(CALL_ANCHOR) != 1:
        print("WARN: /master.m3u8 comment anchor not found exactly once", file=sys.stderr)
        return 1
    t = t.replace(FUNC_ANCHOR, func + "\n" + FUNC_ANCHOR, 1)
    t = t.replace(CALL_ANCHOR, CALL + CALL_ANCHOR, 1)
    handlers.write_text(t)
    print("patched twitterav rewrite")
    return 0


if __name__ == "__main__":
    sys.exit(main())
