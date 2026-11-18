#include <RestArtEngine/Core/RuntimeDiagnostics.h>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <psapi.h>
#endif

namespace rae::diagnostics {
namespace {
std::string_view boundedUtf8Prefix(std::string_view text, std::size_t limit) noexcept {
    if (text.size() <= limit) return text;
    // A byte budget must not split an otherwise valid multibyte UTF-8 scalar.
    // The first excluded byte identifies whether the cut bisected a sequence.
    while (limit != 0 && (static_cast<unsigned char>(text[limit]) & 0xc0u) == 0x80u) --limit;
    return text.substr(0, limit);
}
}
std::uint64_t diagnosticThreadId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    static thread_local const auto id = next.fetch_add(1, std::memory_order_relaxed);
    return id;
}
void RuntimeDiagnostics::log(MessageSeverity severity, std::string_view category, std::string_view text) noexcept {
    try {
        RuntimeMessage message;
        message.frameId = frame_.load(std::memory_order_relaxed);
        message.threadId = diagnosticThreadId();
        message.timeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        message.severity = severity;
        message.category.assign(boundedUtf8Prefix(category, 128));
        message.text.assign(boundedUtf8Prefix(text, 8192));
        if (text.size() > 8192) message.text += " [truncated]";
        std::lock_guard lock(mutex_);
        message.id = next_++;
        if (messages_.size() == capacity) { messages_.erase(messages_.begin()); ++dropped_; }
        messages_.push_back(std::move(message));
    } catch (...) { /* Diagnostics must not interrupt the application. */ }
}
void RuntimeDiagnostics::clear() noexcept {
    std::lock_guard lock(mutex_);
    messages_.clear(); dropped_ = 0;
}
RuntimeSnapshot RuntimeDiagnostics::snapshot() const {
    RuntimeSnapshot result;
    { std::lock_guard lock(mutex_); result.messages = messages_; result.droppedMessages = dropped_; }
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        result.processMemoryAvailable = true;
        result.workingSetBytes = memory.WorkingSetSize;
        result.privateBytes = memory.PrivateUsage;
    }
#endif
    return result;
}
RuntimeDiagnostics& runtimeDiagnostics() noexcept { static RuntimeDiagnostics result; return result; }
} // namespace rae::diagnostics
