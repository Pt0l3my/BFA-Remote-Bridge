// BFARemoteBridge.dll's SKSE entry.
//
//   load            the log (BFARemoteBridge.log in SKSE's log folder), VR refused, the messaging listener
//   post-load       every plugin is loaded: find Better Facial Animations, read its version, attach to its lookup of
//                   ONNX Runtime (Hook.cpp); note whether SkyrimNet is loaded, so a model load waits for its settings;
//                   and, once attached, allow the client's background thread (src/core/Client.h)
//   data loaded,    the settings from SkyrimNet's Settings > Plugins page, on the main thread, once SkyrimNet has
//   main menu,      registered them; also whenever that page is saved. The first read with the bridge on and an
//   pre-load game   address starts the background thread, which looks the server's name up (again after each change
//                   of the address or port), says HELLO with no models and logs the answer
//   game loaded     the settings again; at the first load, the status notification (setting showStatus), and the
//                   model loads' notices held until then
//
// A model load's outcome comes later, on BFA's worker thread; its notice goes to the game's main thread through SKSE's
// task interface. Nothing here waits for the network: the client's Configure never does.
//
// At process exit: the bridge, its client and the background thread live until the process ends and are never
// destroyed, so nothing joins the thread in a static destructor during DLL unload (which could deadlock). Windows ends
// the thread before any DLL detach runs; the log is never destroyed either, so no destructor meets a lock that thread
// held, and every line is flushed as it is written.

#include "plugin/Plugin.h"

#include "core/Status.h"
#include "core/Version.h"

namespace bfarb::plugin {
namespace {

Bridge* g_bridge = nullptr;  // lives until the game exits: BFA's worker may call it at any time
BfaFacts g_bfa;
std::atomic<bool> g_skyrimNet{false};
bool g_settingsRead = false;  // main thread only

std::mutex g_noticeMutex;
bool g_gameLoaded = false;
std::vector<std::string> g_pending;
std::string g_lastShown;

spdlog::logger* g_log = nullptr;  // never destroyed: see the top of this file

void OpenLog() {
    try {
        const auto folder = SKSE::log::log_directory();
        if (!folder) return;
        const auto path = *folder / "BFARemoteBridge.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        g_log = new spdlog::logger("BFARemoteBridge", std::move(sink));
        // UTC: a local time would tell the player's time zone
        g_log->set_pattern("%Y-%m-%d %H:%M:%S.%e UTC [%l] %v", spdlog::pattern_time_type::utc);
        g_log->set_level(spdlog::level::info);
        g_log->flush_on(spdlog::level::info);
    } catch (...) {
    }
}

bool ShowStatus() { return g_bridge && g_bridge->CurrentSettings().showStatus; }

// On the main thread, now or at the first game load; the same line twice in a row is shown once.
void Notify(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard lock(g_noticeMutex);
    if (text == g_lastShown) return;
    g_lastShown = text;
    if (!g_gameLoaded) {
        g_pending.push_back(text);
        return;
    }
    if (const auto* tasks = SKSE::GetTaskInterface()) tasks->AddTask([text] { RE::DebugNotification(text.c_str()); });
}

// On the main thread. Never waits: the client applies the settings at its next run or in its background thread.
void ApplySettings(const char* when) {
    Settings s;
    std::vector<std::string> warnings;
    const SettingsRead read = ReadSkyrimNetSettings(s, warnings);
    if (read == SettingsRead::NoSkyrimNet) {
        if (!g_settingsRead) Log(Level::Warn, std::string("SkyrimNet isn't loaded (") + when + "): the bridge is idle and BFA runs as usual");
        return;
    }
    if (read == SettingsRead::NotRegistered) {
        Log(Level::Info, std::string("SkyrimNet hasn't registered the bridge's settings yet (") + when + ")");
        return;
    }
    g_settingsRead = true;
    for (const auto& w : warnings) Log(Level::Warn, "Setting " + w);
    Log(Level::Info, std::string("Settings (") + when + "): " + Describe(s));
    g_bridge->Apply(s);  // starts the background thread once the bridge is on with an address
}

class ConfigSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
        try {
            if (event && event->eventName.c_str() && event->strArg.c_str() &&
                std::string_view(event->eventName.c_str()) == "SkyrimNet_OnPluginConfigSaved" &&
                std::string_view(event->strArg.c_str()) == kConfigName)
                ApplySettings("saved on SkyrimNet's page");
        } catch (...) {
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// The main menu's opening: before a new game starts BFA's worker.
class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        try {
            if (event && event->opening && event->menuName == RE::MainMenu::MENU_NAME) ApplySettings("main menu");
        } catch (...) {
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

void OnGameLoaded() {
    ApplySettings("game loaded");
    std::vector<std::string> pending;
    {
        std::lock_guard lock(g_noticeMutex);
        if (g_gameLoaded) return;
        g_gameLoaded = true;
        pending.swap(g_pending);
    }
    if (!ShowStatus()) return;
    const Settings s = g_bridge->CurrentSettings();
    StatusFacts f;
    f.enabled = s.enabled;
    f.skyrimNet = g_skyrimNet;
    f.bfaFound = g_bfa.found;
    f.bfaVersion = g_bfa.version;
    f.knownBfa = g_bfa.knownVersion;
    f.hooked = g_bfa.hooked;
    f.addressSet = !s.serverAddress.empty();
    const std::string status = LoadStatusText(f);
    const auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) return;
    if (!status.empty()) tasks->AddTask([status] { RE::DebugNotification(status.c_str()); });
    for (const auto& text : pending) tasks->AddTask([text] { RE::DebugNotification(text.c_str()); });
}

void OnSkse(SKSE::MessagingInterface::Message* message) {
    if (!message) return;
    try {
        switch (message->type) {
            case SKSE::MessagingInterface::kPostLoad:
                g_bfa = FindAndHookBfa();
                g_skyrimNet = SkyrimNetLoaded();
                g_bridge->ExpectSettings(g_skyrimNet);
                if (g_bfa.hooked) g_bridge->Connection().StartBackground();
                break;
            case SKSE::MessagingInterface::kDataLoaded: {
                static ConfigSink configs;
                if (auto* source = SKSE::GetModCallbackEventSource())
                    source->AddEventSink(&configs);
                else
                    Log(Level::Warn, "SKSE's mod event source isn't there: a save on SkyrimNet's page applies at the next game load");
                static MenuSink menus;
                if (auto* ui = RE::UI::GetSingleton()) ui->AddEventSink<RE::MenuOpenCloseEvent>(&menus);
                ApplySettings("data loaded");
                break;
            }
            case SKSE::MessagingInterface::kPreLoadGame:
                ApplySettings("before a game loads");
                break;
            case SKSE::MessagingInterface::kPostLoadGame:
            case SKSE::MessagingInterface::kNewGame:
                OnGameLoaded();
                break;
            default:
                break;
        }
    } catch (...) {
    }
}

}  // namespace

void Log(Level level, const std::string& line) {
    if (!g_log) return;
    try {
        switch (level) {
            case Level::Debug: g_log->debug(line); break;
            case Level::Info: g_log->info(line); break;
            case Level::Warn: g_log->warn(line); break;
            case Level::Error: g_log->error(line); break;
        }
    } catch (...) {
    }
}

}  // namespace bfarb::plugin

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    using namespace bfarb;
    using namespace bfarb::plugin;
    SKSE::Init(skse);
    OpenLog();
    Log(Level::Info, std::string("BFA Remote Bridge ") + kVersionText + " loading");
    if (REL::Module::IsVR()) {
        Log(Level::Warn, "Skyrim VR isn't supported: the bridge stays off");
        return false;
    }
    g_bridge = new Bridge(WinsockFactory(), &WinsockResolve, [](Level level, const std::string& line) { Log(level, line); });
    Bridge::SetCurrent(g_bridge);
    g_bridge->OnNotice([](const Notice& n) {
        if (ShowStatus()) Notify(NoticeText(n, g_bfa.version));
    });
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnSkse)) {
        Log(Level::Error, "SKSE's messaging interface isn't there: the bridge stays off");
        return false;
    }
    return true;
}
