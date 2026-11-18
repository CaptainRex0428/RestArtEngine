#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>

namespace rae::diagnostics {
enum class MessageSeverity { Info, Warning, Error };
struct RuntimeMessage {
    std::uint64_t id = 0, frameId = 0, threadId = 0;
    double timeMs = 0;
    MessageSeverity severity = MessageSeverity::Info;
    std::string category, text;
};
struct RuntimeSnapshot {
    std::vector<RuntimeMessage> messages;
    std::uint64_t droppedMessages = 0, workingSetBytes = 0, privateBytes = 0;
    bool processMemoryAvailable = false;
};
// Bounded diagnostic journal, not a debugger or allocation profiler. Logging
// failures never replace the application error that is being diagnosed.
class RuntimeDiagnostics {
public:
    static constexpr std::size_t capacity = 512;
    void setFrame(std::uint64_t id) noexcept { frame_.store(id, std::memory_order_relaxed); }
    void log(MessageSeverity, std::string_view category, std::string_view text) noexcept;
    void clear() noexcept;
    [[nodiscard]] RuntimeSnapshot snapshot() const;
private:
    mutable std::mutex mutex_;
    std::vector<RuntimeMessage> messages_;
    std::atomic<std::uint64_t> frame_{0};
    std::uint64_t next_ = 1, dropped_ = 0;
};
RuntimeDiagnostics& runtimeDiagnostics() noexcept;
// Shared by CPU traces and the runtime journal; values are process-local,
// stable for a thread lifetime and independent of OS thread ID reuse.
[[nodiscard]] std::uint64_t diagnosticThreadId() noexcept;
} // namespace rae::diagnostics
