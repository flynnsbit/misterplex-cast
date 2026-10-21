// Unit tests for gcast/mini_json.hpp and gcast/lounge_proto.hpp (GCAST P3).
// Mutants: MUT_LP_NO_CAP, MUT_LP_NO_DEDUP, MUT_LP_NO_VIDCHECK.
#include "gcast/lounge_proto.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

using namespace misterplex::gcast;
using namespace misterplex::gcast::lounge;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string frame(const std::string& p) { return std::to_string(p.size()) + "\n" + p; }

int main() {
    // mini_json
    { Json j; CHECK(parseJson("{\"a\":[1,2.5,\"x\\u00e9\"],\"b\":true,\"n\":null}", j));
      CHECK(j.get("a") && j.get("a")->arr.size() == 3 && j.get("a")->arr[2].str == "x\xc3\xa9");
      CHECK(j.get("a")->arr[1].scalar() == "2.5"); CHECK(j.get("b")->b); }
    { Json j; CHECK(!parseJson("[1,2", j)); CHECK(!parseJson("{\"a\" 1}", j)); CHECK(!parseJson("[1] x", j)); CHECK(!parseJson("-", j)); }
    { std::string deep(100, '['); deep += std::string(100, ']'); Json j; CHECK(!parseJson(deep, j)); }

    // chunk framing, split across feeds
    { ChunkParser cp; std::vector<std::string> out;
      std::string s = frame("[[0,[\"c\",\"SID1\",\"\",8]]]") + frame("[[1,[\"S\",\"GS1\"]]]");
      for (size_t i = 0; i < s.size(); i += 3) CHECK(cp.feed(s.data() + i, std::min<size_t>(3, s.size() - i), out));
      CHECK(out.size() == 2); }
    { ChunkParser cp; std::vector<std::string> out; std::string s = "abc\n"; CHECK(!cp.feed(s.data(), s.size(), out)); CHECK(cp.bad()); }
    { ChunkParser cp; std::vector<std::string> out; std::string s = "99999999\n"; CHECK(!cp.feed(s.data(), s.size(), out)); }
    { ChunkParser cp; std::vector<std::string> out; std::string s(40, '1'); CHECK(!cp.feed(s.data(), s.size(), out)); }

    // batch + channel
    { std::vector<Message> ms;
      CHECK(decodeBatch("[[0,[\"c\",\"SIDX\",\"\",8]],[1,[\"S\",\"GSX\"]],[2,[\"noop\"]],[3,[\"setPlaylist\",{\"videoId\":\"dQw4w9WgXcQ\",\"currentTime\":\"12.5\",\"currentIndex\":\"0\",\"listId\":\"RQ1\",\"videoIds\":\"dQw4w9WgXcQ,aaaaaaaaaaa\"}]]]", ms));
      CHECK(ms.size() == 4);
      Channel ch; int app = 0; for (auto& m : ms) if (ch.accept(m)) ++app;
      CHECK(ch.sid == "SIDX" && ch.gsid == "GSX" && ch.aid == 3 && app == 1);
      Event e = toEvent(ms[3]);
      CHECK(e.kind == Ev::Play && e.videoId == "dQw4w9WgXcQ" && e.ms == 12500 && e.listId == "RQ1" && e.videoIds.size() == 2);
      CHECK(!ch.accept(ms[3]));  // replayed index is dropped
    }
    { std::vector<Message> ms; CHECK(!decodeBatch("{}", ms)); CHECK(decodeBatch("[[\"x\"],[1],[2,[]],[3,[7]]]", ms)); CHECK(ms.empty()); }

    // events
    { Message m; m.cmd = "setVideo"; m.args = {{"videoId", "bad id;rm -"}, {"currentTime", "0"}}; CHECK(toEvent(m).kind == Ev::None); }
    { Message m; m.cmd = "setPlaylist"; m.args = {{"videoIds", "dQw4w9WgXcQ,ZZZZZZZZZZZ"}, {"currentIndex", "1"}}; Event e = toEvent(m);
      CHECK(e.kind == Ev::Play && e.videoId == "ZZZZZZZZZZZ" && e.index == 1 && e.ms == 0); }
    { Message m; m.cmd = "seekTo"; m.args = {{"newTime", "61.25"}}; Event e = toEvent(m); CHECK(e.kind == Ev::Seek && e.ms == 61250); }
    { Message m; m.cmd = "seekTo"; m.args = {{"newTime", "-3"}}; CHECK(toEvent(m).kind == Ev::None); }
    { Message m; m.cmd = "seekTo"; m.args = {{"newTime", "nan"}}; CHECK(toEvent(m).kind == Ev::None); }
    { Message m; m.cmd = "pause"; CHECK(toEvent(m).kind == Ev::Pause); m.cmd = "play"; CHECK(toEvent(m).kind == Ev::Resume);
      m.cmd = "stopVideo"; CHECK(toEvent(m).kind == Ev::Stop); m.cmd = "getNowPlaying"; CHECK(toEvent(m).kind == Ev::NowPlaying);
      m.cmd = "weird"; CHECK(toEvent(m).kind == Ev::None); }

    // outgoing
    { std::vector<Out> o{onStateChange(Playing, 1500, 60000, "c p"), nowPlaying("", "", 0, 0, 0, 0, "")};
      std::string b = formBody(o, 7);
      CHECK(b.rfind("count=2&ofs=7&req0__sc=onStateChange", 0) == 0);
      CHECK(b.find("req0_currentTime=1.500") != std::string::npos && b.find("req0_cpn=c%20p") != std::string::npos);
      CHECK(b.find("req1__sc=nowPlaying") != std::string::npos && b.find("req1_videoId") == std::string::npos); }
    { BindIds ids{"u-1", "MiSTer Den", "TOK/+"}; Channel c; c.sid = "S"; c.gsid = "G"; c.aid = 4;
      std::string p = bindPollPath(ids, c, "zz");
      CHECK(p.find("name=MiSTer%20Den") != std::string::npos && p.find("loungeIdToken=TOK%2F%2B") != std::string::npos);
      CHECK(p.find("RID=rpc&SID=S&CI=0&AID=4&gsessionid=G&TYPE=xmlhttp") != std::string::npos);
      CHECK(bindInitialPath(ids, 1234, "zz").find("&RID=1234&CVER=1") != std::string::npos); }

    // KI-7: a long-poll that delivered setPlaylist and then a partial chunk, with no
    // further bytes, must not dispatch stopVideo and must rebind without dropping
    // the ready flag. Test 5's stray stopVideo is the same undelivered command;
    // that run then saw 410 Gone (the server closed the poll). This case is the
    // poll that stays open and silent.
    { ChunkParser cp; std::vector<std::string> out;
      const std::string playlist = "[[12,[\"setPlaylist\",{\"videoId\":\"dQw4w9WgXcQ\",\"currentTime\":\"0\"}]]]";
      const std::string framed = frame(playlist);
      CHECK(cp.feed(framed.data(), framed.size(), out));
      CHECK(out.size() == 1);
      std::vector<Message> ms; CHECK(decodeBatch(out[0], ms));
      CHECK(ms.size() == 1 && ms[0].cmd == "setPlaylist" && ms[0].index == 12);
      Channel ch; CHECK(ch.accept(ms[0])); CHECK(ch.aid == 12);
      const std::string stop = frame("[[13,[\"stopVideo\"]]]");
      CHECK(cp.feed(stop.data(), 4, out));  // "NNN\n" only, body withheld
      CHECK(out.size() == 1 && !cp.bad());
      CHECK(nextPoll(true, 200, "idle", 1) == PollNext::RebindKeepReady);
      CHECK(kLoungePollIdleSec == 15);
      CHECK(nextPoll(true, 200, "", 1) == PollNext::Stay);
      CHECK(nextPoll(true, 410, "gone", 1) == PollNext::RebindDropReady);
      CHECK(nextPoll(false, 0, "read ended", 1) == PollNext::Retry);
      CHECK(nextPoll(false, 0, "read ended", 4) == PollNext::RebindDropReady);
    }

    std::printf("test_gcast_lounge: %d pass, %d fail\n", pass, fail);
    return fail ? 1 : 0;
}
