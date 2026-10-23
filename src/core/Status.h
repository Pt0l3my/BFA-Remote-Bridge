#pragma once
// Every text the player reads on screen, in one place (British spelling). Each goes out through the bridge's one
// note function (core/Monitor.h's sink; src/plugin/Main.cpp's ShowNote). Plain words only: Skyrim's fonts can't
// draw emoji.

#include <string>

#include "core/Reasons.h"

namespace bfarb {

struct StatusFacts {
    bool bfaFound = false;
    std::string bfaVersion;  // "4.3.2.3"; empty when unknown
    bool hooked = false;     // the GetProcAddress patch is in place
    std::string problem;     // why it isn't, in the player's words
};

// The line at the first game load about BFA itself; empty when the bridge is attached (the monitor's lines follow).
std::string LoadStatusText(const StatusFacts& facts);

// The bridge couldn't attach: BFA runs as it would without the bridge.
std::string CantAttachText(const std::string& bfaVersion, const std::string& why);

// The runtime BFA loaded can't be served (known to be incompatible): BFA has no model, lip sync is the game's own.
// newer: the advice is to update the bridge, else BFA.
std::string CantServeText(const std::string& bfaVersion, const std::string& ortVersion, bool newer);

// SkyrimNet, which holds the settings, isn't loaded: no server address, and lip sync is the game's own.
std::string NoServerText();

// No server address is set and none answered on the local network: lip sync is the game's own, and what to do.
std::string NotFoundText(int port);

// "a2f3_fp16.onnx on NVIDIA GeForce RTX 5090 (DirectML 3), PandorumLLM v3.97-p6 Beta A2F: ready"
std::string ModelLine(const std::string& model, const std::string& server, const std::string& device, const std::string& state);

// A model's lines fell back: words is a ReasonWords or a more precise phrase. An empty model speaks of every line.
std::string FallbackLine(const std::string& model, const std::string& words);

// Lines answered by the server again after a fallback.
std::string BackLine(const std::string& model);

// The server's device as the player reads it: "DirectML 3: NVIDIA GeForce RTX 5090" becomes
// "NVIDIA GeForce RTX 5090 (DirectML 3)"; a device without "kind: " stays as given.
std::string DeviceText(const std::string& device);

}  // namespace bfarb
