// Unit tests for src/core/DeviceNames.h: commands find a plug by the name in
// plugin.tplink.json, never by its IP. No FPP and no test framework; `make -C tests`
// builds and runs them. Give part of a test name to run only the tests whose names
// contain it.

#include "DeviceNames.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failedChecks = 0;

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

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        ++g_checks;                                                           \
        const auto actual_ = (actual);                                        \
        const auto expected_ = (expected);                                    \
        if (!(actual_ == expected_)) {                                        \
            std::ostringstream message_;                                      \
            message_ << #actual << " is " << actual_ << ", expected " << expected_; \
            recordFailure(__FILE__, __LINE__, message_.str());                \
        }                                                                     \
    } while (0)

namespace {

using tplink::names::duplicates;
using tplink::names::resolve;

const std::vector<std::string> kHouse = {"LED Roof", "LED Tree", "LED Snow", "LED Snow 2", "Blower"};

void testANameFindsItsPlug() {
    auto r = resolve("LED Tree", kHouse);
    CHECK_EQ(r.indices.size(), 1u);
    CHECK_EQ(r.indices[0], 1u);
    CHECK(r.unknown.empty());
}

void testCaseAndSpacesDoNotMatter() {
    auto r = resolve("  led snow 2 ", kHouse);
    CHECK_EQ(r.indices.size(), 1u);
    CHECK_EQ(r.indices[0], 3u);
}

void testAPrefixIsNotAMatch() {
    auto r = resolve("LED Snow", kHouse);
    CHECK_EQ(r.indices.size(), 1u);
    CHECK_EQ(r.indices[0], 2u);
    CHECK(resolve("LED", kHouse).indices.empty());
}

void testACommaListFindsEachInOrder() {
    auto r = resolve("Blower, LED Roof,LED Tree", kHouse);
    CHECK_EQ(r.indices.size(), 3u);
    CHECK_EQ(r.indices[0], 4u);
    CHECK_EQ(r.indices[1], 0u);
    CHECK_EQ(r.indices[2], 1u);
}

void testANameTwiceIsSentOnce() {
    auto r = resolve("Blower,blower", kHouse);
    CHECK_EQ(r.indices.size(), 1u);
}

void testAnIpIsNotAName() {
    auto r = resolve("192.168.110.158", kHouse);
    CHECK(r.indices.empty());
    CHECK_EQ(r.unknown.size(), 1u);
    CHECK_EQ(r.unknown[0], std::string("192.168.110.158"));
}

void testUnknownNamesAreReportedAndTheRestStillResolve() {
    auto r = resolve("LED Roof, Nope", kHouse);
    CHECK_EQ(r.indices.size(), 1u);
    CHECK_EQ(r.unknown.size(), 1u);
    CHECK_EQ(r.unknown[0], std::string("Nope"));
}

void testBlankTargetResolvesNothing() {
    CHECK(resolve("", kHouse).indices.empty());
    CHECK(resolve(" , ", kHouse).unknown.empty());
}

void testDuplicateNamesAreFoundOnce() {
    std::vector<std::string> names = {"A", "b", "a", "B", "A", "C"};
    auto d = duplicates(names);
    CHECK_EQ(d.size(), 2u);
    CHECK_EQ(d[0], std::string("a"));
    CHECK_EQ(d[1], std::string("B"));
    CHECK(duplicates(kHouse).empty());
}

struct TestCase {
    char const* name;
    void (*run)();
};

const TestCase kTests[] = {
    {"a name finds its plug", testANameFindsItsPlug},
    {"case and spaces do not matter", testCaseAndSpacesDoNotMatter},
    {"a prefix is not a match", testAPrefixIsNotAMatch},
    {"a comma list finds each, in order", testACommaListFindsEachInOrder},
    {"a name twice is sent once", testANameTwiceIsSentOnce},
    {"an IP is not a name", testAnIpIsNotAName},
    {"unknown names are reported, the rest still resolve", testUnknownNamesAreReportedAndTheRestStillResolve},
    {"a blank target resolves nothing", testBlankTargetResolvesNothing},
    {"duplicate names are found once", testDuplicateNamesAreFoundOnce},
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
