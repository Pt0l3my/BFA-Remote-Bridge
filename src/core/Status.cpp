#include "core/Status.h"

namespace bfarb {
namespace {

const std::string kPrefix = "BFA Remote Bridge: ";

std::string Bfa(const std::string& version) { return "Better Facial Animations" + (version.empty() ? std::string() : " " + version); }

}  // namespace

const char* ReasonKey(Reason r) noexcept {
    switch (r) {
        case Reason::NoServer: return "noserver";
        case Reason::Unreachable: return "unreachable";
        case Reason::Paused: return "paused";
        case Reason::TimedOut: return "timeout";
        case Reason::ServerError: return "error";
        case Reason::NotReady: return "notready";
        case Reason::ModelRefused: return "refused";
        case Reason::Incompatible: return "incompatible";
    }
    return "other";
}

const char* ReasonWords(Reason r) noexcept {
    switch (r) {
        case Reason::NoServer: return "no server address is set";
        case Reason::Unreachable: return "the server isn't answering";
        case Reason::Paused: return "the bridge is reconnecting to the server";
        case Reason::TimedOut: return "the server didn't answer in time";
        case Reason::ServerError: return "the server couldn't run it";
        case Reason::NotReady: return "the server doesn't have the model ready";
        case Reason::ModelRefused: return "the server didn't load the model";
        case Reason::Incompatible: return "this Better Facial Animations asked for something the bridge can't serve";
    }
    return "of an unknown reason";
}

std::string LoadStatusText(const StatusFacts& f) {
    if (!f.bfaFound) return kPrefix + "Better Facial Animations wasn't found, so the bridge is idle.";
    if (!f.hooked) return CantAttachText(f.bfaVersion, f.problem);
    return {};
}

std::string CantAttachText(const std::string& bfaVersion, const std::string& why) {
    return kPrefix + "couldn't attach to " + Bfa(bfaVersion) + (why.empty() ? std::string() : " (" + why + ")") +
           ", so BFA runs on this PC as it would without the bridge. Update the bridge.";
}

std::string CantServeText(const std::string& bfaVersion, const std::string& ortVersion, bool newer) {
    return kPrefix + Bfa(bfaVersion) + " uses ONNX Runtime" + (ortVersion.empty() ? std::string() : " " + ortVersion) +
           ", which the bridge can't serve, so lip sync is the game's own. " +
           (newer ? "Update the bridge." : "Update Better Facial Animations.");
}

std::string NoServerText() {
    return kPrefix + "SkyrimNet wasn't found, so the bridge has no server address and lip sync is the game's own.";
}

std::string NotFoundText(int port) {
    const std::string p = std::to_string(port);
    return kPrefix + "no server answered on the local network, so lip sync is the game's own. In PandorumLLM, set the A2F "
                     "server's Remote IP to this PC and allow port " + p +
           " (UDP and TCP) through that PC's firewall, or enter its address on SkyrimNet's Settings > Plugins page.";
}

std::string ModelLine(const std::string& model, const std::string& server, const std::string& device, const std::string& state) {
    std::string where;
    if (!device.empty()) where = " on " + DeviceText(device);
    if (!server.empty()) where += (where.empty() ? " on " : ", ") + server;
    return kPrefix + model + where + ": " + state;
}

std::string FallbackLine(const std::string& model, const std::string& words) {
    return kPrefix + (model.empty() ? std::string("lines get") : model + ": lines get") + " the game's own lip sync, since " + words + ".";
}

std::string BackLine(const std::string& model) {
    return kPrefix + (model.empty() ? std::string("lines are") : model + ": lines are") + " back on the server.";
}

std::string DeviceText(const std::string& device) {
    const auto colon = device.find(": ");
    if (colon == std::string::npos || colon == 0 || colon + 2 >= device.size()) return device;
    return device.substr(colon + 2) + " (" + device.substr(0, colon) + ")";
}

}  // namespace bfarb
