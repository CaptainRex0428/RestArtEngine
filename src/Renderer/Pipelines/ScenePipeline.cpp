#include <RestArtEngine/Renderer/RenderPipeline.h>

#include <array>
#include <cstddef>
#include <utility>

namespace rae::renderer {
namespace {

struct ResourceName {
    PipelineResource resource;
    const char* name;
};

constexpr std::array<ResourceName, 12> resourceNames{{
    {PipelineResource::SceneColor, "SceneColor"},
    {PipelineResource::SceneDepth, "SceneDepth"},
    {PipelineResource::ShadowMap, "ShadowMap"},
    {PipelineResource::GBufferAlbedo, "GBufferAlbedo"},
    {PipelineResource::GBufferNormal, "GBufferNormal"},
    {PipelineResource::GBufferPosition, "GBufferPosition"},
    {PipelineResource::RayTriangleScene, "RayTriangleScene"},
    {PipelineResource::IndirectLighting, "IndirectLighting"},
    {PipelineResource::HardwareImage, "HardwareImage"},
    {PipelineResource::SelectionMask, "SelectionMask"},
    {PipelineResource::DepthOfFieldSource, "DepthOfFieldSource"},
    {PipelineResource::FrameCapture, "FrameCapture"},
}};

bool isForwardOnly(RenderPassKind kind) noexcept {
    return kind == RenderPassKind::ForwardBackground || kind == RenderPassKind::ForwardGeometry;
}

bool isDeferredOnly(RenderPassKind kind) noexcept {
    return kind == RenderPassKind::GBuffer || kind == RenderPassKind::DeferredLighting;
}

} // namespace

std::string describePipelineResources(PipelineResource resources) {
    if (resources == PipelineResource::None) return "none";
    std::string result;
    for (const auto& entry : resourceNames) {
        if (!contains(resources, entry.resource)) continue;
        if (!result.empty()) result += " | ";
        result += entry.name;
    }
    return result.empty() ? "none" : result;
}

const char* toString(RenderPath path) noexcept {
    return path == RenderPath::Deferred ? "Deferred" : "Forward";
}

const char* toString(RenderPassKind kind) noexcept {
    switch (kind) {
    case RenderPassKind::ShadowMap: return "Shadow map";
    case RenderPassKind::HardwareTransport: return "Hardware transport";
    case RenderPassKind::GBuffer: return "G-Buffer";
    case RenderPassKind::DeferredLighting: return "Deferred lighting";
    case RenderPassKind::ForwardBackground: return "Forward background";
    case RenderPassKind::ForwardGeometry: return "Forward geometry";
    case RenderPassKind::HardwareResolve: return "Hardware resolve";
    case RenderPassKind::DepthOfField: return "Depth of field";
    case RenderPassKind::SelectionMask: return "Selection mask";
    case RenderPassKind::SelectionComposite: return "Selection composite";
    case RenderPassKind::FrameCapture: return "Frame capture";
    }
    return "Unknown";
}

RenderPipeline buildScenePipeline(const PipelineBuildInputs& inputs) {
    RenderPipeline pipeline;
    pipeline.path = inputs.path;
    const auto add = [&pipeline](RenderPassKind kind, std::string_view name,
                                 PipelineResource reads, PipelineResource writes) {
        pipeline.passes.push_back({kind, name, reads, writes});
    };

    // Resources consumed by whichever direct-lighting stage runs. Analytic
    // reflections and visibility rays read the prepared triangle scene; the
    // raster shadow map is only a real dependency when the map pass ran.
    PipelineResource directLightingReads = PipelineResource::None;
    if (inputs.rayTriangleScene) directLightingReads |= PipelineResource::RayTriangleScene;
    if (inputs.rasterShadowMap) directLightingReads |= PipelineResource::ShadowMap;
    if (inputs.realtimeIndirect) directLightingReads |= PipelineResource::IndirectLighting;

    if (inputs.rasterShadowMap)
        add(RenderPassKind::ShadowMap, "Shadow map",
            PipelineResource::None, PipelineResource::ShadowMap);

    if (inputs.hardwareTransport) {
        PipelineResource writes = PipelineResource::HardwareImage;
        if (inputs.realtimeIndirect) writes |= PipelineResource::IndirectLighting;
        add(RenderPassKind::HardwareTransport, "Hardware ray tracing transport",
            PipelineResource::None, writes);
    }

    if (inputs.path == RenderPath::Deferred) {
        add(RenderPassKind::GBuffer, "GBuffer",
            PipelineResource::None,
            PipelineResource::GBufferAlbedo | PipelineResource::GBufferNormal |
                PipelineResource::GBufferPosition | PipelineResource::SceneDepth);
        add(RenderPassKind::DeferredLighting, "Deferred lighting / debug",
            PipelineResource::GBufferAlbedo | PipelineResource::GBufferNormal |
                PipelineResource::GBufferPosition | directLightingReads,
            PipelineResource::SceneColor);
    } else {
        // The background writes the first colour; geometry draws on top of it,
        // so SceneColor is both an input and an output of the geometry pass.
        add(RenderPassKind::ForwardBackground, "Forward background",
            PipelineResource::None, PipelineResource::SceneColor);
        add(RenderPassKind::ForwardGeometry, "Forward geometry / debug",
            directLightingReads | PipelineResource::SceneColor,
            PipelineResource::SceneColor | PipelineResource::SceneDepth);
    }

    // A resolved hardware image replaces the raster colour after lighting.
    if (inputs.hardwareTransport)
        add(RenderPassKind::HardwareResolve, "Hardware image resolve",
            PipelineResource::HardwareImage, PipelineResource::SceneColor);

    if (inputs.depthOfField)
        add(RenderPassKind::DepthOfField, "Screen-space depth of field",
            PipelineResource::SceneColor | PipelineResource::SceneDepth,
            PipelineResource::SceneColor | PipelineResource::DepthOfFieldSource);

    if (inputs.selectionOutline) {
        add(RenderPassKind::SelectionMask, "Selection masks",
            PipelineResource::SceneDepth, PipelineResource::SelectionMask);
        add(RenderPassKind::SelectionComposite, "Selection composite",
            PipelineResource::SceneColor | PipelineResource::SceneDepth |
                PipelineResource::SelectionMask,
            PipelineResource::SceneColor);
    }

    if (inputs.frameCapture) {
        PipelineResource reads = PipelineResource::SceneColor | PipelineResource::SceneDepth;
        if (inputs.path == RenderPath::Deferred)
            reads |= PipelineResource::GBufferAlbedo | PipelineResource::GBufferNormal |
                PipelineResource::GBufferPosition;
        add(RenderPassKind::FrameCapture, "Frame capture", reads, PipelineResource::FrameCapture);
    }
    return pipeline;
}

PipelineValidation validatePipeline(const RenderPipeline& pipeline) {
    PipelineValidation result;
    if (pipeline.passes.empty()) {
        result.valid = false;
        result.error = "the pipeline declares no passes.";
        return result;
    }

    // Frame inputs exist before the pass list starts.
    PipelineResource available = pipelineFrameInputs;
    std::vector<std::string_view> names;
    bool hasGBuffer = false, hasDeferredLighting = false;
    bool hasForwardBackground = false, hasForwardGeometry = false;
    names.reserve(pipeline.passes.size());
    for (std::size_t index = 0; index < pipeline.passes.size(); ++index) {
        const auto& pass = pipeline.passes[index];
        const std::string prefix = "pass " + std::to_string(index + 1) + " ('" +
            std::string(pass.name) + "') ";
        if (pass.name.empty()) {
            result.valid = false;
            result.error = prefix + "has no diagnostic name.";
            return result;
        }
        for (const auto existing : names) {
            if (existing == pass.name) {
                result.valid = false;
                result.error = prefix + "reuses the pass name '" + std::string(pass.name) + "'.";
                return result;
            }
        }
        names.push_back(pass.name);
        if (pass.writes == PipelineResource::None) {
            result.valid = false;
            result.error = prefix + "writes no GPU resource.";
            return result;
        }
        const auto missing = static_cast<std::uint64_t>(pass.reads) &
            ~static_cast<std::uint64_t>(available);
        if (missing != 0) {
            result.valid = false;
            result.error = prefix + "reads " +
                describePipelineResources(static_cast<PipelineResource>(missing)) +
                " before any earlier pass produces it.";
            return result;
        }
        if (isForwardOnly(pass.kind)) {
            if (pipeline.path == RenderPath::Deferred) {
                result.valid = false;
                result.error = prefix + "is a Forward pass inside a Deferred pipeline.";
                return result;
            }
        }
        if (isDeferredOnly(pass.kind) && pipeline.path == RenderPath::Forward) {
            result.valid = false;
            result.error = prefix + "is a Deferred pass inside a Forward pipeline.";
            return result;
        }
        switch (pass.kind) {
        case RenderPassKind::GBuffer: hasGBuffer = true; break;
        case RenderPassKind::DeferredLighting: hasDeferredLighting = true; break;
        case RenderPassKind::ForwardBackground: hasForwardBackground = true; break;
        case RenderPassKind::ForwardGeometry: hasForwardGeometry = true; break;
        default: break;
        }
        available |= pass.writes;
    }

    if (!contains(available, PipelineResource::SceneColor)) {
        result.valid = false;
        result.error = "no pass produces SceneColor, the image the caller presents.";
        return result;
    }
    if (pipeline.path == RenderPath::Deferred) {
        if (!hasGBuffer) {
            result.valid = false;
            result.error = "the Deferred pipeline declares no G-Buffer pass.";
        } else if (!hasDeferredLighting) {
            result.valid = false;
            result.error = "the Deferred pipeline declares no deferred lighting pass.";
        }
    } else if (!hasForwardBackground) {
        result.valid = false;
        result.error = "the Forward pipeline declares no background pass.";
    } else if (!hasForwardGeometry) {
        result.valid = false;
        result.error = "the Forward pipeline declares no geometry pass.";
    }
    return result;
}

std::string describePipeline(const RenderPipeline& pipeline) {
    std::string result = std::string("[") + toString(pipeline.path) + "] " +
        std::to_string(pipeline.passes.size()) + " passes\n";
    for (std::size_t index = 0; index < pipeline.passes.size(); ++index) {
        const auto& pass = pipeline.passes[index];
        result += "  " + std::to_string(index + 1) + ". " + std::string(pass.name) +
            " | reads: " + describePipelineResources(pass.reads) +
            " | writes: " + describePipelineResources(pass.writes) + "\n";
    }
    return result;
}

} // namespace rae::renderer
