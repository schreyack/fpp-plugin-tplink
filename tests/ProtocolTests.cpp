// The legacy Kasa protocol: framing, strip addressing and the socket exchange.

#include "TestHarness.h"

#include <cstring>

void testFramingMatchesThePlugProtocol() {
    const std::string command = "{\"system\":{\"get_sysinfo\":{}}}";
    const std::string framed = kasa::frame(command);
    // The well-known opening bytes of get_sysinfo as a Kasa plug expects them.
    const unsigned char opening[] = {0x00, 0x00, 0x00, 0x1d, 0xd0, 0xf2, 0x81, 0xf8,
                                     0x8b, 0xff, 0x9a, 0xf7, 0xd5, 0xef, 0x94, 0xb6};
    CHECK_EQ(framed.size(), command.size() + 4);
    CHECK(framed.size() >= sizeof(opening) && std::memcmp(framed.data(), opening, sizeof(opening)) == 0);
    CHECK_EQ(kasa::decrypt(framed.substr(4)), command);
    // The fake plug's framing is written separately; the two must agree.
    CHECK_EQ(FakeKasaPlug::decode(framed.substr(4)), command);
    CHECK_EQ(kasa::decrypt(FakeKasaPlug::encode(command)), command);
}

// (f)
void testStripOutletCommandCarriesItsChildId() {
    // An HS300 get_sysinfo reply: the shape python-kasa recorded from HS300(US)
    // hw 1.0 fw 1.0.21, with made-up ids in the device's own form (device id
    // plus a two-digit outlet index) and awkward aliases.
    const std::string sysinfo = readFixture("hs300_sysinfo.json");
    CHECK(!sysinfo.empty());
    const std::string device = "80066C8E5A1F3B2D9E7C4A6B8D0F2E4C1A3B5D7F";
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, 0), device);
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, 1), device + "00");
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, 3), device + "02");
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, 6), device + "05");
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, 7), std::string());
    CHECK_EQ(kasa::idFromSysinfo(sysinfo, -1), std::string());
    CHECK_EQ(kasa::idFromSysinfo("{\"system\":", 1), std::string());
    CHECK_EQ(kasa::idFromSysinfo(FakeKasaPlug::singlePlugSysinfo(), 0),
             std::string("8006F1E2D3C4B5A69788796A5B4C3D2E1F001122"));

    CHECK_EQ(kasa::addressedCommand(kasa::relayStateCommand(true), 0, device), kasa::relayStateCommand(true));
    CHECK_EQ(kasa::addressedCommand(kasa::relayStateCommand(false), 2, "ABC01"),
             std::string("{\"context\":{\"child_ids\":[\"ABC01\"]},\"system\":{\"set_relay_state\":{\"state\":0}}}"));

    FakeKasaPlug strip;
    strip.setSysinfoReply(sysinfo);
    CHECK(strip.listen());
    KasaSwitchUnderTest outletThree(strip.port(), 3);
    SequenceRelay relay(outletThree.sendFn());
    relay.start();

    pumpFrames(relay, 255, 10);
    CHECK(waitUntil([&] { return relay.sender().idle(); }, 2000ms));

    const auto commands = strip.relayCommands();
    CHECK_EQ(commands.size(), size_t{1});
    if (!commands.empty()) {
        note("strip received state " + std::to_string(commands[0].state) + " for child " + commands[0].childId);
        CHECK_EQ(commands[0].state, 1);
        CHECK_EQ(commands[0].childId, device + "02");
    }
    CHECK_EQ(strip.sysinfoQueries(), 1);
    relay.stop();
}

void testQueryReadsLongRepliesAndGivesUpOnSilentPlugs() {
    // Longer than the 2,048-byte buffer the plugin used to read into.
    const std::string longReply = "{\"system\":{\"get_sysinfo\":{\"alias\":\"" + std::string(6000, 'x') +
                                  "\",\"deviceId\":\"ABC123\",\"err_code\":0}}}";
    FakeKasaPlug plug;
    plug.setSysinfoReply(longReply);
    CHECK(plug.listen());
    std::string reply;
    std::string error;
    CHECK(kasa::query("127.0.0.1", plug.port(), kasa::sysinfoCommand(0), reply, kasa::QueryOptions{}, &error));
    CHECK_EQ(reply.size(), longReply.size());
    CHECK_EQ(kasa::idFromSysinfo(reply, 0), std::string("ABC123"));

    FakeKasaPlug silent;
    silent.setNeverReply(true);
    CHECK(silent.listen());
    kasa::QueryOptions quick;
    quick.ioTimeout = 300ms;
    auto start = Clock::now();
    CHECK(!kasa::query("127.0.0.1", silent.port(), kasa::relayStateCommand(true), reply, quick, &error));
    const long long silentMs = msSince(start);
    note("silent plug gave up after " + std::to_string(silentMs) + " ms: " + error);
    CHECK(silentMs >= 290 && silentMs < 1500);

    error.clear();
    start = Clock::now();
    CHECK(!kasa::query("127.0.0.1", FakeKasaPlug::unusedPort(), kasa::relayStateCommand(true), reply,
                       kasa::QueryOptions{}, &error));
    note("refused: " + error);
    CHECK(msSince(start) < 1000);
    CHECK(!error.empty());

    error.clear();
    CHECK(!kasa::query("not-an-address", 1, kasa::relayStateCommand(true), reply, kasa::QueryOptions{}, &error));
    CHECK(!error.empty());
}
