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

    bool Connect(const std::string& host, std::uint16_t port, int timeoutMs, std::string& err) override {
        Close();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
            err = "the address doesn't resolve";
            return false;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        err = "no address answered";
        for (addrinfo* a = found; a; a = a->ai_next) {
            const int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s < 0) continue;
            ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL) | O_NONBLOCK);
            int rc = ::connect(s, a->ai_addr, a->ai_addrlen);
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
        freeaddrinfo(found);
        return fd_ >= 0;
    }

    bool SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            pollfd p{fd_, POLLOUT, 0};
            if (!Wait(p, deadline)) {
                err = "timed out";
                return false;
            }
            const ssize_t k = ::send(fd_, data, n, MSG_NOSIGNAL);
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (k <= 0) {
                err = std::strerror(errno);
                return false;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return true;
    }

    bool ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        while (n > 0) {
            pollfd p{fd_, POLLIN, 0};
            if (!Wait(p, deadline)) {
                err = "timed out";
                return false;
            }
            const ssize_t k = ::recv(fd_, data, n, 0);
            if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (k == 0) {
                err = "the connection closed";
                return false;
            }
            if (k < 0) {
                err = std::strerror(errno);
                return false;
            }
            data += k;
            n -= static_cast<std::size_t>(k);
        }
        return true;
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
