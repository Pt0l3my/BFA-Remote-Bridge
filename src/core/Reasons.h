#pragma once
// Why a line gets the game's own lip sync: the one list that the log's fallback lines, the notes on screen
// (core/Monitor.h) and the status check's client string (core/Client.h) share. A reason added later goes at the end,
// and its key never changes once released: a server may parse it.

#include <cstddef>
#include <cstdint>

namespace bfarb {

enum class Reason : std::uint8_t {
    NoServer,      // no server address is set (or SkyrimNet, which holds it, isn't loaded)
    Unreachable,   // no connection, a connect that timed out, a connection lost
    Paused,        // answered at once while the bridge reconnects in the background
    TimedOut,      // the server didn't answer a line in time
    ServerError,   // the server answered ERROR, or an answer that doesn't fit the call
    NotReady,      // the server doesn't have the model ready
    ModelRefused,  // a model load the server didn't accept (not found, a different copy, failed, not allowed, the key)
    Incompatible,  // this BFA or its runtime asked for something the bridge can't serve
};
constexpr std::size_t kReasonCount = 8;

// One word for the client string ("fallback timeout"): never translated, never changed once released.
const char* ReasonKey(Reason reason) noexcept;

// The player's words, for the notes on screen: "the server didn't answer in time". British spelling.
const char* ReasonWords(Reason reason) noexcept;

}  // namespace bfarb
