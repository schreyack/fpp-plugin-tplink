#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A stand-in Kasa plug on 127.0.0.1 that speaks the legacy protocol: a 4-byte
// big-endian length, then JSON XOR-ed with an autokey that starts at 171. Its
// framing is written separately from src/core so that the two check each other.
//
// It records every set_relay_state it receives, in the order received, and
// replies the way a plug does. It can refuse connections (by not listening),
// hold its replies back for a while, or read a request and never answer.
class FakeKasaPlug {
public:
    struct RelayCommand {
        int state;            // 1 on, 0 off
        std::string childId;  // the first entry of context.child_ids, or empty
    };

    FakeKasaPlug();
    ~FakeKasaPlug();

    FakeKasaPlug(FakeKasaPlug const&) = delete;
    FakeKasaPlug& operator=(FakeKasaPlug const&) = delete;

    // Starts listening on 127.0.0.1. Port 0 picks a free port.
    bool listen(uint16_t port = 0);
    // Stops listening; connections are refused from then on.
    void stopListening();
    uint16_t port() const { return m_port; }

    // A port on 127.0.0.1 that nothing is listening on.
    static uint16_t unusedPort();

    // The get_sysinfo reply of a single-outlet HS103, which the fake sends
    // unless told otherwise.
    static std::string const& singlePlugSysinfo();

    void setSysinfoReply(std::string const& reply);
    void setReplyDelay(std::chrono::milliseconds delay);
    void setNeverReply(bool neverReply);

    std::vector<RelayCommand> relayCommands() const;
    size_t relayCommandCount(int state) const;
    int relayState() const;  // -1 until the first set_relay_state
    int requestsReceived() const;
    // The most requests that were ever received and not yet answered at once.
    int maxOverlappingRequests() const;
    int sysinfoQueries() const;

    static std::string encode(std::string const& plain);
    static std::string decode(std::string const& cipher);

private:
    void acceptLoop();
    void serve(int fd);

    mutable std::mutex m_mutex;
    std::vector<RelayCommand> m_commands;
    std::string m_sysinfoReply;
    std::chrono::milliseconds m_replyDelay{0};
    bool m_neverReply = false;
    int m_relayState = -1;
    int m_requests = 0;
    int m_openRequests = 0;
    int m_maxOpenRequests = 0;
    int m_sysinfoQueries = 0;
    std::vector<std::thread> m_connectionThreads;

    int m_listenFd = -1;
    uint16_t m_port = 0;
    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_shuttingDown{false};
    std::thread m_acceptThread;
};
