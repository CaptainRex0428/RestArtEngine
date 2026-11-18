#include <RestArtEngine/Core/RuntimeDiagnostics.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Editor/EditorConsole.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <latch>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace rae::diagnostics;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static_assert(noexcept(std::declval<RuntimeDiagnostics&>().setFrame(1)));
static_assert(noexcept(std::declval<RuntimeDiagnostics&>().log(MessageSeverity::Info, "category", "text")));
static_assert(noexcept(std::declval<RuntimeDiagnostics&>().clear()));

void identityAndOwnership() {
    RuntimeDiagnostics journal;
    auto snapshot = journal.snapshot();
    require(snapshot.messages.empty() && snapshot.droppedMessages == 0,
        "A fresh runtime journal must be empty.");
    journal.setFrame(42);
    std::string category = "Assets", text = "Imported mesh";
    journal.log(MessageSeverity::Info, category, text);
    category.assign("changed"); text.assign("changed");
    journal.log(MessageSeverity::Warning, "Renderer", "Feature unavailable");
    journal.setFrame(43);
    journal.log(MessageSeverity::Error, "Shader", "Compilation failed");
    snapshot = journal.snapshot();
    require(snapshot.messages.size() == 3 && snapshot.droppedMessages == 0,
        "Every successful log call must retain one message.");
    const auto& first = snapshot.messages.front();
    require(first.frameId == 42 && snapshot.messages[1].frameId == 42 && snapshot.messages[2].frameId == 43,
        "Messages must preserve the application frame that originated them.");
    require(first.id > 0 && first.id < snapshot.messages[1].id &&
        snapshot.messages[1].id < snapshot.messages[2].id,
        "Message identities must increase in publication order.");
    require(first.category == "Assets" && first.text == "Imported mesh",
        "Messages must own their strings instead of retaining borrowed views.");
    require(first.threadId == diagnosticThreadId() && first.threadId != 0 &&
        snapshot.messages[1].threadId == first.threadId,
        "Messages must identify their originating thread.");
    require(std::isfinite(first.timeMs) && first.timeMs >= 0 &&
        snapshot.messages.back().timeMs >= first.timeMs,
        "Runtime messages need finite monotonic-clock timestamps.");
    require(first.severity == MessageSeverity::Info &&
        snapshot.messages[1].severity == MessageSeverity::Warning &&
        snapshot.messages[2].severity == MessageSeverity::Error,
        "The journal must retain the supplied severity.");
    const auto lastId = snapshot.messages.back().id;
    journal.clear();
    snapshot = journal.snapshot();
    require(snapshot.messages.empty() && snapshot.droppedMessages == 0,
        "Clear must empty retained messages and overflow accounting.");
    journal.log(MessageSeverity::Info, "After clear", "next");
    snapshot = journal.snapshot();
    require(snapshot.messages.size() == 1 && snapshot.messages[0].id > lastId &&
        snapshot.messages[0].frameId == 43,
        "Clear must not reuse message IDs or reset the application frame.");
    if (snapshot.processMemoryAvailable)
        require(snapshot.workingSetBytes > 0, "Available process working-set data cannot be an invented zero.");
}

void truncationAndOverflow() {
    RuntimeDiagnostics journal;
    journal.setFrame(99);
    journal.log(MessageSeverity::Warning, std::string(256, 'C'), std::string(9000, 'T'));
    auto snapshot = journal.snapshot();
    require(snapshot.messages.size() == 1 && snapshot.messages[0].category.size() == 128,
        "Category strings must be bounded to 128 bytes.");
    require(snapshot.messages[0].text == std::string(8192, 'T') + " [truncated]",
        "Oversized messages must retain a bounded prefix and an explicit truncation marker.");
    journal.clear();
    journal.log(MessageSeverity::Info, std::string(127, 'C') + "\xe4\xb8\xad",
        std::string(8191, 'T') + "\xe4\xb8\xad\xe6\x96\x87");
    snapshot = journal.snapshot();
    require(snapshot.messages[0].category == std::string(127, 'C') &&
        snapshot.messages[0].text == std::string(8191, 'T') + " [truncated]",
        "UTF-8 truncation must not preserve an incomplete multibyte character.");
    journal.clear();
    for (std::size_t i = 0; i < RuntimeDiagnostics::capacity + 27; ++i)
        journal.log(MessageSeverity::Info, "Overflow", std::to_string(i));
    snapshot = journal.snapshot();
    require(snapshot.messages.size() == RuntimeDiagnostics::capacity && snapshot.droppedMessages == 27,
        "The journal must remain bounded to 512 messages and account for eviction.");
    require(snapshot.messages.front().text == "27" &&
        snapshot.messages.back().text == std::to_string(RuntimeDiagnostics::capacity + 26),
        "Overflow must retain the newest messages in order.");
    require(std::all_of(snapshot.messages.begin(), snapshot.messages.end(),
        [](const auto& message) { return message.frameId == 99; }),
        "Overflow must not modify message frame identities.");
    journal.clear();
    snapshot = journal.snapshot();
    require(snapshot.messages.empty() && snapshot.droppedMessages == 0,
        "Clear must reset overflow reporting as well as retained data.");
}

void concurrentPublication() {
    RuntimeDiagnostics journal;
    journal.setFrame(207);
    constexpr unsigned ThreadCount = 4, MessagesPerThread = 200;
    std::latch ready(ThreadCount), start(1);
    std::vector<std::thread> writers;
    for (unsigned thread = 0; thread < ThreadCount; ++thread) {
        writers.emplace_back([&, thread] {
            ready.count_down();
            start.wait();
            for (unsigned message = 0; message < MessagesPerThread; ++message)
                journal.log(MessageSeverity::Info, "Worker " + std::to_string(thread), std::to_string(message));
        });
    }
    ready.wait();
    start.count_down();
    // Snapshot while writers publish; its own lock must provide a coherent copy.
    auto intermediate = journal.snapshot();
    for (auto& writer : writers) writer.join();
    const auto snapshot = journal.snapshot();
    require(intermediate.messages.size() <= RuntimeDiagnostics::capacity,
        "Concurrent snapshots must respect the journal capacity.");
    require(snapshot.messages.size() == RuntimeDiagnostics::capacity &&
        snapshot.droppedMessages == ThreadCount * MessagesPerThread - RuntimeDiagnostics::capacity,
        "Concurrent writers must not lose unaccounted messages or exceed capacity.");
    std::set<std::uint64_t> ids;
    std::uint64_t previous = 0;
    for (const auto& message : snapshot.messages) {
        require(message.id > previous && ids.insert(message.id).second,
            "Concurrent message identities must be unique and ordered.");
        require(message.frameId == 207 && message.category.starts_with("Worker ") &&
            !message.text.empty() && std::isfinite(message.timeMs),
            "Concurrent messages must preserve their payload and origin.");
        previous = message.id;
    }
    require(snapshot.messages.back().id == ThreadCount * MessagesPerThread,
        "All successful concurrent publications must receive one identity.");
}

void consoleAndTraceCorrelation() {
    auto& journal = runtimeDiagnostics();
    journal.clear();
    profiler().start();
    beginFrame();
    const auto frame = currentFrameId();
    rae::editor::EditorConsole console;
    {
        TraceScope trace("Console work", "Test");
        console.append("Console info");
        // The append argument can alias retained console storage.
        console.append(console.entries().front().text);
        require(!console.execute("missing.command").success,
            "An unknown command must still report its normal console error.");
    }
    endFrame();
    profiler().stop();
    const auto runtime = journal.snapshot();
    const auto traced = profiler().snapshot();
    require(runtime.messages.size() == console.entries().size() && runtime.messages.size() == 4,
        "Console mirroring must publish exactly one journal message per appended console entry.");
    for (std::size_t i = 0; i < runtime.messages.size(); ++i) {
        require(runtime.messages[i].category == "EditorConsole" &&
            runtime.messages[i].text == console.entries()[i].text &&
            runtime.messages[i].severity == (console.entries()[i].error
                ? MessageSeverity::Error : MessageSeverity::Info),
            "Console mirror payload and severity must match the committed console entry.");
        require(runtime.messages[i].frameId == frame && runtime.messages[i].threadId == diagnosticThreadId(),
            "The free beginFrame API must associate console messages with the application frame and thread.");
    }
    const auto scope = std::find_if(traced.events.begin(), traced.events.end(),
        [](const auto& event) { return event.name == "Console work"; });
    require(scope != traced.events.end() && scope->frameId == frame &&
        scope->threadId == runtime.messages[0].threadId,
        "CPU trace scopes and runtime messages must share frame/thread identities.");
    console.clearOutput();
    require(console.entries().empty() && journal.snapshot().messages.size() == runtime.messages.size(),
        "Console output clearing must not erase the independent diagnostic journal.");
    // Recording is disabled, but the application frame still advances and
    // log correlation must continue. The two clocks use different origins.
    beginFrame();
    console.append("Recording disabled");
    endFrame();
    require(journal.snapshot().messages.back().frameId == frame + 1 &&
        !profiler().isRecording(),
        "Log frame identity must continue when CPU trace recording is disabled.");
    journal.clear();
    profiler().clear();
}
} // namespace

int main() {
    try {
        identityAndOwnership();
        truncationAndOverflow();
        concurrentPublication();
        consoleAndTraceCorrelation();
        std::cout << "PASS: " << checks << " RuntimeDiagnostics checks.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
