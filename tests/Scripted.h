#pragma once
// A transport whose server is a function: each complete frame the client sends is decoded and handed to the
// script, whose bytes the client then receives. An empty answer reads as a timeout.

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Protocol.h"
#include "core/Transport.h"

namespace bfarb::test {

struct Script {
    // (type, payload) -> the bytes the server sends back
    std::function<std::vector<std::uint8_t>(std::uint16_t, const std::vector<std::uint8_t>&)> answer;
    bool refuseConnect = false;
    int connects = 0;
    int closes = 0;
    std::vector<std::uint16_t> received;  // the types of the frames the server got
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

    bool Connect(const std::string&, std::uint16_t, int, std::string& err) override {
        ++script_->connects;
        if (script_->refuseConnect) {
            err = "connection refused";
            return false;
        }
        return true;
    }

    bool SendAll(const std::uint8_t* data, std::size_t n, Clock::time_point, std::string&) override {
        sent_.insert(sent_.end(), data, data + n);
        while (sent_.size() >= proto::kHeaderSize) {
            std::uint32_t length = 0;
            std::memcpy(&length, sent_.data() + 8, 4);
            if (sent_.size() < proto::kHeaderSize + length) break;
            std::uint16_t type = 0;
            std::memcpy(&type, sent_.data() + 6, 2);
            const std::vector<std::uint8_t> payload(sent_.begin() + proto::kHeaderSize, sent_.begin() + proto::kHeaderSize + length);
            sent_.erase(sent_.begin(), sent_.begin() + proto::kHeaderSize + length);
            script_->received.push_back(type);
            const auto back = script_->answer ? script_->answer(type, payload) : std::vector<std::uint8_t>{};
            pending_.insert(pending_.end(), back.begin(), back.end());
        }
        return true;
    }

    bool ReceiveAll(std::uint8_t* data, std::size_t n, Clock::time_point, std::string& err) override {
        if (pending_.size() < n) {
            err = "timed out";
            return false;
        }
        std::memcpy(data, pending_.data(), n);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        return true;
    }

    void Close() noexcept override { ++script_->closes; }

private:
    std::shared_ptr<Script> script_;
    std::vector<std::uint8_t> sent_;
    std::vector<std::uint8_t> pending_;
};

inline TransportFactory ScriptedFactory(std::shared_ptr<Script> script) {
    return [script] { return std::make_unique<ScriptedTransport>(script); };
}

}  // namespace bfarb::test
