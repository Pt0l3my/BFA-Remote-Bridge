#pragma once
// One TCP connection to the A2F server, and one search for it on the local network (FIND and FOUND on UDP),
// abstracted so the core is tested on Linux: the plugin's are Winsock (src/plugin/WinsockTransport.cpp and
// src/plugin/WinsockDiscovery.cpp, with the adapter list Windows reports), the tests' POSIX sockets
// (tests/PosixTransport.h) or a script.
//
// Name lookup is apart from connecting: it can't be bounded by a timeout, so the client looks the server's name up
// once per settings change, off the game's threads, and connects to the addresses it found.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bfarb {

using Clock = std::chrono::steady_clock;

// One address of the server: a sockaddr's bytes, as the platform's lookup gave it.
struct Endpoint {
    int family = 0;
    std::vector<std::uint8_t> address;
    bool operator==(const Endpoint&) const = default;
};

class Transport {
public:
    // How a send or a receive ended.
    enum class Result {
        Ok,
        TimedOut,  // the deadline passed
        Closed,    // the server closed the connection
        Reset,     // the connection was reset or aborted
        Failed,    // any other error
    };

    virtual ~Transport() = default;
    // Connects to the first of the addresses that answers within timeoutMs, with TCP_NODELAY. False, with err.
    virtual bool Connect(const std::vector<Endpoint>& endpoints, int timeoutMs, std::string& err) = 0;
    // Sends all n bytes by the deadline.
    virtual Result SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) = 0;
    // Receives exactly n bytes by the deadline.
    virtual Result ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) = 0;
    virtual void Close() noexcept = 0;
};

using TransportFactory = std::function<std::unique_ptr<Transport>()>;

// One search for the server on the local network: a UDP socket of its own, closed by Close (or its destructor).
// The client decides what a datagram is (FIND and FOUND are core/Protocol.h's); this only sends and receives.
class Discovery {
public:
    virtual ~Discovery() = default;
    // Sends `datagram` to `port` on each IPv4 adapter's own broadcast address (its address with every host bit set),
    // never to 255.255.255.255: how many addresses it went to; 0, with err, when none.
    virtual int Broadcast(const std::vector<std::uint8_t>& datagram, std::uint16_t port, std::string& err) = 0;
    // The next datagram to that socket by the deadline: its bytes, and its sender as an endpoint on `connectPort` (the
    // TCP port the client connects to there). TimedOut when none came by then; Failed, with err, on another error.
    virtual Transport::Result Receive(std::vector<std::uint8_t>& datagram, std::uint16_t connectPort, Endpoint& sender,
                                      Clock::time_point deadline, std::string& err) = 0;
    virtual void Close() noexcept = 0;
};

using DiscoveryFactory = std::function<std::unique_ptr<Discovery>()>;

// Looks host up (a name or a numeric address) for port: false, with err, when it can't be.
using Resolver = std::function<bool(const std::string& host, std::uint16_t port, std::vector<Endpoint>& out, std::string& err)>;

}  // namespace bfarb
