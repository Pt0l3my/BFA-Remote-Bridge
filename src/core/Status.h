#pragma once
// The texts the player reads on screen (setting showStatus): the state at the first game load, and each model load's
// outcome, which comes later, on BFA's worker (the plugin queues it to the game's main thread). British spelling.

#include <string>

#include "core/Bridge.h"

namespace bfarb {

struct StatusFacts {
    bool enabled = true;
    bool skyrimNet = true;   // SkyrimNet's settings could be read
    bool bfaFound = false;
    std::string bfaVersion;  // "4.3.2"; empty when unknown
    bool hooked = false;     // the GetProcAddress patch is in place
    bool addressSet = false;
};

// The line at the first game load; empty when the bridge is on and BFA's model loads will tell (NoticeText).
std::string LoadStatusText(const StatusFacts& facts);

// The line for a model load's outcome, or for an ONNX Runtime the bridge doesn't know.
std::string NoticeText(const Notice& notice, const std::string& bfaVersion);

}  // namespace bfarb
