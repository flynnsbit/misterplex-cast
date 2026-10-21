/* Static ARM libav must open https://. DASH googlevideo URLs are https, and a
 * build with only file,http,tcp,pipe dies in avformat_open_input as
 * "Protocol not found". Connection failure is fine; that error is not.
 */
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *const url = "https://127.0.0.1:1/v.mp4";
    const char *name = avio_find_protocol_name(url);
    if (!name || strcmp(name, "https") != 0) {
        fprintf(stderr, "https protocol missing (%s)\n", name ? name : "null");
        return 1;
    }
    avformat_network_init();
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "ca_file", "/etc/ssl/certs/ca-certificates.crt", 0);
    av_dict_set(&opts, "tls_verify", "1", 0);
    AVFormatContext *fmt = NULL;
    const int rc = avformat_open_input(&fmt, url, NULL, &opts);
    if (rc == 0)
        avformat_close_input(&fmt);
    char buf[AV_ERROR_MAX_STRING_SIZE];
    buf[0] = 0;
    if (rc < 0)
        av_strerror(rc, buf, sizeof buf);
    av_dict_free(&opts);
    if (rc == AVERROR_PROTOCOL_NOT_FOUND || strstr(buf, "Protocol not found")) {
        fprintf(stderr, "open died: %s\n", buf);
        return 2;
    }
    /* TCP refused means https was registered and handed the URL to tcp.
       A missing protocol never gets that far. */
    fprintf(stderr, "https open reached the stack (%s)\n", rc == 0 ? "ok" : buf);
    return 0;
}
