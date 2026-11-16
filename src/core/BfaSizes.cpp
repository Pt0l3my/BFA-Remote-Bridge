#include "core/BfaSizes.h"

#include <algorithm>

#include "core/BfaMode.h"

namespace bfarb {
namespace {

constexpr std::int64_t kBatchSetting = -1;  // stands for BFA's Batch setting

struct Size {
    const char* dim;
    std::int64_t value;
};
struct FileSizes {
    const char* file;
    std::vector<Size> sizes;
};

// BFA 5.0.6's GPU route (see BfaSizes.h).
const std::vector<FileSizes>& Bfa506() {
    static const std::vector<FileSizes> table = {
        {"a2f3_fp16.onnx",
         {{"window_dynamic_axes_1", 1}, {"identity_dynamic_axes_1", 1}, {"emotion_dynamic_axes_1", 1}, {"input_latents_dynamic_axes_1", 1}}},
        {"network_fp16.onnx", {{"input_dynamic_axes_1", kBatchSetting}, {"emotion_dynamic_axes_1", kBatchSetting}}},
        {"performance.onnx", {{"batch", 1}}},
        {"emotion_fp16.onnx", {{"batch_size", 1}, {"seq_len", 60000}, {"batch", 1}}},
        {"emotion_performance.onnx", {{"batch_size", 1}, {"seq_len", 60000}, {"batch", 1}}},
    };
    return table;
}

// BFA 7.0.0's GPU route, inferred from its models and its DLL's dimension names, its source not read (see BfaSizes.h).
// performance.onnx, a new model with every dimension fixed, gets none, so it isn't listed.
const std::vector<FileSizes>& Bfa700() {
    static const std::vector<FileSizes> table = {
        {"a2f3_fp16.onnx",
         {{"window_dynamic_axes_1", 1}, {"identity_dynamic_axes_1", 1}, {"emotion_dynamic_axes_1", 1}, {"input_latents_dynamic_axes_1", 1}}},
        {"network_fp16.onnx", {{"input_dynamic_axes_1", kBatchSetting}, {"emotion_dynamic_axes_1", kBatchSetting}}},
        {"ultra-performance.onnx", {{"batch", 1}}},
        {"emotion_fp16.onnx", {{"batch_size", 1}, {"seq_len", 60000}, {"batch", 1}}},
        {"emotion_performance.onnx", {{"batch_size", 1}, {"seq_len", 60000}, {"batch", 1}}},
    };
    return table;
}

const std::vector<FileSizes>* TableFor(const std::string& bfaVersion) {
    if (IsBfa506(bfaVersion)) return &Bfa506();
    if (IsBfa700(bfaVersion)) return &Bfa700();
    return nullptr;
}

}  // namespace

bool ListsSizesFor(const std::string& bfaVersion) noexcept { return IsBfa506(bfaVersion) || IsBfa700(bfaVersion); }

std::vector<proto::Override> GpuRouteSizes(const std::string& bfaVersion, const std::string& file, int batch) {
    std::vector<proto::Override> out;
    const std::vector<FileSizes>* table = TableFor(bfaVersion);
    if (!table) return out;
    for (const FileSizes& f : *table) {
        if (file != f.file) continue;
        for (const Size& s : f.sizes) out.push_back(proto::Override{s.dim, s.value == kBatchSetting ? std::max(1, batch) : s.value});
        break;
    }
    return out;
}

}  // namespace bfarb
