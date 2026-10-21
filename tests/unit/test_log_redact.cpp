// Every misterplexd log line goes through MediaPlayer::log, which redacts the
// logged copy. X-Plex-Token and access_token stay out of the sink. A line with
// no secret is unchanged. Live playback argv is not this function.
#include "log_redact.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef MPX_SOURCE_ROOT
#define MPX_SOURCE_ROOT "."
#endif

static int g_fails = 0;

#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ++g_fails;                                                                             \
            std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                              \
            std::fprintf(stderr, __VA_ARGS__);                                                     \
            std::fprintf(stderr, "\n");                                                            \
        }                                                                                          \
    } while (0)

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    using misterplex::redactSensitive;
    using misterplex::redactUrlTokens;
    const std::string q = "media: playing http://pms/v?X-Plex-Token=SECRET&x=1 from 2217 ms";
    const std::string qr = redactSensitive(q);
    CHECK(qr.find("SECRET") == std::string::npos, "query token leaked: %s", qr.c_str());
    CHECK(qr.find("X-Plex-Token=REDACTED") != std::string::npos, "query key kept: %s", qr.c_str());

    const std::string h = "X-Plex-Token: SECRET\r\nHost: pms\r\n";
    const std::string hr = redactSensitive(h);
    CHECK(hr.find("SECRET") == std::string::npos, "header token leaked: %s", hr.c_str());
    CHECK(hr.find("X-Plex-Token: REDACTED") != std::string::npos, "header key kept: %s", hr.c_str());

    const std::string a = "access_token=SECRET&other=1";
    const std::string ar = redactSensitive(a);
    CHECK(ar.find("SECRET") == std::string::npos, "access_token leaked: %s", ar.c_str());
    CHECK(ar.find("access_token=REDACTED") != std::string::npos, "access_token key kept: %s", ar.c_str());

    const std::string plain = "media: OB play started (OwenbRing, epoch 3)";
    CHECK(redactSensitive(plain) == plain, "no secret: byte-identical");
    CHECK(redactUrlTokens(plain) == plain, "no secret url: byte-identical");

    const std::string enc = "src=http%3A%2F%2Fpms%2Fv%3FX-Plex-Token%3DSECRET%26w=1";
    const std::string er = redactUrlTokens(enc);
    CHECK(er.find("SECRET") == std::string::npos, "percent token leaked: %s", er.c_str());
    CHECK(er.find("X-Plex-Token%3DREDACTED") != std::string::npos, "percent key kept: %s", er.c_str());

    const std::string xml = "accessToken=\"sekret\" owned=\"1\"";
    const std::string xmlr = redactSensitive(xml);
    CHECK(xmlr.find("sekret") == std::string::npos, "quoted xml leaked: %s", xmlr.c_str());
    CHECK(xmlr.find("accessToken=\"REDACTED\"") != std::string::npos, "quoted xml key: %s", xmlr.c_str());

    const std::string js = "{\"accessToken\": \"jsonsecret\", \"n\":1}";
    const std::string jsr = redactSensitive(js);
    CHECK(jsr.find("jsonsecret") == std::string::npos, "json leaked: %s", jsr.c_str());
    CHECK(jsr.find("\"accessToken\": \"REDACTED\"") != std::string::npos, "json key: %s", jsr.c_str());

    const std::string sq = "authToken='single'";
    const std::string sqr = redactSensitive(sq);
    CHECK(sqr.find("single") == std::string::npos, "single quote leaked: %s", sqr.c_str());
    CHECK(sqr.find("authToken='REDACTED'") != std::string::npos, "single quote key: %s", sqr.c_str());

    const std::string low = "x-plex-token=lowX&X-PLEX-TOKEN=upX";
    const std::string lowr = redactSensitive(low);
    CHECK(lowr.find("lowX") == std::string::npos && lowr.find("upX") == std::string::npos,
          "case leaked: %s", lowr.c_str());
    CHECK(lowr.find("x-plex-token=REDACTED") != std::string::npos, "lower key: %s", lowr.c_str());
    CHECK(lowr.find("X-PLEX-TOKEN=REDACTED") != std::string::npos, "upper key: %s", lowr.c_str());

    const std::string encAmp = "access_token=ampsecret&amp;x=1";
    const std::string encAmpr = redactUrlTokens(encAmp);
    CHECK(encAmpr.find("ampsecret") == std::string::npos, "amp leaked: %s", encAmpr.c_str());
    CHECK(encAmpr.find("access_token=REDACTED&amp;x=1") != std::string::npos, "amp kept: %s", encAmpr.c_str());

    const std::string dbl = "token%253Ddouble%2526z=1";
    const std::string dblr = redactUrlTokens(dbl);
    CHECK(dblr.find("double") == std::string::npos, "double-encoded leaked: %s", dblr.c_str());
    CHECK(dblr.find("token%253DREDACTED%2526z=1") != std::string::npos, "double-encoded key: %s", dblr.c_str());

    misterplex::LogLineAssembler split;
    CHECK(split.push("play X-Plex-Token=").empty(), "partial line must be held");
    const auto parts = split.push("SPLITSECRET&y=2\n");
    CHECK(parts.size() == 1, "split produced %zu lines", parts.size());
    if (!parts.empty()) {
        CHECK(parts[0].find("SPLITSECRET") == std::string::npos, "split leaked: %s", parts[0].c_str());
        CHECK(parts[0].find("X-Plex-Token=REDACTED") != std::string::npos, "split key: %s", parts[0].c_str());
    }

    const std::string main = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/main.cpp");
    const auto playAt = main.find("misterplexd: PLAY %s off=%lld dur=%lld");
    CHECK(playAt != std::string::npos, "PLAY log");
    if (playAt != std::string::npos) {
        const std::string play = main.substr(playAt, 240);
        CHECK(play.find("redactUrlTokens") != std::string::npos, "PLAY must redact");
        CHECK(play.find("BrowseV2Setting") == std::string::npos, "PLAY redaction must not depend on browse v2");
    }

    const std::string src = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/media_player.cpp");
    const size_t logAt = src.find("void MediaPlayer::log(const std::string& s) const {");
    CHECK(logAt != std::string::npos, "MediaPlayer::log");
    if (logAt != std::string::npos) {
        const std::string body = src.substr(logAt, 400);
        CHECK(body.find("redactUrlTokens") != std::string::npos,
              "the log sink must redact every line");
        CHECK(body.find("fprintf") == std::string::npos || body.find("redactUrlTokens") < body.find("fprintf"),
              "fprintf must see the redacted copy");
    }

    const std::string owenb = readFile(std::string(MPX_SOURCE_ROOT) + "/host/libmisterplex/owenb_session.cpp");
    const auto openAt = owenb.find("\"open \" + ");
    CHECK(openAt != std::string::npos, "owenb open log");
    if (openAt != std::string::npos) {
        const std::string open = owenb.substr(openAt, 220);
        CHECK(open.find("redactUrlTokens(req.url)") != std::string::npos,
              "owenb open url must be redacted: %s", open.c_str());
        CHECK(open.find("req.url +") == std::string::npos, "owenb open must not concatenate the raw url");
    }
    const auto logAtOwenb = owenb.find("void OwenbPlaySession::logLine");
    CHECK(logAtOwenb != std::string::npos, "OwenbPlaySession::logLine");
    if (logAtOwenb != std::string::npos) {
        const std::string body = owenb.substr(logAtOwenb, 240);
        CHECK(body.find("cfg_.log(\"[owenb-play] \" + misterplex::redactUrlTokens(s))") != std::string::npos,
              "owenb log callback must see the redacted copy");
    }
    const std::string bearer = "Authorization: Bearer SECRET";
    const std::string bearerr = redactSensitive(bearer);
    CHECK(bearerr.find("SECRET") == std::string::npos, "bearer leaked: %s", bearerr.c_str());
    CHECK(bearerr.find("Bearer REDACTED") != std::string::npos, "bearer key: %s", bearerr.c_str());
    misterplex::LogLineAssembler bearerSplit;
    CHECK(bearerSplit.push("Authorization: Bearer ").empty(),
          "Bearer trailing space must be held for the next chunk");
    const auto bearerLines = bearerSplit.push("SECRET\n");
    CHECK(bearerLines.size() == 1, "bearer split flushed one line");
    if (!bearerLines.empty()) {
        CHECK(bearerLines[0].find("SECRET") == std::string::npos, "bearer split leaked: %s",
              bearerLines[0].c_str());
        CHECK(bearerLines[0].find("Bearer REDACTED") != std::string::npos, "bearer split key: %s",
              bearerLines[0].c_str());
    }

    const std::string auth = "authenticationToken='sekret'";
    const std::string authr = redactSensitive(auth);
    CHECK(authr.find("sekret") == std::string::npos, "authenticationToken leaked: %s", authr.c_str());
    CHECK(authr.find("authenticationToken='REDACTED'") != std::string::npos, "authenticationToken key: %s",
          authr.c_str());

    const std::string plex = "myPlexAccessToken=\"abc\" plexToken=zzz";
    const std::string plexr = redactSensitive(plex);
    CHECK(plexr.find("abc") == std::string::npos && plexr.find("zzz") == std::string::npos,
          "plex tokens leaked: %s", plexr.c_str());

    misterplex::LogLineAssembler mid;
    CHECK(mid.push("play myPlexAcce").empty(), "mid-key fragment must be held");
    const auto midLines = mid.push("ssToken=SPLITSECRET\n");
    CHECK(!midLines.empty() && midLines[0].find("SPLITSECRET") == std::string::npos,
          "mid-key split leaked");

    CHECK(src.find("logPending_") == std::string::npos, "MediaPlayer::log still uses an unlocked pending string");
    CHECK(src.find("logAsm_.push") != std::string::npos, "MediaPlayer::log must use the locked assembler");
    const std::string comp = readFile(std::string(MPX_SOURCE_ROOT) + "/arm/misterplexd/companion.cpp");
    CHECK(comp.find("logPending_") == std::string::npos, "Companion::log still uses an unlocked pending string");
    CHECK(comp.find("logAsm_.push") != std::string::npos, "Companion::log must use the locked assembler");

    const std::string openLine = "open http://pms/v?X-Plex-Token=SECRET: failed";
    const std::string openRed = redactUrlTokens(openLine);
    CHECK(openRed.find("SECRET") == std::string::npos, "open line leaked: %s", openRed.c_str());
    CHECK(openRed.find("open ") == 0, "open line kept its prefix: %s", openRed.c_str());

    if (g_fails) {
        std::fprintf(stderr, "test_log_redact: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("test_log_redact: OK\n");
    return 0;
}
