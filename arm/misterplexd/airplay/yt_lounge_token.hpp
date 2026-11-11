#pragma once
#include "gcast/https_client.hpp"
#include "gcast/lounge_proto.hpp"
#include "gcast/yt_screen_id.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace misterplex {
namespace airplay {

enum class YtTokenResult { Ready, Retry, Stopped };

inline bool ytTokenRejectsScreenId(int status) {
    return status == 400 || status == 401 || status == 403 || status == 404 || status == 410 || status == 422;
}

inline bool ytStoreScreenId(const std::string& path, const std::string& uuid, const std::string& sid) {
    if (path.empty())
        return true;
    std::ofstream f(path, std::ios::trunc);
    f << ytScreenIdPersistText(uuid, sid);
    f.close();
    if (f)
        return true;
    std::fprintf(stderr, "yt: cannot persist screen_id: %s\n", std::strerror(errno));
    return false;
}

// Client is injectable so the real recovery and persistence path can run without YouTube.
template <typename Client>
YtTokenResult ytFetchLoungeToken(Client& c, const std::string& host, const std::string& path, const std::string& uuid,
                                 std::string& sid, std::string& token, const HttpsRequestFlags& sidFlags) {
    token.clear();
    if (sid.empty() && !path.empty()) {
        std::ifstream f(path);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sid = ytScreenIdFromFile(text, uuid);
        if (sid.empty() && !text.empty())
            std::fprintf(stderr, "yt: screen_id file is cloned or unscoped; generating a new Lounge id\n");
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (sid.empty()) {
            HttpsResult r =
                c.request("GET", host, "/api/lounge/pairing/generate_screen_id", "", "", 15, {}, 2u << 20, sidFlags);
            while (!r.body.empty() && (r.body.back() == '\n' || r.body.back() == '\r'))
                r.body.pop_back();
            if (!r.ok || r.status != 200 || !ytScreenIdPlausible(r.body)) {
                std::fprintf(stderr, "yt: generate_screen_id failed: status=%d %s\n", r.status, r.err.c_str());
                return YtTokenResult::Retry;
            }
            sid = r.body;
            if (!ytStoreScreenId(path, uuid, sid)) {
                sid.clear();
                return YtTokenResult::Stopped;
            }
            std::fprintf(stderr, "yt: new screen_id %s...\n", sid.substr(0, 6).c_str());
        }
        HttpsResult t = c.request("POST", host, "/api/lounge/pairing/get_lounge_token_batch", "",
                                  "screen_ids=" + lounge::formEncode(sid), 15);
        Json j;
        const Json* tok = nullptr;
        if (t.ok && t.status == 200 && parseJson(t.body, j) && j.get("screens") && !j.get("screens")->arr.empty())
            tok = j.get("screens")->arr[0].get("loungeToken");
        if (tok && tok->type == Json::Str && !tok->str.empty()) {
            token = tok->str;
            return YtTokenResult::Ready;
        }
        std::fprintf(stderr, "yt: get_lounge_token_batch failed: status=%d %s\n", t.status, t.err.c_str());
        if (!t.ok || !ytTokenRejectsScreenId(t.status))
            return YtTokenResult::Retry;
#ifndef MUT_YT_TOKEN_KEEP_REJECTED_SCREEN
        sid.clear();
        // Invalidate on disk before requesting a replacement, even if generation fails.
        if (!ytStoreScreenId(path, uuid, sid))
            return YtTokenResult::Stopped;
#endif
        if (attempt == 1)
            return YtTokenResult::Stopped;
        std::fprintf(stderr, "yt: Lounge rejected screen_id; regenerating and retrying token once\n");
    }
    return YtTokenResult::Stopped;
}

} // namespace airplay
} // namespace misterplex
