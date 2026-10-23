#include "core/Monitor.h"

#include "core/Status.h"

namespace bfarb {

Monitor::Monitor(Now now) : now_(now) {}

void Monitor::SetSink(Sink sink) {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
}

void Monitor::SetShowStatus(bool show) {
    std::lock_guard lock(mutex_);
    showStatus_ = show;
}

void Monitor::Emit(const std::vector<std::string>& lines) const {
    if (lines.empty()) return;
    Sink sink;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
    }
    if (!sink) return;
    for (const auto& l : lines) sink(l);
}

void Monitor::ModelState(const std::string& model, const std::string& server, const std::string& device, const std::string& state,
                         bool ready) {
    std::vector<std::string> out;
    {
        std::lock_guard lock(mutex_);
        Entry& e = models_[model];
        if (e.server == server && e.device == device && e.state == state && e.ready == ready) return;
        e = Entry{server, device, state, ready};
        if (!ready || showStatus_) out.push_back(ModelLine(model, server, device, state));
    }
    Emit(out);
}

void Monitor::Forget(const std::string& model) {
    std::lock_guard lock(mutex_);
    models_.erase(model);
}

void Monitor::Loaded() {
    std::vector<std::string> out;
    {
        std::lock_guard lock(mutex_);
        if (!showStatus_) return;
        for (const auto& [model, e] : models_) out.push_back(ModelLine(model, e.server, e.device, e.state));
    }
    Emit(out);
}

void Monitor::Fallback(Reason reason, const std::string& model, const std::string& words) {
    std::vector<std::string> out;
    {
        std::lock_guard lock(mutex_);
        const Clock::time_point now = now_();
        if (anyFallback_ && now - lastFallback_ < kFallbackEvery) return;  // at most one a minute, whatever came between
        anyFallback_ = true;
        inFallback_ = true;
        lastFallback_ = now;
        fallbackModel_ = model;
        out.push_back(FallbackLine(model, words.empty() ? ReasonWords(reason) : words));
    }
    Emit(out);
}

void Monitor::Answered(const std::string& model) {
    std::vector<std::string> out;
    {
        std::lock_guard lock(mutex_);
        if (!inFallback_) return;
        const Clock::time_point now = now_();
        if (anyBack_ && now - lastBack_ < kFallbackEvery) return;  // held: said by a later answer, after the minute
        anyBack_ = true;
        inFallback_ = false;
        lastBack_ = now;
        out.push_back(BackLine(model == fallbackModel_ ? model : std::string()));
    }
    Emit(out);
}

void Monitor::Once(const std::string& key, const std::string& text) {
    {
        std::lock_guard lock(mutex_);
        if (!said_.insert(key).second) return;
    }
    Emit({text});
}

bool Monitor::InFallback() const {
    std::lock_guard lock(mutex_);
    return inFallback_;
}

}  // namespace bfarb
