#include <RestArtEngine/Core/TraceProfiler.h>

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace rae::diagnostics {
namespace {
std::atomic<std::uint64_t> nextThreadId{1};
std::uint64_t threadId() noexcept {
    static thread_local const auto id = nextThreadId.fetch_add(1, std::memory_order_relaxed);
    return id;
}
struct StackEntry {
    const TraceProfiler* owner = nullptr;
    std::uint64_t eventId = 0, generation = 0;
};
struct ScopeStack {
    std::array<StackEntry, 128> entries{};
    std::size_t depth = 0;
};
thread_local ScopeStack scopeStack;
}

TraceProfiler::TraceProfiler(std::size_t eventCapacity) : capacity_(eventCapacity) {}

double TraceProfiler::microseconds(Clock::time_point point) const noexcept {
    return std::chrono::duration<double, std::micro>(point - origin_).count();
}

void TraceProfiler::resetCapture() noexcept {
    ++generation_;
    events_.clear();
    dropped_ = 0;
    frameEventCount_ = 0;
}

void TraceProfiler::start() noexcept {
    std::lock_guard lock(mutex_);
    resetCapture();
    recording_.store(true, std::memory_order_release);
}

void TraceProfiler::stop() noexcept {
    std::lock_guard lock(mutex_);
    recording_.store(false, std::memory_order_release);
    ++generation_; // Scopes spanning Stop cannot append to the frozen recording.
}

void TraceProfiler::clear() noexcept {
    std::lock_guard lock(mutex_);
    resetCapture();
}

void TraceProfiler::beginFrame() noexcept {
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    frameId_.fetch_add(1, std::memory_order_relaxed);
    frameStart_ = now;
    frameActive_ = true;
    frameGeneration_ = generation_;
    frameEventId_ = recording_.load(std::memory_order_relaxed) ? nextEventId_++ : 0;
    frameThreadId_ = threadId();
    frameEventCount_ = 0;
}

void TraceProfiler::append(TraceEvent event) noexcept {
    // Caller holds mutex_. No allocation or event construction occurs while disabled.
    if (events_.size() >= capacity_) { ++dropped_; return; }
    try {
        const bool currentFrame = frameActive_ && event.frameId == frameId_.load(std::memory_order_relaxed);
        events_.push_back(std::move(event));
        if (currentFrame) ++frameEventCount_;
    } catch (...) { ++dropped_; }
}

void TraceProfiler::endFrame() noexcept {
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    if (!frameActive_) return;
    if (recording_.load(std::memory_order_relaxed) && frameGeneration_ == generation_ && frameEventId_) {
        try {
            TraceEvent event;
            event.eventId = frameEventId_;
            event.frameId = frameId_.load(std::memory_order_relaxed);
            event.threadId = frameThreadId_;
            event.name = "Application Frame";
            event.category = "Frame";
            event.startUs = microseconds(frameStart_);
            event.durationUs = std::chrono::duration<double, std::micro>(now - frameStart_).count();
            append(std::move(event));
        } catch (...) { ++dropped_; }
    }
    frames_[frameWrite_] = {frameId_.load(std::memory_order_relaxed),
        std::chrono::duration<double, std::milli>(now - frameStart_).count(), frameEventCount_};
    frameWrite_ = (frameWrite_ + 1) % frames_.size();
    frameCount_ = std::min(frameCount_ + 1, frames_.size());
    frameActive_ = false;
}

void TraceProfiler::beginScope(TraceScope& scope, const char* name, const char* category) noexcept {
    if (!recording_.load(std::memory_order_relaxed)) return;
    const auto now = Clock::now();
    std::lock_guard lock(mutex_);
    if (!recording_.load(std::memory_order_relaxed)) return;
    if (events_.size() >= capacity_) { ++dropped_; return; }
    if (scopeStack.depth == scopeStack.entries.size()) { ++dropped_; return; }
    try {
        scope.event_.name = name ? name : "Unnamed";
        scope.event_.category = category ? category : "CPU";
        scope.event_.eventId = nextEventId_++;
        scope.event_.frameId = frameId_.load(std::memory_order_relaxed);
        scope.event_.threadId = threadId();
        scope.event_.parentEventId = frameActive_ && frameThreadId_ == scope.event_.threadId &&
            frameGeneration_ == generation_ ? frameEventId_ : 0;
        for (std::size_t i = scopeStack.depth; i > 0; --i) {
            const auto& parent = scopeStack.entries[i - 1];
            if (parent.owner == this && parent.generation == generation_) {
                scope.event_.parentEventId = parent.eventId;
                break;
            }
        }
        scope.generation_ = generation_;
        scope.start_ = now;
        scope.active_ = true;
        scopeStack.entries[scopeStack.depth++] = {this, scope.event_.eventId, generation_};
    } catch (...) { ++dropped_; }
}

void TraceProfiler::endScope(TraceScope& scope) noexcept {
    if (!scope.active_) return;
    const auto now = Clock::now();
    for (std::size_t i = scopeStack.depth; i > 0; --i) {
        if (scopeStack.entries[i - 1].owner == this && scopeStack.entries[i - 1].eventId == scope.event_.eventId) {
            for (std::size_t j = i; j < scopeStack.depth; ++j) scopeStack.entries[j - 1] = scopeStack.entries[j];
            --scopeStack.depth;
            break;
        }
    }
    std::lock_guard lock(mutex_);
    if (!recording_.load(std::memory_order_relaxed) || generation_ != scope.generation_) return;
    scope.event_.startUs = microseconds(scope.start_);
    scope.event_.durationUs = std::chrono::duration<double, std::micro>(now - scope.start_).count();
    append(std::move(scope.event_));
}

TraceSnapshot TraceProfiler::snapshot(std::size_t maxEvents) const {
    std::lock_guard lock(mutex_);
    TraceSnapshot result;
    result.recording = recording_.load(std::memory_order_relaxed);
    result.eventCapacity = capacity_;
    result.eventCount = events_.size();
    result.droppedEvents = dropped_;
    result.frames.reserve(frameCount_);
    const auto first = (frameWrite_ + frames_.size() - frameCount_) % frames_.size();
    for (std::size_t i = 0; i < frameCount_; ++i) result.frames.push_back(frames_[(first + i) % frames_.size()]);
    if (!result.frames.empty()) result.lastFrame = result.frames.back();
    const auto count = std::min(maxEvents, events_.size());
    result.events.assign(events_.end() - static_cast<std::ptrdiff_t>(count), events_.end());
    return result;
}

bool TraceProfiler::save(const std::filesystem::path& path, std::string& error) const {
    error.clear();
    try {
        const auto capture = snapshot(capacity_);
        nlohmann::json output;
        output["displayTimeUnit"] = "ms";
        output["metadata"] = {{"format", "RestArtEngine.Trace"}, {"version", 1},
            {"clock", "steady_clock"}, {"timestampUnit", "us"},
            {"eventCapacity", capture.eventCapacity}, {"droppedEvents", capture.droppedEvents},
            {"recording", capture.recording}, {"gpuClockCalibrated", false}};
        auto& events = output["traceEvents"] = nlohmann::json::array();
        std::vector<std::uint64_t> threads;
        for (const auto& event : capture.events) {
            if (std::find(threads.begin(), threads.end(), event.threadId) == threads.end()) {
                threads.push_back(event.threadId);
                events.push_back({{"name", "thread_name"}, {"ph", "M"}, {"pid", 1}, {"tid", event.threadId},
                    {"args", {{"name", "Thread " + std::to_string(event.threadId)}}}});
            }
            events.push_back({{"name", event.name}, {"cat", event.category}, {"ph", "X"},
                {"pid", 1}, {"tid", event.threadId}, {"ts", event.startUs}, {"dur", event.durationUs},
                {"args", {{"frameId", event.frameId}, {"eventId", event.eventId}, {"parentEventId", event.parentEventId}}}});
        }
        if (!capture.events.empty()) {
            const auto& last = capture.events.back();
            events.push_back({{"name", "Trace buffer"}, {"cat", "Diagnostics"}, {"ph", "C"},
                {"pid", 1}, {"tid", last.threadId}, {"ts", last.startUs + last.durationUs},
                {"args", {{"retainedEvents", capture.eventCount}, {"droppedEvents", capture.droppedEvents}}}});
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream) { error = "Cannot open trace output."; return false; }
        stream << output.dump(2);
        stream.close();
        if (!stream) { error = "Cannot write complete trace output."; return false; }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Cannot export trace: ") + exception.what();
        return false;
    }
}

TraceProfiler& profiler() noexcept { static TraceProfiler instance; return instance; }
void beginFrame() noexcept { profiler().beginFrame(); }
void endFrame() noexcept { profiler().endFrame(); }
std::uint64_t currentFrameId() noexcept { return profiler().currentFrameId(); }
TraceScope::TraceScope(const char* name, const char* category) noexcept : TraceScope(profiler(), name, category) {}
TraceScope::TraceScope(TraceProfiler& owner, const char* name, const char* category) noexcept : owner_(&owner) {
    owner.beginScope(*this, name, category);
}
TraceScope::~TraceScope() { owner_->endScope(*this); }

} // namespace rae::diagnostics
