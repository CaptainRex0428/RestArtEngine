#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rae::renderer {

enum class DebugTextureChannel { RGB, R, G, B, A };
enum class GpuTimingStatus { Unavailable, Pending, Ready, Disjoint };

struct RenderPassTiming {
    std::string name;
    double cpuMs = 0.0;
    double gpuMs = 0.0; // Valid only in a Ready GPU sample.
    std::uint64_t drawCalls = 0;
};

struct RenderDrawEvent {
    std::string pass;
    std::uint64_t actorId = 0, meshId = 0, materialId = 0;
    std::uint32_t indexCount = 0, vertexCount = 0;
    std::uint64_t shaderRevision = 0;
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
};

struct CapturedRenderResource {
    std::string name, format;
    std::uint32_t width = 0, height = 0;
    std::uint64_t bytes = 0;
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
};

} // namespace rae::renderer
