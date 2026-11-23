#!/usr/bin/env python3
"""Harden a UxPlay lib/ tree for embedding in misterplexd. Idempotent."""
import pathlib, sys
root = pathlib.Path(sys.argv[1])
h = root / 'lib' / 'raop_handlers.h'
t = h.read_text()
old = 'plist_t initial_volume_node = plist_new_real(raop->callbacks.audio_set_client_volume(raop->callbacks.cls));'
new = (
    'double _gcast_vol = 0.0;\n'
    '    if (raop->callbacks.audio_set_client_volume) {\n'
    '        _gcast_vol = raop->callbacks.audio_set_client_volume(raop->callbacks.cls);\n'
    '    }\n'
    '    plist_t initial_volume_node = plist_new_real(_gcast_vol);'
)
if old in t:
    t = t.replace(old, new, 1)
    print('patched info volume null-check')
elif '_gcast_vol' in t:
    print('info volume already patched')
else:
    print('WARN: info volume site not found', file=sys.stderr)

old2 = (
    "            case 110: {\n"
    "                // Mirroring\n"
    "                raop_destroy_airplay_video(raop, -1);  //cleanup any hls data still present when mirror video starts\n"
    "                unsigned short dport = raop->mirror_data_lport;"
)
new2 = (
    "            case 110: {\n"
    "                // Mirroring — GCAST: refuse before starting RTP if the front end rejects the codec.\n"
    "                if (raop->callbacks.video_set_codec &&\n"
    "                    raop->callbacks.video_set_codec(raop->callbacks.cls, VIDEO_CODEC_UNKNOWN) < 0) {\n"
    "                    logger_log(raop->logger, LOGGER_INFO,\n"
    '                               "SETUP type=110 (mirroring) refused by front end");\n'
    '                    http_response_init(response, "RTSP/1.0", 453, "Not Enough Bandwidth");\n'
    "                    http_response_set_disconnect(response, 1);\n"
    "                    break;\n"
    "                }\n"
    "                raop_destroy_airplay_video(raop, -1);  //cleanup any hls data still present when mirror video starts\n"
    "                unsigned short dport = raop->mirror_data_lport;"
)
if old2 in t:
    t = t.replace(old2, new2, 1)
    print("patched SETUP mirror refuse")
elif "SETUP type=110 (mirroring) refused" in t:
    print("SETUP mirror already patched")
else:
    print("WARN: SETUP 110 site not found", file=sys.stderr)
h.write_text(t)
c = root / "lib" / "httpd.c"
ht = c.read_text()
if "MSG_NOSIGNAL" not in ht:
    olds = "ret = send(connection->socket_fd, data+written, datalen-written, 0);"
    news = (
        "ret = send(connection->socket_fd, data+written, datalen-written,\n"
        "#ifdef MSG_NOSIGNAL\n"
        "                        MSG_NOSIGNAL\n"
        "#else\n"
        "                        0\n"
        "#endif\n"
        "                        );"
    )
    if olds in ht:
        c.write_text(ht.replace(olds, news, 1))
        print("patched httpd MSG_NOSIGNAL")
    else:
        print("WARN: httpd send site not found", file=sys.stderr)
else:
    print("httpd MSG_NOSIGNAL already present")

# GCAST AIRPLAY-AUDIO: fp-setup mode bytes index 4-entry tables (reply_message[mode], message_key[mode] in
# playfair). An unauthenticated LAN sender with mode > 3 makes an out-of-bounds global read (ASan:
# global-buffer-overflow in omg_hax.c via fairplay_decrypt). Reject mode > 3 in both fp-setup steps.
fpf = root / 'lib' / 'fairplay_playfair.c'
ft = fpf.read_text()
if '_gcast_fp_mode' in ft:
    print('fairplay mode bound already patched')
else:
    o1 = '    int mode = req[14];\n'
    n1 = '    int mode = req[14];\n    if (mode < 0 || mode > 3) { /* _gcast_fp_mode */\n        return -1;\n    }\n'
    o2 = '    memcpy(fp->keymsg, req, 164);\n'
    n2 = ('    if (req[12] > 3) { /* _gcast_fp_mode: playfair indexes message_key[req[12]] */\n'
          '        return -1;\n    }\n' + o2)
    if o1 in ft and o2 in ft:
        ft = ft.replace(o1, n1, 1).replace(o2, n2, 1)
        fpf.write_text(ft)
        print('patched fairplay mode bound')
    else:
        print('WARN: fairplay mode sites not found', file=sys.stderr)
print("done")
