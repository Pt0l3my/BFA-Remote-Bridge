// The plugin's transport: Winsock. The name lookup (getaddrinfo, which no timeout bounds) is its own call, made off the
// game's threads; a connection tries the addresses it found, each a non-blocking connect within the timeout, with
// TCP_NODELAY; sends and receives end at the deadline (select). The same contract as the tests' POSIX one
// (tests/PosixTransport.h).

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

std::string ErrorText(int code) {
    switch (code) {
        case WSAECONNREFUSED: return "refused";
        case WSAETIMEDOUT: return "timed out";
        case WSAEHOSTUNREACH: return "host unreachable";
        case WSAENETUNREACH: return "network unreachable";
        case WSAECONNRESET: return "reset";
        case WSAECONNABORTED: return "aborted";
        default: return "Winsock error " + std::to_string(code);
    }
}

int SocketError(SOCKET s) {
    int err = 0;
    int len = sizeof err;
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0) return ::WSAGetLastError();
    return err;
}

enum class Ready { Yes, TimedOut, Error };

// Waits until the socket is writable (or readable) by the deadline; Error when select reports the socket failed.
Ready Wait(SOCKET s, bool write, Clock::time_point deadline) {
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
        if (left <= 0) return Ready::TimedOut;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        fd_set errors;
        FD_ZERO(&errors);
        FD_SET(s, &errors);
        timeval tv{static_cast<long>(left / 1000000), static_cast<long>(left % 1000000)};
        const int rc = ::select(0, write ? nullptr : &set, write ? &set : nullptr, &errors, &tv);
        if (rc > 0) return FD_ISSET(s, &errors) ? Ready::Error : Ready::Yes;
        if (rc == 0) return Ready::TimedOut;
        if (::WSAGetLastError() != WSAEINTR) return Ready::Error;
    }
}

class WinsockTransport final : public Transport {
public:
    ~WinsockTransport() override { Close(); }

    bool Connect(const std::vector<Endpoint>& endpoints, int timeoutMs, std::string& err) override {
        Close();
        if (!Started()) {
            err = "Winsock didn't start";
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        err = "no address to connect to";
        for (const Endpoint& a : endpoints) {
            if (socket_ != INVALID_SOCKET) break;
            SOCKET s = ::socket(a.family, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                err = ErrorText(::WSAGetLastError());
                continue;
            }
            u_long nonBlocking = 1;
            ::ioctlsocket(s, FIONBIO, &nonBlocking);
            int rc = ::connect(s, reinterpret_cast<const sockaddr*>(a.address.data()), static_cast<int>(a.address.size()));
            if (rc == SOCKET_ERROR && ::WSAGetLastError() == WSAEWOULDBLOCK) {
                // a refused connection shows in select's error set: its SO_ERROR says why
                const Ready ready = Wait(s, true, deadline);
                const int soerr = SocketError(s);
                if (ready == Ready::Yes && soerr == 0) {
                    rc = 0;
                } else {
                    err = soerr ? ErrorText(soerr) : ready == Ready::TimedOut ? std::string("timed out") : std::string("failed");
                }
            } else if (rc == SOCKET_ERROR) {
                err = ErrorText(::WSAGetLastError());
            }
            if (rc == 0) {
                const BOOL one = TRUE;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
                socket_ = s;
            } else {
                ::closesocket(s);
            }
        }
        return socket_ != INVALID_SOCKET;
    }

    Result SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            const Ready ready = Wait(socket_, true, deadline);
            if (ready != Ready::Yes) return Failure(ready, err);
            const int chunk = static_cast<int>(std::min<std::size_t>(n, 1 << 20));
            const int k = ::send(socket_, reinterpret_cast<const char*>(data), chunk, 0);
            if (k == SOCKET_ERROR) {
                const int code = ::WSAGetLastError();
                if (code == WSAEWOULDBLOCK) continue;
                err = ErrorText(code);
                return code == WSAECONNRESET || code == WSAECONNABORTED ? Result::Reset : code == WSAESHUTDOWN ? Result::Closed : Result::Failed;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return Result::Ok;
    }

    Result ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            const Ready ready = Wait(socket_, false, deadline);
            if (ready != Ready::Yes) return Failure(ready, err);
            const int chunk = static_cast<int>(std::min<std::size_t>(n, 1 << 20));
            const int k = ::recv(socket_, reinterpret_cast<char*>(data), chunk, 0);
            if (k == 0) {
                err = "the connection closed";
                return Result::Closed;
            }
            if (k == SOCKET_ERROR) {
                const int code = ::WSAGetLastError();
                if (code == WSAEWOULDBLOCK) continue;
                err = ErrorText(code);
                return code == WSAECONNRESET || code == WSAECONNABORTED ? Result::Reset : Result::Failed;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return Result::Ok;
    }

    void Close() noexcept override {
        if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

private:
    Result Failure(Ready ready, std::string& err) {
        if (ready == Ready::TimedOut) {
            err = "timed out";
            return Result::TimedOut;
        }
        const int code = SocketError(socket_);
        err = code ? ErrorText(code) : std::string("the connection closed");
        return code == WSAECONNRESET || code == WSAECONNABORTED ? Result::Reset : Result::Closed;
    }

    SOCKET socket_ = INVALID_SOCKET;
};

}  // namespace

TransportFactory WinsockFactory() {
    return [] { return std::make_unique<WinsockTransport>(); };
}

bool WinsockResolve(const std::string& host, std::uint16_t port, std::vector<Endpoint>& out, std::string& err) {
    if (!Started()) {
        err = "Winsock didn't start";
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found);
    if (rc != 0 || !found) {
        err = "the name doesn't resolve (Winsock error " + std::to_string(rc) + ")";
        return false;
    }
    out.clear();
    for (addrinfo* a = found; a; a = a->ai_next) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(a->ai_addr);
        out.push_back(Endpoint{a->ai_family, std::vector<std::uint8_t>(p, p + a->ai_addrlen)});
    }
    ::freeaddrinfo(found);
    return !out.empty();
}

}  // namespace bfarb::plugin
