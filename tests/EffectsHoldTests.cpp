// Unit tests for src/core/EffectsHold.h: while effects are held, every effect plug's
// channel reads 0 to the switches -- however many plugs have a channel -- and the
// frame is put back as it came in. No FPP and no test framework; `make -C tests`
// builds and runs them. The exit status is non-zero when anything fails. Give part of
// a test name to run only the tests whose names contain it.

#include "EffectsHold.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failedChecks = 0;

// While true, operator new calls are counted in g_allocations.
bool g_countAllocations = false;
std::atomic<unsigned long> g_allocations{0};

void recordFailure(char const* file, int line, std::string const& what) {
    ++g_failedChecks;
    std::printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

}  // namespace

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
            message_ << #actual << " is " << +actual_ << ", expected " << +expected_; \
            recordFailure(__FILE__, __LINE__, message_.str());                      \
        }                                                                           \
    } while (0)

void* operator new(std::size_t size) {
    if (g_countAllocations) {
        g_allocations.fetch_add(1);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

using tplink::EffectsHold;

// A frame with every channel lit, so a held channel reads as a 0 among 255s.
std::vector<uint8_t> litFrame(std::size_t channels) {
    return std::vector<uint8_t>(channels, 255);
}

// The house's three effect plugs today, 1-based 51137, 51138 and 51143 (fppd.log).
void testTheHousesThreePlugsAreHeld() {
    EffectsHold hold;
    hold.setChannels({51136, 51137, 51142});
    auto frame = litFrame(60000);
    std::vector<uint8_t> seen;
    hold.apply(frame.data(), true, [&](uint8_t* data) { seen.assign(data, data + frame.size()); });
    CHECK_EQ(seen[51136], 0);
    CHECK_EQ(seen[51137], 0);
    CHECK_EQ(seen[51142], 0);
    CHECK_EQ(seen[51138], 255);
    CHECK(frame == litFrame(60000));
}

// The fixed buffer this replaced held the first eight and let the ninth fire.
void testEveryPlugIsHeldPastEight() {
    for (unsigned plugs : {9u, 12u, 64u}) {
        EffectsHold hold;
        std::vector<unsigned int> channels;
        for (unsigned i = 0; i < plugs; ++i) {
            channels.push_back(100 + 3 * i);
        }
        hold.setChannels(channels);
        auto frame = litFrame(400);
        unsigned heldSeen = 0;
        hold.apply(frame.data(), true, [&](uint8_t* data) {
            for (unsigned int c : channels) {
                heldSeen += data[c] == 0 ? 1 : 0;
            }
        });
        CHECK_EQ(heldSeen, plugs);
        CHECK(frame == litFrame(400));
    }
}

void testLetGoTheShowReachesThePlugs() {
    EffectsHold hold;
    hold.setChannels({10, 20});
    auto frame = litFrame(32);
    std::vector<uint8_t> seen;
    hold.apply(frame.data(), false, [&](uint8_t* data) { seen.assign(data, data + frame.size()); });
    CHECK(seen == litFrame(32));
    CHECK(frame == litFrame(32));
}

// Each channel comes back with the value it had, not a neighbour's, in any order.
void testEachChannelComesBackAsItWas() {
    EffectsHold hold;
    hold.setChannels({30, 5, 17, 9, 22, 1, 28, 12, 3, 25});
    std::vector<uint8_t> frame(32);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        frame[i] = static_cast<uint8_t>(7 * i + 1);
    }
    auto const before = frame;
    hold.apply(frame.data(), true, [](uint8_t*) {});
    CHECK(frame == before);
}

// Two switches given one start channel: the channel must not come back as 0.
void testTwoPlugsOnOneChannelLeaveItAsItCame() {
    EffectsHold hold;
    hold.setChannels({40, 40, 41});
    auto frame = litFrame(64);
    uint8_t seen40 = 1;
    hold.apply(frame.data(), true, [&](uint8_t* data) { seen40 = data[40]; });
    CHECK_EQ(seen40, 0);
    CHECK_EQ(frame[40], 255);
    CHECK(frame == litFrame(64));
}

void testASendThatThrowsStillGetsTheFrameBack() {
    EffectsHold hold;
    hold.setChannels({2, 4, 6, 8, 10, 12, 14, 16, 18});
    auto frame = litFrame(24);
    bool threw = false;
    try {
        hold.apply(frame.data(), true, [](uint8_t*) { throw std::runtime_error("a plug failed"); });
    } catch (std::runtime_error const&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(frame == litFrame(24));
}

void testNoPlugsHoldsNothing() {
    EffectsHold hold;
    auto frame = litFrame(16);
    int sends = 0;
    hold.apply(frame.data(), true, [&](uint8_t*) { ++sends; });
    CHECK_EQ(sends, 1);
    CHECK(frame == litFrame(16));
}

// fppd calls this for every frame on its output thread: no allocation there.
void testAFrameDoesNotAllocate() {
    EffectsHold hold;
    std::vector<unsigned int> channels;
    for (unsigned i = 0; i < 40; ++i) {
        channels.push_back(i * 2);
    }
    hold.setChannels(channels);
    auto frame = litFrame(128);
    g_allocations = 0;
    g_countAllocations = true;
    for (int i = 0; i < 1000; ++i) {
        hold.apply(frame.data(), (i % 2) == 0, [](uint8_t*) {});
    }
    g_countAllocations = false;
    CHECK_EQ(g_allocations.load(), 0ul);
}

struct TestCase {
    char const* name;
    void (*run)();
};

const TestCase kTests[] = {
    {"the house's three effect plugs are held", testTheHousesThreePlugsAreHeld},
    {"every effect plug is held, past eight", testEveryPlugIsHeldPastEight},
    {"let go, the show reaches the plugs", testLetGoTheShowReachesThePlugs},
    {"each channel comes back as it was", testEachChannelComesBackAsItWas},
    {"two plugs on one channel leave it as it came", testTwoPlugsOnOneChannelLeaveItAsItCame},
    {"a send that throws still gets the frame back", testASendThatThrowsStillGetsTheFrameBack},
    {"no effect plugs holds nothing", testNoPlugsHoldsNothing},
    {"a frame does not allocate", testAFrameDoesNotAllocate},
};

}  // namespace

int main(int argc, char** argv) {
    int ran = 0;
    int failedTests = 0;
    for (TestCase const& test : kTests) {
        if (argc > 1 && std::strstr(test.name, argv[1]) == nullptr) {
            continue;
        }
        std::printf("%s\n", test.name);
        std::fflush(stdout);
        const int failuresBefore = g_failedChecks;
        test.run();
        ++ran;
        const bool passed = g_failedChecks == failuresBefore;
        if (!passed) {
            ++failedTests;
        }
        std::printf("  %s\n", passed ? "ok" : "FAILED");
        std::fflush(stdout);
    }
    std::printf("\n%d tests: %d passed, %d failed (%d checks, %d failed)\n", ran, ran - failedTests, failedTests,
                g_checks, g_failedChecks);
    return failedTests == 0 ? 0 : 1;
}
