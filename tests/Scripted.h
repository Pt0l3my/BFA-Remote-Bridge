#pragma once
// A transport whose server is a function: each complete frame the client sends is decoded and handed to the
// script, whose bytes the client then receives. An empty answer reads as a timeout; the bytes of an answer that came
// later (a late answer, held by the script) arrive before the next frame's. Safe to share between threads.

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Protocol.h"
#include "core/Transport.h"

namespace bfarb::test {

struct Script {
    // (type, payload) -> the bytes the server sends back
    std::function<std::vector<std::uint8_t>(std::uint16_t, const std::vector<std::uint8_t>&)> answer;
    std::atomic<bool> refuseConnect{false};
    std::atomic<int> answerDelayMs{0};          // the server takes this long to answer each frame
    std::atomic<bool> closeBeforeAnswer{false};  // the next frame finds the connection closed (an idle connection the server dropped)
    std::atomic<bool> resetBeforeAnswer{false};  // the next frame finds the connection reset
    std::atomic<bool> failSend{false};           // the next send fails outright
    std::atomic<int> connects{0};
    std::atomic<int> closes{0};

    mutable std::mutex mutex;
    std::vector<std::uint16_t> received;                   // the types of the frames the server got
    std::vector<std::vector<std::uint16_t>> byConnection;  // the same, per connection made, in the order made

    std::vector<std::uint16_t> Received() const {
        std::lock_guard lock(mutex);
        return received;
    }
    std::vector<std::vector<std::uint16_t>> ByConnection() const {
        std::lock_guard lock(mutex);
        return byConnection;
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

    bool Connect(const std::vector<Endpoint>&, int, std::string& err) override {
        ++script_->connects;
        if (script_->refuseConnect) {
            err = "connection refused";
            return false;
        }
        std::lock_guard lock(script_->mutex);
        index_ = script_->byConnection.size();
        script_->byConnection.emplace_back();
        return true;
    }

    Result SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point, std::string& err) override {
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
            std::uint16_t type = 0;
            std::memcpy(&type, sent_.data() + 6, 2);
            const std::vector<std::uint8_t> payload(sent_.begin() + proto::kHeaderSize, sent_.begin() + proto::kHeaderSize + length);
            sent_.erase(sent_.begin(), sent_.begin() + proto::kHeaderSize + length);
            {
                std::lock_guard lock(script_->mutex);
                script_->received.push_back(type);
                if (index_ < script_->byConnection.size()) script_->byConnection[index_].push_back(type);
            }
            if (const int delay = script_->answerDelayMs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            const auto back = script_->answer ? script_->answer(type, payload) : std::vector<std::uint8_t>{};
            pending_.insert(pending_.end(), back.begin(), back.end());
        }
        return Result::Ok;
    }

    Result ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point, std::string& err) override {
        if (closed_ != Result::Ok) {
            err = closed_ == Result::Reset ? "reset" : "the connection closed";
            return closed_;
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
    std::vector<std::uint8_t> pending_;
    Result closed_ = Result::Ok;
};

inline TransportFactory ScriptedFactory(std::shared_ptr<Script> script) {
    return [script] { return std::make_unique<ScriptedTransport>(script); };
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
