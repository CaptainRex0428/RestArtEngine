#include "RestArtEngine/Editor/EditorConsole.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rae::editor {
namespace {
bool space(char c) noexcept { return c == ' ' || c == '\t'; }
bool validName(std::string_view text) {
    return !text.empty() && text.size() <= EditorConsole::MaxCommandLength &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}
ConsoleResult tokenize(std::string_view input, std::vector<std::string>& tokens) {
    std::string token;
    char quote = 0;
    bool started = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c == '\\') {
            if (i + 1 == input.size()) return {false, "A trailing backslash needs an escaped character."};
            const char next = input[i + 1];
            if (space(next) || next == '\'' || next == '"' || next == '\\') {
                token.push_back(next);
                ++i;
            } else token.push_back(c);
            started = true;
        } else if (quote) {
            if (c == quote) quote = 0;
            else token.push_back(c);
        } else if (c == '\'' || c == '"') {
            quote = c;
            started = true;
        } else if (space(c)) {
            if (started) { tokens.push_back(std::move(token)); token.clear(); started = false; }
        } else {
            token.push_back(c);
            started = true;
        }
    }
    if (quote) return {false, "Unterminated quoted argument."};
    if (started) tokens.push_back(std::move(token));
    return {};
}
} // namespace

EditorConsole::EditorConsole() {
    // Built-ins use dispatch rather than callbacks capturing this, so copying
    // or moving the model does not retain pointers into the old model.
    commands_.emplace("help", Command{"List commands, optionally filtered by prefix: help [prefix]", {}});
    commands_.emplace("clear", Command{"Clear console output (history is retained).", {}});
}

void EditorConsole::add(std::string name, std::string description, Callback callback) {
    if (!validName(name)) throw std::invalid_argument("Invalid console command name.");
    if (!callback) throw std::invalid_argument("Console command callback must not be empty.");
    commands_.insert_or_assign(std::move(name), Command{std::move(description), std::make_shared<Callback>(std::move(callback))});
}

void EditorConsole::append(std::string_view text, bool error) {
    // Copy before eviction: callers may append an existing entry's text.
    ConsoleEntry entry{error, std::string(text)};
    if (entries_.size() == MaxEntries) entries_.erase(entries_.begin());
    entries_.push_back(std::move(entry));
}

void EditorConsole::clearOutput() noexcept { entries_.clear(); }

ConsoleResult EditorConsole::help(std::span<const std::string> args) const {
    if (args.size() > 1) return {false, "Usage: help [prefix]"};
    const std::string_view prefix = args.empty() ? std::string_view{} : args.front();
    std::string text;
    for (const auto& [name, command] : commands_) {
        if (!name.starts_with(prefix)) continue;
        if (!text.empty()) text += '\n';
        text += name;
        if (!command.description.empty()) { text += " - "; text += command.description; }
    }
    if (text.empty()) text = "No commands match that prefix.";
    return {true, std::move(text)};
}

ConsoleResult EditorConsole::execute(std::string_view input) {
    // Own the input: a view into entries/history must survive output eviction.
    // Validate size before copying so an oversized pasted input is not retained.
    const auto reject = [this](std::string message) {
        historyPosition_ = history_.size();
        draft_.clear();
        append(message, true);
        return ConsoleResult{false, std::move(message)};
    };
    if (input.size() > MaxCommandLength) return reject("Command exceeds the 4096-byte limit.");
    for (const unsigned char c : input)
        if ((c < 32 && c != '\t') || c == 127)
            return reject("Commands must be a single line without control characters.");
    // Do not trim the parsed input: an escaped trailing space is an argument.
    const std::string command(input);
    std::vector<std::string> words;
    if (auto parsed = tokenize(command, words); !parsed.success) return reject(std::move(parsed.message));
    if (words.empty()) {
        historyPosition_ = history_.size();
        draft_.clear();
        return {};
    }
    if (!validName(words.front())) return reject("Invalid console command name.");
    // Preserve exact spelling: trimming an escaped trailing space changes its
    // meaning and would make recalling an otherwise valid command fail.
    const std::string& historyText = command;
    if (history_.empty() || history_.back() != historyText) {
        if (history_.size() == MaxHistory) history_.erase(history_.begin());
        history_.push_back(historyText);
    }
    historyPosition_ = history_.size();
    draft_.clear();
    append("> " + command);

    ConsoleResult result;
    const auto found = commands_.find(words.front());
    if (found == commands_.end()) {
        result = {false, "Unknown command: " + words.front() + ". Type help to list commands."};
    } else {
        try {
            // Retain the callable while it runs, even if it re-registers itself.
            // Do not copy std::function: mutable value captures must persist.
            auto callback = found->second.callback;
            const std::span<const std::string> args(words.data() + 1, words.size() - 1);
            if (callback) result = (*callback)(args);
            else if (words.front() == "help") result = help(args);
            else if (!args.empty()) result = {false, "Usage: clear"};
            else clearOutput();
        } catch (const std::exception& failure) {
            result = {false, std::string("Command failed: ") + failure.what()};
        } catch (...) {
            result = {false, "Command failed with an unknown exception."};
        }
    }
    if (!result.message.empty()) append(result.message, !result.success);
    return result;
}

std::vector<std::string> EditorConsole::complete(std::string_view prefix) const {
    while (!prefix.empty() && space(prefix.front())) prefix.remove_prefix(1);
    if (!prefix.empty() && !validName(prefix)) return {};
    std::vector<std::string> matches;
    for (const auto& [name, command] : commands_)
        if (name.starts_with(prefix)) matches.push_back(name);
    return matches;
}

std::string EditorConsole::historyPrevious(std::string_view currentDraft) {
    if (historyPosition_ == history_.size()) draft_ = currentDraft;
    if (historyPosition_ > 0) --historyPosition_;
    return history_.empty() ? draft_ : history_[historyPosition_];
}

std::string EditorConsole::historyNext() {
    if (historyPosition_ < history_.size()) ++historyPosition_;
    return historyPosition_ == history_.size() ? draft_ : history_[historyPosition_];
}

} // namespace rae::editor
