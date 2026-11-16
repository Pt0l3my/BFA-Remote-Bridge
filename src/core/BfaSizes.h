#pragma once
// The sizes the bridge lists for a BFA version: the free-dimension overrides BFA's GPU route sets on each model's
// session (AddFreeDimensionOverrideByName), by BFA version and file name, for the versions whose CPU route sets none.
//
// In its CPU mode BFA sets no overrides, so its HELLO would list none. For BFA 4.3.2.3 a version 2 server fills in
// its sizes itself (the sizes rule, docs/PROTOCOL.md), matched by the files' SHA-256, and the bridge lists nothing:
// that HELLO is unchanged. For BFA 5.0.6 and 7.0.0 the bridge lists the GPU route's overrides itself, from a table per
// version, so the server builds the session it would build for BFA's GPU route, exactly as listed, whatever its own
// table holds. A HELLO for which BFA set overrides (its GPU route) is sent with BFA's own.
//
// From BFA 5.0.6's source (read for facts only; src/inference/ort_net.cc, the functions that make each model's
// session): the folded V3 network fixes its four batch dimensions at 1; the Legacy network its two at BFA's Batch
// setting ([General] Batch, 2 by default, at least 1); the Performance network its batch at 1; both emotion models,
// Full and Performance, share one list, batch_size 1, seq_len 60000 and batch 1. ONNX Runtime ignores an override
// whose dimension a model lacks (the Performance emotion model's input is fixed: [1, 48, 188]), so the list is sent
// as BFA sets it; PandorumLLM's server accepts such a list (tested in game with BFA 5.0.6). The table
// was also checked against the lists 5.0.6.3's own DLL builds, and applies to every 5.0.6.N by file name alone (the
// SHA-256 is logged, never compared): a later 5.0.6 build with other shapes would see the server's shape check fail.
//
// BFA 7.0.0's table is INFERRED, not read from its source (which wasn't read): from its models' shapes, read from the
// files of 7.0.0.1, and from the dimension names its DLL carries, the same as 5.0.6.3's (window_dynamic_axes_1,
// identity_dynamic_axes_1, emotion_dynamic_axes_1, input_latents_dynamic_axes_1, input_dynamic_axes_1, batch_size,
// seq_len and batch). It ships a2f3_fp16.onnx, network_fp16.onnx, emotion_fp16.onnx and emotion_performance.onnx as
// 5.0.6 does, and the bridge lists theirs as for 5.0.6. Its ultra-performance.onnx is 5.0.6's performance.onnx under a
// new name (Thin120, the same free dimensions), so it gets the batch at 1 that 5.0.6 set on that model. Its
// performance.onnx is a new model whose every dimension is fixed (mel [1, 120, 80], identity [1], emotion [1, 120, 10],
// past_k and past_v [6, 1, 4, 256, 152], past_frames [1]), so the bridge lists nothing for it: an override could only
// name a dimension it lacks. By file name alone, as for 5.0.6, for every 7.0.0.N.
#include <string>
#include <vector>

#include "core/Protocol.h"

namespace bfarb {

// The overrides BFA's GPU route sets on file's session in that BFA version, in BFA's order (5.0.6's, and 7.0.0's as
// inferred above); empty when the bridge lists none for it (another version, 4.3.2.3 among them, another file, or
// 7.0.0's performance.onnx). batch: BFA's [General] Batch.
std::vector<proto::Override> GpuRouteSizes(const std::string& bfaVersion, const std::string& file, int batch);

// Whether the bridge lists sizes for that BFA version: 5.0.6 or 7.0.0 (any build).
bool ListsSizesFor(const std::string& bfaVersion) noexcept;

}  // namespace bfarb
