// The plugin's search for the server on the local network (core/Transport.h's Discovery): one UDP socket, FIND sent to
// each IPv4 adapter's own broadcast address, as the adapter list Windows reports it (GetAdaptersAddresses), and the
// answers read until the deadline (select). Never to 255.255.255.255: only adapters that are up, not loopback, with a
// prefix shorter than /31. Nothing here is logged: an address says where someone's PC is. The tests' counterpart is
// tests/PosixDiscovery.h.

#include "plugin/Plugin.h"

#include <algorithm>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET  // mstcpip.h's, which some SDKs' headers lack
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace bfarb::plugin {
namespace {

// Each IPv4 adapter's broadcast address (host byte order), once each.
std::vector<std::uint32_t> BroadcastAddresses(std::string& err) {
    std::vector<std::uint32_t> out;
    ULONG size = 16 * 1024;
    std::vector<std::uint8_t> buffer;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buffer.resize(size);
        rc = ::GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (rc == ERROR_NO_DATA) {  // no adapter at all
        err = "no network adapter with an IPv4 address is up";
        return out;
    }
    if (rc != NO_ERROR) {
        err = "the adapter list couldn't be read (error " + std::to_string(rc) + ")";
        return out;
    }
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const unsigned prefix = u->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 31) continue;  // no broadcast address of its own
            const auto* in = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
            const std::uint32_t address = ntohl(in->sin_addr.s_addr);
            if ((address >> 24) == 127) continue;
            const std::uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
            const std::uint32_t broadcast = address | ~mask;
            if (broadcast == 0xFFFFFFFFu) continue;
            if (std::find(out.begin(), out.end(), broadcast) == out.end()) out.push_back(broadcast);
        }
    }
    if (out.empty()) err = "no network adapter with an IPv4 address is up";
    return out;
}

class WinsockDiscoverySocket final : public Discovery {
public:
    ~WinsockDiscoverySocket() override { Close(); }

    int Broadcast(const std::vector<std::uint8_t>& datagram, std::uint16_t port, std::string& err) override {
        if (!WinsockStarted()) {
            err = "Winsock didn't start";
            return 0;
        }
        const std::vector<std::uint32_t> targets = BroadcastAddresses(err);
        if (targets.empty()) return 0;
        if (socket_ == INVALID_SOCKET) {
            socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_ == INVALID_SOCKET) {
                err = "Winsock error " + std::to_string(::WSAGetLastError());
                return 0;
            }
            const BOOL one = TRUE;
            ::setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
            // an ICMP "port unreachable" must not end the wait for another adapter's answer
            BOOL reset = FALSE;
            DWORD bytes = 0;
            ::WSAIoctl(socket_, SIO_UDP_CONNRESET, &reset, sizeof reset, nullptr, 0, &bytes, nullptr, nullptr);
            sockaddr_in any{};
            any.sin_family = AF_INET;
            any.sin_addr.s_addr = htonl(INADDR_ANY);
            any.sin_port = 0;
            if (::bind(socket_, reinterpret_cast<const sockaddr*>(&any), sizeof any) != 0) {
                err = "Winsock error " + std::to_string(::WSAGetLastError());
                Close();
                return 0;
            }
        }
        int sent = 0;
        for (const std::uint32_t b : targets) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = htonl(b);
            to.sin_port = htons(port);
            if (::sendto(socket_, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof to) == static_cast<int>(datagram.size()))
                ++sent;
            else
                err = "Winsock error " + std::to_string(::WSAGetLastError());
        }
        return sent;
    }

    Transport::Result Receive(std::vector<std::uint8_t>& datagram, std::uint16_t connectPort, Endpoint& sender, Clock::time_point deadline,
                              std::string& err) override {
        if (socket_ == INVALID_SOCKET) {
            err = "nothing was sent";
            return Transport::Result::Failed;
        }
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
            if (left <= 0) return Transport::Result::TimedOut;
            fd_set set;
            FD_ZERO(&set);
            FD_SET(socket_, &set);
            timeval tv{static_cast<long>(left / 1000000), static_cast<long>(left % 1000000)};
            const int rc = ::select(0, &set, nullptr, nullptr, &tv);
            if (rc == 0) return Transport::Result::TimedOut;
            if (rc < 0) {
                if (::WSAGetLastError() == WSAEINTR) continue;
                err = "Winsock error " + std::to_string(::WSAGetLastError());
                return Transport::Result::Failed;
            }
            std::uint8_t buffer[2048];
            sockaddr_in from{};
            int fromLength = sizeof from;
            const int k = ::recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (k < 0) {
                const int code = ::WSAGetLastError();
                if (code == WSAECONNRESET || code == WSAEMSGSIZE || code == WSAEWOULDBLOCK) continue;  // not an answer: wait on
                err = "Winsock error " + std::to_string(code);
                return Transport::Result::Failed;
            }
            if (from.sin_family != AF_INET) continue;
            datagram.assign(buffer, buffer + k);
            // the server's address, on the TCP port FIND reached
            from.sin_port = htons(connectPort);
            const auto* p = reinterpret_cast<const std::uint8_t*>(&from);
            sender = Endpoint{AF_INET, std::vector<std::uint8_t>(p, p + sizeof from)};
            return Transport::Result::Ok;
        }
    }

    void Close() noexcept override {
        if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

}  // namespace

DiscoveryFactory WinsockDiscovery() {
    return [] { return std::make_unique<WinsockDiscoverySocket>(); };
}

}  // namespace bfarb::plugin
