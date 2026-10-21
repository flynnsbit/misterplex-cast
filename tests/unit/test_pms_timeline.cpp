// Unit tests for PMS /:/timeline reporting (no network).
#include "pms_timeline.hpp"
#include "plex_resolve.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

static int fails = 0;
#define CHECK(cond)                                                                              \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
            ++fails;                                                                             \
        }                                                                                        \
    } while (0)

static std::map<std::string, std::string> queryParams(const std::string& url) {
    std::map<std::string, std::string> out;
    auto q = url.find('?');
    if (q == std::string::npos)
        return out;
    ++q;
    while (q < url.size()) {
        auto amp = url.find('&', q);
        auto part = url.substr(q, amp == std::string::npos ? std::string::npos : amp - q);
        auto eq = part.find('=');
        if (eq != std::string::npos)
            out[part.substr(0, eq)] = misterplex::urlDecode(part.substr(eq + 1));
        if (amp == std::string::npos)
            break;
        q = amp + 1;
    }
    return out;
}

static std::string header(const misterplex::PmsTimelineHttpRequest& req,
                          const std::string& name) {
    for (const auto& h : req.headers) {
        if (h.first == name)
            return h.second;
    }
    return {};
}

int main() {
    using namespace misterplex;

    // FIX A: non-2xx must never count as success (401 HTML body was the old trap).
    CHECK(!plexHttpStatusOk(0));
    CHECK(!plexHttpStatusOk(401));
    CHECK(!plexHttpStatusOk(403));
    CHECK(!plexHttpStatusOk(404));
    CHECK(!plexHttpStatusOk(500));
    CHECK(!plexHttpStatusOk(199));
    CHECK(plexHttpStatusOk(200));
    CHECK(plexHttpStatusOk(204));
    CHECK(plexHttpStatusOk(299));
    CHECK(parseCurlHttpCode("401\n") == 401);
    CHECK(parseCurlHttpCode("200") == 200);
    CHECK(parseCurlHttpCode("  204  ") == 204);
    CHECK(parseCurlHttpCode("") == 0);
    CHECK(parseCurlHttpCode("ok") == 0);
    // Simulated sink: 401 body would be non-empty but must report failed.
    {
        std::vector<std::string> logs;
        std::vector<PmsTimelineHttpRequest> sent401;
        PmsTimelineReporter badSink(
            [&](const PmsTimelineHttpRequest& req) {
                sent401.push_back(req);
                // Mirror measured device fact: 401 Unauthorized HTML is non-empty.
                const std::string fakeBody =
                    "<html><head><title>Unauthorized</title></head><body>"
                    "<h1>401 Unauthorized</h1></body></html>";
                CHECK(!fakeBody.empty());
                // Correct sink: status drives ok, not body.
                return PmsTimelineSinkResult{plexHttpStatusOk(401), 401};
            },
            false);
        badSink.setLog([&](const std::string& line) { logs.push_back(line); });
        PmsTimelineSession s401;
        s401.baseUrl = "https://cast-host.example:32400";
        s401.token = "cast-session-token";
        s401.ratingKey = "40868";
        s401.key = "/library/metadata/40868";
        badSink.beginSession(s401, 0, 1000);
        CHECK(sent401.size() == 1);
        CHECK(!logs.empty());
        CHECK(logs[0].find("update failed") != std::string::npos);
        CHECK(logs[0].find("http=401") != std::string::npos);
        CHECK(logs[0].find("cast-session-token") == std::string::npos); // redacted
    }

    std::vector<PmsTimelineHttpRequest> sent;
    PmsTimelineReporter reporter(
        [&](const PmsTimelineHttpRequest& req) {
            sent.push_back(req);
            return PmsTimelineSinkResult{true, 200};
        },
        false);

    PmsTimelineSession s;
    s.baseUrl = "http://pms.local:32400/";
    s.token = "unit-token";
    s.ratingKey = "123";
    s.key = "/library/metadata/123";
    s.playQueueItemId = "456";
    s.containerKey = "/playQueues/99?own=1";

    reporter.beginSession(s, 1000, 90000);
    CHECK(sent.size() == 1);
    CHECK(sent[0].url.rfind("http://pms.local:32400/:/timeline?", 0) == 0);
    auto p = queryParams(sent[0].url);
    CHECK(p["ratingKey"] == "123");
    CHECK(p["key"] == "/library/metadata/123");
    CHECK(p["state"] == "buffering");
    CHECK(p["time"] == "1000");
    CHECK(p["duration"] == "90000");
    CHECK(p["type"] == "video");
    CHECK(p["identifier"] == "com.plexapp.plugins.library");
    CHECK(p["playQueueItemID"] == "456");
    CHECK(p["containerKey"] == "/playQueues/99?own=1");
    CHECK(header(sent[0], "X-Plex-Token") == "unit-token");
    CHECK(header(sent[0], "X-Plex-Client-Identifier") == "misterplex");
    CHECK(header(sent[0], "X-Plex-Product") == "Plex Web");
    CHECK(header(sent[0], "X-Plex-Version") == "4.125.0");
    CHECK(header(sent[0], "X-Plex-Device-Name") == "Chrome");

    reporter.reportState("buffering", 1500, 90000); // unchanged state: skipped
    CHECK(sent.size() == 1);
    reporter.reportState("playing", 2000, 90000); // state transition: sent immediately
    reporter.reportState("playing", 2500, 90000); // under 1s cadence: skipped
    reporter.reportState("paused", 3000, 90000);
    reporter.reportState("playing", 3500, 90000); // state transition: sent immediately
    reporter.endSession(45000, 90000);
    CHECK(sent.size() == 5);
    CHECK(queryParams(sent[1].url)["state"] == "playing");
    CHECK(queryParams(sent[1].url)["time"] == "2000");
    CHECK(queryParams(sent[2].url)["state"] == "paused");
    CHECK(queryParams(sent[2].url)["time"] == "3000");
    CHECK(queryParams(sent[3].url)["state"] == "playing");
    CHECK(queryParams(sent[3].url)["time"] == "3500");
    CHECK(queryParams(sent[4].url)["state"] == "stopped");
    CHECK(queryParams(sent[4].url)["time"] == "45000");

    // Token refresh must apply to subsequent reports (mid-session cast token rotate).
    sent.clear();
    reporter.beginSession(s, 0, 90000);
    reporter.updateToken("refreshed-token");
    reporter.reportState("playing", 5000, 90000);
    CHECK(sent.size() == 2);
    CHECK(header(sent[1], "X-Plex-Token") == "refreshed-token");

    sent.clear();
    reporter.beginSession(s, 0, 0);
    CHECK(sent.size() == 1);
    CHECK(queryParams(sent[0].url)["duration"] == "0");
    CHECK(queryParams(sent[0].url)["time"] == "0");
    reporter.endSession(1234, 0);
    CHECK(sent.size() == 2);
    CHECK(queryParams(sent[1].url)["duration"] == "0");
    CHECK(queryParams(sent[1].url)["time"] == "1234");

    sent.clear();
    PmsTimelineSession noToken = s;
    noToken.token.clear();
    reporter.beginSession(noToken, 0, 1000);
    CHECK(sent.empty());

    PmsTimelineSession noRatingKey = s;
    noRatingKey.ratingKey.clear();
    reporter.beginSession(noRatingKey, 0, 1000);
    CHECK(sent.empty());

    PmsTimelineHttpRequest req;
    PmsTimelineSession noKey = s;
    noKey.key.clear();
    CHECK(buildPmsTimelineHttpRequest(noKey, "buffering", -99, -1, req));
    auto np = queryParams(req.url);
    CHECK(np["state"] == "buffering");
    CHECK(np["key"] == "/library/metadata/123");
    CHECK(np["time"] == "0");
    CHECK(np["duration"] == "0");
    CHECK(np["type"] == "video");

    if (fails) {
        std::fprintf(stderr, "test_pms_timeline: %d failures\n", fails);
        return 1;
    }
    std::printf("test_pms_timeline: OK\n");
    return 0;
}
