#!/usr/bin/env python3
"""N2 test server: one MPEG-TS clip from 127.0.0.1 in the response shapes that decide FFmpeg's
end-of-stream code (dev-480i 10/4, review-rel-batch-1428-1004.md F1-F5).

  /proxy.ts        remote_fpga_ladder_proxy's shape: Connection: close, no Content-Length,
                   the whole clip, then close  -> FFmpeg http: EIO "Stream ends prematurely"
  /cut50.ts        the same, closed after half the clip (TS packet aligned): a mid-stream drop
  /length.ts       Content-Length, the whole clip  -> AVERROR_EOF
  /lengthshort.ts  Content-Length 64 KiB longer than the clip, then close  -> EIO, known size
  /chunked.ts      Transfer-Encoding: chunked with the 0 chunk  -> EIO in libavformat 63
  /ladder.ts?...offset_ms=N  the proxy shape serving the resume clip (TAIL), as the lab ladder
                   proxy applies offset_ms= itself (media_player urlHasUniversalOffset)
  /rangeable.ts    Content-Length and byte Range (206), serving the all-IDR clip (IDR): the
                   demux's own seek (opts.startMs) needs a seekable input
  /stall.ts        Content-Length, the whole clip, with a pause of N2_STALL_MS (default 3000)
                   after 40% of it (TS packet aligned): the daemon's PCM producer starves, so a
                   consumer that mirrors sys/alsa.sv catches up and leaves a gap, not a stop

usage: n2_eof_server.py PORTFILE FULL.ts TAIL.ts [IDR.ts]   (binds 127.0.0.1:0, writes the port)
"""
import http.server
import os
import re
import sys
import time

portfile, full, tail = sys.argv[1:4]
clips = {"full": open(full, "rb").read(), "tail": open(tail, "rb").read()}
if len(sys.argv) > 4:
    clips["idr"] = open(sys.argv[4], "rb").read()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def head(self, **extra):
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
        self.send_header("Cache-Control", "no-store")
        for k, v in extra.items():
            self.send_header(k.replace("_", "-"), v)
        self.end_headers()

    def body(self, data):
        for i in range(0, len(data), 65536):
            self.wfile.write(data[i:i + 65536])

    def do_GET(self):
        try:
            self.serve()
        except (BrokenPipeError, ConnectionResetError):
            # A client may hang up mid-body (FFmpeg's seek reopens with a Range request).
            self.close_connection = True

    def serve(self):
        path = self.path.split("?")[0]
        data = clips["full"]
        if path == "/ladder.ts" and "offset_ms=" in self.path:
            data = clips["tail"]
            path = "/proxy.ts"
        if path == "/proxy.ts":
            self.head(Connection="close")
            self.body(data)
        elif path == "/cut50.ts":
            self.head(Connection="close")
            self.body(data[:(len(data) // 2) // 188 * 188])
        elif path == "/length.ts":
            self.head(Content_Length=str(len(data)), Connection="close")
            self.body(data)
        elif path == "/stall.ts":
            cut = (len(data) * 2 // 5) // 188 * 188
            self.head(Content_Length=str(len(data)), Connection="close")
            self.body(data[:cut])
            self.wfile.flush()
            time.sleep(int(os.environ.get("N2_STALL_MS", "3000")) / 1000.0)
            self.body(data[cut:])
        elif path == "/lengthshort.ts":
            self.head(Content_Length=str(len(data) + 65536), Connection="close")
            self.body(data)
        elif path == "/chunked.ts":
            self.head(Transfer_Encoding="chunked", Connection="close")
            for i in range(0, len(data), 65536):
                part = data[i:i + 65536]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        elif path == "/rangeable.ts" and "idr" in clips:
            data = clips["idr"]
            m = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range", ""))
            first = int(m.group(1)) if m else 0
            last = int(m.group(2)) if m and m.group(2) else len(data) - 1
            if first >= len(data):
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % len(data))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            part = data[first:last + 1]
            self.send_response(206 if m else 200)
            self.send_header("Content-Type", "video/mp2t")
            self.send_header("Accept-Ranges", "bytes")
            if m:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (first, first + len(part) - 1, len(data)))
            self.send_header("Content-Length", str(len(part)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.body(part)
        else:
            self.send_error(404)
            return
        self.close_connection = True


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
tmp = portfile + ".tmp"
with open(tmp, "w") as f:
    f.write(str(srv.server_address[1]))
os.replace(tmp, portfile)
srv.serve_forever()
