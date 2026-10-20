// Unit tests for src/core/EffectsHold.h: while effects are held, every effect plug's
// channel reads 0 to the switches -- however many plugs have a channel -- and the
// frame is put back as it came in; every DMX effect machine's channels (fog, smoke) go
// out at 0 and stay 0 in the frame the controllers get (ticket #1032). These guard the
// house's fog machines. No FPP and no test framework; `make -C tests`
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
    std::string why;
    CHECK(hold.addZeroedRange(100, 20, 128, why));
    auto frame = litFrame(128);
    g_allocations = 0;
    g_countAllocations = true;
    for (int i = 0; i < 1000; ++i) {
        hold.apply(frame.data(), (i % 2) == 0, [](uint8_t*) {});
    }
    g_countAllocations = false;
    CHECK_EQ(g_allocations.load(), 0ul);
}


// The house's three DMX fog machines on Falcon_1, 1-based 78475, 78476 and 78477 (ticket #1032).
const unsigned long kFogFirst = 78475;
const unsigned long kFogCount = 3;
const std::size_t kHouseFrame = 103023;  // what FPP reads on the house (103,023 channels)

void addHousesFog(EffectsHold& hold) {
    std::string why;
    CHECK(hold.addZeroedRange(kFogFirst, kFogCount, kHouseFrame, why));
    CHECK(why.empty());
}

// Held: the fog channels are 0 in the frame that goes out, and stay 0 (they are not put back,
// unlike a plug's channel); the channels beside them are untouched.
void testHeldTheFogChannelsGoOutAtZero() {
    EffectsHold hold;
    addHousesFog(hold);
    auto frame = litFrame(kHouseFrame);
    std::vector<uint8_t> seenBySend;
    hold.apply(frame.data(), true, [&](uint8_t* data) { seenBySend.assign(data, data + frame.size()); });
    for (unsigned long channel = kFogFirst; channel < kFogFirst + kFogCount; ++channel) {
        CHECK_EQ(seenBySend[channel - 1], 0);
        CHECK_EQ(frame[channel - 1], 0);
    }
    CHECK_EQ(frame[kFogFirst - 2], 255);
    CHECK_EQ(frame[kFogFirst + kFogCount - 1], 255);
    std::size_t zeros = 0;
    for (uint8_t value : frame) {
        zeros += value == 0 ? 1 : 0;
    }
    CHECK_EQ(zeros, static_cast<std::size_t>(kFogCount));
}

// Released: the frame reaches the controllers exactly as the show wrote it.
void testReleasedTheFogChannelsAreUntouched() {
    EffectsHold hold;
    addHousesFog(hold);
    hold.setChannels({51136, 51137, 51142});
    auto frame = litFrame(kHouseFrame);
    std::vector<uint8_t> seenBySend;
    hold.apply(frame.data(), false, [&](uint8_t* data) { seenBySend.assign(data, data + frame.size()); });
    CHECK(seenBySend == litFrame(kHouseFrame));
    CHECK(frame == litFrame(kHouseFrame));
}

// Held, with the house's plugs and fog: the plugs read 0 to the switches and come back; the fog
// stays 0 -- the two ways of holding side by side.
void testHeldPlugsComeBackAndFogStaysZero() {
    EffectsHold hold;
    addHousesFog(hold);
    hold.setChannels({51136, 51137, 51142});
    auto frame = litFrame(kHouseFrame);
    std::vector<uint8_t> seenBySend;
    hold.apply(frame.data(), true, [&](uint8_t* data) { seenBySend.assign(data, data + frame.size()); });
    CHECK_EQ(seenBySend[51136], 0);
    CHECK_EQ(seenBySend[51137], 0);
    CHECK_EQ(seenBySend[51142], 0);
    CHECK_EQ(seenBySend[kFogFirst - 1], 0);
    CHECK_EQ(frame[51136], 255);
    CHECK_EQ(frame[51137], 255);
    CHECK_EQ(frame[51142], 255);
    CHECK_EQ(frame[kFogFirst - 1], 0);
    CHECK_EQ(frame[kFogFirst + 1], 0);
}

// A plug whose channel sits inside a fog range: the fog's zero wins, the plug is not put back lit.
void testAPlugInsideAFogRangeStaysZero() {
    EffectsHold hold;
    std::string why;
    CHECK(hold.addZeroedRange(11, 5, 64, why));
    hold.setChannels({12});
    auto frame = litFrame(64);
    hold.apply(frame.data(), true, [](uint8_t*) {});
    CHECK_EQ(frame[12], 0);
    CHECK_EQ(frame[10], 0);
    CHECK_EQ(frame[14], 0);
    CHECK_EQ(frame[15], 255);
}

// The fog is held with no effect plug at all.
void testFogIsHeldWithNoPlugs() {
    EffectsHold hold;
    addHousesFog(hold);
    auto frame = litFrame(kHouseFrame);
    int sends = 0;
    hold.apply(frame.data(), true, [&](uint8_t*) { ++sends; });
    CHECK_EQ(sends, 1);
    CHECK_EQ(frame[kFogFirst - 1], 0);
}

// A range that is not whole is refused in words and adds nothing: the machine is never half held.
void testARangeThatIsNotWholeIsRefused() {
    EffectsHold hold;
    std::string why;
    CHECK(!hold.addZeroedRange(0, 3, 100, why));
    CHECK(!why.empty());
    why.clear();
    CHECK(!hold.addZeroedRange(5, 0, 100, why));
    CHECK(!why.empty());
    why.clear();
    CHECK(!hold.addZeroedRange(99, 3, 100, why));
    CHECK(!why.empty());
    CHECK_EQ(hold.zeroedRangeCount(), 0ul);
    // The last channel FPP has is inside.
    why.clear();
    CHECK(hold.addZeroedRange(100, 1, 100, why));
    CHECK_EQ(hold.zeroedRangeCount(), 1ul);
    auto frame = litFrame(100);
    hold.apply(frame.data(), true, [](uint8_t*) {});
    CHECK_EQ(frame[99], 0);
    CHECK_EQ(frame[98], 255);
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
    {"held, the fog channels go out at zero", testHeldTheFogChannelsGoOutAtZero},
    {"released, the fog channels are untouched", testReleasedTheFogChannelsAreUntouched},
    {"held, plugs come back and fog stays zero", testHeldPlugsComeBackAndFogStaysZero},
    {"a plug inside a fog range stays zero", testAPlugInsideAFogRangeStaysZero},
    {"fog is held with no plugs", testFogIsHeldWithNoPlugs},
    {"a range that is not whole is refused", testARangeThatIsNotWholeIsRefused},
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
