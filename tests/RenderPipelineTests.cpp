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

void screenSpaceGiInputs() {
    auto build = inputs(RenderPath::Deferred);
    build.screenSpaceGi = true;
    const auto pipeline = buildScenePipeline(build);
    const auto trace = indexOf(pipeline, RenderPassKind::ScreenSpaceGi);
    const auto composite = indexOf(pipeline, RenderPassKind::ScreenSpaceGiComposite);
    require(trace < pipeline.passes.size() && composite < pipeline.passes.size(),
        "SSGI trace/composite passes are missing.");
    require(contains(passAt(pipeline, trace).reads, PipelineResource::ScreenSpaceDirectHistory),
        "SSGI trace must declare previous direct-colour history.");
    require(contains(passAt(pipeline, trace).writes, PipelineResource::ScreenSpaceRawIndirect),
        "SSGI trace must publish raw indirect.");
    require(contains(passAt(pipeline, composite).reads, PipelineResource::ScreenSpaceRawIndirect) &&
        contains(passAt(pipeline, composite).reads, PipelineResource::ScreenSpaceHistory),
        "SSGI composite must consume raw indirect and denoised history.");
    require(contains(passAt(pipeline, composite).writes, PipelineResource::ScreenSpaceHistory),
        "SSGI composite must publish the next indirect history.");
    require(validatePipeline(pipeline).valid, "SSGI pipeline is inconsistent.");
}

void probeGridInputs() {
    auto build = inputs(RenderPath::Deferred);
    build.probeGridGi = true;
    const auto pipeline = buildScenePipeline(build);
    require(contains(passAt(pipeline, 2).reads, PipelineResource::ProbeAtlas),
        "Deferred lighting must consume the DDGI probe atlas.");
    const auto update = indexOf(pipeline, RenderPassKind::ProbeUpdate);
    require(update < pipeline.passes.size(), "DDGI probe update pass missing.");
    require(contains(passAt(pipeline, update).writes, PipelineResource::ProbeAtlas),
        "DDGI update must publish the probe atlas.");
    require(validatePipeline(pipeline).valid, "DDGI pipeline is inconsistent.");
    build.giDebugProbes = true;
    const auto debugPipeline = buildScenePipeline(build);
    const auto overlay = indexOf(debugPipeline, RenderPassKind::GiDebugOverlay);
    require(overlay < debugPipeline.passes.size(), "GI probe debug overlay pass missing.");
    require(contains(passAt(debugPipeline, overlay).reads, PipelineResource::ProbeAtlas) &&
        contains(passAt(debugPipeline, overlay).writes, PipelineResource::SceneColor),
        "GI probe debug overlay must sample the atlas and draw into the scene colour.");
    require(validatePipeline(debugPipeline).valid, "GI probe debug pipeline is inconsistent.");
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

void antiAliasingChain() {
    for (auto path : {RenderPath::Forward,RenderPath::Deferred}) {
        for (bool temporal : {false,true}) {
            auto build=inputs(path);
            build.depthOfField=true; build.antiAliasing=true; build.temporalAntiAliasing=temporal;
            build.selectionOutline=true; build.frameCapture=true;
            const auto pipeline=buildScenePipeline(build);
            require(validatePipeline(pipeline).valid,"AA pipeline has an unproduced dependency.");
            const auto aa=indexOf(pipeline,RenderPassKind::AntiAliasing);
            require(indexOf(pipeline,RenderPassKind::DepthOfField)<aa &&
                aa<indexOf(pipeline,RenderPassKind::SelectionMask),"AA must run after DOF and before editor overlays.");
            require(contains(passAt(pipeline,aa).reads,PipelineResource::AntiAliasingSource),
                "AA must read a distinct source image to avoid read/write aliasing.");
            if (temporal) {
                require(contains(passAt(pipeline,aa).reads,PipelineResource::PreviousAntiAliasingHistory),
                    "TAA needs its previous-frame dependency.");
                const auto present=indexOf(pipeline,RenderPassKind::AntiAliasingPresent);
                require(aa<present && present<indexOf(pipeline,RenderPassKind::SelectionMask),
                    "TAA must publish the new history before overlays.");
                require(contains(passAt(pipeline,present).writes,PipelineResource::SceneColor),
                    "TAA presentation must produce final SceneColor.");
            } else require(!hasKind(pipeline,RenderPassKind::AntiAliasingPresent),"FXAA writes final scene directly.");
        }
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

    auto strictBackend = capableDevice();
    strictBackend.hardwareRayTracingBackendAllowed = false;
    strictBackend.hardwareRayTracingDevice = false;
    const auto blockedBackend = evaluateRenderFeatures(requests, strictBackend);
    require(blockedBackend.at(RenderFeature::HardwareRayTracing).state == FeatureState::UnsupportedByDevice,
        "Strict D3D11 blocks the backend layer, not the compiled module.");
    require(blockedBackend.at(RenderFeature::HardwareRayTracing).reason.find("strict D3D11") != std::string::npos,
        "Backend blocking must not be misreported as missing DXR hardware.");
    require(blockedBackend.at(RenderFeature::PathTracingGI).reason.find("strict D3D11") != std::string::npos,
        "GI must explain the backend constraint while retaining the request.");

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

void nativeBackendFeatureAvailability() {
    auto requests = featureRequests();
    requests.rayTracing = true;
    requests.depthOfField = true;
    requests.selectionOutline = true;
    requests.hardwareRayTracing = true;
    requests.globalIllumination = GlobalIlluminationMethod::PathTracing;
    requests.globalIlluminationMode = GlobalIlluminationMode::Realtime;
    requests.shadowMethod = ShadowMethod::ShadowMapPCSS;
    auto native = capableDevice();
    native.nativeD3D12 = true;
    native.analyticRayTracingAvailable = false;
    native.depthOfFieldAvailable = false;
    native.selectionOutlineAvailable = false;
    native.editorSelection = true;
    native.hardwareRayTracingBackendAllowed = false;
    const auto matrix = evaluateRenderFeatures(requests, native);
    for (const auto feature : {RenderFeature::AnalyticRayTracing, RenderFeature::DepthOfField,
            RenderFeature::SelectionOutline}) {
        require(matrix.at(feature).state == FeatureState::UnavailableInBuild,
            "A feature not ported to the active native backend cannot be reported as running.");
        require(matrix.at(feature).reason.find("active backend") != std::string::npos &&
            matrix.at(feature).reason.find("retained") != std::string::npos,
            "Missing backend implementations must explain that the request is retained.");
    }
    require(matrix.at(RenderFeature::HardwareRayTracing).state == FeatureState::UnavailableInBuild,
        "The missing native DXR implementation is a build limitation, not missing D3D12 hardware.");
    for (const auto feature : {RenderFeature::HardwareRayTracing, RenderFeature::PathTracingGI,
            RenderFeature::RealtimeGI}) {
        require(matrix.at(feature).reason.find("Native D3D12") != std::string::npos &&
            matrix.at(feature).reason.find("strict D3D11") == std::string::npos,
            "Native D3D12 feature diagnostics must not claim that strict D3D11 was selected.");
    }
    require(matrix.at(RenderFeature::PathTracingGI).state == FeatureState::BlockedByDependency &&
        matrix.at(RenderFeature::RealtimeGI).state == FeatureState::BlockedByDependency,
        "Requested GI remains visible but blocked on the absent native DXR implementation.");
    require(matrix.isActive(RenderFeature::DeferredPath) && matrix.isActive(RenderFeature::ShadowMapPCSS),
        "Native backend limitations do not disable its shared raster path or shadows.");
    for (const auto method : {ShadowMethod::GTAO, ShadowMethod::ContactShadow}) {
        auto screen = requests;
        screen.shadowMethod = method;
        const auto supported = evaluateRenderFeatures(screen, native);
        require(supported.isActive(RenderFeature::ShadowMapPCF) && supported.isActive(
            method == ShadowMethod::GTAO ? RenderFeature::AmbientOcclusionGTAO : RenderFeature::ContactShadows),
            "Shared Deferred GTAO and contact shadows remain available in native D3D12.");
    }

    auto builtOut = native;
    builtOut.hardwareRayTracingBuilt = false;
    const auto noNativeDxr = evaluateRenderFeatures(requests, builtOut);
    require(noNativeDxr.at(RenderFeature::HardwareRayTracing).state == FeatureState::UnavailableInBuild &&
        noNativeDxr.at(RenderFeature::HardwareRayTracing).reason.find("Native D3D12") != std::string::npos,
        "Missing native DXR must not say that D3D12 itself is absent from the build.");

    // The same retained requests become usable when returning to a backend
    // that implements them; capability evaluation must never clear preferences.
    auto compatible = capableDevice();
    compatible.editorSelection = true;
    const auto restored = evaluateRenderFeatures(requests, compatible);
    for (const auto feature : {RenderFeature::AnalyticRayTracing, RenderFeature::DepthOfField,
            RenderFeature::SelectionOutline, RenderFeature::HardwareRayTracing, RenderFeature::RealtimeGI})
        require(restored.isActive(feature), "Switching to a capable backend restores the retained feature request.");

    auto unavailableThinLens = native;
    unavailableThinLens.thinLensDepthOfField = true;
    require(evaluateRenderFeatures(requests, unavailableThinLens).at(RenderFeature::DepthOfField).state ==
        FeatureState::UnavailableInBuild,
        "Missing backend DOF cannot be hidden by a stale thin-lens capability.");
    unavailableThinLens.editorSelection = false;
    require(evaluateRenderFeatures(requests, unavailableThinLens).at(RenderFeature::SelectionOutline).state ==
        FeatureState::UnavailableInBuild,
        "The missing outline implementation is reported before frame-specific editor prerequisites.");

    auto shadowOnly = featureRequests();
    shadowOnly.shadowMethod = ShadowMethod::AnalyticRay;
    require(evaluateRenderFeatures(shadowOnly, native).at(RenderFeature::AnalyticRayTracing).state ==
        FeatureState::UnavailableInBuild,
        "Analytic shadows also check implementation availability without the reflection request.");
    const auto off = evaluateRenderFeatures(featureRequests(), native);
    for (const auto feature : {RenderFeature::AnalyticRayTracing, RenderFeature::DepthOfField,
            RenderFeature::SelectionOutline, RenderFeature::HardwareRayTracing})
        require(off.at(feature).state == FeatureState::Off,
            "An unavailable implementation does not turn an unrequested feature into an error.");
}

} // namespace

int main() {
    try {
        deferredBaseline();
        forwardBaseline();
        disabledShadowMap();
        analyticRayInputs();
        screenSpaceGiInputs();
        probeGridInputs();
        hardwareTransport();
        postChain();
        antiAliasingChain();
        rejectsBrokenDeclarations();
        pathAndShadowFeatures();
        hardwareFeatureLayers();
        postAndEditorFeatures();
        nativeBackendFeatureAvailability();
        std::cout << "RenderPipelineTests: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "RenderPipelineTests failed: " << failure.what() << '\n';
        return 1;
    }
}
