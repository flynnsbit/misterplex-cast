// Review #208 nit 3: a one-shot delayed callback that is joined, never detached. The OSD restore
// ("Server can't transcode this title" for 10 s) used a detached thread holding &player/&playGen,
// which main() destroys at shutdown. Destroying/cancelling this wakes the wait and joins, so the
// callback never runs after its owner (declared after the captured objects) is gone.
#pragma once
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace misterplex {

class DelayedTask {
public:
    DelayedTask() = default;
    DelayedTask(const DelayedTask&) = delete;
    DelayedTask& operator=(const DelayedTask&) = delete;
    ~DelayedTask() { cancel(); }

    // Replaces any pending task (cancelled + joined first).
    void schedule(std::chrono::milliseconds delay, std::function<void()> fn) {
        std::lock_guard<std::mutex> op(op_);
        cancelLocked();
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = false;
        }
        t_ = std::thread([this, delay, fn = std::move(fn)] {
            std::unique_lock<std::mutex> lk(m_);
            if (cv_.wait_for(lk, delay, [this] { return stop_; }))
                return;
            lk.unlock();
            fn();
        });
#ifdef MUT_DTASK_DETACH
        t_.detach();
#endif
    }

    void cancel() {
        std::lock_guard<std::mutex> op(op_);
        cancelLocked();
    }

private:
    void cancelLocked() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (t_.joinable())
            t_.join();
    }

    std::mutex op_;   // serialises schedule/cancel callers (t_ itself)
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread t_;
};

}  // namespace misterplex
