#pragma once
// The monitor: what the server does, told to the player in one line per model, from what the bridge learns of it
// (each HELLO_OK's server string, its device and each model's state) and from each run.
//
//   a model's state     a line when its server, its card or its state changed ("ready", "not ready (...)"); a ready
//                       line only while Status on load allows it, a line about a problem always
//   a game load         every model's line, while Status on load allows it
//   a fallback          a line falls back: a note naming its reason (core/Reasons.h), at most one a minute, however
//                       the lines alternate
//   back on the server  the first answered line after a fallback note, at most one a minute: named by its model
//                       when it is the one the note named
//   once per session    a note said once (BFA's GPU route, no server address, the bridge can't attach)
//
// Every note goes to one function, the sink (src/plugin/Main.cpp's ShowNote: Skyrim's own on-screen note today,
// SkyrimNet's notification call once its public API has one), outside the monitor's lock.

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/Reasons.h"
#include "core/Transport.h"

namespace bfarb {

class Monitor {
public:
    using Sink = std::function<void(const std::string& text)>;
    using Now = Clock::time_point (*)();
    static constexpr std::chrono::seconds kFallbackEvery{60};

    explicit Monitor(Now now = &Clock::now);

    void SetSink(Sink sink);
    void SetShowStatus(bool show);

    void ModelState(const std::string& model, const std::string& server, const std::string& device, const std::string& state, bool ready);
    void Forget(const std::string& model);  // a model BFA released: no line
    void Loaded();
    // words: a more precise phrase than ReasonWords(reason), or empty. An empty model speaks of every line.
    void Fallback(Reason reason, const std::string& model, const std::string& words = {});
    void Answered(const std::string& model);
    void Once(const std::string& key, const std::string& text);

    bool InFallback() const;

private:
    struct Entry {
        std::string server, device, state;
        bool ready = false;
    };
    void Emit(const std::vector<std::string>& lines) const;

    Now now_;
    mutable std::mutex mutex_;
    Sink sink_;
    bool showStatus_ = true;
    std::map<std::string, Entry> models_;
    std::set<std::string> said_;
    bool inFallback_ = false;  // a fallback note was shown and no back note since
    bool anyFallback_ = false, anyBack_ = false;
    Clock::time_point lastFallback_{};
    Clock::time_point lastBack_{};
    std::string fallbackModel_;
};

}  // namespace bfarb
