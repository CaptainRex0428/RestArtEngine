#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <RestArtEngine/Core/TraceProfiler.h>

namespace rae::renderer {

enum class DebugTextureChannel { RGB, R, G, B, A };
enum class GpuTimingStatus { Unavailable, Pending, Ready, Disjoint };

struct RenderPassTiming {
    std::string name;
    double cpuMs = 0.0;
    double gpuMs = 0.0; // Valid only in a Ready GPU sample.
    std::uint64_t drawCalls = 0;
    double cpuStartMs = 0.0;
    std::uint64_t firstEventId = 0, lastEventId = 0;
    std::vector<std::string> reads, writes;
};

struct RenderResourceBinding {
    std::string stage, kind, name, format;
    std::uint32_t slot = 0, width = 0, height = 0, layer = 0, stride = 0, offset = 0;
    std::uint64_t bytes = 0;
};
struct RenderConstantBinding {
    std::string stage, name;
    std::uint32_t slot = 0;
    std::uint64_t version = 0;
    std::vector<std::uint8_t> bytes;
};

struct RenderDrawEvent {
    std::string pass;
    std::uint64_t actorId = 0, meshId = 0, materialId = 0;
    std::uint32_t indexCount = 0, vertexCount = 0;
    std::uint64_t shaderRevision = 0;
    std::uint64_t eventId = 0;
    std::uint32_t passIndex = 0, firstIndex = 0, materialSlot = 0;
    std::int32_t baseVertex = 0;
    std::string command, actorName, meshName, materialName, vertexShader, pixelShader;
    std::string pipelineState;
    std::array<float, 6> viewport{};
    std::vector<RenderResourceBinding> bindings;
    std::vector<RenderConstantBinding> constants;
    bool detailedState = false;
};

struct RenderDiagnosticsFrame {
    std::uint64_t frameId = 0;
    double cpuSubmitMs = 0.0;
    std::uint64_t drawCalls = 0, triangles = 0;
    std::vector<RenderPassTiming> passes;
    std::vector<RenderDrawEvent> events;
    // GPU results are asynchronous and explicitly identify their own frame.
    std::uint64_t gpuFrameId = 0;
    GpuTimingStatus gpuStatus = GpuTimingStatus::Unavailable;
    double gpuTotalMs = 0.0;
    std::vector<RenderPassTiming> gpuPasses;
    std::uint64_t droppedEvents = 0;
};

struct CapturedRenderResource {
    std::string name, format;
    std::uint32_t width = 0, height = 0;
    std::uint64_t bytes = 0;
    std::string pass, boundary = "Final";
    std::uint32_t layer = 0;
    std::uint32_t displayKind = 0;
    std::uint64_t eventId = 0;
};

struct FrameCaptureOptions {
    // Empty selects final outputs. A named logical pass captures only that
    // pass's produced attachments. Before images exist only where initialized.
    std::string pass;
    bool beforeAfter = false;
    std::uint64_t budgetBytes = 256ull * 1024 * 1024;
};

struct RenderFrameCapture {
    bool valid = false;
    std::uint64_t frameId = 0, shaderRevision = 0;
    std::string renderPath;
    double cpuSubmitMs = 0.0;
    std::vector<CapturedRenderResource> resources;
    std::vector<RenderDrawEvent> events;
    std::string status = "No frame captured.";
    bool metadataOnlyExport = true;
    std::string backend;
    std::uint32_t width = 0, height = 0;
    float nearPlane = .05f, farPlane = 100.f;
    bool orthographic = false, offline = false;
    std::uint64_t gpuFrameId = 0;
    GpuTimingStatus gpuStatus = GpuTimingStatus::Unavailable;
    double gpuTotalMs = 0;
    std::vector<RenderPassTiming> gpuPasses;
    std::vector<RenderPassTiming> passes;
    std::string materialShaderSource, pipelineShaderSource;
    rae::diagnostics::TraceSnapshot cpuTrace;
    std::uint64_t droppedEvents = 0;
};

} // namespace rae::renderer
