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
    bool knownVersion = false;  // 4.3.x, the only versions the bridge attaches to
    bool hooked = false;
    std::string problem;  // why it isn't hooked
};

// Finds BetterFacialAnimations.dll among the loaded modules, reads its version and, for 4.3.x only, patches its import
// address table's GetProcAddress, so its lookup of onnxruntime.dll's OrtGetApiBase gets the bridge's. Leaves everything
// alone when BFA isn't loaded, is another version, its runtime already is loaded, or the patch can't be made.
BfaFacts FindAndHookBfa();

// ---- SkyrimNet.cpp

// The settings through SkyrimNet's PublicGetPluginConfigValue("BFARemoteBridge", ...). Read only once SkyrimNet has
// registered the config (PublicGetPluginConfig names it): before that, its answers would be the defaults asked for, not
// the player's. warnings lists the values repaired.
enum class SettingsRead { Read, NotRegistered, NoSkyrimNet };
SettingsRead ReadSkyrimNetSettings(Settings& out, std::vector<std::string>& warnings);
bool SkyrimNetLoaded();

// ---- WinsockTransport.cpp

TransportFactory WinsockFactory();
bool WinsockResolve(const std::string& host, std::uint16_t port, std::vector<Endpoint>& out, std::string& err);

// ---- Main.cpp

void Log(Level level, const std::string& line);

}  // namespace bfarb::plugin
