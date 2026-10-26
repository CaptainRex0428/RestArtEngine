#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>

namespace rae {

struct GarbageCollectionSettings {
    bool enabled = true;
    double intervalSeconds = 60;
    double memoryIdleSeconds = 60;
    double temporaryCaptureMaxAgeSeconds = 3600;
};

struct GarbageCollectionMemoryStats {
    std::uint64_t cpuBytesReleased = 0;
    std::uint64_t gpuBytesReleasedEstimate = 0;
    std::uint64_t objectsReleased = 0;
};

struct GarbageCollectionStats {
    std::uint64_t collections = 0;
    // Memory and file counters describe the most recent collection.
    GarbageCollectionMemoryStats memory;
    std::uint64_t temporaryFilesRemoved = 0;
    std::uint64_t temporaryBytesRemoved = 0;
    double lastDurationMs = 0;
    std::string lastError;
};

// Main-thread service. update() receives frame delta seconds, not absolute time.
class GarbageCollector final {
public:
    GarbageCollector() = default;
    GarbageCollector(const GarbageCollector&) = delete;
    GarbageCollector& operator=(const GarbageCollector&) = delete;

    [[nodiscard]] const GarbageCollectionSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const GarbageCollectionStats& stats() const noexcept { return stats_; }
    bool setSettings(const GarbageCollectionSettings& settings, std::string& error);
    bool loadSettings(const std::filesystem::path& path, std::string& error);
    bool saveSettings(const std::filesystem::path& path, std::string& error) const;
    void requestCollection() noexcept { requested_ = true; }
    bool update(double elapsedSeconds,
        const std::function<GarbageCollectionMemoryStats(double memoryIdleSeconds)>& collectMemory);

    bool initializeTemporaryCaptures(const std::filesystem::path& root, std::string& error);
    // Production capture API. The synchronous writer borrows the stream and
    // must neither close it nor retain it. Windows pins the directory and file
    // throughout writing, then verifies file identity before registration.
    bool captureTemporary(const std::function<bool(std::FILE*, std::string&)>& writer,
        std::filesystem::path& output, std::string& error);
    // Low-level test/controlled-call interfaces: a returned name does not protect
    // path-based writes against replacement. Use captureTemporary in production.
    [[nodiscard]] std::filesystem::path temporaryCapturePath(std::string& error);
    bool registerTemporaryCapture(const std::filesystem::path& path, std::string& error);

private:
    void collectTemporaryCaptures();
    GarbageCollectionSettings settings_;
    GarbageCollectionStats stats_;
    double elapsedSeconds_ = 0;
    bool requested_ = false;
    std::filesystem::path temporaryRoot_;
    std::unordered_set<std::string> issuedCaptures_;
    std::filesystem::directory_iterator scan_;
};

} // namespace rae
