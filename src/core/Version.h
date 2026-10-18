#pragma once
// The bridge's version (CMakeLists.txt's project version, also the SkyrimNet manifest's) and the ONNX Runtime
// whose C API the bridge knows: 1.24.4, ORT_API_VERSION 24, as Better Facial Animations 4.3.2 ships it.

#ifndef BFARB_VERSION
#error "BFARB_VERSION comes from CMakeLists.txt"
#endif

namespace bfarb {

constexpr const char* kVersionText = BFARB_VERSION;
constexpr const char* kOrtVersionText = "1.24.4";
constexpr unsigned kOrtApiVersion = 24;
constexpr const char* kTestedBfaVersion = "4.3.2";

}  // namespace bfarb
