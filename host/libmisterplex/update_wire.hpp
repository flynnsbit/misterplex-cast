// Self-update wiring (frontier item 6), pure + injectable: everything the daemon needs around the existing
// verified engine (update_swap.hpp runUpdate / recoverOnBoot), with no OpenSSL, no network and no thread
// unless the caller supplies them. Default stays OFF: the updater is usable only when UPDATE_ENABLE=1, UPDATE_URL
// is https AND the build holds a real (non-zero) Ed25519 public key. Nothing is fetched at construction or boot.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "update_swap.hpp"

namespace mpx { namespace upd {

// A usable public key is exactly 64 hex chars and not the all-zero placeholder.
inline bool keyUsable(const std::string& pubHex) {
    if (pubHex.size() != 64) return false;
    bool nonzero = false;
    for (char c : pubHex) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
        if (c != '0') nonzero = true;
    }
#ifdef MUT_WIRE_ZERO_KEY_OK
    return true;
#endif
    return nonzero;
}

enum class WireState { Off, Idle, Running };

// Result of one raw https GET supplied by the daemon (tlsVerifiedGet with redirects off).
struct HttpGetResult { bool ok = false; int status = 0; std::string body; };
using HttpGet = std::function<HttpGetResult(const std::string& url, uint64_t maxBytes)>;

// TLS-verified fetcher: https URLs only (anything else is refused before the getter is called), HTTP 200 only,
// body capped at maxBytes. The getter is where chain + hostname verification lives.
inline Fetch makeHttpsFetch(HttpGet get) {
    return [get](const std::string& url, uint64_t maxBytes, std::string& body) -> FetchStatus {
#ifndef MUT_WIRE_ALLOW_HTTP
        if (url.compare(0, 8, "https://") != 0) return FetchStatus::Failed;
#endif
        if (!get) return FetchStatus::Failed;
        const HttpGetResult r = get(url, maxBytes);
        if (!r.ok || r.status != 200) return FetchStatus::Failed;
        if (r.body.size() > maxBytes) return FetchStatus::TooBig;
        body = r.body;
        return FetchStatus::Ok;
    };
}

class Updater {
  public:
    Updater(Fs& fs, Paths paths, Ctx ctx, bool keyOk, std::function<void(const std::string&)> log = nullptr)
        : fs_(fs), P_(std::move(paths)), c_(std::move(ctx)), log_(std::move(log)) {
        if (!keyOk) c_.conf.enabled = false;  // no usable key: never fetch, never swap
    }
    ~Updater() { if (t_.joinable()) t_.join(); }
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    bool enabled() const { return c_.conf.enabled; }
    WireState state() const {
        if (!c_.conf.enabled) return WireState::Off;
        return running_.load() ? WireState::Running : WireState::Idle;
    }
    // Starts one update on a worker thread. False (and nothing happens) when off or already running.
    bool start() {
        if (!c_.conf.enabled) return false;
        bool expect = false;
        if (!running_.compare_exchange_strong(expect, true)) return false;
        if (t_.joinable()) t_.join();  // the previous worker already finished (running_ was false)
        t_ = std::thread([this] {
            const Result r = runUpdate(fs_, P_, c_);
            last_.store(static_cast<int>(r));
            if (log_) log_(r == Result::Swapped ? "update: swapped; restart the daemon to run it"
                                                : "update: not applied (result " + std::to_string(static_cast<int>(r)) + ")");
            running_.store(false);
        });
        return true;
    }
    // Synchronous variant for tests and a console: same gates, no thread.
    Result runNow() { return runUpdate(fs_, P_, c_); }
    void join() { if (t_.joinable()) t_.join(); }
    Result last() const { return static_cast<Result>(last_.load()); }

  private:
    Fs& fs_;
    Paths P_;
    Ctx c_;
    std::function<void(const std::string&)> log_;
    std::atomic<bool> running_{false};
    std::atomic<int> last_{static_cast<int>(Result::Disabled)};
    std::thread t_;
};

}}  // namespace mpx::upd
