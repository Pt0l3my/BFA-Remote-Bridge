// The plugin's transport: one Winsock TCP connection, a non-blocking connect within the timeout, TCP_NODELAY, and sends
// and receives that end at the deadline (select). The same contract as the tests' POSIX one (tests/PosixTransport.h).

#include "plugin/Plugin.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace bfarb::plugin {
namespace {

bool Started() {
    static const bool started = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

std::string LastError() { return "Winsock error " + std::to_string(::WSAGetLastError()); }

// Waits until the socket is writable (or readable) by the deadline.
bool Wait(SOCKET s, bool write, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        fd_set errors;
        FD_ZERO(&errors);
        FD_SET(s, &errors);
        timeval tv{static_cast<long>(left / 1000000), static_cast<long>(left % 1000000)};
        const int rc = ::select(0, write ? nullptr : &set, write ? &set : nullptr, &errors, &tv);
        if (rc > 0) return FD_ISSET(s, &set) != 0;
        if (rc == 0) return false;
        if (::WSAGetLastError() != WSAEINTR) return false;
    }
}

class WinsockTransport final : public Transport {
public:
    ~WinsockTransport() override { Close(); }

    bool Connect(const std::string& host, std::uint16_t port, int timeoutMs, std::string& err) override {
        Close();
        if (!Started()) {
            err = "Winsock didn't start";
            return false;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* found = nullptr;
        if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
            err = "the address doesn't resolve";
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        err = "no address answered";
        for (addrinfo* a = found; a && socket_ == INVALID_SOCKET; a = a->ai_next) {
            SOCKET s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == INVALID_SOCKET) continue;
            u_long nonBlocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);
            int rc = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen));
            if (rc == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) {
                rc = Wait(s, true, deadline) ? 0 : SOCKET_ERROR;
                int soerr = 0;
                int len = sizeof soerr;
                if (rc == 0 && (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len) != 0 || soerr != 0)) rc = SOCKET_ERROR;
                if (rc != 0) err = soerr ? "Winsock error " + std::to_string(soerr) : std::string("timed out");
            } else if (rc == SOCKET_ERROR) {
                err = LastError();
            }
            if (rc == 0) {
                const BOOL one = TRUE;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
                socket_ = s;
            } else {
                ::closesocket(s);
            }
        }
        ::freeaddrinfo(found);
        return socket_ != INVALID_SOCKET;
    }

    bool SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            if (!Wait(socket_, true, deadline)) {
                err = "timed out";
                return false;
            }
            const int chunk = static_cast<int>(std::min<std::size_t>(n, 1 << 20));
            const int k = ::send(socket_, reinterpret_cast<const char*>(data), chunk, 0);
            if (k == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (k <= 0) {
                err = LastError();
                return false;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return true;
    }

    bool ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            if (!Wait(socket_, false, deadline)) {
                err = "timed out";
                return false;
            }
            const int chunk = static_cast<int>(std::min<std::size_t>(n, 1 << 20));
            const int k = ::recv(socket_, reinterpret_cast<char*>(data), chunk, 0);
            if (k == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) continue;
            if (k == 0) {
                err = "the connection closed";
                return false;
            }
            if (k < 0) {
                err = LastError();
                return false;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return true;
    }

    void Close() noexcept override {
        if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

}  // namespace

TransportFactory WinsockFactory() {
    return [] { return std::make_unique<WinsockTransport>(); };
}

}  // namespace bfarb::plugin
