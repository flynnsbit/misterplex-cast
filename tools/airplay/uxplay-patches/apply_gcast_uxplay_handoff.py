#!/usr/bin/env python3
"""AirPlay→YouTube handoff: no SIGSEGV on playlist delete. Idempotent.

lab 2026-10-28: AirPlay from X, then a YouTube cast. The log line immediately
before misterplexd FATAL signal 11 is

    airplay: deleting playlist playback_uuid … duration (seconds) 0.000000

Two stock UxPlay holes on that path (UxPlay 4e95a9e):

1. playlistRemove clears current_video but keeps the duration-0 direct-URL
   playlist. The next /play sweeps it (the log line) and, for a non-http
   master URL (YouTube FCUP, clientProcName YouTube), asks the phone for the
   master playlist. select_master_playlist_language then strlen's
   lang_system, which misterplexd never sets. SIGSEGV 11. No /play URL is
   logged, so the YouTube id never reaches lounge/extract.
2. Content-Location present but not a string (plist_get_string_val leaves the
   out pointer NULL) is strstr(NULL) immediately after the same delete log.
   A non-string clientProcName on a /master.m3u8 play is strstr's second
   argument NULL (the fresh-process reverse reconnects, no delete log).

A bad /play or FCUP answer becomes HTTP 400 and the receiver stays up, so a
following DIAL/lounge command can run. Mutant: GCAST_MUT_HANDOFF=1.
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
        print("usage: apply_gcast_uxplay_handoff.py UXPLAY_SRC", file=sys.stderr)
        sys.exit(2)
    if os.environ.get("GCAST_MUT_HANDOFF") == "1":
        print("MUTANT: AirPlay handoff guards not applied")
        return
    root = pathlib.Path(sys.argv[1])
    video = root / "lib" / "airplay_video.c"
    handlers = root / "lib" / "http_handlers.h"
    raop = root / "lib" / "raop.c"

    sub(
        video,
        old=(
            "char * select_master_playlist_language(airplay_video_t *airplay_video, char *master_playlist) {\n"
            "    assert(master_playlist);\n"
        ),
        new=(
            "char * select_master_playlist_language(airplay_video_t *airplay_video, char *master_playlist) {\n"
            "    assert(master_playlist);\n"
            "    /* GCAST handoff: lang_system is unset (misterplexd never calls raop_set_lang).\n"
            "       Stock slicer strlen(NULL) is SIGSEGV 11 on the FCUP master playlist that\n"
            "       follows \"deleting playlist\" when AirPlay hands off to YouTube. Keep the\n"
            "       playlist unchanged so the cast can reach the player or fail as HTTP 400. */\n"
            "    if (!airplay_video || !airplay_video->lang_system) {\n"
            "        return master_playlist;\n"
            "    }\n"
        ),
        already="GCAST handoff: lang_system is unset",
        name="master playlist language NULL lang_system",
    )

    sub(
        video,
        old=(
            "void\n"
            "airplay_video_destroy(airplay_video_t *airplay_video) {\n"
            "    if (airplay_video->apple_session_id) {\n"
        ),
        new=(
            "void\n"
            "airplay_video_destroy(airplay_video_t *airplay_video) {\n"
            "    if (!airplay_video) { /* GCAST handoff: full-table sweep may pass NULL */\n"
            "        return;\n"
            "    }\n"
            "    if (airplay_video->apple_session_id) {\n"
        ),
        already="GCAST handoff: full-table sweep may pass NULL",
        name="airplay_video_destroy NULL",
    )

    sub(
        video,
        old="void set_apple_session_id(airplay_video_t *airplay_video, const char * apple_session_id, size_t len) {\n"
            "    assert(apple_session_id && len == 36);\n",
        new="void set_apple_session_id(airplay_video_t *airplay_video, const char * apple_session_id, size_t len) {\n"
            "    if (!airplay_video || !apple_session_id || len == 0 || len > 256) { /* GCAST handoff: not assert/SIGABRT */\n"
            "        return;\n"
            "    }\n",
        already="GCAST handoff: not assert/SIGABRT",
        name="set_apple_session_id no abort",
    )

    sub(
        video,
        old="void set_playback_uuid(airplay_video_t *airplay_video, const char *playback_uuid, size_t len) {\n"
            "    assert(playback_uuid && len == 36);\n",
        new="void set_playback_uuid(airplay_video_t *airplay_video, const char *playback_uuid, size_t len) {\n"
            "    if (!airplay_video || !playback_uuid || len == 0 || len > 256) { /* GCAST handoff: not assert/SIGABRT */\n"
            "        return;\n"
            "    }\n",
        already="GCAST handoff: not assert/SIGABRT */\n"
            "        return;\n"
            "    }\n"
            "    char *str = (char *) calloc(len + 1, sizeof(char));\n"
            "    if (!str) {\n"
            "        printf(\"Memory allocation failed (str)\\n\");\n"
            "        exit(1);\n"
            "    }\n"
            "    strncpy(str, playback_uuid, len);\n",
        name="set_playback_uuid no abort",
    )

    sub(
        handlers,
        old=(
            "        if (raop->airplay_video[i] && !strcmp(uuid, get_playback_uuid(raop->airplay_video[i]))) {\n"
        ),
        new=(
            "        const char *have = raop->airplay_video[i] ? get_playback_uuid(raop->airplay_video[i]) : NULL;\n"
            "        if (uuid && have && !strcmp(uuid, have)) { /* GCAST handoff */\n"
        ),
        already="GCAST handoff */",
        name="get_playlist_by_uuid NULL uuid",
    )

    sub(
        handlers,
        old=(
            "    if (!apple_session_id) {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"Play request had no X-Apple-Session-ID\");\n"
            "        goto play_error;\n"
            "    }\n"
        ),
        new=(
            "    if (!apple_session_id || strlen(apple_session_id) == 0 || strlen(apple_session_id) > 256) {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"Play request had no usable X-Apple-Session-ID\"); /* GCAST handoff */\n"
            "        goto play_error;\n"
            "    }\n"
        ),
        already="no usable X-Apple-Session-ID",
        name="play session id bound",
    )

    sub(
        handlers,
        old=(
            "    char* playback_uuid = NULL;\n"
            "    plist_get_string_val(req_uuid_node, &playback_uuid);\n"
        ),
        new=(
            "    char* playback_uuid = NULL;\n"
            "    plist_get_string_val(req_uuid_node, &playback_uuid);\n"
            "    /* GCAST handoff: a present uuid node that is not a string leaves the pointer NULL.\n"
            "       strcmp/strlen on it is SIGSEGV 11 during the playlist sweep. */\n"
            "    if (!playback_uuid || strlen(playback_uuid) == 0 || strlen(playback_uuid) > 256) {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"Play uuid is not a usable string\");\n"
            "        goto play_error;\n"
            "    }\n"
        ),
        already="Play uuid is not a usable string",
        name="play uuid string",
    )

    sub(
        handlers,
        old=(
            "        plist_get_string_val(req_content_location_node, &playback_location);\n"
            "    }\n"
            "    /* we support HLS playlists if the playback location is terminated by \"/master.m3u8\", otherwise pass location to player */\n"
            "    const char *uri_suffix = strstr(playback_location, \"/master.m3u8\");    \n"
        ),
        new=(
            "        plist_get_string_val(req_content_location_node, &playback_location);\n"
            "    }\n"
            "    /* GCAST handoff: node present but not a string. strstr(NULL) is SIGSEGV 11\n"
            "       immediately after \"deleting playlist\" (the lab AirPlay→YouTube line). */\n"
            "    if (!playback_location) {\n"
            "        logger_log(raop->logger, LOGGER_ERR, \"Play Content-Location is not a string\");\n"
            "        goto play_error;\n"
            "    }\n"
            "    /* we support HLS playlists if the playback location is terminated by \"/master.m3u8\", otherwise pass location to player */\n"
            "    const char *uri_suffix = strstr(playback_location, \"/master.m3u8\");    \n"
        ),
        already="Play Content-Location is not a string",
        name="play Content-Location NULL",
    )

    sub(
        handlers,
        old=(
            "            plist_get_string_val(req_client_proc_name_node, &client_proc_name);\n"
            "            if (!strstr(supported_hls_proc_names, client_proc_name)){\n"
        ),
        new=(
            "            plist_get_string_val(req_client_proc_name_node, &client_proc_name);\n"
            "            if (client_proc_name && !strstr(supported_hls_proc_names, client_proc_name)){ /* GCAST handoff */\n"
        ),
        already="client_proc_name && !strstr(supported_hls_proc_names, client_proc_name)",
        name="play clientProcName NULL",
    )

    sub(
        handlers,
        old=(
            "        char *end = strstr(uri_prefix, \"/master.m3u8\");\n"
            "        *end = '\\0';\n"
        ),
        new=(
            "        char *end = strstr(uri_prefix, \"/master.m3u8\");\n"
            "        /* GCAST handoff: no suffix, or the location is only \"/master.m3u8\" so the\n"
            "           prefix length is 0 and set_uri_prefix's assert aborts the daemon. */\n"
            "        if (!end || end == uri_prefix) {\n"
            "            free(uri_prefix);\n"
            "            goto play_error;\n"
            "        }\n"
            "        *end = '\\0';\n"
        ),
        already="end == uri_prefix",
        name="play master suffix NULL",
    )

    sub(
        video,
        old="void set_uri_prefix(airplay_video_t *airplay_video, const char *uri_prefix, size_t len) {\n"
            "    assert(uri_prefix && len );\n",
        new="void set_uri_prefix(airplay_video_t *airplay_video, const char *uri_prefix, size_t len) {\n"
            "    if (!airplay_video || !uri_prefix || len == 0 || len > 4096) { /* GCAST handoff */\n"
            "        return;\n"
            "    }\n",
        already="set_uri_prefix(airplay_video_t *airplay_video, const char *uri_prefix, size_t len) {\n"
            "    if (!airplay_video || !uri_prefix || len == 0 || len > 4096)",
        name="set_uri_prefix no abort",
    )

    sub(
        video,
        old="void set_playback_location(airplay_video_t *airplay_video, const char *location, size_t len) {\n"
            "    assert(location && len );\n",
        new="void set_playback_location(airplay_video_t *airplay_video, const char *location, size_t len) {\n"
            "    if (!airplay_video || !location || len == 0 || len > 4096) { /* GCAST handoff */\n"
            "        return;\n"
            "    }\n",
        already="set_playback_location(airplay_video_t *airplay_video, const char *location, size_t len) {\n"
            "    if (!airplay_video || !location || len == 0 || len > 4096)",
        name="set_playback_location no abort",
    )

    sub(
        handlers,
        old=(
            "        logger_log(raop->logger, LOGGER_INFO,\n"
            "                   \"deleting playlist playback_uuid %s duration (seconds) %f\",\n"
            "                   get_playback_uuid(raop->airplay_video[next]),\n"
            "                   get_duration(raop->airplay_video[next]));\n"
            "        airplay_video_destroy(raop->airplay_video[next]);\n"
            "        raop->airplay_video[next] = NULL;\n"
        ),
        new=(
            "        if (raop->airplay_video[next]) { /* GCAST handoff */\n"
            "        logger_log(raop->logger, LOGGER_INFO,\n"
            "                   \"deleting playlist playback_uuid %s duration (seconds) %f\",\n"
            "                   get_playback_uuid(raop->airplay_video[next]),\n"
            "                   get_duration(raop->airplay_video[next]));\n"
            "        airplay_video_destroy(raop->airplay_video[next]);\n"
            "        raop->airplay_video[next] = NULL;\n"
            "        }\n"
        ),
        already="if (raop->airplay_video[next]) { /* GCAST handoff */",
        name="full-table delete NULL slot",
    )

    sub(
        handlers,
        old=(
            "    const char *apple_session_id = get_apple_session_id(airplay_video);\n"
            "    if (strcmp(session_id, apple_session_id)){\n"
        ),
        new=(
            "    const char *apple_session_id = get_apple_session_id(airplay_video);\n"
            "    if (!apple_session_id || strcmp(session_id, apple_session_id)){ /* GCAST handoff */\n"
        ),
        already="!apple_session_id || strcmp(session_id, apple_session_id)",
        name="action session id NULL",
    )

    sub(
        handlers,
        old='        data_is_binary_plist = (strstr(header_str, "x-apple-binary-plist") != NULL);\n',
        new='        data_is_binary_plist = (header_str && strstr(header_str, "x-apple-binary-plist") != NULL); /* GCAST handoff */\n',
        already="header_str && strstr(header_str, \"x-apple-binary-plist\")",
        name="play header string NULL",
    )

    sub(
        handlers,
        old='    data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);\n',
        new='    data_is_plist = (header_str && strstr(header_str,"apple-binary-plist") != NULL); /* GCAST handoff */\n',
        already='header_str && strstr(header_str,"apple-binary-plist")',
        name="action header string NULL",
    )

    sub(
        raop,
        old=(
            "                raop_ntp_t *raop_ntp = raop_conn->raop_ntp;\n"
            "                if (raop_rtp) {\n"
            "                    logger_log(raop->logger, LOGGER_DEBUG, \"New AirPlay connection: stopping NTP time\"\n"
            "                               \" service on RAOP connection %p\", raop_conn);\n"
            "                    raop_ntp_stop(raop_ntp);\n"
            "                }\n"
        ),
        new=(
            "                raop_ntp_t *raop_ntp = raop_conn->raop_ntp;\n"
            "                if (raop_ntp) { /* GCAST handoff: was if (raop_rtp), so a live audio session with no NTP SIGSEGV'd */\n"
            "                    logger_log(raop->logger, LOGGER_DEBUG, \"New AirPlay connection: stopping NTP time\"\n"
            "                               \" service on RAOP connection %p\", raop_conn);\n"
            "                    raop_ntp_stop(raop_ntp);\n"
            "                }\n"
        ),
        already="GCAST handoff: was if (raop_rtp)",
        name="ntp stop NULL",
    )


if __name__ == "__main__":
    main()
