#include "core/Status.h"

namespace bfarb {
namespace {

const std::string kPrefix = "BFA Remote Bridge: ";

std::string Bfa(const std::string& version) { return "Better Facial Animations" + (version.empty() ? std::string() : " " + version); }

}  // namespace

std::string LoadStatusText(const StatusFacts& f) {
    if (!f.enabled) return kPrefix + "off.";
    if (!f.bfaFound) return kPrefix + "Better Facial Animations wasn't found, so the bridge is idle.";
    if (!f.hooked) return kPrefix + Bfa(f.bfaVersion) + " found, but the bridge couldn't attach to it, so BFA runs as usual.";
    if (!f.addressSet)
        return kPrefix + (f.skyrimNet ? "no server address is set, so the bridge is idle and BFA runs as usual."
                                      : "SkyrimNet wasn't found, so no server address is set and the bridge is idle.");
    return {};
}

std::string NoticeText(const Notice& n, const std::string& bfaVersion) {
    switch (n.kind) {
        case Notice::Kind::Served: return kPrefix + "on. " + Bfa(bfaVersion) + " found; the server runs its model.";
        case Notice::Kind::NoAnswer:
            return kPrefix +
                   "the server didn't answer, so BFA's model isn't loaded and lip sync is the game's own. Start the server, then reload "
                   "the model in BFA's menu or restart.";
        case Notice::Kind::WrongKey:
            return kPrefix + "the server refused the access key, so BFA's model isn't loaded and lip sync is the game's own.";
        case Notice::Kind::Missing: return kPrefix + "the server lacks " + n.model + " or has a different copy; lip sync is the game's own.";
        case Notice::Kind::CouldNotLoad:
            return kPrefix + "the server couldn't load " + n.model + ", so lip sync is the game's own. The bridge's log says why.";
        case Notice::Kind::UnknownRuntime:
            return kPrefix + "this Better Facial Animations uses an ONNX Runtime the bridge doesn't know, so BFA runs as usual.";
    }
    return {};
}

}  // namespace bfarb
