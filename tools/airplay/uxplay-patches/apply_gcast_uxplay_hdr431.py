#!/usr/bin/env python3
"""GCAST: finish the 431 overlong-header response and close that connection. Idempotent.

Stock UxPlay 4e95a9e conn_request answers 431 Request Header Fields Too Large and
returns without http_response_finish or http_response_set_disconnect. httpd then
calls http_response_get_data, which asserts response->complete (abort) — or, with
NDEBUG, sends an unfinished buffer and leaves the socket up. Finish the response
and mark disconnect so the client gets 431 and that connection is closed.

Mutant: GCAST_MUT_HDR431=1 leaves the tree unpatched.
Applied by scripts/build_arm_airplay.sh onto UxPlay lib/.
"""
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
if os.environ.get("GCAST_MUT_HDR431"):
    print("MUTANT: hdr431 finish/disconnect not applied")
    sys.exit(0)

rc = root / "lib" / "raop.c"
t = rc.read_text()
old = (
    '        *response = http_response_create();\n'
    '        http_response_init(*response, protocol, 431, "Request Header Fields Too Large");\n'
    '        return;\n'
)
new = (
    '        *response = http_response_create();\n'
    '        http_response_init(*response, protocol, 431, "Request Header Fields Too Large");\n'
    '        /* GCAST: finish + disconnect. Stock returned here with complete==0;\n'
    '           httpd http_response_get_data then asserted and aborted the daemon. */\n'
    '        http_response_finish(*response, NULL, 0);\n'
    '        http_response_set_disconnect(*response, 1);\n'
    '        return;\n'
)
if "GCAST: finish + disconnect" in t:
    print("raop.c 431 already patched")
elif old in t:
    rc.write_text(t.replace(old, new, 1))
    print("patched raop.c 431 finish/disconnect")
else:
    print("WARN: raop.c 431 site not found", file=sys.stderr)
    sys.exit(1)
