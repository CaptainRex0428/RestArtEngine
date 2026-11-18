#include <RestArtEngine/Renderer/FrameCaptureMetadata.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace rae::renderer {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maxJsonBytes = 16ull * 1024 * 1024;
constexpr std::size_t maxConstantBytes = 8ull * 1024 * 1024;
constexpr std::uint64_t maxResourceBytes = 256ull * 1024 * 1024;
constexpr std::size_t maxStringBytes = 65536, maxEvents = 65536, maxResources = 4096, maxPasses = 128;
constexpr std::string_view schema = "RestArtEngine.FrameInspector.Capture";

[[noreturn]] void fail(const std::string& reason) { throw std::runtime_error(reason); }
void require(bool condition, const std::string& reason) { if (!condition) fail(reason); }
void fields(const Json& object, std::initializer_list<const char*> names, const char* context) {
    require(object.is_object(), std::string(context) + " must be an object.");
    for (const auto* name : names)
        require(object.contains(name), std::string(context) + " is missing field '" + name + "'.");
    require(object.size() == names.size(), std::string(context) + " contains unrecognized fields.");
}
const Json& array(const Json& value, std::size_t maximum, const char* name) {
    require(value.is_array(), std::string(name) + " must be an array.");
    require(value.size() <= maximum, std::string(name) + " exceeds its element budget.");
    return value;
}
std::string string(const Json& value, const char* name) {
    require(value.is_string(), std::string(name) + " must be a string.");
    const auto& text = value.get_ref<const std::string&>();
    require(text.size() <= maxStringBytes, std::string(name) + " exceeds its string budget.");
    return text;
}
std::string shaderSource(const Json& value, const char* name) {
    require(value.is_string(), std::string(name) + " must be a string.");
    const auto& text = value.get_ref<const std::string&>();
    require(text.size() <= 2ull * 1024 * 1024, std::string(name) + " exceeds its 2 MiB source budget.");
    return text;
}
bool boolean(const Json& value, const char* name) {
    require(value.is_boolean(), std::string(name) + " must be a boolean.");
    return value.get<bool>();
}
std::uint64_t natural(const Json& value, std::uint64_t maximum, const char* name) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) result = value.get<std::uint64_t>();
    else {
        require(value.is_number_integer(), std::string(name) + " must be an unsigned integer.");
        const auto signedValue = value.get<std::int64_t>();
        require(signedValue >= 0, std::string(name) + " must not be negative.");
        result = static_cast<std::uint64_t>(signedValue);
    }
    require(result <= maximum, std::string(name) + " exceeds its numeric range.");
    return result;
}
std::uint64_t u64(const Json& value, const char* name) {
    return natural(value, std::numeric_limits<std::uint64_t>::max(), name);
}
std::uint32_t u32(const Json& value, const char* name) {
    return static_cast<std::uint32_t>(natural(value, std::numeric_limits<std::uint32_t>::max(), name));
}
std::int32_t i32(const Json& value, const char* name) {
    if (value.is_number_unsigned())
        return static_cast<std::int32_t>(natural(value, std::numeric_limits<std::int32_t>::max(), name));
    require(value.is_number_integer(), std::string(name) + " must be an integer.");
    const auto result = value.get<std::int64_t>();
    require(result >= std::numeric_limits<std::int32_t>::min() && result <= std::numeric_limits<std::int32_t>::max(),
        std::string(name) + " exceeds signed 32-bit range.");
    return static_cast<std::int32_t>(result);
}
double real(const Json& value, const char* name, bool nonnegative = true) {
    require(value.is_number(), std::string(name) + " must be a finite number.");
    const auto result = value.get<double>();
    require(std::isfinite(result) && (!nonnegative || result >= 0), std::string(name) + " is not a valid finite number.");
    return result;
}
float scalar(const Json& value, const char* name) {
    const auto result = real(value, name, false);
    require(result >= -std::numeric_limits<float>::max() && result <= std::numeric_limits<float>::max(),
        std::string(name) + " exceeds finite float range.");
    return static_cast<float>(result);
}
std::vector<std::string> strings(const Json& value, const char* name) {
    const auto& items = array(value, maxResources, name);
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const auto& item : items) result.push_back(string(item, name));
    return result;
}
std::string hex(std::span<const std::uint8_t> bytes) {
    require(bytes.size() <= maxConstantBytes, "Constant bytes exceed the 8 MiB budget.");
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(bytes.size() * 2, '0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = alphabet[bytes[index] >> 4];
        result[index * 2 + 1] = alphabet[bytes[index] & 15];
    }
    return result;
}
std::vector<std::uint8_t> unhex(const Json& value, std::size_t& total) {
    require(value.is_string(), "Constant bytes must be a hexadecimal string.");
    const auto& text = value.get_ref<const std::string&>();
    require(text.size() % 2 == 0, "Constant byte hex has an odd length.");
    const auto size = text.size() / 2;
    require(size <= maxConstantBytes - total, "Constant bytes exceed the total 8 MiB budget.");
    const auto digit = [](char character) -> std::uint8_t {
        if (character >= '0' && character <= '9') return static_cast<std::uint8_t>(character - '0');
        if (character >= 'a' && character <= 'f') return static_cast<std::uint8_t>(character - 'a' + 10);
        if (character >= 'A' && character <= 'F') return static_cast<std::uint8_t>(character - 'A' + 10);
        fail("Constant byte hex contains an invalid digit.");
    };
    std::vector<std::uint8_t> result(size);
    for (std::size_t index = 0; index < size; ++index)
        result[index] = static_cast<std::uint8_t>(digit(text[index * 2]) * 16 + digit(text[index * 2 + 1]));
    total += size;
    return result;
}

Json encodePass(const RenderPassTiming& pass) {
    return {{"name", pass.name}, {"cpuMs", pass.cpuMs}, {"gpuMs", pass.gpuMs}, {"drawCalls", pass.drawCalls},
        {"cpuStartMs", pass.cpuStartMs}, {"firstEventId", pass.firstEventId}, {"lastEventId", pass.lastEventId},
        {"reads", pass.reads}, {"writes", pass.writes}};
}
RenderPassTiming decodePass(const Json& value) {
    fields(value, {"name", "cpuMs", "gpuMs", "drawCalls", "cpuStartMs", "firstEventId", "lastEventId", "reads", "writes"}, "Pass");
    RenderPassTiming result;
    result.name = string(value.at("name"), "Pass.name");
    result.cpuMs = real(value.at("cpuMs"), "Pass.cpuMs");
    result.gpuMs = real(value.at("gpuMs"), "Pass.gpuMs");
    result.drawCalls = u64(value.at("drawCalls"), "Pass.drawCalls");
    result.cpuStartMs = real(value.at("cpuStartMs"), "Pass.cpuStartMs");
    result.firstEventId = u64(value.at("firstEventId"), "Pass.firstEventId");
    result.lastEventId = u64(value.at("lastEventId"), "Pass.lastEventId");
    require(result.lastEventId == 0 || result.lastEventId >= result.firstEventId, "Pass event range is reversed.");
    result.reads = strings(value.at("reads"), "Pass.reads");
    result.writes = strings(value.at("writes"), "Pass.writes");
    return result;
}
Json encodeBinding(const RenderResourceBinding& binding) {
    return {{"stage", binding.stage}, {"kind", binding.kind}, {"name", binding.name}, {"format", binding.format},
        {"slot", binding.slot}, {"width", binding.width}, {"height", binding.height}, {"layer", binding.layer},
        {"stride", binding.stride}, {"offset", binding.offset}, {"bytes", binding.bytes}};
}
RenderResourceBinding decodeBinding(const Json& value) {
    fields(value, {"stage", "kind", "name", "format", "slot", "width", "height", "layer", "stride", "offset", "bytes"}, "Resource binding");
    RenderResourceBinding result;
    result.stage = string(value.at("stage"), "Binding.stage");
    result.kind = string(value.at("kind"), "Binding.kind");
    result.name = string(value.at("name"), "Binding.name");
    result.format = string(value.at("format"), "Binding.format");
    result.slot = u32(value.at("slot"), "Binding.slot");
    result.width = static_cast<std::uint32_t>(natural(value.at("width"), 16384, "Binding.width"));
    result.height = static_cast<std::uint32_t>(natural(value.at("height"), 16384, "Binding.height"));
    result.layer = u32(value.at("layer"), "Binding.layer");
    result.stride = u32(value.at("stride"), "Binding.stride");
    result.offset = u32(value.at("offset"), "Binding.offset");
    result.bytes = u64(value.at("bytes"), "Binding.bytes");
    return result;
}
Json encodeDraw(const RenderDrawEvent& event, std::size_t& constantBytes) {
    Json bindings = Json::array(), constants = Json::array();
    for (const auto& binding : event.bindings) bindings.push_back(encodeBinding(binding));
    for (const auto& constant : event.constants) {
        require(constant.bytes.size() <= maxConstantBytes - constantBytes, "Constant bytes exceed the total 8 MiB budget.");
        constantBytes += constant.bytes.size();
        constants.push_back({{"stage", constant.stage}, {"name", constant.name}, {"slot", constant.slot},
            {"version", constant.version}, {"bytes", hex(constant.bytes)}});
    }
    return {{"pass", event.pass}, {"actorId", event.actorId}, {"meshId", event.meshId}, {"materialId", event.materialId},
        {"indexCount", event.indexCount}, {"vertexCount", event.vertexCount}, {"shaderRevision", event.shaderRevision},
        {"eventId", event.eventId}, {"passIndex", event.passIndex}, {"firstIndex", event.firstIndex}, {"materialSlot", event.materialSlot},
        {"baseVertex", event.baseVertex}, {"command", event.command}, {"actorName", event.actorName}, {"meshName", event.meshName},
        {"materialName", event.materialName}, {"vertexShader", event.vertexShader}, {"pixelShader", event.pixelShader},
        {"pipelineState", event.pipelineState}, {"viewport", event.viewport}, {"bindings", std::move(bindings)},
        {"constants", std::move(constants)}, {"detailedState", event.detailedState}};
}
RenderDrawEvent decodeDraw(const Json& value, std::size_t& constantBytes) {
    fields(value, {"pass", "actorId", "meshId", "materialId", "indexCount", "vertexCount", "shaderRevision", "eventId", "passIndex",
        "firstIndex", "materialSlot", "baseVertex", "command", "actorName", "meshName", "materialName", "vertexShader", "pixelShader",
        "pipelineState", "viewport", "bindings", "constants", "detailedState"}, "Draw event");
    RenderDrawEvent result;
    result.pass = string(value.at("pass"), "Draw.pass");
    result.actorId = u64(value.at("actorId"), "Draw.actorId");
    result.meshId = u64(value.at("meshId"), "Draw.meshId");
    result.materialId = u64(value.at("materialId"), "Draw.materialId");
    result.indexCount = u32(value.at("indexCount"), "Draw.indexCount");
    result.vertexCount = u32(value.at("vertexCount"), "Draw.vertexCount");
    result.shaderRevision = u64(value.at("shaderRevision"), "Draw.shaderRevision");
    result.eventId = u64(value.at("eventId"), "Draw.eventId");
    result.passIndex = static_cast<std::uint32_t>(natural(value.at("passIndex"), maxPasses - 1, "Draw.passIndex"));
    result.firstIndex = u32(value.at("firstIndex"), "Draw.firstIndex");
    result.materialSlot = u32(value.at("materialSlot"), "Draw.materialSlot");
    result.baseVertex = i32(value.at("baseVertex"), "Draw.baseVertex");
    result.command = string(value.at("command"), "Draw.command");
    result.actorName = string(value.at("actorName"), "Draw.actorName");
    result.meshName = string(value.at("meshName"), "Draw.meshName");
    result.materialName = string(value.at("materialName"), "Draw.materialName");
    result.vertexShader = string(value.at("vertexShader"), "Draw.vertexShader");
    result.pixelShader = string(value.at("pixelShader"), "Draw.pixelShader");
    result.pipelineState = string(value.at("pipelineState"), "Draw.pipelineState");
    const auto& viewport = array(value.at("viewport"), 6, "Draw.viewport");
    require(viewport.size() == 6, "Draw.viewport must contain exactly six values.");
    for (std::size_t index = 0; index < 6; ++index) result.viewport[index] = scalar(viewport[index], "Draw.viewport");
    for (const auto& binding : array(value.at("bindings"), 256, "Draw.bindings")) result.bindings.push_back(decodeBinding(binding));
    for (const auto& constant : array(value.at("constants"), 128, "Draw.constants")) {
        fields(constant, {"stage", "name", "slot", "version", "bytes"}, "Constant binding");
        RenderConstantBinding binding;
        binding.stage = string(constant.at("stage"), "Constant.stage");
        binding.name = string(constant.at("name"), "Constant.name");
        binding.slot = u32(constant.at("slot"), "Constant.slot");
        binding.version = u64(constant.at("version"), "Constant.version");
        binding.bytes = unhex(constant.at("bytes"), constantBytes);
        result.constants.push_back(std::move(binding));
    }
    result.detailedState = boolean(value.at("detailedState"), "Draw.detailedState");
    return result;
}
Json encodeFrame(const diagnostics::FrameSummary& frame) {
    return {{"frameId", frame.frameId}, {"durationMs", frame.durationMs}, {"eventCount", frame.eventCount}};
}
diagnostics::FrameSummary decodeFrame(const Json& value) {
    fields(value, {"frameId", "durationMs", "eventCount"}, "Trace frame");
    return {u64(value.at("frameId"), "TraceFrame.frameId"), real(value.at("durationMs"), "TraceFrame.durationMs"),
        static_cast<std::size_t>(natural(value.at("eventCount"), maxEvents, "TraceFrame.eventCount"))};
}
Json encodeTrace(const diagnostics::TraceSnapshot& trace) {
    Json frames = Json::array(), events = Json::array();
    for (const auto& frame : trace.frames) frames.push_back(encodeFrame(frame));
    for (const auto& event : trace.events)
        events.push_back({{"eventId", event.eventId}, {"parentEventId", event.parentEventId}, {"frameId", event.frameId},
            {"threadId", event.threadId}, {"name", event.name}, {"category", event.category}, {"startUs", event.startUs},
            {"durationUs", event.durationUs}});
    return {{"recording", trace.recording}, {"eventCapacity", trace.eventCapacity}, {"eventCount", trace.eventCount},
        {"droppedEvents", trace.droppedEvents}, {"lastFrame", encodeFrame(trace.lastFrame)},
        {"frames", std::move(frames)}, {"events", std::move(events)}};
}
diagnostics::TraceSnapshot decodeTrace(const Json& value) {
    fields(value, {"recording", "eventCapacity", "eventCount", "droppedEvents", "lastFrame", "frames", "events"}, "CPU trace");
    diagnostics::TraceSnapshot result;
    result.recording = boolean(value.at("recording"), "Trace.recording");
    result.eventCapacity = static_cast<std::size_t>(natural(value.at("eventCapacity"), maxEvents, "Trace.eventCapacity"));
    result.eventCount = static_cast<std::size_t>(natural(value.at("eventCount"), maxEvents, "Trace.eventCount"));
    require(result.eventCount <= result.eventCapacity, "Trace event count exceeds its capacity.");
    result.droppedEvents = u64(value.at("droppedEvents"), "Trace.droppedEvents");
    result.lastFrame = decodeFrame(value.at("lastFrame"));
    for (const auto& frame : array(value.at("frames"), 120, "Trace.frames")) result.frames.push_back(decodeFrame(frame));
    for (const auto& event : array(value.at("events"), maxEvents, "Trace.events")) {
        fields(event, {"eventId", "parentEventId", "frameId", "threadId", "name", "category", "startUs", "durationUs"}, "Trace event");
        diagnostics::TraceEvent item;
        item.eventId = u64(event.at("eventId"), "TraceEvent.eventId");
        item.parentEventId = u64(event.at("parentEventId"), "TraceEvent.parentEventId");
        item.frameId = u64(event.at("frameId"), "TraceEvent.frameId");
        item.threadId = u64(event.at("threadId"), "TraceEvent.threadId");
        item.name = string(event.at("name"), "TraceEvent.name");
        item.category = string(event.at("category"), "TraceEvent.category");
        item.startUs = real(event.at("startUs"), "TraceEvent.startUs");
        item.durationUs = real(event.at("durationUs"), "TraceEvent.durationUs");
        require(std::isfinite(item.startUs + item.durationUs), "Trace event endpoint exceeds the finite range.");
        result.events.push_back(std::move(item));
    }
    require(result.events.size() <= result.eventCount, "Trace contains more event payloads than its retained count.");
    return result;
}

const char* encodeGpuStatus(GpuTimingStatus status) {
    switch (status) {
    case GpuTimingStatus::Unavailable: return "Unavailable";
    case GpuTimingStatus::Pending: return "Pending";
    case GpuTimingStatus::Ready: return "Ready";
    case GpuTimingStatus::Disjoint: return "Disjoint";
    }
    fail("Capture GPU timing status is invalid.");
}
GpuTimingStatus decodeGpuStatus(const Json& value) {
    const auto name = string(value, "Capture.gpuStatus");
    if (name == "Unavailable") return GpuTimingStatus::Unavailable;
    if (name == "Pending") return GpuTimingStatus::Pending;
    if (name == "Ready") return GpuTimingStatus::Ready;
    if (name == "Disjoint") return GpuTimingStatus::Disjoint;
    fail("Capture GPU timing status is not recognized.");
}

// The library's DOM parse-callback path scans a parent array at every object
// end to look for discarded values, even if the callback discards nothing.
// Validate duplicate keys/depth/budgets with streaming SAX instead, then use
// the ordinary DOM parser. Both passes are linear for large event arrays.
class MetadataGuard final : public nlohmann::json_sax<Json> {
public:
    bool null() override { consume(); return true; }
    bool boolean(bool) override { consume(); return true; }
    bool number_integer(number_integer_t) override { consume(); return true; }
    bool number_unsigned(number_unsigned_t) override { consume(); return true; }
    bool number_float(number_float_t value, const string_t&) override {
        require(std::isfinite(value), "Capture metadata contains a non-finite number.");
        consume(); return true;
    }
    bool string(string_t&) override { consume(); return true; }
    bool binary(binary_t&) override { fail("Binary values are not valid capture JSON."); }
    bool start_object(std::size_t) override {
        consume(); push(true, {}, 0); return true;
    }
    bool key(string_t& name) override {
        require(!stack_.empty() && stack_.back().object, "Invalid capture metadata object nesting.");
        auto& frame = stack_.back();
        require(frame.keys.size() < 256, "Capture object exceeds its field budget.");
        require(frame.keys.insert(name).second, "Capture metadata contains duplicate object keys.");
        frame.pendingKey = name;
        return true;
    }
    bool end_object() override { stack_.pop_back(); return true; }
    bool start_array(std::size_t) override {
        const auto name = !stack_.empty() && stack_.back().object ? stack_.back().pendingKey : std::string{};
        consume();
        std::size_t limit = maxEvents;
        if (name == "resources" || name == "reads" || name == "writes") limit = maxResources;
        else if (name == "passes" || name == "gpuPasses" || name == "constants") limit = maxPasses;
        else if (name == "frames") limit = 120;
        else if (name == "bindings") limit = 256;
        else if (name == "viewport") limit = 6;
        push(false, name, limit); return true;
    }
    bool end_array() override { stack_.pop_back(); return true; }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception& exception) override {
        fail(std::string("Invalid capture JSON: ") + exception.what());
    }
private:
    struct Frame {
        bool object = false;
        std::unordered_set<std::string> keys;
        std::string pendingKey, name;
        std::size_t count = 0, limit = 0;
    };
    void push(bool object, std::string name, std::size_t limit) {
        require(stack_.size() < 32, "Capture metadata nesting exceeds the schema depth budget.");
        Frame frame;
        frame.object = object; frame.name = std::move(name); frame.limit = limit;
        stack_.push_back(std::move(frame));
    }
    void consume() {
        if (stack_.empty() || stack_.back().object) return;
        auto& frame = stack_.back();
        require(frame.count < frame.limit, "Capture array '" + frame.name + "' exceeds its element budget.");
        ++frame.count;
    }
    std::vector<Frame> stack_;
};

RenderFrameCapture decode(const Json& value) {
    fields(value, {"format", "version", "supportsReplay", "containsImagePayloads", "valid", "frameId", "shaderRevision", "renderPath",
        "cpuSubmitMs", "resources", "events", "status", "metadataOnlyExport", "backend", "width", "height", "nearPlane", "farPlane",
        "orthographic", "offline", "passes", "cpuTrace", "droppedEvents", "materialShaderSource", "pipelineShaderSource",
        "gpuFrameId", "gpuStatus", "gpuTotalMs", "gpuPasses"}, "Capture metadata");
    require(string(value.at("format"), "Capture.format") == schema, "Unrecognized capture metadata schema.");
    require(natural(value.at("version"), std::numeric_limits<std::uint32_t>::max(), "Capture.version") == 2,
        "Unsupported capture metadata version; expected version 2.");
    require(!boolean(value.at("supportsReplay"), "Capture.supportsReplay"), "Replay captures are not supported.");
    require(boolean(value.at("containsImagePayloads"), "Capture.containsImagePayloads"), "Capture bundle payload declaration is required.");
    require(!boolean(value.at("metadataOnlyExport"), "Capture.metadataOnlyExport"), "Metadata-only exports are not capture bundles.");
    RenderFrameCapture result;
    result.valid = boolean(value.at("valid"), "Capture.valid");
    require(result.valid, "The metadata does not contain a valid captured frame.");
    result.frameId = u64(value.at("frameId"), "Capture.frameId");
    result.shaderRevision = u64(value.at("shaderRevision"), "Capture.shaderRevision");
    result.renderPath = string(value.at("renderPath"), "Capture.renderPath");
    result.cpuSubmitMs = real(value.at("cpuSubmitMs"), "Capture.cpuSubmitMs");
    result.status = string(value.at("status"), "Capture.status");
    result.metadataOnlyExport = false;
    result.backend = string(value.at("backend"), "Capture.backend");
    result.materialShaderSource = shaderSource(value.at("materialShaderSource"), "Capture.materialShaderSource");
    result.pipelineShaderSource = shaderSource(value.at("pipelineShaderSource"), "Capture.pipelineShaderSource");
    result.width = static_cast<std::uint32_t>(natural(value.at("width"), 16384, "Capture.width"));
    result.height = static_cast<std::uint32_t>(natural(value.at("height"), 16384, "Capture.height"));
    require((result.width == 0) == (result.height == 0), "Capture dimensions must both be zero or both be positive.");
    result.nearPlane = scalar(value.at("nearPlane"), "Capture.nearPlane");
    result.farPlane = scalar(value.at("farPlane"), "Capture.farPlane");
    result.orthographic = boolean(value.at("orthographic"), "Capture.orthographic");
    result.offline = boolean(value.at("offline"), "Capture.offline");
    result.gpuFrameId = u64(value.at("gpuFrameId"), "Capture.gpuFrameId");
    result.gpuStatus = decodeGpuStatus(value.at("gpuStatus"));
    result.gpuTotalMs = real(value.at("gpuTotalMs"), "Capture.gpuTotalMs");
    if (result.gpuStatus == GpuTimingStatus::Ready || result.gpuStatus == GpuTimingStatus::Pending)
        require(result.gpuFrameId == result.frameId, "GPU timing must identify the same captured frame.");
    else if (result.gpuStatus == GpuTimingStatus::Unavailable)
        require(result.gpuFrameId == 0, "Unavailable GPU timing cannot identify a ready frame.");
    else require(result.gpuFrameId == 0 || result.gpuFrameId == result.frameId,
        "Disjoint GPU timing cannot identify a different frame.");
    for (const auto& pass : array(value.at("gpuPasses"), maxPasses, "Capture.gpuPasses")) result.gpuPasses.push_back(decodePass(pass));
    std::uint64_t resourceBytes = 0;
    for (const auto& resource : array(value.at("resources"), maxResources, "Capture.resources")) {
        fields(resource, {"name", "format", "width", "height", "bytes", "pass", "boundary", "layer", "displayKind", "eventId"}, "Captured resource");
        CapturedRenderResource item;
        item.name = string(resource.at("name"), "Resource.name");
        item.format = string(resource.at("format"), "Resource.format");
        require(item.format == "RGBA8_UNORM" || item.format == "RGBA16F" || item.format == "R32_FLOAT", "Unsupported captured resource format.");
        item.width = static_cast<std::uint32_t>(natural(resource.at("width"), 16384, "Resource.width"));
        item.height = static_cast<std::uint32_t>(natural(resource.at("height"), 16384, "Resource.height"));
        require(item.width > 0 && item.height > 0, "Captured resource dimensions must be positive.");
        item.bytes = u64(resource.at("bytes"), "Resource.bytes");
        const auto expected = static_cast<std::uint64_t>(item.width) * item.height * (item.format == "RGBA16F" ? 8u : 4u);
        require(item.bytes == expected, "Captured resource byte count does not match its dimensions and format.");
        require(item.bytes <= maxResourceBytes - resourceBytes, "Captured resource payloads exceed the total 256 MiB budget.");
        resourceBytes += item.bytes;
        item.pass = string(resource.at("pass"), "Resource.pass");
        item.boundary = string(resource.at("boundary"), "Resource.boundary");
        item.layer = u32(resource.at("layer"), "Resource.layer");
        item.displayKind = static_cast<std::uint32_t>(natural(resource.at("displayKind"), 5, "Resource.displayKind"));
        item.eventId = u64(resource.at("eventId"), "Resource.eventId");
        result.resources.push_back(std::move(item));
    }
    if (!result.resources.empty()) {
        require(result.width > 0 && result.height > 0, "Image captures require positive frame dimensions.");
        require(result.nearPlane > 0 && result.farPlane > result.nearPlane, "Image captures require valid positive near/far planes.");
    }
    for (const auto& pass : array(value.at("passes"), maxPasses, "Capture.passes")) result.passes.push_back(decodePass(pass));
    std::size_t constantBytes = 0;
    for (const auto& event : array(value.at("events"), maxEvents, "Capture.events")) result.events.push_back(decodeDraw(event, constantBytes));
    result.cpuTrace = decodeTrace(value.at("cpuTrace"));
    result.droppedEvents = u64(value.at("droppedEvents"), "Capture.droppedEvents");
    return result;
}
} // namespace

std::string encodeCaptureMetadata(const RenderFrameCapture& capture) {
    require(capture.resources.size() <= maxResources && capture.events.size() <= maxEvents && capture.passes.size() <= maxPasses &&
        capture.gpuPasses.size() <= maxPasses,
        "Capture metadata exceeds its resource/event/pass element budget.");
    require(capture.cpuTrace.events.size() <= maxEvents && capture.cpuTrace.frames.size() <= 120,
        "CPU trace exceeds its element budget.");
    Json resources = Json::array(), events = Json::array(), passes = Json::array(), gpuPasses = Json::array();
    for (const auto& resource : capture.resources)
        resources.push_back({{"name", resource.name}, {"format", resource.format}, {"width", resource.width},
            {"height", resource.height}, {"bytes", resource.bytes}, {"pass", resource.pass}, {"boundary", resource.boundary},
            {"layer", resource.layer}, {"displayKind", resource.displayKind}, {"eventId", resource.eventId}});
    std::size_t constantBytes = 0;
    for (const auto& event : capture.events) events.push_back(encodeDraw(event, constantBytes));
    for (const auto& pass : capture.passes) passes.push_back(encodePass(pass));
    for (const auto& pass : capture.gpuPasses) gpuPasses.push_back(encodePass(pass));
    Json data{{"format", std::string(schema)}, {"version", 2}, {"supportsReplay", false}, {"containsImagePayloads", true},
        {"valid", capture.valid}, {"frameId", capture.frameId}, {"shaderRevision", capture.shaderRevision},
        {"renderPath", capture.renderPath}, {"cpuSubmitMs", capture.cpuSubmitMs}, {"resources", std::move(resources)},
        {"events", std::move(events)}, {"status", capture.status}, {"metadataOnlyExport", false}, {"backend", capture.backend},
        {"width", capture.width}, {"height", capture.height}, {"nearPlane", capture.nearPlane}, {"farPlane", capture.farPlane},
        {"orthographic", capture.orthographic}, {"offline", capture.offline}, {"passes", std::move(passes)},
        {"cpuTrace", encodeTrace(capture.cpuTrace)}, {"droppedEvents", capture.droppedEvents},
        {"materialShaderSource", capture.materialShaderSource}, {"pipelineShaderSource", capture.pipelineShaderSource},
        {"gpuFrameId", capture.gpuFrameId}, {"gpuStatus", encodeGpuStatus(capture.gpuStatus)}, {"gpuTotalMs", capture.gpuTotalMs},
        {"gpuPasses", std::move(gpuPasses)}};
    (void)decode(data); // Apply the same strict value/range rules before serialization.
    auto result = data.dump();
    require(result.size() <= maxJsonBytes, "Capture metadata exceeds the 16 MiB JSON budget.");
    return result;
}

bool decodeCaptureMetadata(std::string_view text, RenderFrameCapture& capture, std::string& error) {
    error.clear();
    try {
        require(text.size() <= maxJsonBytes, "Capture metadata exceeds the 16 MiB JSON budget.");
        require(!text.empty(), "Capture metadata is empty.");
        MetadataGuard guard;
        require(Json::sax_parse(text.begin(), text.end(), &guard), "Invalid frame capture metadata JSON.");
        auto candidate = decode(Json::parse(text.begin(), text.end()));
        capture = std::move(candidate);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid frame capture metadata: ") + exception.what();
        return false;
    }
}
} // namespace rae::renderer
