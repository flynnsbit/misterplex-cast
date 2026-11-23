#!/usr/bin/env python3
"""RC-1027: refuse AirPlay-port requests that carry an Origin header (a browser page). Idempotent.

AirPlay senders (iOS, macOS, AirPlay-capable apps) never send Origin; a web page POSTing to the receiver port
(CSRF / DNS-rebinding style) always does. conn_request answers 403 and disconnects before CSeq, the session
check or any handler runs. GCAST_MUT_ORIGIN=1 leaves the tree unpatched (test mutant).
Touches lib/http_request.{c,h} (case-insensitive presence helper) and lib/raop.c (the check). Applied by
scripts/build_arm_airplay.sh onto UxPlay lib/.
"""
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
if os.environ.get("AIRPLAY_MUT_ORIGIN") or os.environ.get("GCAST_MUT_ORIGIN"):
    print("MUTANT: origin patch not applied")
    sys.exit(0)

hh = root / "lib" / "http_request.h"
t = hh.read_text()
decl = "const char *http_request_get_header(http_request_t *request, const char *name);"
if "http_request_has_header_ci" in t:
    print("http_request.h already patched")
elif decl in t:
    hh.write_text(t.replace(decl, decl + "\nint http_request_has_header_ci(http_request_t *request, const char *name); /* GCAST */", 1))
    print("patched http_request.h")
else:
    print("WARN: http_request_get_header decl not found", file=sys.stderr)
    sys.exit(1)

hc = root / "lib" / "http_request.c"
t = hc.read_text()
helper = '''
/* GCAST: 1 if a request header named `name` is present (case-insensitive), else 0. */
int
http_request_has_header_ci(http_request_t *request, const char *name)
{
    if (!request || !name || request->is_reverse || !request->headers || request->headers_index < 0) {
        return 0;
    }
    for (int i = 0; i <= request->headers_index; i += 2) {
        if (request->headers[i] && !strcasecmp(request->headers[i], name)) return 1;
    }
    return 0;
}
'''
if "#include <strings.h>" not in t:
    t = t.replace("#include <string.h>\n", "#include <string.h>\n#include <strings.h>\n", 1)
anchor = "size_t\nhttp_request_header_get_size("
if "http_request_has_header_ci" in t:
    print("http_request.c already patched")
elif anchor in t:
    hc.write_text(t.replace(anchor, helper.lstrip("\n") + "\n" + anchor, 1))
    print("patched http_request.c")
else:
    print("WARN: http_request_header_get_size anchor not found", file=sys.stderr)
    sys.exit(1)

rc = root / "lib" / "raop.c"
t = rc.read_text()
anchor = "    /* handle CSeq header carefully, as it will be included in response: value should be non-negative int */"
check = '''    /* GCAST RC-1027: AirPlay senders never send Origin; a browser page always does. Refuse before any handler. */
    if (http_request_has_header_ci(request, "Origin")) {
        logger_log(raop->logger, LOGGER_ERR, "rejecting request carrying an Origin header (browser page)");
        *response = http_response_create();
        http_response_init(*response, protocol, 403, "Forbidden");
        http_response_finish(*response, NULL, 0);
        http_response_set_disconnect(*response, 1);
        return;
    }

'''
if "carrying an Origin header" in t:
    print("raop.c already patched")
elif anchor in t:
    rc.write_text(t.replace(anchor, check + anchor, 1))
    print("patched raop.c conn_request Origin refusal")
else:
    print("WARN: conn_request CSeq anchor not found", file=sys.stderr)
    sys.exit(1)
