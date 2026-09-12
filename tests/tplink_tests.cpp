// Unit tests for src/core: the ON/OFF flip rule, the per-plug sender and the
// legacy Kasa protocol, run against a fake plug on 127.0.0.1. No FPP and no
// test framework; `make -C tests` builds and runs them. The exit status is
// non-zero when anything fails. Give part of a test name to run only the tests
// whose names contain it.

#include "TestHarness.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <new>

thread_local bool t_countAllocations = false;
std::atomic<unsigned long> g_allocationsCounted{0};

void* operator new(std::size_t size) {
    if (t_countAllocations) {
        g_allocationsCounted.fetch_add(1);
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

struct TestCase {
    char const* name;
    void (*run)();
};

const TestCase kTests[] = {
    {"framing matches the Kasa plug protocol", testFramingMatchesThePlugProtocol},
    {"flip rule: 127 is on, unknown then off does nothing", testFlipRule},
    {"(a) unknown channel at 0 for 5,000 frames sends nothing", testUnknownChannelAtZeroSendsNothing},
    {"(b) 0 -> 255 sends one ON, holding 255 for 5,000 frames sends nothing more",
     testRiseSendsOneOnAndHoldingSendsNothingMore},
    {"(c) 255 -> 0 sends exactly one OFF", testFallSendsOneOff},
    {"(d) rapid flips end on the newest state, in order, never overlapping", testRapidFlipsEndOnTheNewestState},
    {"(e) a refused plug is retried with backoff", testRefusedPlugIsRetriedWithBackoff},
    {"(e) a newer state supersedes the pending retry", testNewerStateSupersedesThePendingRetry},
    {"(f) a strip outlet's command carries its child id", testStripOutletCommandCarriesItsChildId},
    {"(g) retries stop once the window has passed, and a plug that comes back stays off",
     testRetriesStopOnceTheWindowHasPassed},
    {"(h) after giving up, the next change is sent at once", testAfterGivingUpTheNextChangeIsSentAtOnce},
    {"(i) a newer request starts again from the first retry delay",
     testANewerRequestStartsAgainFromTheFirstRetryDelay},
    {"frame path does not allocate or wait on the plug", testFramePathDoesNotAllocateOrWaitOnThePlug},
    {"stop is prompt mid-exchange and mid-backoff", testStopIsPrompt},
    {"a throwing send is retried, not latched", testAThrowingSendIsRetriedNotLatched},
    {"query reads long replies and gives up on silent plugs", testQueryReadsLongRepliesAndGivesUpOnSilentPlugs},
};

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
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
