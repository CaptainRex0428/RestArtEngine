#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rae::editor {

struct ConsoleResult {
    bool success = true;
    std::string message;
};
struct ConsoleEntry {
    bool error = false;
    std::string text;
};

// UI-independent, single-threaded engine command dispatch. Never invokes a
// shell, expands environment variables, or interprets command separators.
class EditorConsole {
public:
    using Callback = std::function<ConsoleResult(std::span<const std::string>)>;
    static constexpr std::size_t MaxEntries = 512;
    static constexpr std::size_t MaxHistory = 100;
    static constexpr std::size_t MaxCommandLength = 4096; // UTF-8 bytes, including whitespace.

    EditorConsole();
    // Case-sensitive names contain ASCII letters/digits, '.', '_' or '-'.
    // Replaces existing registration; invalid names/empty callbacks throw
    // invalid_argument. Arguments exclude the command name and live for the call.
    // Registered callback state is retained between calls and shared if this
    // model is copied; built-ins always operate on their receiving model.
    void add(std::string name, std::string description, Callback callback);
    // Space/tab separate arguments. Single/double quotes preserve whitespace;
    // adjacent quoted and unquoted fragments form one argument ("" is empty).
    // Backslash escapes space, tab, either quote or backslash in every context.
    // Before other characters it remains literal (e.g. C:\Models\car.fbx).
    // Dangling backslashes, unclosed quotes, controls/newlines and oversized
    // commands fail before dispatch. No \n/\t escape expansion is performed.
    ConsoleResult execute(std::string_view command);
    // Sorted command-name matches; leading space/tab is allowed, but arguments
    // or whitespace following the first command word disable completion.
    [[nodiscard]] std::vector<std::string> complete(std::string_view prefix) const;
    [[nodiscard]] const std::vector<ConsoleEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const std::vector<std::string>& history() const noexcept { return history_; }
    void append(std::string_view text, bool error = false);
    void clearOutput() noexcept;
    // On the first Up, retains the current unfinished input. Down past the
    // newest entry restores it. Execution resets browsing; only consecutive
    // duplicate commands are collapsed. Empty/invalid input is not history.
    std::string historyPrevious(std::string_view currentDraft);
    std::string historyNext();
    // UI should ignore Down when false, preserving input not yet passed to Up.
    [[nodiscard]] bool historyBrowsing() const noexcept { return historyPosition_ < history_.size(); }

private:
    struct Command { std::string description; std::shared_ptr<Callback> callback; };
    std::map<std::string, Command, std::less<>> commands_;
    std::vector<ConsoleEntry> entries_;
    std::vector<std::string> history_;
    std::size_t historyPosition_ = 0;
    std::string draft_;
    ConsoleResult help(std::span<const std::string> args) const;
};

} // namespace rae::editor
