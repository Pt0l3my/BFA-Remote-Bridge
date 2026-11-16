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
// BFA 7.0.0's table is read from its DLL's code (7.0.0.1's), its source not being available: the DLL calls
// AddFreeDimensionOverrideByName in one place, its GPU session builder, which applies a list of names and values that
// each model's caller gives it. The callers set: ultra-performance.onnx batch 1 and frames 124; performance.onnx
// batch 1; both emotion models batch_size 1, seq_len 60000 and batch 1; a2f3_fp16.onnx window_dynamic_axes_1,
// identity_dynamic_axes_1, emotion_dynamic_axes_1 and input_latents_dynamic_axes_1 at 1; network_fp16.onnx
// input_dynamic_axes_1 and emotion_dynamic_axes_1 at BFA's Batch setting. Its frames of 124 match the window BFA feeds
// its Ultra Performance model (124 frames, 120 for its Performance model, on either device). Its performance.onnx fixes
// every dimension (mel [1, 120, 80], identity [1], emotion [1, 120, 10], past_k and past_v [6, 1, 4, 256, 152],
// past_frames [1]), so its batch names a dimension it lacks; the bridge lists it as BFA sets it, as it lists 5.0.6's
// emotion models'. Its ultra-performance.onnx has 5.0.6's Thin120 inputs (mel [batch, frames, 80], identity [batch],
// emotion [batch, frames, 10]). a2f3_fp16.onnx, network_fp16.onnx, emotion_fp16.onnx and emotion_performance.onnx
// weren't among the files read: the DLL names them, and the lists above are its lists for them. By file name alone,
// as for 5.0.6, for every 7.0.0.N.
#include <string>
#include <vector>

#include "core/Protocol.h"

namespace bfarb {

// The overrides BFA's GPU route sets on file's session in that BFA version, in BFA's order (5.0.6's and 7.0.0's);
// empty when the bridge lists none for it (another version, 4.3.2.3 among them, or another file). batch: BFA's
// [General] Batch.
std::vector<proto::Override> GpuRouteSizes(const std::string& bfaVersion, const std::string& file, int batch);

// Whether the bridge lists sizes for that BFA version: 5.0.6 or 7.0.0 (any build).
bool ListsSizesFor(const std::string& bfaVersion) noexcept;

}  // namespace bfarb
