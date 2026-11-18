#include <RestArtEngine/Renderer/FrameCaptureMetadata.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace rae::renderer;
using Json = nlohmann::json;
unsigned checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

RenderFrameCapture fixture() {
    RenderFrameCapture capture;
    capture.valid = true;
    capture.frameId = 403; capture.shaderRevision = 17;
    capture.renderPath = "Deferred"; capture.backend = "d3d12";
    capture.width = 64; capture.height = 32;
    capture.nearPlane = 0.2f; capture.farPlane = 500.f; capture.orthographic = true;
    capture.cpuSubmitMs = 4.5;
    capture.status = "Frozen selected pass outputs; no replay.";
    capture.droppedEvents = 11;
    capture.materialShaderSource = "// material source\nfloat3 Material() { return float3(1,0,0); }\n";
    capture.pipelineShaderSource = "// main pipeline source, includes retained as paths\n#include \"Material.hlsli\"\n";
    capture.resources.push_back({"Depth layer 1", "R32_FLOAT", 16, 16, 1024, "GBuffer", "Before", 1, 5, 0});
    capture.resources.push_back({"Albedo", "RGBA16F", 16, 16, 2048, "GBuffer", "After", 0, 1, 2});
    RenderPassTiming pass;
    pass.name = "GBuffer"; pass.cpuMs = 2.5; pass.gpuMs = 0.75; pass.drawCalls = 2;
    pass.cpuStartMs = 1.25; pass.firstEventId = 1; pass.lastEventId = 2;
    pass.reads = {"Shadow cascade 0", "Shadow cascade 1"}; pass.writes = {"Albedo", "Depth"};
    capture.passes.push_back(pass);
    capture.gpuFrameId = capture.frameId; capture.gpuStatus = GpuTimingStatus::Ready;
    capture.gpuTotalMs = 1.25; capture.gpuPasses.push_back(pass);
    for (std::uint64_t version = 1; version < 3; ++version) {
        RenderDrawEvent event;
        event.pass = "GBuffer"; event.actorId = std::numeric_limits<std::uint64_t>::max();
        event.meshId = 8; event.materialId = 11;
        event.indexCount = 30; event.vertexCount = 24; event.shaderRevision = 17;
        event.eventId = version; event.passIndex = 0; event.firstIndex = 6; event.materialSlot = 2;
        event.baseVertex = -2; event.command = "DrawIndexed";
        event.actorName = "Actor \"A\""; event.meshName = "Cube"; event.materialName = "Copper";
        event.vertexShader = "Scene.VSMain"; event.pixelShader = "Scene.PSDeferred";
        event.pipelineState = "Cull=Back; ZTest=Less; ZWrite=On";
        event.viewport = {-1.5f, 2.f, 64.f, 32.f, 0.f, 1.f};
        event.bindings.push_back({"IA", "Vertex", "Mesh positions", "float3", 0, 0, 0, 0, 24, 8, 576});
        event.bindings.push_back({"PS", "Sampled", "Shadow array", "Depth32Float", 3, 16, 16, 1, 0, 0, 1024});
        event.constants.push_back({"VS", "Scene constants", 0, version,
            {0, 1, 0x7f, 0x80, 0xff, static_cast<std::uint8_t>(version)}});
        event.constants.push_back({"PS", "Material constants", 1, version + 10, {1, 2, 3, 4}});
        event.detailedState = true;
        capture.events.push_back(std::move(event));
    }
    auto& trace = capture.cpuTrace;
    trace.recording = true; trace.eventCapacity = 65536; trace.eventCount = 3; trace.droppedEvents = 5;
    trace.lastFrame = {403, 16.25, 3};
    trace.frames = {{402, 15.5, 2}, {403, 16.25, 3}};
    trace.events = {{10, 0, 403, 7, "Application frame", "Application", 1000, 16250},
        {11, 10, 403, 7, "Submit scene", "Renderer CPU", 1100, 4500},
        {12, 10, 403, 9, "Worker prepare", "Jobs", 1200, 1000}};
    return capture;
}

void roundtrip() {
    std::cerr << "[metadata] detailed roundtrip\n";
    const auto original = fixture();
    const auto text = encodeCaptureMetadata(original);
    const auto encoded = Json::parse(text);
    require(encoded.at("version") == 2 && encoded.at("format") == "RestArtEngine.FrameInspector.Capture",
        "The bundle metadata must have an explicit own schema and version.");
    require(encoded.at("supportsReplay") == false && encoded.at("containsImagePayloads") == true &&
        encoded.at("metadataOnlyExport") == false, "Bundle capabilities must distinguish payloads from replay.");
    require(encoded.at("events")[0].at("constants")[0].at("bytes") == "00017f80ff01",
        "Raw constants must use compact lossless hexadecimal bytes.");
    RenderFrameCapture decoded;
    std::string error = "Previous error";
    require(decodeCaptureMetadata(text, decoded, error) && error.empty(), "Valid detailed metadata must decode: " + error);
    require(decoded.events[0].constants[0].version == 1 && decoded.events[1].constants[0].version == 2 &&
        decoded.events[0].constants[0].bytes != decoded.events[1].constants[0].bytes,
        "Distinct draw versions of the same constant binding must remain independent.");
    require(decoded.events[0].actorId == std::numeric_limits<std::uint64_t>::max() && decoded.events[0].baseVertex == -2,
        "Unsigned asset identity and signed base vertex must preserve their full ranges.");
    require(decoded.resources[0].pass == "GBuffer" && decoded.resources[0].boundary == "Before" &&
        decoded.resources[0].layer == 1 && decoded.resources[0].displayKind == 5 &&
        decoded.resources[1].displayKind == 1 && decoded.resources[1].eventId == 2,
        "Captured resources must retain exact pass, boundary, layer and event provenance.");
    require(decoded.passes[0].reads == original.passes[0].reads && decoded.passes[0].gpuMs == .75 &&
        decoded.passes[0].cpuStartMs == 1.25, "Pass dependencies and separate CPU/GPU timing must roundtrip.");
    require(decoded.gpuFrameId == decoded.frameId && decoded.gpuStatus == GpuTimingStatus::Ready &&
        decoded.gpuTotalMs == 1.25 && decoded.gpuPasses[0].gpuMs == .75,
        "Ready GPU samples must remain associated with their exact captured frame.");
    require(decoded.events[0].bindings[0].stride == 24 && decoded.events[0].bindings[0].offset == 8 &&
        decoded.events[0].viewport == original.events[0].viewport && decoded.events[0].detailedState,
        "Draw resource bindings, viewport and detail state must roundtrip.");
    require(decoded.cpuTrace.recording && decoded.cpuTrace.events[1].parentEventId == 10 &&
        decoded.cpuTrace.events[2].threadId == 9 && decoded.cpuTrace.events[2].category == "Jobs" &&
        decoded.cpuTrace.events[2].startUs == 1200 && decoded.cpuTrace.events[2].durationUs == 1000 &&
        decoded.cpuTrace.droppedEvents == 5 && decoded.droppedEvents == 11,
        "CPU trace identity, hierarchy, timing and both dropped counters must roundtrip.");
    require(decoded.materialShaderSource == original.materialShaderSource && decoded.pipelineShaderSource == original.pipelineShaderSource,
        "Captured material and pipeline source text must roundtrip without claiming expanded includes.");
    require(Json::parse(encodeCaptureMetadata(decoded)) == encoded, "Every encoded metadata field must survive a full roundtrip.");

    auto eventsOnly = original;
    eventsOnly.resources.clear(); eventsOnly.width = eventsOnly.height = 0;
    require(decodeCaptureMetadata(encodeCaptureMetadata(eventsOnly), decoded, error) && decoded.resources.empty(),
        "A bounded events-only capture must not require image payload dimensions.");
    auto offline = original; offline.offline = true;
    require(decodeCaptureMetadata(encodeCaptureMetadata(offline), decoded, error) && decoded.offline,
        "Offline provenance must roundtrip independently of the current renderer.");
    auto pending = original; pending.gpuStatus = GpuTimingStatus::Pending;
    pending.gpuTotalMs = 0; pending.gpuPasses.clear();
    require(decodeCaptureMetadata(encodeCaptureMetadata(pending), decoded, error) && decoded.gpuStatus == GpuTimingStatus::Pending,
        "A pending captured-frame timing sample must remain pending after loading.");
    pending.gpuStatus = GpuTimingStatus::Unavailable; pending.gpuFrameId = 0;
    require(decodeCaptureMetadata(encodeCaptureMetadata(pending), decoded, error) && decoded.gpuStatus == GpuTimingStatus::Unavailable &&
        decoded.gpuFrameId == 0, "Unavailable timing must retain explicit absence of a GPU frame identity.");
}

void failedLoadPreserves(const std::string& input, const char* context) {
    std::cerr << "[metadata] decoding rejection: " << context << '\n';
    auto previous = fixture(); previous.frameId = 999; previous.status = "Previously loaded capture";
    previous.gpuFrameId = previous.frameId;
    const auto before = encodeCaptureMetadata(previous);
    std::string error;
    require(!decodeCaptureMetadata(input, previous, error), std::string(context) + " must be rejected.");
    require(!error.empty(), std::string(context) + " must explain the failure.");
    require(encodeCaptureMetadata(previous) == before, std::string(context) + " must leave the entire previous capture intact.");
}
template<class Change>
void rejects(Change change, const char* context) {
    std::cerr << "[metadata] preparing rejection: " << context << '\n';
    auto input = Json::parse(encodeCaptureMetadata(fixture()));
    change(input);
    failedLoadPreserves(input.dump(), context);
}

void malformedAndBudgets() {
    failedLoadPreserves("", "Empty input");
    const auto valid = encodeCaptureMetadata(fixture());
    failedLoadPreserves(valid.substr(0, valid.size() / 2), "Truncated JSON");
    failedLoadPreserves(std::string(16ull * 1024 * 1024 + 1, ' '), "Oversized JSON input");
    failedLoadPreserves("{\"version\":2," + valid.substr(1), "Duplicate schema keys");
    failedLoadPreserves(std::string(34, '[') + "0" + std::string(34, ']'), "Excessive nesting");
    rejects([](Json& data) { data["format"] = "ForeignCapture"; }, "Foreign schema");
    rejects([](Json& data) { data["version"] = 3; }, "Future version");
    rejects([](Json& data) { data["version"] = 2.0; }, "Floating point schema version");
    rejects([](Json& data) { data["supportsReplay"] = true; }, "Unsupported replay claim");
    rejects([](Json& data) { data["containsImagePayloads"] = false; }, "Metadata-only payload declaration");
    rejects([](Json& data) { data["metadataOnlyExport"] = true; }, "Metadata-only export as a bundle");
    rejects([](Json& data) { data["valid"] = false; }, "Uncaptured frame");
    rejects([](Json& data) { data.erase("cpuTrace"); }, "Missing required field");
    rejects([](Json& data) { data["unrecognized"] = 1; }, "Unrecognized root field");
    rejects([](Json& data) { data["offline"] = 1; }, "Non-boolean offline state");
    rejects([](Json& data) { data["frameId"] = -1; }, "Negative frame identity");
    rejects([](Json& data) { data["frameId"] = 1.5; }, "Fractional frame identity");
    rejects([](Json& data) { data["cpuSubmitMs"] = nullptr; }, "Non-numeric timing");
    rejects([](Json& data) { data["cpuSubmitMs"] = -1; }, "Negative CPU timing");
    rejects([](Json& data) { data["gpuStatus"] = "Unknown"; }, "Unknown GPU timing status");
    rejects([](Json& data) { data["gpuStatus"] = 2; }, "Non-string GPU timing status");
    rejects([](Json& data) { data["gpuFrameId"] = 402; }, "Ready GPU sample for another frame");
    rejects([](Json& data) { data["gpuTotalMs"] = -1; }, "Negative GPU timing");
    rejects([](Json& data) { data["gpuPasses"][0]["gpuMs"] = -1; }, "Negative GPU pass timing");
    rejects([](Json& data) { data["nearPlane"] = 0; }, "Zero near plane with images");
    rejects([](Json& data) { data["farPlane"] = data["nearPlane"]; }, "Reversed depth interval");
    rejects([](Json& data) { data["width"] = 0; }, "Partial frame dimensions");
    rejects([](Json& data) { data["resources"][0]["width"] = 0; }, "Zero image width");
    rejects([](Json& data) { data["resources"][0]["width"] = 16385; }, "Oversized image width");
    rejects([](Json& data) { data["resources"][0]["bytes"] = 1; }, "Mismatched payload byte count");
    rejects([](Json& data) { data["resources"][0]["format"] = "BC7"; }, "Unsupported image format");
    rejects([](Json& data) { data["resources"][0]["displayKind"] = 6; }, "Unsupported resource display conversion");
    rejects([](Json& data) {
        auto image = data["resources"][0];
        image["width"] = image["height"] = 16384; image["format"] = "RGBA16F";
        image["bytes"] = 16384ull * 16384 * 8; data["resources"] = Json::array({image});
    }, "Image payload budget overflow");
    rejects([](Json& data) {
        auto image = data["resources"][0]; image["width"] = image["height"] = 8192;
        image["bytes"] = 8192ull * 8192 * 4; data["resources"] = Json::array({image, image});
    }, "Aggregate image budget overflow");
    rejects([](Json& data) { data["resources"] = std::vector<Json>(4097, Json::object()); }, "Resource element budget");
    rejects([](Json& data) { data["events"] = std::vector<Json>(65537, Json::object()); }, "Draw event element budget");
    rejects([](Json& data) { data["passes"] = std::vector<Json>(129, Json::object()); }, "Pass element budget");
    rejects([](Json& data) { data["events"][0]["baseVertex"] = 2147483648ull; }, "Signed draw range overflow");
    rejects([](Json& data) { data["events"][0]["indexCount"] = 4294967296ull; }, "Unsigned draw range overflow");
    rejects([](Json& data) { data["events"][0]["viewport"][0] = 1e300; }, "Viewport float range overflow");
    rejects([](Json& data) { data["events"][0]["viewport"] = {1, 2}; }, "Incomplete viewport");
    rejects([](Json& data) { data["events"][0]["constants"][0]["bytes"] = "f"; }, "Odd-length constant hex");
    rejects([](Json& data) { data["events"][0]["constants"][0]["bytes"] = "xx"; }, "Non-hex constant data");
    rejects([](Json& data) { data["events"][0]["constants"][0]["bytes"] = {1, 2}; }, "Integer-array constant data");
    rejects([](Json& data) { data["cpuTrace"]["events"] = std::vector<Json>(65537, Json::object()); }, "CPU trace element budget");
    rejects([](Json& data) { data["cpuTrace"]["eventCapacity"] = 2; }, "Trace count beyond capacity");
    rejects([](Json& data) { data["cpuTrace"]["events"][0]["durationUs"] = -1; }, "Negative trace duration");
    rejects([](Json& data) { data["cpuTrace"]["events"][0]["parentEventId"] = "10"; }, "Non-integer trace parent");
    rejects([](Json& data) { data["materialShaderSource"] = std::string(2ull * 1024 * 1024 + 1, 'x'); }, "Shader source budget");

    auto nonfinite = fixture();
    std::cerr << "[metadata] encoder non-finite rejection\n";
    nonfinite.cpuSubmitMs = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try { (void)encodeCaptureMetadata(nonfinite); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "The encoder must reject NaN rather than emit null.");
    auto excessConstants = fixture();
    std::cerr << "[metadata] encoder constant budget rejection\n";
    excessConstants.events[0].constants[0].bytes.resize(8ull * 1024 * 1024 + 1);
    rejected = false;
    try { (void)encodeCaptureMetadata(excessConstants); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "The encoder must enforce the raw constant budget.");
}
} // namespace

int main() {
    try {
        roundtrip(); malformedAndBudgets();
        std::cout << "Frame capture metadata tests passed (" << checks << " checks).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Frame capture metadata tests failed: " << error.what() << '\n';
        return 1;
    }
}
