// A plug under sequence control: the flip rule and the per-plug sender, driven
// frame by frame against the fake plug.

#include "TestHarness.h"

#include <mutex>
#include <stdexcept>

void testFlipRule() {
    using Action = RelayFollower::Action;
    RelayFollower follower;
    CHECK(follower.onValue(0) == Action::None);
    CHECK(follower.onValue(126) == Action::None);
    CHECK(!follower.seenOn());
    CHECK(follower.onValue(127) == Action::SwitchOn);
    CHECK(follower.seenOn());
    CHECK(follower.onValue(255) == Action::None);
    CHECK(follower.onValue(126) == Action::SwitchOff);
    CHECK(follower.onValue(0) == Action::None);
    CHECK(follower.onValue(200) == Action::SwitchOn);

    RelayFollower startsOn;
    CHECK(startsOn.onValue(255) == Action::SwitchOn);
}

// (a)
void testUnknownChannelAtZeroSendsNothing() {
    FakeKasaPlug plug;
    CHECK(plug.listen());
    KasaSwitchUnderTest plugSwitch(plug.port());
    SequenceRelay relay(plugSwitch.sendFn());
    relay.start();

    pumpFrames(relay, 0, 5000);
    pumpFrames(relay, 126, 100);
    std::this_thread::sleep_for(300ms);  // anything sent late has landed by now

    note(describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayCommands().size(), size_t{0});
    CHECK_EQ(relay.sender().stats().attempts, uint64_t{0});
    relay.stop();
}

// (b)
void testRiseSendsOneOnAndHoldingSendsNothingMore() {
    FakeKasaPlug plug;
    CHECK(plug.listen());
    KasaSwitchUnderTest plugSwitch(plug.port());
    SequenceRelay relay(plugSwitch.sendFn());
    relay.start();

    pumpFrames(relay, 0, 10);
    pumpFrames(relay, 255, 5000);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 2000ms));
    std::this_thread::sleep_for(300ms);

    note(describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayCommandCount(1), size_t{1});
    CHECK_EQ(plug.relayCommandCount(0), size_t{0});
    CHECK_EQ(plug.relayState(), 1);
    relay.stop();
}

// (c)
void testFallSendsOneOff() {
    FakeKasaPlug plug;
    CHECK(plug.listen());
    KasaSwitchUnderTest plugSwitch(plug.port());
    SequenceRelay relay(plugSwitch.sendFn());
    relay.start();

    pumpFrames(relay, 255, 100);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 2000ms));
    pumpFrames(relay, 0, 5000);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 2000ms));
    std::this_thread::sleep_for(300ms);

    const auto commands = plug.relayCommands();
    note(describeCommands(commands));
    CHECK_EQ(plug.relayCommandCount(1), size_t{1});
    CHECK_EQ(plug.relayCommandCount(0), size_t{1});
    CHECK(commands.size() == 2 && commands[0].state == 1 && commands[1].state == 0);
    CHECK_EQ(plug.relayState(), 0);
    relay.stop();
}

// (d)
void testRapidFlipsEndOnTheNewestState() {
    for (bool endsOn : {true, false}) {
        FakeKasaPlug plug;
        plug.setReplyDelay(40ms);  // each send is still in flight when the next flip comes
        CHECK(plug.listen());
        KasaSwitchUnderTest plugSwitch(plug.port());
        SequenceRelay relay(plugSwitch.sendFn());
        relay.start();

        const std::vector<uint8_t> pattern =
            endsOn ? std::vector<uint8_t>{255, 0, 255, 0, 255} : std::vector<uint8_t>{255, 0, 255, 0};
        for (uint8_t value : pattern) {
            relay.onChannelValue(value);
            std::this_thread::sleep_for(5ms);
        }
        CHECK(waitUntil([&] { return relay.sender().idle(); }, 3000ms));
        std::this_thread::sleep_for(200ms);

        const auto commands = plug.relayCommands();
        note(std::string(endsOn ? "255/0/255/0/255, " : "255/0/255/0, ") + describeCommands(commands) +
             ", most requests open at once: " + std::to_string(plug.maxOverlappingRequests()));
        const int wanted = endsOn ? 1 : 0;
        CHECK_EQ(plug.relayState(), wanted);
        CHECK(!commands.empty() && commands.back().state == wanted);
        CHECK(commands.size() <= pattern.size());
        CHECK_EQ(plug.maxOverlappingRequests(), 1);
        relay.stop();
    }
}

// (e), first half
void testRefusedPlugIsRetriedWithBackoff() {
    const uint16_t port = FakeKasaPlug::unusedPort();
    KasaSwitchUnderTest plugSwitch(port);
    const RelaySender::SendFn send = plugSwitch.sendFn();
    std::mutex attemptsMutex;
    std::vector<Clock::time_point> attempts;
    SequenceRelay relay(
        [&](bool on, std::atomic<bool> const& stop) {
            {
                std::lock_guard<std::mutex> lock(attemptsMutex);
                attempts.push_back(Clock::now());
            }
            return send(on, stop);
        },
        RelaySender::Timing{40ms, 160ms});
    relay.start();

    relay.onChannelValue(255);
    std::this_thread::sleep_for(900ms);

    std::vector<Clock::time_point> seen;
    {
        std::lock_guard<std::mutex> lock(attemptsMutex);
        seen = attempts;
    }
    std::string gapsText = "tries: " + std::to_string(seen.size()) + ", gaps (ms):";
    std::vector<long long> gaps;
    for (size_t i = 1; i < seen.size(); ++i) {
        gaps.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(seen[i] - seen[i - 1]).count());
        gapsText += " " + std::to_string(gaps.back());
    }
    note(gapsText);

    // Tries at about 0, 40, 120, 280, 440, 600 and 760 ms.
    CHECK(seen.size() >= 5);
    CHECK(seen.size() <= 9);
    const long long shortest[] = {40, 80, 160, 160, 160, 160, 160, 160};
    for (size_t i = 0; i < gaps.size() && i < 8; ++i) {
        CHECK(gaps[i] >= shortest[i] - 1);
        CHECK(gaps[i] <= 160 + 150);
    }
    CHECK_EQ(relay.sender().stats().delivered, uint64_t{0});
    CHECK(!relay.sender().idle());

    // The plug comes back, and the ON that is still wanted gets through.
    FakeKasaPlug plug;
    CHECK(plug.listen(port));
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 1000ms));
    note(describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayCommandCount(1), size_t{1});
    CHECK_EQ(plug.relayState(), 1);
    relay.stop();
}

// (e), second half
void testNewerStateSupersedesThePendingRetry() {
    const uint16_t port = FakeKasaPlug::unusedPort();
    KasaSwitchUnderTest plugSwitch(port);
    SequenceRelay relay(plugSwitch.sendFn(), RelaySender::Timing{1500ms, 1500ms});
    relay.start();

    relay.onChannelValue(255);  // refused; the ON now waits 1.5 s to be tried again
    CHECK(waitUntil([&] { return relay.sender().stats().failed == 1; }, 1000ms));

    FakeKasaPlug plug;
    CHECK(plug.listen(port));  // the plug is reachable again
    const auto changed = Clock::now();
    relay.onChannelValue(0);  // and the show now wants it off

    CHECK(waitUntil([&] { return plug.relayCommandCount(0) == 1; }, 1000ms));
    const long long offAfterMs = msSince(changed);
    note("OFF landed " + std::to_string(offAfterMs) + " ms after the change");
    CHECK(offAfterMs < 500);              // at once, not when the 1.5 s retry was due
    std::this_thread::sleep_for(1800ms);  // past the moment the ON retry was due

    note(describeCommands(plug.relayCommands()));
    CHECK_EQ(plug.relayCommandCount(1), size_t{0});
    CHECK_EQ(plug.relayState(), 0);
    CHECK_EQ(relay.sender().stats().attempts, uint64_t{2});
    relay.stop();
}

// Requirement 3: the per-frame work is a compare plus a hand-off.
void testFramePathDoesNotAllocateOrWaitOnThePlug() {
    FakeKasaPlug plug;
    plug.setReplyDelay(20ms);  // a frame path that waited on the plug would take seconds
    CHECK(plug.listen());
    KasaSwitchUnderTest plugSwitch(plug.port());
    SequenceRelay relay(plugSwitch.sendFn());
    relay.start();

    std::vector<uint8_t> frames(5000);
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i] = (i / 100) % 2 == 0 ? 0 : 255;  // 49 flips, ending ON
    }

    g_allocationsCounted.store(0);
    t_countAllocations = true;
    const auto start = Clock::now();
    for (uint8_t value : frames) {
        relay.onChannelValue(value);
    }
    const auto elapsed = Clock::now() - start;
    t_countAllocations = false;

    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    note("5,000 frames, 49 flips: " + std::to_string(elapsedUs) + " us, " +
         std::to_string(g_allocationsCounted.load()) + " allocations");
    CHECK_EQ(g_allocationsCounted.load(), 0ul);
    CHECK(elapsed < 200ms);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 2000ms));
    CHECK_EQ(plug.relayState(), 1);
    relay.stop();
}

// Requirement 3: stop and shutdown stay prompt.
void testStopIsPrompt() {
    {
        FakeKasaPlug plug;
        plug.setNeverReply(true);
        CHECK(plug.listen());
        KasaSwitchUnderTest plugSwitch(plug.port());
        SequenceRelay relay(plugSwitch.sendFn());
        relay.start();
        relay.onChannelValue(255);
        CHECK(waitUntil([&] { return plug.requestsReceived() == 1; }, 1000ms));
        const auto start = Clock::now();
        relay.stop();
        const long long stopMs = msSince(start);
        note("stop while the plug holds a reply: " + std::to_string(stopMs) + " ms");
        CHECK(stopMs < 250);
    }
    {
        const uint16_t port = FakeKasaPlug::unusedPort();
        KasaSwitchUnderTest plugSwitch(port);
        SequenceRelay relay(plugSwitch.sendFn(), RelaySender::Timing{30000ms, 30000ms});
        relay.start();
        relay.onChannelValue(255);
        CHECK(waitUntil([&] { return relay.sender().stats().failed == 1; }, 1000ms));
        const auto start = Clock::now();
        relay.stop();
        const long long stopMs = msSince(start);
        note("stop during a 30 s retry wait: " + std::to_string(stopMs) + " ms");
        CHECK(stopMs < 250);
    }
}

// Requirement 4: an exception is a failed send, retried, never a latch.
void testAThrowingSendIsRetriedNotLatched() {
    std::atomic<int> calls{0};
    SequenceRelay relay(
        [&](bool, std::atomic<bool> const&) -> bool {
            if (++calls <= 2) {
                throw std::runtime_error("network fell over");
            }
            return true;
        },
        RelaySender::Timing{5ms, 20ms});
    relay.start();
    relay.onChannelValue(255);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 1000ms));
    CHECK_EQ(calls.load(), 3);
    CHECK_EQ(relay.sender().stats().failed, uint64_t{2});
    CHECK_EQ(relay.sender().stats().delivered, uint64_t{1});
    relay.stop();
}
