// Several threads push partial secret keys through one SyncedLogAssembler.
// TSan fails if the pending line is moved without the mutex.
#include "log_redact.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

int main() {
    misterplex::SyncedLogAssembler sink;
    {
        const auto held = sink.push("myPlexAcce");
        if (!held.empty()) {
            std::fprintf(stderr, "test_log_race: mid-key was not held\n");
            return 1;
        }
        const auto lines = sink.push("ssToken=SPLITSECRET\n");
        if (lines.size() != 1 || lines[0].find("SPLITSECRET") != std::string::npos) {
            std::fprintf(stderr, "test_log_race: split leaked\n");
            return 1;
        }
    }
    std::atomic<int> leaked{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&sink, &leaked, t] {
            for (int i = 0; i < 200; ++i) {
                const std::string secret = "S" + std::to_string(t) + "X" + std::to_string(i);
                const auto lines = sink.push("myPlexAccessToken=" + secret + " note\n");
                for (const auto& line : lines) {
                    if (line.find(secret) != std::string::npos)
                        leaked.fetch_add(1);
                }
                const auto bearer = sink.push("Authorization: Bearer " + secret + "\n");
                for (const auto& line : bearer) {
                    if (line.find(secret) != std::string::npos)
                        leaked.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads)
        th.join();
    if (leaked.load() != 0) {
        std::fprintf(stderr, "test_log_race: leaked %d\n", leaked.load());
        return 1;
    }
    std::printf("test_log_race: ok\n");
    return 0;
}
