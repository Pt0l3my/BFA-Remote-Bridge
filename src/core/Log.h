#pragma once
// Where the core's log lines go: spdlog in the plugin (SKSE/BFARemoteBridge.log), a list in the tests. Lines
// never carry the access key, the server's address or a path from the player's PC.

#include <functional>
#include <string>

namespace bfarb {

enum class Level { Debug, Info, Warn, Error };

using LogSink = std::function<void(Level, const std::string&)>;

}  // namespace bfarb
