#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace tplink {

// The one sender a plug has for sequence control.
//
// It owns a single thread that does all of this plug's sequence I/O, so sends
// never overlap and never land out of order. The frame thread hands it a
// desired state with request(): a lock-free store plus a wake-up, with no
// allocation and no network I/O. The sender always sends the newest desired
// state, so states superseded while a send was in flight are skipped.
//
// A failed send is retried after a delay that starts at Timing::firstRetry and
// doubles up to Timing::maxRetry, for as long as that state is still the one
// wanted. A newer desired state ends the wait at once and is sent instead.
// Nothing ever latches the plug silent.
class RelaySender {
public:
    // Switches the relay; runs only on the sender thread. Returns true when the
    // plug acknowledged. `stop` turns true when the sender is told to stop, and
    // slow I/O should give up when it does.
    using SendFn = std::function<bool(bool on, std::atomic<bool> const& stop)>;

    struct Timing {
        std::chrono::milliseconds firstRetry;
        std::chrono::milliseconds maxRetry;
    };

    struct Stats {
        uint64_t attempts;
        uint64_t delivered;
        uint64_t failed;
    };

    // 250 ms, then 500 ms, 1 s, 2 s, 4 s, and every 8 s after that.
    static Timing defaultTiming() {
        return Timing{std::chrono::milliseconds(250), std::chrono::milliseconds(8000)};
    }

    RelaySender(SendFn send, Timing timing);
    ~RelaySender();

    RelaySender(RelaySender const&) = delete;
    RelaySender& operator=(RelaySender const&) = delete;

    // Starts the sender thread. Throws std::system_error if it cannot.
    void start();

    // Frame thread: asks for the relay to be on or off.
    void request(bool on);

    // Tells the sender to stop without waiting for it. An exchange in flight
    // sees `stop` turn true.
    void requestStop();

    // Stops the sender and waits for its thread. Safe to call more than once.
    void stop();

    // True when the newest requested state has been delivered, or nothing has
    // been requested.
    bool idle() const;

    Stats stats() const;

private:
    void run();
    bool attempt(bool on);
    void wake();

    SendFn m_send;
    Timing m_timing;

    // The newest desired state as (generation << 1) | on. Generation 0 means
    // nothing has been requested yet.
    std::atomic<uint64_t> m_request{0};
    // The request word most recently delivered.
    std::atomic<uint64_t> m_deliveredRequest{0};
    std::atomic<bool> m_stop{false};

    std::atomic<uint64_t> m_attempts{0};
    std::atomic<uint64_t> m_delivered{0};
    std::atomic<uint64_t> m_failed{0};

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
};

}  // namespace tplink
