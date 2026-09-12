#pragma once

// Shared pieces of the unit tests: a very small check harness, helpers for
// driving a plug under sequence control, and the list of test functions.

#include "FakeKasaPlug.h"

#include "KasaProtocol.h"
#include "RelayFollower.h"
#include "RelaySender.h"
#include "SequenceRelay.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "fixtures"
#endif

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using tplink::RelayFollower;
using tplink::RelaySender;
using tplink::SequenceRelay;
namespace kasa = tplink::kasa;

inline int g_checks = 0;
inline int g_failedChecks = 0;

// While true on a thread, operator new calls on that thread are counted in
// g_allocationsCounted. Both live in tplink_tests.cpp.
extern thread_local bool t_countAllocations;
extern std::atomic<unsigned long> g_allocationsCounted;

inline void recordFailure(char const* file, int line, std::string const& what) {
    ++g_failedChecks;
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

#define CHECK(condition)                                   \
    do {                                                   \
        ++g_checks;                                        \
        if (!(condition)) {                                \
            recordFailure(__FILE__, __LINE__, #condition); \
        }                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                                  \
    do {                                                                            \
        ++g_checks;                                                                 \
        const auto actual_ = (actual);                                              \
        const auto expected_ = (expected);                                          \
        if (!(actual_ == expected_)) {                                              \
            std::ostringstream message_;                                            \
            message_ << #actual << " is " << actual_ << ", expected " << expected_; \
            recordFailure(__FILE__, __LINE__, message_.str());                      \
        }                                                                           \
    } while (0)

inline void note(std::string const& text) {
    std::printf("    %s\n", text.c_str());
}

inline bool waitUntil(std::function<bool()> const& done, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (!done()) {
        if (Clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

inline long long msBetween(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

inline long long msSince(Clock::time_point start) {
    return msBetween(start, Clock::now());
}

// Feeds `count` frames of one channel value, at a fast show's pace.
inline void pumpFrames(SequenceRelay& relay, uint8_t value, int count) {
    for (int i = 0; i < count; ++i) {
        relay.onChannelValue(value);
        std::this_thread::sleep_for(100us);
    }
}

inline std::string readFixture(char const* name) {
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name, std::ios::binary);
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

inline std::string describeCommands(std::vector<FakeKasaPlug::RelayCommand> const& commands) {
    std::string out = "plug received:";
    if (commands.empty()) {
        out += " nothing";
    }
    for (auto const& command : commands) {
        out += command.state == 1 ? " ON" : command.state == 0 ? " OFF" : " ?";
    }
    return out;
}

// What TPLinkSwitch does for the sequence, built from the same core calls:
// address the relay command to the outlet (a strip outlet's child id comes from
// sysinfo, looked up once), send it over the legacy protocol, and count any
// reply as delivered.
class KasaSwitchUnderTest {
public:
    explicit KasaSwitchUnderTest(uint16_t port, int plugNumber = 0) : m_port(port), m_plugNumber(plugNumber) {}

    RelaySender::SendFn sendFn() {
        return [this](bool on, std::atomic<bool> const& stop) { return send(on, stop); };
    }

private:
    bool send(bool on, std::atomic<bool> const& stop) {
        kasa::QueryOptions options;
        options.cancel = &stop;
        if (m_plugNumber != 0 && m_childId.empty()) {
            std::string sysinfo;
            if (kasa::query("127.0.0.1", m_port, kasa::sysinfoCommand(m_plugNumber), sysinfo, options)) {
                m_childId = kasa::idFromSysinfo(sysinfo, m_plugNumber);
            }
        }
        std::string reply;
        const std::string command = kasa::addressedCommand(kasa::relayStateCommand(on), m_plugNumber, m_childId);
        return kasa::query("127.0.0.1", m_port, command, reply, options) && !reply.empty();
    }

    uint16_t m_port;
    int m_plugNumber;
    std::string m_childId;  // sender thread only
};

// Records when each send was tried, and for which state, in front of a real
// send function.
class AttemptLog {
public:
    struct Attempt {
        Clock::time_point at;
        bool on;
    };

    RelaySender::SendFn wrap(RelaySender::SendFn inner) {
        return [this, inner](bool on, std::atomic<bool> const& stop) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_attempts.push_back(Attempt{Clock::now(), on});
            }
            return inner(on, stop);
        };
    }

    std::vector<Attempt> attempts() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_attempts;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_attempts.size();
    }

    size_t count(bool on) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t n = 0;
        for (auto const& attempt : m_attempts) {
            n += attempt.on == on ? 1 : 0;
        }
        return n;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<Attempt> m_attempts;
};

// ProtocolTests.cpp
void testFramingMatchesThePlugProtocol();
void testStripOutletCommandCarriesItsChildId();
void testQueryReadsLongRepliesAndGivesUpOnSilentPlugs();

// SequenceTests.cpp
void testFlipRule();
void testUnknownChannelAtZeroSendsNothing();
void testRiseSendsOneOnAndHoldingSendsNothingMore();
void testFallSendsOneOff();
void testRapidFlipsEndOnTheNewestState();
void testRefusedPlugIsRetriedWithBackoff();
void testNewerStateSupersedesThePendingRetry();
void testFramePathDoesNotAllocateOrWaitOnThePlug();
void testStopIsPrompt();
void testAThrowingSendIsRetriedNotLatched();

// GiveUpTests.cpp
void testRetriesStopOnceTheWindowHasPassed();
void testAfterGivingUpTheNextChangeIsSentAtOnce();
void testANewerRequestStartsAgainFromTheFirstRetryDelay();
