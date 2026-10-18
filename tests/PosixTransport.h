#pragma once
// The tests' transport: POSIX sockets, the same contract as the plugin's Winsock one (src/plugin/WinsockTransport.cpp):
// a non-blocking connect within the timeout, TCP_NODELAY, and sends and receives that end at the deadline.

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/Transport.h"

namespace bfarb::test {

class PosixTransport final : public Transport {
public:
    ~PosixTransport() override { Close(); }

    static bool Resolve(const std::string& host, std::uint16_t port, std::vector<Endpoint>& out, std::string& err) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
            err = "the address doesn't resolve";
            return false;
        }
        out.clear();
        for (addrinfo* a = found; a; a = a->ai_next) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(a->ai_addr);
            out.push_back(Endpoint{a->ai_family, std::vector<std::uint8_t>(p, p + a->ai_addrlen)});
        }
        freeaddrinfo(found);
        return true;
    }

    bool Connect(const std::vector<Endpoint>& endpoints, int timeoutMs, std::string& err) override {
        Close();
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        err = "no address answered";
        for (const Endpoint& a : endpoints) {
            const int s = ::socket(a.family, SOCK_STREAM, 0);
            if (s < 0) continue;
            ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) | O_NONBLOCK);
            int rc = ::connect(s, reinterpret_cast<const sockaddr*>(a.address.data()), static_cast<socklen_t>(a.address.size()));
            if (rc != 0 && errno == EINPROGRESS) {
                pollfd p{s, POLLOUT, 0};
                rc = Wait(p, deadline) ? 0 : -1;
                int soerr = 0;
                socklen_t len = sizeof soerr;
                if (rc == 0 && (::getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0)) rc = -1;
                if (rc != 0) err = soerr ? std::strerror(soerr) : "timed out";
            } else if (rc != 0) {
                err = std::strerror(errno);
            }
            if (rc == 0) {
                const int one = 1;
                ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                fd_ = s;
                break;
            }
            ::close(s);
        }
        return fd_ >= 0;
    }

    Result SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            pollfd p{fd_, POLLOUT, 0};
            if (!Wait(p, deadline)) {
                err = "timed out";
                return Result::TimedOut;
            }
            const ssize_t k = ::send(fd_, data, n, MSG_NOSIGNAL);
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (k <= 0) {
                err = std::strerror(errno);
                return errno == EPIPE || errno == ECONNRESET ? Result::Closed : Result::Failed;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return Result::Ok;
    }

    Result ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            pollfd p{fd_, POLLIN, 0};
            if (!Wait(p, deadline)) {
                err = "timed out";
                return Result::TimedOut;
            }
            const ssize_t k = ::recv(fd_, data, n, 0);
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (k == 0) {
                err = "the connection closed";
                return Result::Closed;
            }
            if (k < 0) {
                err = std::strerror(errno);
                return errno == ECONNRESET ? Result::Closed : Result::Failed;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return Result::Ok;
    }

    void Close() noexcept override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    static bool Wait(pollfd& p, Clock::time_point deadline) {
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return false;
            const int rc = ::poll(&p, 1, static_cast<int>(left));
            if (rc > 0) return true;
            if (rc == 0) return false;
            if (errno != EINTR) return false;
        }
    }
    int fd_ = -1;
};

}  // namespace bfarb::test
