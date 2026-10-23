#pragma once
// A transport whose server is a function: each complete frame the client sends is decoded and handed to the
// script, whose bytes the client then receives. An empty answer reads as a timeout; the bytes of an answer that came
// later (a late answer, held by the script) arrive before the next frame's. Safe to share between threads.
//
// The versions, as a server keeps the rule: a connection's version is its first frame's, and the script's answers
// (encoded in version 1) are framed in it. With `speaks` 1, a version 1 server: a first frame of version 2 gets ERROR
// 6 framed in version 1, then the connection is closed. NOTICE is one-way: kept in `notices`, never answered.
//
// Each connect's timeout and each frame's deadline are recorded, per connection. With `timed` on, the transport keeps
// time as a socket does: each answer arrives answerDelayMs after its frame (in order), and a receive waits for it until
// its deadline; an answer that comes after the deadline stays on the way, a late answer.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Protocol.h"
#include "core/Transport.h"

namespace bfarb::test {

// One frame the server got, with the deadline the client gave it.
struct Sent {
    std::uint16_t type = 0;
    std::uint16_t version = 0;
    Clock::time_point at{};        // when it was sent
    Clock::time_point deadline{};  // its exchange's deadline
    long long BudgetMs() const { return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - at).count(); }
};

// One connection made, in the order made.
struct Connection {
    int connectTimeoutMs = 0;
    std::vector<Sent> sent;
};

struct Script {
    // (type, payload) -> the bytes the server sends back
    std::function<std::vector<std::uint8_t>(std::uint16_t, const std::vector<std::uint8_t>&)> answer;
    std::atomic<bool> refuseConnect{false};
    std::atomic<bool> timed{false};             // keep time as a socket does (above)
    std::atomic<int> answerDelayMs{0};          // the server takes this long to answer each frame
    std::atomic<bool> closeBeforeAnswer{false};  // the next frame finds the connection closed (an idle connection the server dropped)
    std::atomic<bool> resetBeforeAnswer{false};  // the next frame finds the connection reset
    std::atomic<bool> failSend{false};           // the next send fails outright
    std::atomic<int> speaks{2};                  // the newest protocol version the server speaks
    std::atomic<bool> p6{false};                 // as PandorumLLM v3.97-p6: a newer version's HELLO reset, no frame back
    std::atomic<int> resetHellos{0};             // the next this many connections reset at their first frame
    std::atomic<int> connects{0};
    std::atomic<int> closes{0};

    mutable std::mutex mutex;
    std::vector<std::uint16_t> received;                   // the types of the frames the server got
    std::vector<std::vector<std::uint16_t>> byConnection;  // the same, per connection made, in the order made
    std::vector<Connection> connections;                   // each connection made: its connect's timeout and its frames
    std::vector<int> connectTimeouts;                      // every connect tried, made or not
    std::vector<std::uint16_t> versions;                   // the version of each frame the server got
    std::vector<proto::Notice> notices;                    // each NOTICE the server got

    std::vector<std::uint16_t> Versions() const {
        std::lock_guard lock(mutex);
        return versions;
    }
    std::vector<proto::Notice> Notices() const {
        std::lock_guard lock(mutex);
        return notices;
    }

    std::vector<std::uint16_t> Received() const {
        std::lock_guard lock(mutex);
        return received;
    }
    std::vector<std::vector<std::uint16_t>> ByConnection() const {
        std::lock_guard lock(mutex);
        return byConnection;
    }
    std::vector<Connection> Connections() const {
        std::lock_guard lock(mutex);
        return connections;
    }
    std::vector<int> ConnectTimeouts() const {
        std::lock_guard lock(mutex);
        return connectTimeouts;
    }
    int Count(proto::MsgType t) const {
        std::lock_guard lock(mutex);
        int n = 0;
        for (auto x : received) n += x == static_cast<std::uint16_t>(t);
        return n;
    }
};

inline std::vector<std::uint8_t> Frame(proto::MsgType type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> f;
    std::string err;
    proto::EncodeFrame(type, payload, f, err);
    return f;
}

class ScriptedTransport final : public Transport {
public:
    explicit ScriptedTransport(std::shared_ptr<Script> script) : script_(std::move(script)) {}

    bool Connect(const std::vector<Endpoint>&, int timeoutMs, std::string& err) override {
        ++script_->connects;
        {
            std::lock_guard lock(script_->mutex);
            script_->connectTimeouts.push_back(timeoutMs);
        }
        if (script_->refuseConnect) {
            err = "connection refused";
            return false;
        }
        std::lock_guard lock(script_->mutex);
        index_ = script_->byConnection.size();
        script_->byConnection.emplace_back();
        script_->connections.push_back(Connection{timeoutMs, {}});
        return true;
    }

    Result SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        if (script_->failSend.exchange(false)) {
            err = "send failed";
            return Result::Failed;
        }
        if (script_->closeBeforeAnswer.exchange(false)) closed_ = Result::Closed;
        if (script_->resetBeforeAnswer.exchange(false)) closed_ = Result::Reset;
        if (closed_ != Result::Ok) return Result::Ok;  // the bytes go into the void; the answer never comes
        sent_.insert(sent_.end(), data, data + n);
        while (sent_.size() >= proto::kHeaderSize) {
            std::uint32_t length = 0;
            std::memcpy(&length, sent_.data() + 8, 4);
            if (sent_.size() < proto::kHeaderSize + length) break;
            std::uint16_t type = 0, version = 0;
            std::memcpy(&type, sent_.data() + 6, 2);
            std::memcpy(&version, sent_.data() + 4, 2);
            const std::vector<std::uint8_t> payload(sent_.begin() + proto::kHeaderSize, sent_.begin() + proto::kHeaderSize + length);
            sent_.erase(sent_.begin(), sent_.begin() + proto::kHeaderSize + length);
            {
                std::lock_guard lock(script_->mutex);
                script_->received.push_back(type);
                script_->versions.push_back(version);
                if (index_ < script_->byConnection.size()) {
                    script_->byConnection[index_].push_back(type);
                    script_->connections[index_].sent.push_back(Sent{type, version, Clock::now(), deadline});
                }
            }
            if (closing_) continue;  // after a closing ERROR: the server reads no more
            if (version_ == 0 && (script_->resetHellos.load() > 0 || (script_->p6 && version > script_->speaks.load()))) {
                if (script_->resetHellos.load() > 0) --script_->resetHellos;
                closed_ = Result::Reset;  // no frame back: the connection was reset
                continue;
            }
            if (version_ == 0) {
                if (version > script_->speaks.load()) {
                    // a version this server doesn't speak: ERROR 6 framed in the newest it does, then closed
                    version_ = static_cast<std::uint16_t>(script_->speaks.load());
                    std::vector<std::uint8_t> p, f;
                    std::string err;
                    proto::EncodeError({0, 6, "protocol version not supported"}, p, err);
                    proto::EncodeFrame(proto::MsgType::Error, p, f, err, version_);
                    pending_.insert(pending_.end(), f.begin(), f.end());
                    closing_ = true;
                    continue;
                }
                version_ = version;
            }
            if (type == static_cast<std::uint16_t>(proto::MsgType::Notice)) {  // one-way
                proto::Notice n;
                std::string err;
                proto::DecodeNotice(payload.data(), payload.size(), n, err);
                std::lock_guard lock(script_->mutex);
                script_->notices.push_back(n);
                continue;
            }
            const int delay = script_->answerDelayMs.load();
            if (delay && !script_->timed) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            auto back = script_->answer ? script_->answer(type, payload) : std::vector<std::uint8_t>{};
            // the script's frames (version 1) in the connection's version
            for (std::size_t at = 0; at + proto::kHeaderSize <= back.size();) {
                if (back[at + 4] == 1 && back[at + 5] == 0) back[at + 4] = static_cast<std::uint8_t>(version_);
                std::uint32_t n = 0;
                std::memcpy(&n, back.data() + at + 8, 4);
                at += proto::kHeaderSize + n;
            }
            if (!script_->timed) {
                pending_.insert(pending_.end(), back.begin(), back.end());
            } else if (!back.empty()) {
                // the server answers in order: after the answer before it
                lastArrival_ = std::max(lastArrival_, Clock::now()) + std::chrono::milliseconds(delay);
                arrivals_.emplace_back(lastArrival_, back);
            }
        }
        return Result::Ok;
    }

    Result ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point deadline, std::string& err) override {
        if (closed_ != Result::Ok) {
            err = closed_ == Result::Reset ? "reset" : "the connection closed";
            return closed_;
        }
        for (;;) {
            while (!arrivals_.empty() && arrivals_.front().first <= Clock::now()) {
                pending_.insert(pending_.end(), arrivals_.front().second.begin(), arrivals_.front().second.end());
                arrivals_.pop_front();
            }
            if (pending_.size() >= n || !script_->timed) break;
            if (!arrivals_.empty() && arrivals_.front().first <= deadline) {
                std::this_thread::sleep_until(arrivals_.front().first);
                continue;
            }
            std::this_thread::sleep_until(deadline);
            break;
        }
        if (pending_.size() < n && closing_) {
            err = "the connection closed";
            return Result::Closed;
        }
        if (pending_.size() < n) {
            // what has come is taken, as a socket would: a frame cut short stays cut
            pending_.clear();
            err = "timed out";
            return Result::TimedOut;
        }
        std::memcpy(data, pending_.data(), n);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        return Result::Ok;
    }

    void Close() noexcept override { ++script_->closes; }

private:
    std::shared_ptr<Script> script_;
    std::size_t index_ = static_cast<std::size_t>(-1);
    std::vector<std::uint8_t> sent_;
    std::vector<std::uint8_t> pending_;                                           // arrived, not yet read
    std::deque<std::pair<Clock::time_point, std::vector<std::uint8_t>>> arrivals_;  // timed: on the way
    Clock::time_point lastArrival_{};
    Result closed_ = Result::Ok;
    std::uint16_t version_ = 0;  // the connection's, from its first frame
    bool closing_ = false;       // a closing ERROR was sent: the connection ends after it
};

inline TransportFactory ScriptedFactory(std::shared_ptr<Script> script) {
    return [script] { return std::make_unique<ScriptedTransport>(script); };
}

// The search, scripted: each Broadcast is recorded (its datagram), and each answer the script gives is received from
// `from`. answer(datagram) returns the datagrams the network sends back; none reads as no answer.
struct Network {
    std::function<std::vector<std::vector<std::uint8_t>>(const std::vector<std::uint8_t>&)> answer;
    Endpoint from{2, {9, 9, 9, 9}};
    std::atomic<int> adapters{1};  // broadcast addresses the datagram goes to
    mutable std::mutex mutex;
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::uint16_t> ports;
    int Sent() const {
        std::lock_guard lock(mutex);
        return static_cast<int>(sent.size());
    }
};

class ScriptedDiscovery final : public Discovery {
public:
    explicit ScriptedDiscovery(std::shared_ptr<Network> net) : net_(std::move(net)) {}
    int Broadcast(const std::vector<std::uint8_t>& datagram, std::uint16_t port, std::string& err) override {
        if (net_->adapters.load() == 0) {
            err = "no network adapter with an IPv4 address is up";
            return 0;
        }
        {
            std::lock_guard lock(net_->mutex);
            net_->sent.push_back(datagram);
            net_->ports.push_back(port);
        }
        if (net_->answer)
            for (auto& d : net_->answer(datagram)) inbox_.push_back(std::move(d));
        return net_->adapters.load();
    }
    Transport::Result Receive(std::vector<std::uint8_t>& datagram, std::uint16_t, Endpoint& sender, Clock::time_point, std::string&) override {
        if (inbox_.empty()) return Transport::Result::TimedOut;  // at once: the tests' clock is their own
        datagram = std::move(inbox_.front());
        inbox_.pop_front();
        sender = net_->from;
        return Transport::Result::Ok;
    }
    void Close() noexcept override {}

private:
    std::shared_ptr<Network> net_;
    std::deque<std::vector<std::uint8_t>> inbox_;
};

inline DiscoveryFactory ScriptedSearch(std::shared_ptr<Network> net) {
    return [net] { return std::make_unique<ScriptedDiscovery>(net); };
}

// FOUND from a server called `server`.
inline std::vector<std::uint8_t> FoundDatagram(const std::string& server = "scripted") {
    std::vector<std::uint8_t> p, d;
    std::string err;
    proto::EncodeFound(server, p, err);
    proto::EncodeDatagram(proto::MsgType::Found, p, d, err);
    return d;
}

// A lookup that finds one made-up address for any name but those ending in ".invalid", and counts its calls.
inline Resolver ScriptedResolver(std::shared_ptr<std::atomic<int>> calls = std::make_shared<std::atomic<int>>(0)) {
    return [calls](const std::string& host, std::uint16_t, std::vector<Endpoint>& out, std::string& err) {
        ++*calls;
        if (host.size() >= 8 && host.compare(host.size() - 8, 8, ".invalid") == 0) {
            err = "no such host";
            return false;
        }
        out = {Endpoint{2, {1, 2, 3, 4}}};
        return true;
    };
}

}  // namespace bfarb::test
