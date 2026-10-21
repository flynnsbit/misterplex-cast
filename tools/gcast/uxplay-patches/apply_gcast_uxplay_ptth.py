#!/usr/bin/env python3
"""FIELD-1018 F2: UxPlay reverse-HTTP (PTTH/1.0) fix. Idempotent.

iOS AirPlay (Photos, Safari, YouTube, Plex HLS) opens POST /reverse (Upgrade: PTTH/1.0), and the receiver then sends
POST /event (FCUP Request for the HLS playlists) down that socket. The iPhone answers "HTTP/1.1 200 OK". UxPlay
4e95a9e only recognises a client response on a connection's first 8 bytes (staging_len < 8), and the POST /reverse
already used them, so the answer went into llhttp: HPE_INVALID_METHOD, the reverse socket closed, and the iPhone gave up
(a lab MiSTer 10:2x CT: "airplay: httpd error in parsing: HPE_INVALID_METHOD / HTTP/1.1 200 OK"). The PTTH branch meant to
mark every new request on that socket as a reverse response called the getter http_request_is_reverse (a no-op)
instead of http_request_set_reverse. Mutant: env GCAST_MUT_PTTH=1 leaves the tree unpatched (the test proves the bug)."""
import os, pathlib, sys
root = pathlib.Path(sys.argv[1])
c = root / "lib" / "httpd.c"
t = c.read_text()
old = ("                if (connection->type == CONNECTION_TYPE_PTTH) {\n"
       "                    http_request_is_reverse(connection->request);\n")
new = ("                if (connection->type == CONNECTION_TYPE_PTTH) {\n"
       "                    http_request_set_reverse(connection->request);   /* GCAST F2: was the getter (no-op) */\n")
if os.environ.get("GCAST_MUT_PTTH") == "1":
    print("MUTANT: PTTH fix not applied")
elif old in t:
    c.write_text(t.replace(old, new, 1)); print("patched httpd PTTH reverse responses")
elif "GCAST F2" in t:
    print("httpd PTTH already patched")
else:
    print("WARN: httpd PTTH site not found", file=sys.stderr); sys.exit(1)
