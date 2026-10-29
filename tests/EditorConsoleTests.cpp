#include "RestArtEngine/Editor/EditorConsole.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rae::editor;
int checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void dispatchAndArguments() {
    EditorConsole console;
    std::vector<std::string> received;
    unsigned calls = 0;
    console.add("echo", "Return the supplied arguments.", [&](std::span<const std::string> args) {
        ++calls;
        received.assign(args.begin(), args.end());
        return ConsoleResult{true, "received"};
    });
    require(console.execute(" \t ").success && console.entries().empty() && console.history().empty(), "Empty input has no side effects.");
    require(console.execute(R"(echo plain "two words" 'three words' "" pre"mid dle"post)" ).success, "Quoted arguments parse.");
    require(received == std::vector<std::string>{"plain", "two words", "three words", "", "premid dlepost"}, "Arguments preserve quoting and empty strings.");
    require(console.execute(R"(echo one\ two "say \"hi\"" 'it\'s' C:\Models\car.fbx \\server "\n")").success, "Backslash escapes parse.");
    require(received == std::vector<std::string>{"one two", "say \"hi\"", "it's", R"(C:\Models\car.fbx)", R"(\server)", R"(\n)"}, "Escapes are explicit; Windows paths and literal backslash-n remain literal.");
    require(console.execute("echo tail\\ ").success && received == std::vector<std::string>{"tail "}, "Escaped trailing space is preserved.");
    const auto recalled = console.historyPrevious("draft");
    require(recalled == "echo tail\\ " && console.execute(recalled).success && received.front() == "tail ", "History preserves escaped trailing whitespace.");
    const auto beforeInvalid = calls;
    const auto historyBeforeInvalid = console.history().size();
    for (const auto& input : std::vector<std::string>{"echo \"unclosed", "echo 'unclosed", "echo tail\\", "echo\nclear", "echo\rclear", std::string("echo a\0b", 8), std::string("echo \x01")}) {
        require(!console.execute(input).success, "Malformed quote/control input is rejected.");
    }
    require(calls == beforeInvalid && console.history().size() == historyBeforeInvalid, "Invalid input does not dispatch or enter history.");
    require(!console.execute("does-not-exist").success && console.entries().back().error, "Unknown command reports an error.");
    require(console.entries()[console.entries().size()-2].text == "> does-not-exist", "Unknown commands retain their echo.");
    require(console.execute("echo a;clear $HOME $(command) && other").success, "Shell-looking text stays ordinary arguments.");
    require(received == std::vector<std::string>{"a;clear", "$HOME", "$(command)", "&&", "other"}, "No shell expansion or command chaining occurs.");
    console.add("disabled", "An unavailable engine action.", [](auto) { return ConsoleResult{false, "No project is open."}; });
    const auto disabled = console.execute("disabled");
    require(!disabled.success && disabled.message == "No project is open." && console.entries().back().error, "Callback disabled result propagates intact.");
    console.add("throws", "Throw standard exception.", [](auto) -> ConsoleResult { throw std::runtime_error("detail"); });
    require(!console.execute("throws").success && console.entries().back().text.find("detail") != std::string::npos, "Standard exceptions become errors.");
    console.add("throws", "Throw nonstandard exception.", [](auto) -> ConsoleResult { throw 7; });
    require(!console.execute("throws").success, "Nonstandard exceptions become errors; re-registration works.");
    console.add("self", "Replace callback during invocation.", [&](auto) {
        console.add("self", "Replacement.", [](auto) { return ConsoleResult{true, "replacement"}; });
        return ConsoleResult{true, "original"};
    });
    require(console.execute("self").message == "original" && console.execute("self").message == "replacement", "Self replacement keeps active callback alive.");
    console.add("counter", "Retain mutable callback state.", [count = 0](auto) mutable {
        return ConsoleResult{true, std::to_string(++count)};
    });
    require(console.execute("counter").message == "1" && console.execute("counter").message == "2", "Mutable callback state survives consecutive dispatches.");
    require(console.execute("Echo").success == false, "Names are case-sensitive.");
    bool threw = false;
    try { console.add("bad name", "", [](auto) { return ConsoleResult{}; }); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "Invalid registration names rejected.");
    threw = false;
    try { console.add("valid", "", {}); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "Empty callback registration rejected.");
}
void helpAndCompletion() {
    EditorConsole console;
    console.add("zebra", "Zebra description.", [](auto) { return ConsoleResult{}; });
    console.add("alpha", "Alpha description.", [](auto) { return ConsoleResult{}; });
    console.add("alpine", "Alpine description.", [](auto) { return ConsoleResult{}; });
    const auto result = console.execute("help");
    require(result.success && result.message.find("alpha - Alpha description.") != std::string::npos, "Help shows registered descriptions.");
    require(result.message.find("alpha") < result.message.find("alpine") && result.message.find("alpine") < result.message.find("clear") &&
        result.message.find("help") < result.message.find("zebra"), "Help is sorted by command name.");
    const auto filtered = console.execute("help al");
    require(filtered.success && filtered.message.find("alpha") != std::string::npos && filtered.message.find("zebra") == std::string::npos, "Help accepts a prefix.");
    require(console.execute("help absent").message == "No commands match that prefix.", "Help reports empty matches.");
    require(!console.execute("help one two").success, "Help rejects extra arguments.");
    require(console.complete("al") == std::vector<std::string>{"alpha", "alpine"}, "Completion returns sorted names.");
    require(console.complete(" \tal") == console.complete("al"), "Completion tolerates leading whitespace.");
    require(console.complete("alpha ").empty() && console.complete("help al").empty() && console.complete("alpha\t").empty(), "Completion does not suggest arguments.");
    const auto all = console.complete("");
    require(all.size() == 5 && std::is_sorted(all.begin(), all.end()), "Empty completion lists all commands including built-ins.");
    require(console.complete("AL").empty() && console.complete("al\n").empty(), "Completion follows case and input rules.");
    const auto count = console.entries().size();
    require(!console.execute("clear extra").success && console.entries().size() > count, "Invalid clear does not discard output.");
    const auto historySize = console.history().size();
    require(console.execute("clear").success && console.entries().empty() && console.history().size() == historySize + 1, "Clear removes output but keeps history.");
    auto copy = console;
    copy.append("Only in the copy.");
    console.append("Original output.");
    require(copy.execute("clear").success && copy.entries().empty() && console.entries().size() == 1, "Copied built-ins target their own model.");
    copy.add("clear", "Override.", [](auto) { return ConsoleResult{true, "overridden"}; });
    require(copy.execute("clear").message == "overridden", "Built-in registrations can be replaced.");
}
void historyAndBounds() {
    EditorConsole console;
    require(!console.historyBrowsing(), "New console is not browsing history.");
    require(console.historyPrevious("unfinished") == "unfinished" && console.historyNext() == "unfinished", "Empty history preserves draft.");
    require(!console.historyBrowsing(), "Empty history cannot enter browsing state.");
    console.add("run", "Test command.", [](auto) { return ConsoleResult{}; });
    console.execute("run one"); console.execute("run two"); console.execute("run two");
    require(console.history().size() == 2, "Consecutive duplicate commands share a history entry.");
    require(!console.historyBrowsing(), "Executing leaves history browsing inactive.");
    require(console.historyPrevious("run draft") == "run two", "Up selects newest command.");
    require(console.historyBrowsing(), "Up enters history browsing.");
    require(console.historyPrevious("edited recall") == "run one", "Next Up selects older command without replacing draft.");
    require(console.historyPrevious("anything") == "run one", "Up stops at oldest command.");
    require(console.historyNext() == "run two" && console.historyNext() == "run draft" && console.historyNext() == "run draft", "Down returns through history and restores the draft.");
    require(!console.historyBrowsing(), "Restored draft leaves history browsing.");
    console.historyPrevious("new draft");
    console.execute("run three");
    require(console.historyNext().empty(), "Executing resets history browsing and old draft.");
    require(!console.historyBrowsing(), "Execution resets browsing state.");
    console.historyPrevious("another draft");
    require(!console.execute("run \"unfinished").success && !console.historyBrowsing(), "Rejected input also resets browsing state.");
    for (int i = 0; i < 130; ++i) console.execute("run " + std::to_string(i));
    require(console.history().size() == EditorConsole::MaxHistory && console.history().front() == "run 30" && console.history().back() == "run 129", "History evicts oldest commands at 100 entries.");
    console.clearOutput();
    for (int i = 0; i < 600; ++i) console.append(std::to_string(i), i % 2 != 0);
    require(console.entries().size() == EditorConsole::MaxEntries && console.entries().front().text == "88" && console.entries().back().text == "599", "Output evicts oldest entries at 512.");
    require(console.entries().back().error, "Append preserves error styling flag.");
    console.append(console.entries().front().text);
    require(console.entries().back().text == "88", "Appending borrowed entry text survives eviction.");
    unsigned calls = 0;
    console.add("size", "Command size boundary.", [&](auto) { ++calls; return ConsoleResult{}; });
    const auto accepted = "size " + std::string(EditorConsole::MaxCommandLength - 5, 'x');
    require(console.execute(accepted).success && calls == 1, "Exactly 4096 input bytes are accepted.");
    require(!console.execute(accepted + "x").success && calls == 1, "Oversized command is rejected before dispatch.");
    require(console.history().back() == accepted && console.entries().size() <= EditorConsole::MaxEntries, "Oversized input does not enter bounded history or overflow output.");
    console.execute(console.history().back());
    require(calls == 2, "Execution safely accepts a view into history.");
}
} // namespace

int main() {
    try {
        dispatchAndArguments();
        helpAndCompletion();
        historyAndBounds();
        std::cout << "Editor console: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
