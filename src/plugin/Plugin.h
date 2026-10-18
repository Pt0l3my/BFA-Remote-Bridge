#pragma once
// The plugin's parts (Windows only): the hook in Better Facial Animations' import of GetProcAddress, SkyrimNet's
// settings, the Winsock transport and the log.

#include <string>
#include <vector>

#include "core/Bridge.h"
#include "core/Settings.h"
#include "core/Transport.h"

namespace bfarb::plugin {

// ---- Hook.cpp

struct BfaFacts {
    bool found = false;
    std::string version;  // from its SKSEPlugin_Version, e.g. "4.3.2.3"; empty when unreadable
    bool hooked = false;
    std::string problem;  // why it isn't hooked
};

// Finds BetterFacialAnimations.dll among the loaded modules, reads its version and patches its import address table's
// GetProcAddress, so its lookup of onnxruntime.dll's OrtGetApiBase gets the bridge's. Leaves everything alone when
// BFA isn't loaded, its runtime already is, or the patch can't be made.
BfaFacts FindAndHookBfa();

// ---- SkyrimNet.cpp

// The settings through SkyrimNet's PublicGetPluginConfigValue("BFARemoteBridge", ...); false, with the defaults,
// when SkyrimNet (or that export) isn't there. warnings lists the values repaired.
bool ReadSkyrimNetSettings(Settings& out, std::vector<std::string>& warnings);

// ---- WinsockTransport.cpp

TransportFactory WinsockFactory();

// ---- Main.cpp

void Log(Level level, const std::string& line);

}  // namespace bfarb::plugin
