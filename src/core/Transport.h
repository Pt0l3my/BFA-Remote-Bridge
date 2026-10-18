#pragma once
// One TCP connection to the A2F server, abstracted so the core is tested on Linux: the plugin's is Winsock
// (src/plugin/WinsockTransport.cpp), the tests' POSIX sockets (tests/PosixTransport.h) or a script.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bfarb {

using Clock = std::chrono::steady_clock;

class Transport {
public:
    virtual ~Transport() = default;
    // Connects within timeoutMs, with TCP_NODELAY. False, with err, on failure.
    virtual bool Connect(const std::string& host, std::uint16_t port, int timeoutMs, std::string& err) = 0;
    // Sends all n bytes, or fails (with err) by the deadline.
    virtual bool SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) = 0;
    // Receives exactly n bytes, or fails (with err) by the deadline or when the server closes the connection.
    virtual bool ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) = 0;
    virtual void Close() noexcept = 0;
};

using TransportFactory = std::function<std::unique_ptr<Transport>()>;

}  // namespace bfarb
