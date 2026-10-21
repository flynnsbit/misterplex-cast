#!/usr/bin/env python3
"""FIELD-1018 F2b: defensive guard for UxPlay's "deleting playlist ... duration 0" sweep in POST /play. Idempotent.
A new /play deletes every stored playlist shorter than MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS. A live/unknown or
refused HLS playlist has duration 0, so the sweep also freed the playlist of the session still being torn down (the
one the HLS server / gcast-xcode may still be reading and current_video points at). The guard keeps the current
playlist out of the sweep; the slot is reused or destroyed by the normal paths. Mutant: GCAST_MUT_ZERODUR=1."""
import os, pathlib, sys
c = pathlib.Path(sys.argv[1]) / "lib" / "http_handlers.h"
t = c.read_text()
old = "            if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS ) { //likely to be an advertisement\n"
new = ("            if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS && i == raop->current_video)\n"
       "                logger_log(raop->logger, LOGGER_INFO, \"GCAST F2b: keeping current playlist %s (duration %f) out of the sweep\",\n"
       "                           get_playback_uuid(raop->airplay_video[i]), duration);\n"
       "            if (duration < (float) MIN_STORED_AIRPLAY_VIDEO_DURATION_SECONDS &&\n"
       "                i != raop->current_video) {   /* GCAST F2b: never sweep the live one (it is counted instead) */\n")
if os.environ.get("GCAST_MUT_ZERODUR") == "1":
    print("MUTANT: zero-duration guard not applied")
elif old in t:
    c.write_text(t.replace(old, new, 1)); print("patched /play zero-duration sweep guard")
elif "GCAST F2b" in t:
    print("zero-duration guard already patched")
else:
    print("WARN: zero-duration sweep site not found", file=sys.stderr); sys.exit(1)
