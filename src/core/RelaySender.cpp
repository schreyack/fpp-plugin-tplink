#include "RelaySender.h"

#include <algorithm>
#include <utility>

namespace tplink {

RelaySender::RelaySender(SendFn send, Timing timing)
    : m_send(std::move(send)), m_timing(timing) {
    if (m_timing.firstRetry < std::chrono::milliseconds(1)) {
        m_timing.firstRetry = std::chrono::milliseconds(1);
    }
    if (m_timing.maxRetry < m_timing.firstRetry) {
        m_timing.maxRetry = m_timing.firstRetry;
    }
}

RelaySender::~RelaySender() {
    stop();
}

void RelaySender::start() {
    if (m_thread.joinable()) {
        return;
    }
    m_stop.store(false);
    m_thread = std::thread(&RelaySender::run, this);
}

void RelaySender::request(bool on) {
    uint64_t current = m_request.load(std::memory_order_relaxed);
    uint64_t next = 0;
    do {
        next = (((current >> 1) + 1) << 1) | (on ? 1u : 0u);
    } while (!m_request.compare_exchange_weak(current, next));
    wake();
}

void RelaySender::requestStop() {
    m_stop.store(true);
    wake();
}

void RelaySender::stop() {
    requestStop();
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
        m_thread.join();
    }
}

bool RelaySender::idle() const {
    return m_request.load() == m_deliveredRequest.load();
}

RelaySender::Stats RelaySender::stats() const {
    return Stats{m_attempts.load(), m_delivered.load(), m_failed.load()};
}

void RelaySender::wake() {
    // Holding the mutex for an instant closes the gap between the sender
    // checking for work and starting to wait. The sender never holds it across
    // I/O, so this never waits on the network.
    { std::lock_guard<std::mutex> lock(m_mutex); }
    m_wake.notify_one();
}

bool RelaySender::attempt(bool on) {
    m_attempts.fetch_add(1);
    bool ok = false;
    try {
        ok = m_send && m_send(on, m_stop);
    } catch (...) {
        ok = false;
    }
    (ok ? m_delivered : m_failed).fetch_add(1);
    return ok;
}

void RelaySender::run() {
    auto retryDelay = m_timing.firstRetry;
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] {
            return m_stop.load() || m_request.load() != m_deliveredRequest.load();
        });
        if (m_stop.load()) {
            return;
        }

        const uint64_t word = m_request.load();
        lock.unlock();
        const bool ok = attempt((word & 1u) != 0);
        lock.lock();

        if (ok) {
            m_deliveredRequest.store(word);
            retryDelay = m_timing.firstRetry;
            continue;
        }

        // Wait before trying this state again. A newer desired state or a stop
        // ends the wait at once.
        m_wake.wait_for(lock, retryDelay, [this, word] {
            return m_stop.load() || m_request.load() != word;
        });
        retryDelay = std::min(retryDelay * 2, m_timing.maxRetry);
    }
}

}  // namespace tplink
