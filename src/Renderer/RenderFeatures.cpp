#include <RestArtEngine/Renderer/RenderFeatures.h>

#include <utility>

namespace rae::renderer {

const char* toString(RenderFeature feature) noexcept {
    switch (feature) {
    case RenderFeature::ForwardPath: return "Forward path";
    case RenderFeature::DeferredPath: return "Deferred path";
    case RenderFeature::ShadowMapPCF: return "Shadow map (2x2 PCF)";
    case RenderFeature::ShadowMapHard: return "Shadow map (hard)";
    case RenderFeature::ShadowMapPCF9: return "Shadow map (3x3 PCF)";
    case RenderFeature::ShadowMapPCSS: return "Shadow map (PCSS)";
    case RenderFeature::ShadowMapCSM2: return "Shadow map (2-cascade CSM)";
    case RenderFeature::AmbientOcclusionGTAO: return "GTAO";
    case RenderFeature::ContactShadows: return "Deferred contact shadows";
    case RenderFeature::AnalyticRayTracing: return "Analytic ray effects";
    case RenderFeature::HardwareRayTracing: return "Hardware ray tracing";
    case RenderFeature::HardwareReflections: return "Hardware reflections";
    case RenderFeature::HardwareShadows: return "Hardware shadows";
    case RenderFeature::PathTracingGI: return "Path-traced GI";
    case RenderFeature::RealtimeGI: return "Realtime GI";
    case RenderFeature::DepthOfField: return "Depth of field";
    case RenderFeature::SelectionOutline: return "Selection outline";
    case RenderFeature::Count: break;
    }
    return "Unknown";
}

const char* toString(FeatureState state) noexcept {
    switch (state) {
    case FeatureState::Off: return "Off";
    case FeatureState::Active: return "Active";
    case FeatureState::Degraded: return "Degraded";
    case FeatureState::UnavailableInBuild: return "Unavailable in build";
    case FeatureState::UnsupportedByDevice: return "Unsupported by device";
    case FeatureState::BlockedByDependency: return "Blocked by dependency";
    }
    return "Unknown";
}

std::size_t RenderFeatureMatrix::effectiveCount() const {
    std::size_t count = 0;
    for (const auto& feature : features) {
        if (feature.state == FeatureState::Active || feature.state == FeatureState::Degraded)
            ++count;
    }
    return count;
}

std::string RenderFeatureMatrix::summary() const {
    std::string result;
    for (const auto& feature : features) {
        if (feature.state == FeatureState::Off) continue;
        if (!result.empty()) result += "\n";
        result += std::string("[") + toString(feature.state) + "] " + toString(feature.feature);
        if (!feature.reason.empty()) result += ": " + feature.reason;
    }
    return result;
}

RenderFeatureMatrix evaluateRenderFeatures(const FeatureRequests& requests,
                                           const FeatureCapabilities& capabilities) {
    RenderFeatureMatrix matrix;
    for (std::size_t index = 0; index < renderFeatureCount; ++index) {
        matrix.features[index] = {static_cast<RenderFeature>(index), FeatureState::Off, {}};
    }
    const auto set = [&matrix](RenderFeature feature, FeatureState state, std::string reason = {}) {
        matrix.features[static_cast<std::size_t>(feature)] = {feature, state, std::move(reason)};
    };

    // Pipeline organization: exactly one path runs; the other is simply off.
    set(RenderFeature::ForwardPath,
        requests.path == RenderPath::Forward ? FeatureState::Active : FeatureState::Off);
    set(RenderFeature::DeferredPath,
        requests.path == RenderPath::Deferred ? FeatureState::Active : FeatureState::Off);

    // Shadow strategy. Raster variants produce the shared depth map; GTAO and
    // contact shadows layer on top of that raster map and need the deferred
    // surface buffers, so Forward keeps the plain map and reports the screen
    // space feature as blocked rather than silently pretending it ran.
    bool rasterShadowMap = false;
    RenderFeature rasterVariant = RenderFeature::ShadowMapPCF;
    switch (requests.shadowMethod) {
    case ShadowMethod::ShadowMap: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapPCF; break;
    case ShadowMethod::ShadowMapHard: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapHard; break;
    case ShadowMethod::ShadowMapPCF9: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapPCF9; break;
    case ShadowMethod::ShadowMapPCSS: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapPCSS; break;
    case ShadowMethod::ShadowMapCSM2: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapCSM2; break;
    case ShadowMethod::GTAO: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapPCF; break;
    case ShadowMethod::ContactShadow: rasterShadowMap = true; rasterVariant = RenderFeature::ShadowMapPCF; break;
    case ShadowMethod::AnalyticRay:
    case ShadowMethod::Disabled: break;
    }
    if (rasterShadowMap) set(rasterVariant, FeatureState::Active);
    if (requests.shadowMethod == ShadowMethod::GTAO) {
        if (requests.path == RenderPath::Deferred) {
            set(RenderFeature::AmbientOcclusionGTAO, FeatureState::Active);
        } else {
            set(RenderFeature::AmbientOcclusionGTAO, FeatureState::BlockedByDependency,
                "GTAO needs the Deferred G-buffer; Forward keeps the raster shadow map.");
        }
    }
    if (requests.shadowMethod == ShadowMethod::ContactShadow) {
        if (requests.path == RenderPath::Deferred) {
            set(RenderFeature::ContactShadows, FeatureState::Active);
        } else {
            set(RenderFeature::ContactShadows, FeatureState::BlockedByDependency,
                "Contact shadows need the Deferred position buffer; Forward keeps the raster shadow map.");
        }
    }
    if (requests.shadowMethod == ShadowMethod::AnalyticRay || requests.rayTracing)
        set(RenderFeature::AnalyticRayTracing, FeatureState::Active);

    // Hardware ray tracing, checked layer by layer. The first failing layer is
    // reported; later requests inherit the same reason.
    FeatureState hardwareState = FeatureState::Off;
    std::string hardwareReason = "Hardware ray tracing was not requested.";
    if (requests.hardwareRayTracing) {
        if (!capabilities.shaderPipelineReady) {
            hardwareState = FeatureState::BlockedByDependency;
            hardwareReason = "The raster shader pipeline is not ready.";
        } else if (!capabilities.hardwareRayTracingBuilt) {
            hardwareState = FeatureState::UnavailableInBuild;
            hardwareReason = "This build has no DX12/DXR module.";
        } else if (!capabilities.hardwareRayTracingDevice) {
            hardwareState = FeatureState::UnsupportedByDevice;
            hardwareReason = "The selected device or adapter does not support DXR 1.1.";
        } else if (!capabilities.hardwarePipelineReady) {
            hardwareState = FeatureState::BlockedByDependency;
            hardwareReason = "The DXR compute pipeline is not ready for the active material source.";
        } else if (!capabilities.hardwareDepthCompatible) {
            hardwareState = FeatureState::BlockedByDependency;
            hardwareReason = "A visible material uses a raster-only depth state.";
        } else if (capabilities.hardwareFrameFailed) {
            hardwareState = FeatureState::BlockedByDependency;
            hardwareReason = "The hardware frame failed at runtime; the raster image is retained.";
        } else {
            hardwareState = FeatureState::Active;
            hardwareReason.clear();
        }
        set(RenderFeature::HardwareRayTracing, hardwareState,
            hardwareState == FeatureState::Active ? std::string{} : hardwareReason);
    }
    const bool hardwareActive = hardwareState == FeatureState::Active;

    if (requests.hardwareRayTracing && requests.hardwareRayTracingReflections) {
        if (requests.globalIllumination != GlobalIlluminationMethod::Disabled) {
            set(RenderFeature::HardwareReflections, FeatureState::BlockedByDependency,
                "Path-traced GI owns indirect specular while it is enabled.");
        } else if (!hardwareActive) {
            set(RenderFeature::HardwareReflections, FeatureState::BlockedByDependency, hardwareReason);
        } else {
            set(RenderFeature::HardwareReflections, FeatureState::Active);
        }
    }

    if (requests.hardwareRayTracing && requests.hardwareRayTracingShadows) {
        if (requests.shadowMethod == ShadowMethod::Disabled) {
            set(RenderFeature::HardwareShadows, FeatureState::BlockedByDependency,
                "The scene shadow method is Disabled.");
        } else if (!hardwareActive) {
            set(RenderFeature::HardwareShadows, FeatureState::BlockedByDependency, hardwareReason);
        } else if (requests.globalIllumination != GlobalIlluminationMethod::Disabled &&
                   requests.globalIlluminationMode == GlobalIlluminationMode::Realtime) {
            set(RenderFeature::HardwareShadows, FeatureState::BlockedByDependency,
                "Realtime GI transports direct shadows; the ray-traced shadow preference is retained.");
        } else {
            set(RenderFeature::HardwareShadows, FeatureState::Active);
        }
    }

    if (requests.globalIllumination == GlobalIlluminationMethod::PathTracing) {
        set(RenderFeature::PathTracingGI, hardwareActive ? FeatureState::Active
            : FeatureState::BlockedByDependency,
            hardwareActive ? std::string{} : hardwareReason);
        if (requests.globalIlluminationMode == GlobalIlluminationMode::Realtime) {
            set(RenderFeature::RealtimeGI, hardwareActive ? FeatureState::Active
                : FeatureState::BlockedByDependency,
                hardwareActive ? std::string{} : hardwareReason);
        }
    }

    if (requests.depthOfField) {
        if (capabilities.thinLensDepthOfField) {
            set(RenderFeature::DepthOfField, FeatureState::Degraded,
                "Progressive path tracing resolves thin-lens depth of field; the screen-space pass is skipped.");
        } else {
            set(RenderFeature::DepthOfField, FeatureState::Active);
        }
    }

    if (requests.selectionOutline) {
        if (!capabilities.editorSelection) {
            set(RenderFeature::SelectionOutline, FeatureState::BlockedByDependency,
                "The editor has no selection to outline.");
        } else {
            set(RenderFeature::SelectionOutline, FeatureState::Active);
        }
    }
    return matrix;
}

} // namespace rae::renderer
