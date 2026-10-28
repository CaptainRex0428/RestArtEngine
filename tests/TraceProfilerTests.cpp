#include <RestArtEngine/Core/TraceProfiler.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
const rae::diagnostics::TraceEvent& find(const rae::diagnostics::TraceSnapshot& capture, const char* name) {
    const auto found = std::find_if(capture.events.begin(), capture.events.end(),
        [&](const auto& event) { return event.name == name; });
    if (found == capture.events.end()) throw std::runtime_error("Expected trace event is missing.");
    return *found;
}
struct TemporaryFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("RestArtEngine-Trace-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ~TemporaryFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
}

int main() {
    using namespace rae::diagnostics;
    try {
        TraceProfiler trace(32);
        trace.beginFrame();
        { TraceScope disabled(trace, "Disabled"); }
        trace.endFrame();
        auto capture = trace.snapshot();
        require(!capture.recording && capture.eventCount == 0, "Disabled scopes must not record.");
        require(capture.lastFrame.frameId == 1 && capture.lastFrame.durationMs >= 0,
            "Frame IDs / full frame wall time must exist while tracing is disabled.");

        trace.start();
        trace.beginFrame();
        {
            TraceScope parent(trace, "Parent", "Submit");
            { TraceScope child(trace, "Child", "Submit"); }
            std::thread worker([&] { TraceScope work(trace, "Worker", "Asset IO"); });
            worker.join();
        }
        trace.endFrame();
        capture = trace.snapshot();
        const auto parent = find(capture, "Parent");
        const auto child = find(capture, "Child");
        const auto worker = find(capture, "Worker");
        const auto frame = find(capture, "Application Frame");
        require(child.parentEventId == parent.eventId && parent.parentEventId == frame.eventId,
            "Nested CPU events must preserve their parent and application frame.");
        require(parent.frameId == 2 && child.frameId == 2 && worker.frameId == 2,
            "Events must retain their originating application frame ID.");
        require(child.threadId == parent.threadId && worker.threadId != parent.threadId && worker.parentEventId == 0,
            "Worker events must have distinct thread IDs without a false main-thread parent.");
        require(child.startUs >= parent.startUs && child.durationUs >= 0 &&
            child.startUs + child.durationUs <= parent.startUs + parent.durationUs,
            "Steady-clock nested event timing must be ordered and non-negative.");
        require(capture.lastFrame.frameId == 2 && capture.lastFrame.eventCount == 4,
            "Completed frame summary must count its completed captured events.");
        require(trace.snapshot(1).events.size() == 1 && trace.snapshot(1).eventCount == 4,
            "Limited UI snapshots must retain the full buffer event count.");

        {
            TraceScope crossingStop(trace, "Crosses Stop");
            trace.stop();
            require(!trace.isRecording(), "Stop must disable recording.");
        }
        require(trace.snapshot().eventCount == 4, "Stop must freeze data even when an active scope later exits.");
        trace.clear();
        require(trace.snapshot().eventCount == 0 && !trace.isRecording(), "Clear must preserve stopped state.");

        trace.start();
        {
            TraceScope beforeClear(trace, "Before Clear");
            trace.clear();
            { TraceScope afterClear(trace, "After Clear"); }
        }
        capture = trace.snapshot();
        require(capture.recording && capture.eventCount == 1 && find(capture, "After Clear").parentEventId == 0,
            "Clear must invalidate old scopes and keep recording new independent events.");

        TraceProfiler bounded(2);
        bounded.start();
        for (int i = 0; i < 5; ++i) { TraceScope event(bounded, "Bounded"); }
        const auto full = bounded.snapshot();
        require(full.eventCount == 2 && full.eventCapacity == 2 && full.droppedEvents == 3,
            "Trace storage must stay bounded and report dropped events.");

        trace.start();
        const std::string escapedName = "Quote \" slash \\ newline\n tab\t UTF-8 \xe8\xa7\x86\xe5\x8f\xa3";
        { TraceScope event(trace, escapedName.c_str(), "Capture\\Export"); }
        trace.stop();
        TemporaryFile file;
        std::string error;
        require(trace.save(file.path, error), "Stopped trace must export successfully.");
        std::ifstream stream(file.path, std::ios::binary);
        const auto json = nlohmann::json::parse(stream);
        require(json["metadata"]["timestampUnit"] == "us" && json["metadata"]["clock"] == "steady_clock" &&
            json["metadata"]["gpuClockCalibrated"] == false, "Trace export must state its clock domain and units.");
        bool foundEscaped = false, foundThread = false, foundCounter = false;
        for (const auto& event : json["traceEvents"]) {
            if (event["ph"] == "X") {
                foundEscaped = event["name"] == escapedName && event["cat"] == "Capture\\Export";
                require(event["dur"].get<double>() >= 0 && event["args"].contains("frameId"),
                    "Exported complete events must contain duration and frame correlation.");
            }
            foundThread = foundThread || event["ph"] == "M";
            foundCounter = foundCounter || event["ph"] == "C";
        }
        require(foundEscaped && foundThread && foundCounter, "Chrome JSON must round-trip escaped UTF-8 names, metadata and counters.");
        require(!trace.save(file.path / "missing" / "trace.json", error) && !error.empty(),
            "Export failure must return an actionable error.");

        for (int i = 0; i < 130; ++i) { trace.beginFrame(); trace.endFrame(); }
        capture = trace.snapshot();
        require(capture.frames.size() == 120 && capture.frames.front().frameId < capture.lastFrame.frameId,
            "Frame history must remain bounded and chronological.");
        std::cout << "PASS: TraceProfilerTests " << checks << " checks.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
