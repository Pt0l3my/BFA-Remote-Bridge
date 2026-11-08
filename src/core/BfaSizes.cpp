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

}  // namespace

bool ListsSizesFor(const std::string& bfaVersion) noexcept { return IsBfa506(bfaVersion); }

std::vector<proto::Override> GpuRouteSizes(const std::string& bfaVersion, const std::string& file, int batch) {
    std::vector<proto::Override> out;
    if (!ListsSizesFor(bfaVersion)) return out;
    for (const FileSizes& f : Bfa506()) {
        if (file != f.file) continue;
        for (const Size& s : f.sizes) out.push_back(proto::Override{s.dim, s.value == kBatchSetting ? std::max(1, batch) : s.value});
        break;
    }
    return out;
}

}  // namespace bfarb
