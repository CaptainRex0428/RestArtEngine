#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rae::diagnostics {

struct TraceEvent {
    std::uint64_t eventId = 0, parentEventId = 0, frameId = 0, threadId = 0;
    std::string name, category;
    double startUs = 0, durationUs = 0;
};

struct FrameSummary {
    std::uint64_t frameId = 0;
    // Full application frame wall time, including Present / waits; not CPU active time.
    double durationMs = 0;
    std::size_t eventCount = 0;
};

struct TraceSnapshot {
    bool recording = false;
    std::size_t eventCapacity = 0, eventCount = 0;
    std::uint64_t droppedEvents = 0;
    FrameSummary lastFrame;
    std::vector<FrameSummary> frames;
    // Recent completed events; eventCount describes the entire retained recording.
    std::vector<TraceEvent> events;
};

class TraceScope;

class TraceProfiler final {
public:
    explicit TraceProfiler(std::size_t eventCapacity = 65536);
    TraceProfiler(const TraceProfiler&) = delete;
    TraceProfiler& operator=(const TraceProfiler&) = delete;

    // Start a new bounded recording. Stop retains data; clear preserves recording state.
    void start() noexcept;
    void stop() noexcept;
    void clear() noexcept;
    [[nodiscard]] bool isRecording() const noexcept { return recording_.load(std::memory_order_relaxed); }
    // Main application loop only. IDs advance even while tracing is disabled.
    void beginFrame() noexcept;
    void endFrame() noexcept;
    [[nodiscard]] std::uint64_t currentFrameId() const noexcept { return frameId_.load(std::memory_order_relaxed); }
    [[nodiscard]] TraceSnapshot snapshot(std::size_t maxEvents = 512) const;
    // Exports all retained events as Chrome Trace JSON (microseconds, steady clock).
    bool save(const std::filesystem::path& path, std::string& error) const;

private:
    friend class TraceScope;
    using Clock = std::chrono::steady_clock;
    void beginScope(TraceScope& scope, const char* name, const char* category) noexcept;
    void endScope(TraceScope& scope) noexcept;
    void append(TraceEvent event) noexcept;
    void resetCapture() noexcept;
    double microseconds(Clock::time_point point) const noexcept;

    mutable std::mutex mutex_;
    std::atomic<bool> recording_{false};
    std::atomic<std::uint64_t> frameId_{0};
    const std::size_t capacity_;
    const Clock::time_point origin_ = Clock::now();
    std::uint64_t generation_ = 0, nextEventId_ = 1, dropped_ = 0;
    std::vector<TraceEvent> events_;
    std::array<FrameSummary, 120> frames_{};
    std::size_t frameCount_ = 0, frameWrite_ = 0;
    bool frameActive_ = false;
    Clock::time_point frameStart_{};
    std::uint64_t frameGeneration_ = 0, frameEventId_ = 0, frameThreadId_ = 0;
    std::size_t frameEventCount_ = 0;
};

[[nodiscard]] TraceProfiler& profiler() noexcept;
void beginFrame() noexcept;
void endFrame() noexcept;
[[nodiscard]] std::uint64_t currentFrameId() noexcept;

class TraceScope final {
public:
    explicit TraceScope(const char* name, const char* category = "CPU") noexcept;
    TraceScope(TraceProfiler& owner, const char* name, const char* category = "CPU") noexcept;
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
    TraceScope(TraceScope&&) = delete;
    TraceScope& operator=(TraceScope&&) = delete;

private:
    friend class TraceProfiler;
    TraceProfiler* owner_ = nullptr;
    TraceEvent event_;
    std::uint64_t generation_ = 0;
    std::chrono::steady_clock::time_point start_{};
    bool active_ = false;
};

} // namespace rae::diagnostics
