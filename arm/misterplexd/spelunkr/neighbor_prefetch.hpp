#pragma once

// Background prefetch of the Ch Up / Ch Down neighbour moov/header into the
// existing HeaderCacheStore. Archive items are resolved through resolveArchivePlay
// in the fetch lambda; Back When first-party direct URLs use decideNeighborFetch's
// applyDirectPlayHeaderCacheKey so the neighbour key matches the on-path tune (#656 N3).
// Bounded so one recording is in flight at a time
// (12 MiB per-entry cap, about 17 MB at the measured 1.4R share) and at most
// two jobs exist. A zap must cancelAll() so a still-running fetch cannot race
// the new tune or outlive the session. The destructor cancels and joins.

#include "spelunkr/header_cache.hpp"
#include "spelunkr/neighbor_key.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace misterplex {
namespace spelunkr {

inline constexpr int kNeighborPrefetchMaxJobs = 2;
// One 12 MiB recording plus ~1.4x libav share is about 17 MB, under the 24 MB
// growth gate. A second simultaneous recording would not be.
inline constexpr int kNeighborPrefetchMaxRecording = 1;

// NeighborPrefetchSpec lives in neighbor_key.hpp (key identity + skip policy).

// Return true if the header is already warm or was persisted. Must poll cancel
// frequently and return promptly when it is set.
using NeighborPrefetchFetch =
    std::function<bool(const NeighborPrefetchSpec&, std::atomic<bool>& cancel)>;

class NeighborHeaderPrefetcher {
public:
    explicit NeighborHeaderPrefetcher(NeighborPrefetchFetch fetch = {})
        : fetch_(std::move(fetch)) {}

    NeighborHeaderPrefetcher(const NeighborHeaderPrefetcher&) = delete;
    NeighborHeaderPrefetcher& operator=(const NeighborHeaderPrefetcher&) = delete;

    ~NeighborHeaderPrefetcher() {
        cancelAll();
        joinAll();
    }

    void setFetch(NeighborPrefetchFetch fetch) { fetch_ = std::move(fetch); }

    void cancelAll() {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& j : jobs_)
            j->cancel.store(true, std::memory_order_release);
        recCv_.notify_all();
    }

    void request(std::vector<NeighborPrefetchSpec> specs) {
        reapLockedPrep();
        for (auto& s : specs) {
            if (s.url.empty() && s.archiveId.empty() && s.file.empty())
                continue;
            std::lock_guard<std::mutex> lock(mu_);
            if (static_cast<int>(liveUnlocked()) >= kNeighborPrefetchMaxJobs)
                break;
            bool already = false;
            for (const auto& j : jobs_) {
                if (!j->done.load(std::memory_order_acquire) && j->spec.id == s.id && !s.id.empty()) {
                    already = true;
                    break;
                }
            }
            if (already)
                continue;
            auto job = std::make_unique<Job>();
            job->spec = std::move(s);
            Job* raw = job.get();
            raw->thr = std::thread([this, raw]() { runJob(*raw); });
            jobs_.push_back(std::move(job));
        }
    }

    int inFlight() const {
        std::lock_guard<std::mutex> lock(mu_);
        return liveUnlocked();
    }

    int recording() const {
        std::lock_guard<std::mutex> lock(mu_);
        return recording_;
    }

    bool waitIdle(int64_t timeoutMs) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mu_);
        if (timeoutMs < 0) {
            idleCv_.wait(lock, [&] { return liveUnlocked() == 0; });
            return true;
        }
        return idleCv_.wait_until(lock, deadline, [&] { return liveUnlocked() == 0; });
    }

private:
    struct Job {
        NeighborPrefetchSpec spec;
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::thread thr;
    };

    NeighborPrefetchFetch fetch_;
    mutable std::mutex mu_;
    mutable std::condition_variable recCv_;
    mutable std::condition_variable idleCv_;
    std::vector<std::unique_ptr<Job>> jobs_;
    int recording_ = 0;

    int liveUnlocked() const {
        int n = 0;
        for (const auto& j : jobs_) {
            if (!j->done.load(std::memory_order_acquire))
                ++n;
        }
        return n;
    }

    void reapLockedPrep() {
        // Keep Job objects alive until their threads have been joined so
        // runJob cannot touch a destroyed cancel/spec.
        std::vector<std::thread> join;
        std::vector<std::unique_ptr<Job>> dying;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                if ((*it)->done.load(std::memory_order_acquire) && (*it)->thr.joinable()) {
                    join.push_back(std::move((*it)->thr));
                    dying.push_back(std::move(*it));
                    it = jobs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& t : join)
            t.join();
    }

    void joinAll() {
        std::vector<std::thread> join;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& j : jobs_) {
                if (j->thr.joinable())
                    join.push_back(std::move(j->thr));
            }
        }
        for (auto& t : join)
            t.join();
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.clear();
    }

    void runJob(Job& job) {
        bool gotSlot = false;
        {
            std::unique_lock<std::mutex> lock(mu_);
            recCv_.wait(lock, [&] {
                return job.cancel.load(std::memory_order_acquire) ||
                       recording_ < kNeighborPrefetchMaxRecording;
            });
            if (!job.cancel.load(std::memory_order_acquire)) {
                ++recording_;
                gotSlot = true;
            }
        }
        if (gotSlot && fetch_ && !job.cancel.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "misterplexd: spelunkr: neighbor prefetch start id=%s\n",
                         job.spec.id.c_str());
            bool ok = false;
            try {
                ok = fetch_(job.spec, job.cancel);
            } catch (const std::bad_alloc&) {
                std::fprintf(stderr,
                             "misterplexd: spelunkr: neighbor prefetch bad_alloc id=%s\n",
                             job.spec.id.c_str());
            } catch (const std::exception& ex) {
                std::fprintf(stderr,
                             "misterplexd: spelunkr: neighbor prefetch exception id=%s what=%s\n",
                             job.spec.id.c_str(), ex.what());
            }
            std::fprintf(stderr, "misterplexd: spelunkr: neighbor prefetch %s id=%s\n",
                         ok ? "ok" : "end", job.spec.id.c_str());
        }
        if (gotSlot) {
            std::lock_guard<std::mutex> lock(mu_);
            --recording_;
            recCv_.notify_all();
        }
        job.done.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mu_);
            idleCv_.notify_all();
        }
    }
};

}}  // namespace misterplex::spelunkr
