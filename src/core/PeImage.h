#pragma once
// What the plugin reads of Better Facial Animations' DLL as Windows loaded it, written over a plain image in
// memory so it is tested on Linux: the import address table's slot of a function (the bridge patches
// KERNEL32's GetProcAddress there), an export's address (SKSEPlugin_Version), SKSE's packed version, and the
// one GetProcAddress lookup the bridge answers. PE32+ (x64) only. Every read is bounds-checked against the image.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bfarb::pe {

// The RVA of the import address table entry that holds `function` imported from `dll` (matched case-insensitively,
// with or without ".dll"), found through the import lookup table. False when it isn't imported by name.
bool FindImportSlot(const std::uint8_t* image, std::size_t size, std::string_view dll, std::string_view function, std::size_t& slotRva);

// The RVA of the export named `name`. False when there is none (or it is forwarded).
bool FindExport(const std::uint8_t* image, std::size_t size, std::string_view name, std::uint32_t& rva);

// Lays a DLL file's sections out at their RVAs, as the loader would (no relocations, no imports bound). For the tests.
bool MapFile(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& image);

// SKSE's packed plugin version (major << 24 | minor << 16 | patch << 4 | build) as "4.3.2", or "4.3.2.1" with a build.
std::string UnpackVersion(std::uint32_t packed);

// SKSE's PluginVersionData, as far as the bridge reads it.
struct SksePluginVersion {
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char pluginName[256];
};

// Whether a GetProcAddress call is the one the bridge answers: the name (not an ordinal) is exactly OrtGetApiBase,
// and the module's file name ends in onnxruntime.dll (case-insensitively, after a path separator or alone).
bool IsOrtGetApiBaseLookup(const char* nameOrOrdinal, std::string_view moduleFileName) noexcept;

}  // namespace bfarb::pe
