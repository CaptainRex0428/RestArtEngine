#include "RestArtEngine/Renderer/RenderFeatures.h"
#include "RestArtEngine/Renderer/RenderPipeline.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace rae::renderer;
unsigned checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

PipelineBuildInputs inputs(RenderPath path) {
    PipelineBuildInputs result;
    result.path = path;
    result.rasterShadowMap = true;
    return result;
}

const RenderPassDeclaration& passAt(const RenderPipeline& pipeline, std::size_t index) {
    require(index < pipeline.passes.size(), "Pass index out of range.");
    return pipeline.passes[index];
}

bool hasKind(const RenderPipeline& pipeline, RenderPassKind kind) {
    for (const auto& pass : pipeline.passes)
        if (pass.kind == kind) return true;
    return false;
}

std::size_t indexOf(const RenderPipeline& pipeline, RenderPassKind kind) {
    for (std::size_t index = 0; index < pipeline.passes.size(); ++index)
        if (pipeline.passes[index].kind == kind) return index;
    return pipeline.passes.size();
}

void deferredBaseline() {
    auto build = inputs(RenderPath::Deferred);
    const auto pipeline = buildScenePipeline(build);
    require(pipeline.passes.size() == 3, "A plain Deferred frame is three passes.");
    require(passAt(pipeline, 0).kind == RenderPassKind::ShadowMap, "Shadow map must run first.");
    require(passAt(pipeline, 1).kind == RenderPassKind::GBuffer, "G-Buffer must follow the shadow map.");
    require(passAt(pipeline, 2).kind == RenderPassKind::DeferredLighting, "Lighting must follow the G-Buffer.");
    require(passAt(pipeline, 2).name == "Deferred lighting / debug", "Deferred lighting pass label changed.");
    require(contains(passAt(pipeline, 2).reads, PipelineResource::GBufferAlbedo) &&
        contains(passAt(pipeline, 2).reads, PipelineResource::GBufferNormal) &&
        contains(passAt(pipeline, 2).reads, PipelineResource::GBufferPosition),
        "Deferred lighting must declare the three G-buffer inputs.");
    require(contains(passAt(pipeline, 2).writes, PipelineResource::SceneColor),
        "Deferred lighting produces SceneColor.");
    require(contains(passAt(pipeline, 1).writes, PipelineResource::SceneDepth),
        "G-Buffer must declare SceneDepth as its output.");
    require(contains(passAt(pipeline, 2).reads, PipelineResource::ShadowMap),
        "With the raster map enabled, lighting consumes it.");
    const auto validation = validatePipeline(pipeline);
    require(validation.valid, "Deferred baseline rejected: " + validation.error);
    const auto text = describePipeline(pipeline);
    require(text.find("[Deferred] 3 passes") == 0, "Pipeline description does not name the path and count.");
    require(text.find("GBuffer") != std::string::npos &&
        text.find("Deferred lighting / debug") != std::string::npos,
        "Pipeline description is missing pass labels.");
}

void forwardBaseline() {
    auto build = inputs(RenderPath::Forward);
    const auto pipeline = buildScenePipeline(build);
    require(pipeline.passes.size() == 3, "A plain Forward frame is three passes.");
    require(passAt(pipeline, 1).kind == RenderPassKind::ForwardBackground, "Forward draws the background first.");
    require(passAt(pipeline, 2).kind == RenderPassKind::ForwardGeometry, "Forward geometry follows the background.");
    require(contains(passAt(pipeline, 2).reads, PipelineResource::SceneColor),
        "Forward geometry must read the background colour it composites onto.");
    require(contains(passAt(pipeline, 2).writes, PipelineResource::SceneDepth),
        "Forward geometry owns the scene depth.");
    const auto validation = validatePipeline(pipeline);
    require(validation.valid, "Forward baseline rejected: " + validation.error);
}

void disabledShadowMap() {
    auto build = inputs(RenderPath::Deferred);
    build.rasterShadowMap = false;
    const auto pipeline = buildScenePipeline(build);
    require(!hasKind(pipeline, RenderPassKind::ShadowMap), "No shadow map pass may be declared when it does not run.");
    require(!contains(passAt(pipeline, 1).reads, PipelineResource::ShadowMap),
        "Lighting must not declare a shadow map it cannot read.");
    require(validatePipeline(pipeline).valid, "Pipeline without a shadow map is inconsistent.");
}

void analyticRayInputs() {
    auto build = inputs(RenderPath::Deferred);
    build.rayTriangleScene = true;
    const auto pipeline = buildScenePipeline(build);
    require(contains(passAt(pipeline, 2).reads, PipelineResource::RayTriangleScene),
        "Analytic ray effects must declare the triangle scene input.");
    require(validatePipeline(pipeline).valid, "Ray pipeline is inconsistent.");
}

void hardwareTransport() {
    auto build = inputs(RenderPath::Deferred);
    build.hardwareTransport = true;
    build.realtimeIndirect = true;
    const auto pipeline = buildScenePipeline(build);
    const auto transport = indexOf(pipeline, RenderPassKind::HardwareTransport);
    require(transport < pipeline.passes.size(), "Hardware transport pass missing.");
    require(transport < indexOf(pipeline, RenderPassKind::GBuffer), "Transport must precede raster shading.");
    require(contains(passAt(pipeline, transport).writes, PipelineResource::IndirectLighting),
        "Realtime transport must declare the indirect image it publishes.");
    require(contains(passAt(pipeline, transport + 2).reads, PipelineResource::IndirectLighting),
        "Raster lighting must declare the indirect image it consumes.");
    require(hasKind(pipeline, RenderPassKind::HardwareResolve), "Hardware resolve pass missing.");
    require(validatePipeline(pipeline).valid, "Hardware pipeline is inconsistent.");
}

void postChain() {
    auto build = inputs(RenderPath::Deferred);
    build.depthOfField = true;
    build.selectionOutline = true;
    build.frameCapture = true;
    const auto pipeline = buildScenePipeline(build);
    const auto dof = indexOf(pipeline, RenderPassKind::DepthOfField);
    const auto mask = indexOf(pipeline, RenderPassKind::SelectionMask);
    const auto composite = indexOf(pipeline, RenderPassKind::SelectionComposite);
    const auto capture = indexOf(pipeline, RenderPassKind::FrameCapture);
    require(dof < mask && mask < composite && composite < capture,
        "Post-processing order must be depth of field, outline, then capture.");
    require(contains(passAt(pipeline, mask).writes, PipelineResource::SelectionMask),
        "The mask pass produces the outline mask.");
    require(contains(passAt(pipeline, composite).reads, PipelineResource::SelectionMask),
        "The composite pass consumes the outline mask.");
    require(validatePipeline(pipeline).valid, "Post-processing pipeline is inconsistent.");
}

void rejectsBrokenDeclarations() {
    {
        RenderPipeline pipeline;
        pipeline.path = RenderPath::Deferred;
        pipeline.passes.push_back({RenderPassKind::DeferredLighting, "Lighting",
            PipelineResource::GBufferAlbedo | PipelineResource::GBufferNormal |
                PipelineResource::GBufferPosition,
            PipelineResource::SceneColor});
        pipeline.passes.push_back({RenderPassKind::GBuffer, "GBuffer",
            PipelineResource::None,
            PipelineResource::GBufferAlbedo | PipelineResource::GBufferNormal |
                PipelineResource::GBufferPosition});
        const auto validation = validatePipeline(pipeline);
        require(!validation.valid, "A consumer before its producer must be rejected.");
        require(validation.error.find("reads") != std::string::npos &&
            validation.error.find("GBufferAlbedo") != std::string::npos,
            "Rejection must name the missing resource: " + validation.error);
    }
    {
        auto build = inputs(RenderPath::Deferred);
        auto pipeline = buildScenePipeline(build);
        pipeline.passes[2].name = "GBuffer";
        const auto validation = validatePipeline(pipeline);
        require(!validation.valid && validation.error.find("reuses") != std::string::npos,
            "Duplicate pass names must be rejected.");
    }
    {
        auto build = inputs(RenderPath::Deferred);
        auto pipeline = buildScenePipeline(build);
        pipeline.passes.push_back({RenderPassKind::ForwardGeometry, "Forward geometry",
            PipelineResource::SceneColor, PipelineResource::SceneColor});
        const auto validation = validatePipeline(pipeline);
        require(!validation.valid && validation.error.find("Forward pass") != std::string::npos,
            "A Forward pass inside a Deferred pipeline must be rejected.");
    }
    {
        RenderPipeline pipeline;
        pipeline.path = RenderPath::Forward;
        pipeline.passes.push_back({RenderPassKind::ForwardGeometry, "Forward geometry",
            PipelineResource::None, PipelineResource::SceneColor});
        const auto validation = validatePipeline(pipeline);
        require(!validation.valid, "A Forward pipeline without a background pass must be rejected.");
    }
}

FeatureRequests featureRequests(RenderPath path = RenderPath::Deferred) {
    FeatureRequests requests;
    requests.path = path;
    return requests;
}

FeatureCapabilities capableDevice() {
    FeatureCapabilities capabilities;
    capabilities.shaderPipelineReady = true;
    capabilities.hardwareRayTracingBuilt = true;
    capabilities.hardwareRayTracingDevice = true;
    capabilities.hardwarePipelineReady = true;
    capabilities.hardwareDepthCompatible = true;
    return capabilities;
}

void pathAndShadowFeatures() {
    auto requests = featureRequests();
    requests.shadowMethod = ShadowMethod::ShadowMapPCSS;
    const auto matrix = evaluateRenderFeatures(requests, FeatureCapabilities{});
    require(matrix.isActive(RenderFeature::DeferredPath), "Deferred path must be active.");
    require(!matrix.isActive(RenderFeature::ForwardPath), "Forward path must be off for a Deferred frame.");
    require(matrix.isActive(RenderFeature::ShadowMapPCSS), "PCSS must be active.");
    require(!matrix.isActive(RenderFeature::ShadowMapPCF), "Only one raster variant runs.");
    require(matrix.effectiveCount() == 2, "Deferred + PCSS is exactly two effective features.");

    auto forward = featureRequests(RenderPath::Forward);
    forward.shadowMethod = ShadowMethod::GTAO;
    const auto gtao = evaluateRenderFeatures(forward, FeatureCapabilities{});
    require(gtao.isActive(RenderFeature::ShadowMapPCF),
        "GTAO keeps the raster base map in Forward.");
    require(gtao.at(RenderFeature::AmbientOcclusionGTAO).state == FeatureState::BlockedByDependency,
        "GTAO in Forward must report a blocked dependency, not silently run.");
    require(gtao.at(RenderFeature::AmbientOcclusionGTAO).reason.find("Deferred") != std::string::npos,
        "The blocking reason must name the Deferred G-buffer.");

    auto analytic = featureRequests();
    analytic.shadowMethod = ShadowMethod::AnalyticRay;
    analytic.rayTracing = true;
    const auto rays = evaluateRenderFeatures(analytic, FeatureCapabilities{});
    require(rays.isActive(RenderFeature::AnalyticRayTracing), "Analytic rays must be active.");
    require(!rays.isActive(RenderFeature::ShadowMapPCF), "Analytic shadows do not run the raster map.");
}

void hardwareFeatureLayers() {
    auto requests = featureRequests();
    requests.hardwareRayTracing = true;
    requests.globalIllumination = GlobalIlluminationMethod::PathTracing;
    requests.globalIlluminationMode = GlobalIlluminationMode::Progressive;
    auto capabilities = capableDevice();
    capabilities.hardwareRayTracingDevice = false;
    const auto unsupported = evaluateRenderFeatures(requests, capabilities);
    require(unsupported.at(RenderFeature::HardwareRayTracing).state == FeatureState::UnsupportedByDevice,
        "A request on a device without DXR must report the device layer.");
    require(unsupported.at(RenderFeature::HardwareRayTracing).reason.find("DXR") != std::string::npos,
        "The device layer reason must name DXR.");
    require(unsupported.at(RenderFeature::PathTracingGI).state == FeatureState::BlockedByDependency,
        "GI inherits the hardware blocking reason.");

    auto builtOut = capableDevice();
    builtOut.hardwareRayTracingBuilt = false;
    const auto noBuild = evaluateRenderFeatures(requests, builtOut);
    require(noBuild.at(RenderFeature::HardwareRayTracing).state == FeatureState::UnavailableInBuild,
        "Build availability is checked before the device.");

    auto depth = capableDevice();
    depth.hardwareDepthCompatible = false;
    const auto incompatible = evaluateRenderFeatures(requests, depth);
    require(incompatible.at(RenderFeature::HardwareRayTracing).state == FeatureState::BlockedByDependency,
        "A raster-only depth material blocks the hardware frame.");
    require(incompatible.at(RenderFeature::HardwareRayTracing).reason.find("depth state") != std::string::npos,
        "The depth reason must be reported.");

    auto failed = capableDevice();
    failed.hardwareFrameFailed = true;
    const auto runtime = evaluateRenderFeatures(requests, failed);
    require(runtime.at(RenderFeature::HardwareRayTracing).state == FeatureState::BlockedByDependency,
        "A failed dispatch must not be reported as active.");
    require(runtime.at(RenderFeature::HardwareRayTracing).reason.find("failed at runtime") != std::string::npos,
        "The runtime failure reason must be reported.");

    auto active = requests;
    active.globalIllumination = GlobalIlluminationMethod::PathTracing;
    active.globalIlluminationMode = GlobalIlluminationMode::Progressive;
    const auto gi = evaluateRenderFeatures(active, capableDevice());
    require(gi.isActive(RenderFeature::HardwareRayTracing), "Hardware tracing must be active.");
    require(gi.isActive(RenderFeature::PathTracingGI), "Progressive path-traced GI must be active.");
    require(!gi.isActive(RenderFeature::RealtimeGI), "Progressive mode does not run realtime GI.");
    require(gi.at(RenderFeature::HardwareReflections).state == FeatureState::BlockedByDependency,
        "Path-traced GI owns indirect specular; reflections must report the takeover.");
    require(gi.isActive(RenderFeature::HardwareShadows), "Progressive GI still uses hardware shadows.");

    active.globalIlluminationMode = GlobalIlluminationMode::Realtime;
    const auto realtime = evaluateRenderFeatures(active, capableDevice());
    require(realtime.isActive(RenderFeature::RealtimeGI), "Realtime GI must be active.");
    require(realtime.at(RenderFeature::HardwareShadows).state == FeatureState::BlockedByDependency,
        "Realtime GI owns direct shadows; the hardware shadow request is retained but blocked.");
    require(realtime.at(RenderFeature::HardwareShadows).reason.find("Realtime") != std::string::npos,
        "The realtime shadow reason must say who took over.");
}

void postAndEditorFeatures() {
    auto requests = featureRequests();
    requests.depthOfField = true;
    auto capabilities = FeatureCapabilities{};
    capabilities.thinLensDepthOfField = true;
    const auto thinLens = evaluateRenderFeatures(requests, capabilities);
    require(thinLens.at(RenderFeature::DepthOfField).state == FeatureState::Degraded,
        "Thin-lens path tracing must report screen-space depth of field as degraded.");

    requests.selectionOutline = true;
    const auto noSelection = evaluateRenderFeatures(requests, FeatureCapabilities{});
    require(noSelection.at(RenderFeature::SelectionOutline).state == FeatureState::BlockedByDependency,
        "An outline request without editor state must be reported as blocked.");
    const auto summary = noSelection.summary();
    require(!summary.empty() && summary.find("Selection outline") != std::string::npos,
        "The summary must list non-off features with their state.");
}

} // namespace

int main() {
    try {
        deferredBaseline();
        forwardBaseline();
        disabledShadowMap();
        analyticRayInputs();
        hardwareTransport();
        postChain();
        rejectsBrokenDeclarations();
        pathAndShadowFeatures();
        hardwareFeatureLayers();
        postAndEditorFeatures();
        std::cout << "RenderPipelineTests: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "RenderPipelineTests failed: " << failure.what() << '\n';
        return 1;
    }
}
