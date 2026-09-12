// The bounded retry: a state is retried only within its window, a plug that
// comes back afterwards is left alone, and a newer request starts fresh.

#include "TestHarness.h"

#include <mutex>

// (g)
void testRetriesStopOnceTheWindowHasPassed() {
    const uint16_t port = FakeKasaPlug::unusedPort();
    KasaSwitchUnderTest plugSwitch(port);
    AttemptLog log;
    std::mutex gaveUpMutex;
    std::vector<std::string> gaveUpCalls;
    SequenceRelay relay(log.wrap(plugSwitch.sendFn()), RelaySender::Timing{5ms, 20ms, 200ms},
                        [&](bool on, unsigned tries, std::chrono::milliseconds window) {
                            std::lock_guard<std::mutex> lock(gaveUpMutex);
                            gaveUpCalls.push_back(std::string(on ? "on" : "off") + " after " + std::to_string(tries) +
                                                  " tries over " + std::to_string(window.count()) + " ms");
                        });
    relay.start();

    const auto requested = Clock::now();
    relay.onChannelValue(255);  // the plug refuses, and the ON is retried
    CHECK(waitUntil([&] { return relay.sender().gaveUp(); }, 1000ms));
    const long long gaveUpMs = msSince(requested);
    const size_t triesInWindow = log.size();
    std::this_thread::sleep_for(400ms);  // a while after the window

    const auto attempts = log.attempts();
    const long long lastTryMs = attempts.empty() ? -1 : msBetween(requested, attempts.back().at);
    note("gave up after " + std::to_string(gaveUpMs) + " ms; tries in the window: " + std::to_string(triesInWindow) +
         ", the last at " + std::to_string(lastTryMs) + " ms; tries in the 400 ms after: " +
         std::to_string(attempts.size() - triesInWindow));
    CHECK(gaveUpMs >= 195 && gaveUpMs < 600);
    CHECK(triesInWindow >= 5);
    CHECK(lastTryMs >= 0 && lastTryMs <= 205);
    CHECK_EQ(attempts.size(), triesInWindow);
    {
        std::lock_guard<std::mutex> lock(gaveUpMutex);
        note(gaveUpCalls.empty() ? std::string("no give-up call") : "give-up call: " + gaveUpCalls[0]);
        CHECK_EQ(gaveUpCalls.size(), size_t{1});
        CHECK(gaveUpCalls.size() == 1 &&
              gaveUpCalls[0] == "on after " + std::to_string(triesInWindow) + " tries over 200 ms");
    }
    const auto stats = relay.sender().stats();
    CHECK_EQ(stats.abandoned, uint64_t{1});
    CHECK_EQ(stats.delivered, uint64_t{0});
    CHECK(relay.sender().idle());

    // Hours later, in miniature: the plug comes back, and nothing switches it on.
    FakeKasaPlug plug;
    CHECK(plug.listen(port));
    std::this_thread::sleep_for(300ms);
    note("after the plug came back, " + describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayCommands().size(), size_t{0});
    relay.stop();
}

// (h)
void testAfterGivingUpTheNextChangeIsSentAtOnce() {
    const uint16_t port = FakeKasaPlug::unusedPort();
    KasaSwitchUnderTest plugSwitch(port);
    SequenceRelay relay(plugSwitch.sendFn(), RelaySender::Timing{5ms, 20ms, 200ms});
    relay.start();

    relay.onChannelValue(255);
    CHECK(waitUntil([&] { return relay.sender().gaveUp(); }, 1000ms));

    FakeKasaPlug plug;
    CHECK(plug.listen(port));  // the plug is back; the ON that was given up stays given up
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(plug.relayCommands().size(), size_t{0});

    const auto changed = Clock::now();
    relay.onChannelValue(0);  // the next change on its channel
    CHECK(waitUntil([&] { return plug.relayCommandCount(0) == 1; }, 1000ms));
    const long long offMs = msSince(changed);
    CHECK(offMs < 200);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 1000ms));
    CHECK(!relay.sender().gaveUp());

    relay.onChannelValue(255);  // and the change after that
    CHECK(waitUntil([&] { return plug.relayCommandCount(1) == 1; }, 1000ms));
    note("OFF arrived " + std::to_string(offMs) + " ms after the change; " + describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayState(), 1);
    relay.stop();
}

// (i)
void testANewerRequestStartsAgainFromTheFirstRetryDelay() {
    const uint16_t port = FakeKasaPlug::unusedPort();
    KasaSwitchUnderTest plugSwitch(port);
    AttemptLog log;
    SequenceRelay relay(log.wrap(plugSwitch.sendFn()), RelaySender::Timing{20ms, 1000ms, 60000ms});
    relay.start();

    relay.onChannelValue(255);           // refused: ON tries at about 0, 20, 60, 140, 300 and 620 ms
    std::this_thread::sleep_for(700ms);  // by now the ON is in a 640 ms wait
    relay.onChannelValue(0);             // the OFF supersedes it during that wait

    CHECK(waitUntil([&] { return log.count(false) >= 2; }, 1500ms));
    std::vector<Clock::time_point> onTries;
    std::vector<Clock::time_point> offTries;
    for (auto const& attempt : log.attempts()) {
        (attempt.on ? onTries : offTries).push_back(attempt.at);
    }
    const long long lastOnGap =
        onTries.size() >= 2 ? msBetween(onTries[onTries.size() - 2], onTries.back()) : -1;
    const long long firstOffGap = offTries.size() >= 2 ? msBetween(offTries[0], offTries[1]) : -1;
    note("ON tries: " + std::to_string(onTries.size()) + ", the last gap " + std::to_string(lastOnGap) +
         " ms; the OFF's first retry came " + std::to_string(firstOffGap) + " ms after its first try");
    CHECK(lastOnGap >= 300);                        // the ON's delay had doubled well past 20 ms
    CHECK(firstOffGap >= 19 && firstOffGap < 200);  // the OFF started again from 20 ms
    relay.stop();
}
