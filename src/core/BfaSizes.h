#pragma once
// The sizes the bridge lists for a BFA version: the free-dimension overrides BFA's GPU route sets on each model's
// session (AddFreeDimensionOverrideByName), by BFA version and file name, for the versions whose CPU route sets none.
//
// In its CPU mode BFA sets no overrides, so its HELLO would list none. For BFA 4.3.2.3 a version 2 server fills in
// its sizes itself (the sizes rule, docs/PROTOCOL.md), matched by the files' SHA-256, and the bridge lists nothing:
// that HELLO is unchanged. For BFA 5.0.6 the bridge lists the GPU route's overrides itself, so the server builds the
// session it would build for BFA's GPU route, exactly as listed, whatever its own table holds. A HELLO for which BFA
// set overrides (its GPU route) is sent with BFA's own.
//
// From BFA 5.0.6's source (read for facts only; src/inference/ort_net.cc, the functions that make each model's
// session): the folded V3 network fixes its four batch dimensions at 1; the Legacy network its two at BFA's Batch
// setting ([General] Batch, 2 by default, at least 1); the Performance network its batch at 1; both emotion models,
// Full and Performance, share one list, batch_size 1, seq_len 60000 and batch 1. ONNX Runtime ignores an override
// whose dimension a model lacks (the Performance emotion model's input is fixed: [1, 48, 188]), so the list is sent
// as BFA sets it.

#include <string>
#include <vector>

#include "core/Protocol.h"

namespace bfarb {

// The overrides BFA's GPU route sets on file's session in that BFA version, in BFA's order; empty when the bridge
// lists none for it (another version, 4.3.2.3 among them, or another file). batch: BFA's [General] Batch.
std::vector<proto::Override> GpuRouteSizes(const std::string& bfaVersion, const std::string& file, int batch);

// Whether the bridge lists sizes for that BFA version.
bool ListsSizesFor(const std::string& bfaVersion) noexcept;

}  // namespace bfarb
