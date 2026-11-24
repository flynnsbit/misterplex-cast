#!/usr/bin/env python3
"""GH#8 / FIELD-1018 F2: UxPlay reverse-HTTP (PTTH/1.0) leftover. Idempotent.

iOS AirPlay opens POST /reverse (Upgrade: PTTH/1.0). CAST origin/main previously
only swapped http_request_is_reverse (a no-op getter) for http_request_set_reverse.
That is not enough:

1. GET /server-info and POST /reverse are HTTP/1.1 with Host and no CSeq. Stock
   UxPlay marks them HLS (`hls_request = host && !cseq && !client_session_id`)
   so http_handler_reverse never runs → HTTP 404 then hang (lab  lab device d3155a4a).
2. Stock reverse handler set-then-count: a second /reverse is typed PTTH with
   no http_response_init (404/hang). Glass grants 101 only when count==0, else 409.
3. staging_len leftover from POST /reverse feeds "HTTP/1.1 200 OK" to llhttp
   (HPE_INVALID_METHOD / SIGSEGV). Reset staging_len after set_reverse.
4. reverse-before-/info with X-Apple-Session-ID asserted against NULL → SIGSEGV.
5. INFO 101 finish: utils_data_to_text(NULL) / strchr-no-NL → SIGSEGV.

Mutant: GCAST_MUT_PTTH=1 leaves the tree unpatched (airplay_ptth_repro.sh).
Never  lab device.
"""
import os
import pathlib
import sys


def sub(path: pathlib.Path, old: str, new: str, already: str, name: str) -> None:
    t = path.read_text()
    if already in t:
        print("%s already patched" % name)
        return
    if old not in t:
        print("WARN: %s site not found" % name, file=sys.stderr)
        sys.exit(1)
    path.write_text(t.replace(old, new, 1))
    print("patched %s" % name)


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: apply_gcast_uxplay_ptth.py UXPLAY_SRC", file=sys.stderr)
        sys.exit(2)
    root = pathlib.Path(sys.argv[1])
    if os.environ.get("GCAST_MUT_PTTH") == "1":
        print("MUTANT: PTTH fix not applied")
        return

    httpd = root / "lib" / "httpd.c"
    handlers = root / "lib" / "http_handlers.h"
    raop = root / "lib" / "raop.c"
    raop_h = root / "lib" / "raop_handlers.h"

    # --- httpd.c: setter + clear staging_len (upgrade the F2-only leftover) ---
    ht = httpd.read_text()
    weak = (
        "                if (connection->type == CONNECTION_TYPE_PTTH) {\n"
        "                    http_request_set_reverse(connection->request);   /* GCAST F2: was the getter (no-op) */\n"
        "                }\n"
    )
    full = (
        "                if (connection->type == CONNECTION_TYPE_PTTH) {\n"
        "                    /* GCAST F2: getter was a no-op; leftover staging_len==8 from\n"
        "                       POST /reverse skipped the HTTP/1.1 prefix check and fed\n"
        "                       \"HTTP/1.1 200 OK\" to llhttp (HPE_INVALID_METHOD / SIGSEGV). */\n"
        "                    http_request_set_reverse(connection->request);\n"
        "                    connection->staging_len = 0;\n"
        "                }\n"
    )
    stock = (
        "                if (connection->type == CONNECTION_TYPE_PTTH) {\n"
        "                    http_request_is_reverse(connection->request);\n"
        "                }\n"
    )
    if full in ht:
        print("httpd PTTH already patched")
    elif weak in ht:
        httpd.write_text(ht.replace(weak, full, 1))
        print("patched httpd PTTH reverse responses (upgraded F2 leftover)")
    elif stock in ht:
        httpd.write_text(ht.replace(stock, full, 1))
        print("patched httpd PTTH reverse responses")
    else:
        print("WARN: httpd PTTH site not found", file=sys.stderr)
        sys.exit(1)

    # --- http_handlers.h: 101 only on first reverse, 409 on second ---
    sub(
        handlers,
        old=(
            "    logger_log(raop->logger, LOGGER_INFO, \"client requested reverse connection: %s; purpose: %s  \\\"%s\\\"\",\n"
            "               connection, upgrade, purpose);\n"
            "\n"
            "    httpd_set_connection_type(raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);\n"
            "    int type_PTTH = httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_PTTH);\n"
            "\n"
            "    if (type_PTTH == 1) {\n"
            "        logger_log(raop->logger, LOGGER_DEBUG, \"will use socket %d for %s connections\", socket_fd, purpose);\n"
            "        http_response_init(response, \"HTTP/1.1\", 101, \"Switching Protocols\");\n"
            "        http_response_add_header(response, \"Connection\", \"Upgrade\");\n"
            "        http_response_add_header(response, \"Upgrade\", \"PTTH/1.0\");\n"
            "    } else {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"multiple TPPH connections (%d) are forbidden\", type_PTTH );\n"
            "    }    \n"
        ),
        new=(
            "    logger_log(raop->logger, LOGGER_INFO, \"client requested reverse connection: %s; purpose: %s  \\\"%s\\\"\",\n"
            "               connection ? connection : \"\", upgrade ? upgrade : \"\", purpose ? purpose : \"\");\n"
            "\n"
            "    /* GH#8 reverse-twice: mark PTTH only when granting 101. The old\n"
            "       set-then-count left a second socket typed PTTH with no upgrade. */\n"
            "    int type_PTTH = httpd_count_connection_type(raop->httpd, CONNECTION_TYPE_PTTH);\n"
            "    if (type_PTTH == 0) {\n"
            "        httpd_set_connection_type(raop->httpd, (void *) conn, CONNECTION_TYPE_PTTH);\n"
            "        logger_log(raop->logger, LOGGER_DEBUG, \"will use socket %d for %s connections\", socket_fd, purpose);\n"
            "        http_response_init(response, \"HTTP/1.1\", 101, \"Switching Protocols\");\n"
            "        http_response_add_header(response, \"Connection\", \"Upgrade\");\n"
            "        http_response_add_header(response, \"Upgrade\", \"PTTH/1.0\");\n"
            "    } else {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"multiple TPPH connections (%d) are forbidden\", type_PTTH + 1);\n"
            "        http_response_init(response, \"HTTP/1.1\", 409, \"Conflict\");\n"
            "    }\n"
        ),
        already="GH#8 reverse-twice",
        name="http_handler_reverse 101/409",
    )

    # --- raop.c: do not early-return HTTP/1.1 /reverse with no CSeq ---
    sub(
        raop,
        old=(
            " /* this rejects messages from _airplay._tcp for video streaming protocol unless bool raop->hls_support is true*/   \n"
            "    if (!cseq && !raop->hls_support && !ble) {\n"
        ),
        new=(
            " /* this rejects messages from _airplay._tcp for video streaming protocol unless bool raop->hls_support is true*/\n"
            "    /* GH#8: iPhone POST /reverse is HTTP/1.1 with no CSeq. The old return left\n"
            "       no response (\"httpd didn't get response\") so EVENT/1.0 200 OK on the\n"
            "       same socket became HPE_INVALID_METHOD. /reverse must be safe even when\n"
            "       the HLS event channel is unused. */\n"
            "    bool http_airplay = (protocol && !strcmp(protocol, \"HTTP/1.1\"));\n"
            "    if (!cseq && !raop->hls_support && !ble && !http_airplay) {\n"
        ),
        already="bool http_airplay = (protocol && !strcmp(protocol, \"HTTP/1.1\"))",
        name="raop HTTP/1.1 /reverse no-CSeq",
    )

    sub(
        raop,
        old="    hls_request =  (host && !cseq && !client_session_id);\n",
        new=(
            "    /* GH#8 lab  lab device: iPhone POST /reverse and GET /server-info are HTTP/1.1\n"
            "       with Host and no CSeq. The HLS test stole them (404) so\n"
            "       http_handler_reverse never ran. Control URLs are not localhost HLS. */\n"
            "    bool airplay_ctrl = url && (\n"
            "        !strcmp(url, \"/reverse\") || !strcmp(url, \"/play\") || !strcmp(url, \"/stop\") ||\n"
            "        !strcmp(url, \"/server-info\") || !strcmp(url, \"/playback-info\") ||\n"
            "        !strcmp(url, \"/action\") || !strcmp(url, \"/fp-setup2\") ||\n"
            "        !strncmp(url, \"/scrub?\", 7) || !strncmp(url, \"/rate?\", 6) ||\n"
            "        !strncmp(url, \"/getProperty?\", 13) || !strncmp(url, \"/setProperty?\", 13));\n"
            "    hls_request = (host && !cseq && !client_session_id && !airplay_ctrl);\n"
        ),
        already="bool airplay_ctrl = url && (",
        name="raop airplay_ctrl not HLS",
    )

    sub(
        raop,
        old=(
            "    if (client_session_id) {\n"
            "        assert(!strcmp(client_session_id, conn->client_session_id));\n"
            "    }\n"
        ),
        new=(
            "    /* GH#8: reverse-before-/info and a second reverse can arrive with\n"
            "       X-Apple-Session-ID before conn->client_session_id is filled. The\n"
            "       old assert(!strcmp(..., NULL)) was SIGSEGV 11. */\n"
            "    if (client_session_id) {\n"
            "        if (!conn->client_session_id) {\n"
            "            conn->client_session_id = calloc(strlen(client_session_id) + 1, sizeof(char));\n"
            "            if (conn->client_session_id)\n"
            "                memcpy(conn->client_session_id, client_session_id, strlen(client_session_id));\n"
            "        } else if (strcmp(client_session_id, conn->client_session_id) != 0) {\n"
            "            logger_log(raop->logger, LOGGER_WARNING, \"X-Apple-Session-ID mismatch on connection\");\n"
            "        }\n"
            "    }\n"
        ),
        already="X-Apple-Session-ID mismatch on connection",
        name="raop session-id null assert",
    )

    sub(
        raop,
        old=(
            "    header_str =  utils_data_to_text(data, len);\n"
            "    bool data_is_plist = (strstr(header_str,\"apple-binary-plist\") != NULL);\n"
            "    bool data_is_text = (strstr(header_str,\"text/\") != NULL ||\n"
            "                         strstr(header_str, \"x-mpegURL\") != NULL);\n"
            "\n"
            "    if (!logger_debug) {\n"
            "        char *ptr = strchr(header_str, '\\n');\n"
            "        *(++ptr) = '\\0';\n"
            "    }\n"
            "    logger_log(raop->logger, LOGGER_DEBUG, \"%s\", header_str);\n"
            "    free(header_str);\n"
        ),
        new=(
            "    /* GH#8: INFO 101 finish. utils_data_to_text(NULL/neg) and strchr-no-NL\n"
            "       were SIGSEGV 11 after \"client requested reverse connection\" (rc5). */\n"
            "    if (len < 0) {\n"
            "        len = 0;\n"
            "    }\n"
            "    header_str = (data && len > 0) ? utils_data_to_text(data, len) : NULL;\n"
            "    bool data_is_plist = (header_str && strstr(header_str,\"apple-binary-plist\") != NULL);\n"
            "    bool data_is_text = (header_str && (strstr(header_str,\"text/\") != NULL ||\n"
            "                         strstr(header_str, \"x-mpegURL\") != NULL));\n"
            "\n"
            "    if (header_str && !logger_debug) {\n"
            "        char *ptr = strchr(header_str, '\\n');\n"
            "        if (ptr && ptr[1] != '\\0') {\n"
            "            *(++ptr) = '\\0';\n"
            "        }\n"
            "    }\n"
            "    if (header_str) {\n"
            "        logger_log(raop->logger, LOGGER_DEBUG, \"%s\", header_str);\n"
            "        free(header_str);\n"
            "        header_str = NULL;\n"
            "    }\n"
        ),
        already="INFO 101 finish",
        name="raop response header null-safe",
    )

    # Volume NULL is covered by apply_gcast_uxplay_hardening.py (_gcast_vol) when
    # that runs first. Keep a GH#8-shaped patch for a bare UxPlay tree.
    rh = raop_h.read_text()
    if "_gcast_vol" in rh or "GH#8 GET /info: audio_set_client_volume unset" in rh:
        print("raop_handlers initialVolume already null-safe")
    else:
        sub(
            raop_h,
            old=(
                "    plist_t initial_volume_node = plist_new_real(raop->callbacks.audio_set_client_volume(raop->callbacks.cls)); \n"
            ),
            new=(
                "    /* GH#8: reverse-before-/info RTSP GET /info with CSeq. A HLS-only\n"
                "       receiver (gcast) leaves audio_set_client_volume NULL → SIGSEGV. */\n"
                "    double initial_volume = 0.0;\n"
                "    if (raop->callbacks.audio_set_client_volume) {\n"
                "        initial_volume = raop->callbacks.audio_set_client_volume(raop->callbacks.cls);\n"
                "    } else {\n"
                "        logger_log(raop->logger, LOGGER_DEBUG,\n"
                "                   \"GH#8 GET /info: audio_set_client_volume unset, initialVolume=0\");\n"
                "    }\n"
                "    plist_t initial_volume_node = plist_new_real(initial_volume); \n"
            ),
            already="GH#8 GET /info: audio_set_client_volume unset",
            name="raop_handlers initialVolume null",
        )


if __name__ == "__main__":
    main()
