#include "FakeKasaPlug.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

// Shaped like the get_sysinfo reply of an HS103(US) hw 2.1, fw 1.1.2, with the
// ids made up.
const std::string kSinglePlugSysinfo =
    R"json({"system":{"get_sysinfo":{"sw_ver":"1.1.2 Build 191113 Rel.095623","hw_ver":"2.1","model":"HS103(US)",)json"
    R"json("deviceId":"8006F1E2D3C4B5A69788796A5B4C3D2E1F001122","oemId":"00000000000000000000000000000000",)json"
    R"json("hwId":"00000000000000000000000000000000","rssi":-58,"latitude_i":0,"longitude_i":0,"alias":"Snow",)json"
    R"json("status":"new","mic_type":"IOT.SMARTPLUGSWITCH","feature":"TIM","mac":"00:00:00:00:00:00","updating":0,)json"
    R"json("led_off":0,"relay_state":0,"on_time":0,"active_mode":"none","icon_hash":"",)json"
    R"json("dev_name":"Smart Wi-Fi Plug Lite","next_action":{"type":-1},"err_code":0}}})json";

const char* const kRelayReply = "{\"system\":{\"set_relay_state\":{\"err_code\":0}}}";

void setBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
}

void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

// Reads exactly `size` bytes; false on end of stream, error, timeout or `quit`.
bool readAll(int fd, char* buffer, size_t size, std::chrono::milliseconds timeout, std::atomic<bool> const& quit) {
    const auto deadline = Clock::now() + timeout;
    size_t got = 0;
    while (got < size) {
        if (quit.load() || Clock::now() >= deadline) {
            return false;
        }
        pollfd entry{fd, POLLIN, 0};
        const int ready = ::poll(&entry, 1, 20);
        if (ready < 0 && errno != EINTR) {
            return false;
        }
        if (ready <= 0) {
            continue;
        }
        const ssize_t n = ::recv(fd, buffer + got, size - got, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool writeAll(int fd, std::string const& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
#endif
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

int numberAfter(std::string const& json, std::string const& key) {
    size_t at = json.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    at += key.size();
    int value = 0;
    bool any = false;
    while (at < json.size() && json[at] >= '0' && json[at] <= '9') {
        value = value * 10 + (json[at] - '0');
        any = true;
        ++at;
    }
    return any ? value : -1;
}

std::string quotedAfter(std::string const& json, std::string const& key) {
    size_t at = json.find(key);
    if (at == std::string::npos) {
        return std::string();
    }
    at += key.size();
    const size_t end = json.find('"', at);
    return end == std::string::npos ? std::string() : json.substr(at, end - at);
}

std::string lengthHeader(size_t length) {
    std::string out(4, '\0');
    out[0] = static_cast<char>((length >> 24) & 0xff);
    out[1] = static_cast<char>((length >> 16) & 0xff);
    out[2] = static_cast<char>((length >> 8) & 0xff);
    out[3] = static_cast<char>(length & 0xff);
    return out;
}

}  // namespace

FakeKasaPlug::FakeKasaPlug() : m_sysinfoReply(kSinglePlugSysinfo) {}

FakeKasaPlug::~FakeKasaPlug() {
    stopListening();
    m_shuttingDown.store(true);
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        threads.swap(m_connectionThreads);
    }
    for (auto& thread : threads) {
        thread.join();
    }
}

std::string const& FakeKasaPlug::singlePlugSysinfo() {
    return kSinglePlugSysinfo;
}

bool FakeKasaPlug::listen(uint16_t port) {
    stopListening();
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, 64) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return false;
    }
    m_listenFd = fd;
    m_port = ntohs(address.sin_port);
    m_listening.store(true);
    m_acceptThread = std::thread(&FakeKasaPlug::acceptLoop, this);
    return true;
}

void FakeKasaPlug::stopListening() {
    m_listening.store(false);
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
}

uint16_t FakeKasaPlug::unusedPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    uint16_t port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
        port = ntohs(address.sin_port);
    }
    ::close(fd);
    return port;
}

void FakeKasaPlug::setSysinfoReply(std::string const& reply) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sysinfoReply = reply;
}

void FakeKasaPlug::setReplyDelay(std::chrono::milliseconds delay) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_replyDelay = delay;
}

void FakeKasaPlug::setNeverReply(bool neverReply) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_neverReply = neverReply;
}

std::vector<FakeKasaPlug::RelayCommand> FakeKasaPlug::relayCommands() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_commands;
}

size_t FakeKasaPlug::relayCommandCount(int state) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<size_t>(std::count_if(m_commands.begin(), m_commands.end(),
                                             [state](RelayCommand const& c) { return c.state == state; }));
}

int FakeKasaPlug::relayState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_relayState;
}

int FakeKasaPlug::requestsReceived() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
}

int FakeKasaPlug::maxOverlappingRequests() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_maxOpenRequests;
}

int FakeKasaPlug::sysinfoQueries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sysinfoQueries;
}

std::string FakeKasaPlug::encode(std::string const& plain) {
    std::string out;
    int key = 171;
    for (unsigned char c : plain) {
        const int x = key ^ c;
        out.push_back(static_cast<char>(x));
        key = x;
    }
    return out;
}

std::string FakeKasaPlug::decode(std::string const& cipher) {
    std::string out;
    int key = 171;
    for (unsigned char c : cipher) {
        out.push_back(static_cast<char>(key ^ c));
        key = c;
    }
    return out;
}

void FakeKasaPlug::acceptLoop() {
    while (m_listening.load()) {
        pollfd entry{m_listenFd, POLLIN, 0};
        if (::poll(&entry, 1, 20) <= 0) {
            continue;
        }
        const int fd = ::accept(m_listenFd, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectionThreads.emplace_back(&FakeKasaPlug::serve, this, fd);
    }
}

void FakeKasaPlug::serve(int fd) {
    setBlocking(fd);
    noSigpipe(fd);

    char head[4];
    std::string cipher;
    bool ok = readAll(fd, head, sizeof(head), 3000ms, m_shuttingDown);
    if (ok) {
        const uint32_t length = (static_cast<uint32_t>(static_cast<uint8_t>(head[0])) << 24) |
                                (static_cast<uint32_t>(static_cast<uint8_t>(head[1])) << 16) |
                                (static_cast<uint32_t>(static_cast<uint8_t>(head[2])) << 8) |
                                static_cast<uint32_t>(static_cast<uint8_t>(head[3]));
        ok = length <= (1u << 20);
        if (ok && length > 0) {
            cipher.assign(length, '\0');
            ok = readAll(fd, &cipher[0], length, 3000ms, m_shuttingDown);
        }
    }
    if (!ok) {
        ::close(fd);
        return;
    }

    const std::string json = decode(cipher);
    std::string reply;
    std::chrono::milliseconds delay{0};
    bool neverReply = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_requests;
        ++m_openRequests;
        m_maxOpenRequests = std::max(m_maxOpenRequests, m_openRequests);
        delay = m_replyDelay;
        neverReply = m_neverReply;
        if (json.find("\"set_relay_state\"") != std::string::npos) {
            RelayCommand command{numberAfter(json, "\"state\":"), quotedAfter(json, "\"child_ids\":[\"")};
            m_relayState = command.state;
            m_commands.push_back(command);
            reply = kRelayReply;
        } else if (json.find("\"get_sysinfo\"") != std::string::npos) {
            ++m_sysinfoQueries;
            reply = m_sysinfoReply;
        } else {
            reply = "{\"err_code\":-1,\"err_msg\":\"module not support\"}";
        }
    }

    if (neverReply) {
        // Hold the request unanswered until the client gives up or the fake shuts down.
        while (!m_shuttingDown.load()) {
            pollfd entry{fd, POLLIN, 0};
            if (::poll(&entry, 1, 20) > 0) {
                char byte;
                if (::recv(fd, &byte, 1, 0) <= 0) {
                    break;
                }
            }
        }
    } else {
        const auto replyAt = Clock::now() + delay;
        while (!m_shuttingDown.load() && Clock::now() < replyAt) {
            std::this_thread::sleep_for(2ms);
        }
    }

    // The request counts as open until just before the reply goes out, so a
    // client that waits for each reply can never show as overlapping.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        --m_openRequests;
    }
    if (!neverReply && !m_shuttingDown.load()) {
        writeAll(fd, lengthHeader(reply.size()) + encode(reply));
    }
    ::close(fd);
}
