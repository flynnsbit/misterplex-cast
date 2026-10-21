// GCAST P6 sender caps: g++ -std=c++17 -Iarm/misterplexd tests/unit/test_gcast_caps.cpp && ./a.out (repo root)
// Mutants: -DMUT_CAPS_NO_PAIRCAP, -DMUT_CAPS_NO_DEADLINE must fail.
#include "gcast/sender_caps.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>
using namespace misterplex::gcast::caps;
static int pass = 0, fail = 0;
#define CHECK(c) do { if (c) ++pass; else { ++fail; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
static std::string slurp(const char* p) { std::ifstream f(p); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
int main() {
    std::deque<std::string> q;
    CHECK(pushPairCode(q, "AAA"));
    CHECK(!pushPairCode(q, "AAA"));   // repeat ignored
    CHECK(!pushPairCode(q, ""));
    CHECK(!pushPairCode(q, std::string(65, 'x')));
    for (int i = 0; i < 1000; ++i) pushPairCode(q, "C" + std::to_string(i));
    CHECK(q.size() == kMaxPairCodes);
    CHECK(q.back() == "C999");
    CHECK(q.front() == "C996");   // the oldest dropped
    Deadline d(30);
    CHECK(!d.expired());
    CHECK(d.leftMs() > 0 && d.leftMs() <= 30);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(d.expired());
    CHECK(d.leftMs() == 0);
    const std::string y = slurp("arm/misterplexd/gcast/yt_fe.cpp");
    CHECK(y.find("caps::Deadline dl(caps::kDialConnMs)") != std::string::npos);
    CHECK(y.find("caps::pushPairCode(pairq_, r.pairingCode)") != std::string::npos);
    CHECK(y.find("pairq_.push_back") == std::string::npos);
    CHECK(y.find("cmdq_.clear();   // P6: latest play wins") != std::string::npos);
    CHECK(y.find("cmdq_.size() >= 32") == std::string::npos);
    std::printf("gcast_caps %d/%d\n", pass, fail);
    return fail ? 1 : 0;
}
