#pragma once
// The tests' search for the server (core/Transport.h's Discovery), the plugin's counterpart in POSIX sockets
// (src/plugin/WinsockDiscovery.cpp): one UDP socket, FIND to each of `targets` (IPv4, host byte order) in place of the
// adapters' broadcast addresses, since the reference server listens on 127.0.0.1 in the tests, then the answers read
// until the deadline.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/Transport.h"

namespace bfarb::test {

class PosixDiscovery final : public Discovery {
public:
    explicit PosixDiscovery(std::vector<std::uint32_t> targets) : targets_(std::move(targets)) {}
    ~PosixDiscovery() override { Close(); }

    int Broadcast(const std::vector<std::uint8_t>& datagram, std::uint16_t port, std::string& err) override {
        if (fd_ < 0) {
            fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (fd_ < 0) {
                err = std::strerror(errno);
                return 0;
            }
            const int one = 1;
            ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        }
        int sent = 0;
        for (const std::uint32_t t : targets_) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = htonl(t);
            to.sin_port = htons(port);
            if (::sendto(fd_, datagram.data(), datagram.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) ==
                static_cast<ssize_t>(datagram.size()))
                ++sent;
            else
                err = std::strerror(errno);
        }
        return sent;
    }

    Transport::Result Receive(std::vector<std::uint8_t>& datagram, std::uint16_t connectPort, Endpoint& sender, Clock::time_point deadline,
                              std::string& err) override {
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) return Transport::Result::TimedOut;
            pollfd p{fd_, POLLIN, 0};
            const int rc = ::poll(&p, 1, static_cast<int>(left));
            if (rc == 0) return Transport::Result::TimedOut;
            if (rc < 0) {
                if (errno == EINTR) continue;
                err = std::strerror(errno);
                return Transport::Result::Failed;
            }
            std::uint8_t buffer[2048];
            sockaddr_in from{};
            socklen_t length = sizeof from;
            const ssize_t k = ::recvfrom(fd_, buffer, sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &length);
            if (k < 0) {
                if (errno == EINTR || errno == ECONNREFUSED || errno == EAGAIN) continue;  // no answer: wait on
                err = std::strerror(errno);
                return Transport::Result::Failed;
            }
            datagram.assign(buffer, buffer + k);
            from.sin_port = htons(connectPort);
            const auto* b = reinterpret_cast<const std::uint8_t*>(&from);
            sender = Endpoint{AF_INET, std::vector<std::uint8_t>(b, b + sizeof from)};
            return Transport::Result::Ok;
        }
    }

    void Close() noexcept override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    std::vector<std::uint32_t> targets_;
    int fd_ = -1;
};

// A search that sends FIND to 127.0.0.1 alone.
inline DiscoveryFactory LoopbackSearch() {
    return [] { return std::make_unique<PosixDiscovery>(std::vector<std::uint32_t>{INADDR_LOOPBACK}); };
}

}  // namespace bfarb::test
