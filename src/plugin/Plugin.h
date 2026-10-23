#pragma once
// The plugin's parts (Windows only): the hook in Better Facial Animations' import of GetProcAddress, SkyrimNet's
// settings, the Winsock transport and search, and the log.

#include <string>
#include <vector>

#include "core/BfaMode.h"
#include "core/Bridge.h"
#include "core/Settings.h"
#include "core/Transport.h"

namespace bfarb::plugin {

// ---- Hook.cpp

struct BfaFacts {
    bool found = false;
    std::string version;  // from its SKSEPlugin_Version, e.g. "4.3.2.3"; empty when unreadable
    bool hooked = false;
    std::string problem;  // why it isn't hooked, in the player's words
};

// Finds BetterFacialAnimations.dll among the loaded modules, reads its version and, whatever the version, patches its
// import address table's GetProcAddress, so its lookup of onnxruntime.dll's OrtGetApiBase gets the bridge's. Leaves
// everything alone when BFA isn't loaded, its runtime already is loaded, or the patch can't be made.
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
bool WinsockStarted();  // WSAStartup, once

// ---- WinsockDiscovery.cpp

// FIND and FOUND on UDP: FIND to each IPv4 adapter's own broadcast address, from the adapter list Windows reports
// (GetAdaptersAddresses: adapters that are up, not loopback, prefixes shorter than /31), never to 255.255.255.255.
DiscoveryFactory WinsockDiscovery();

// ---- BfaIni.cpp

// BFA's INI (Data\SKSE\Plugins\BetterFacialAnimations.ini, the relative path BFA itself uses) and the bridge's own
// state (Data\SKSE\Plugins\BFARemoteBridge.ini, [BfaMode] ForceCPUSetByBridge), through Windows' profile calls, as
// BFA's. BFA's INI is never created.
std::unique_ptr<IniStore> BfaIniStore();

// ---- Main.cpp

void Log(Level level, const std::string& line);

// The bridge's one note function: every text the player reads on screen goes through it. Today Skyrim's own corner
// note, on the main thread, queued until the first game load; SkyrimNet sees each as a "notification" workflow event
// (its WORKFLOW_TRIGGERS.md). This is the seam for SkyrimNet's own notification call, once its public API has one
// (its CppAPI, API version 12 in Beta28, has none).
void ShowNote(const std::string& text);

}  // namespace bfarb::plugin
