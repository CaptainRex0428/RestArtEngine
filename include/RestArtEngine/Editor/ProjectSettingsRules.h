#pragma once

#include <RestArtEngine/Project/Project.h>

namespace rae::editor {

enum class ProjectRenderingEdit { Backend, HardwareRayTracing, GiMethod, GiMode };

[[nodiscard]] inline const char* giMethodLabel(renderer::GlobalIlluminationMethod method) noexcept {
    using enum renderer::GlobalIlluminationMethod;
    switch (method) {
    case Disabled: return "Off";
    case PathTracing: return "Path tracing";
    case ScreenSpace: return "SSGI";
    case ProbeGrid: return "DDGI";
    case SoftwareWorld: return "Software world GI";
    }
    return "Off";
}

[[nodiscard]] inline const char* giQualityLabel(renderer::GlobalIlluminationQuality quality) noexcept {
    using enum renderer::GlobalIlluminationQuality;
    switch (quality) {
    case Custom: return "Custom";
    case Low: return "Low";
    case Medium: return "Medium";
    case High: return "High";
    }
    return "Custom";
}

[[nodiscard]] inline const char* giExecutionLabel(const renderer::GlobalIlluminationStatus& status) noexcept {
    using enum renderer::GlobalIlluminationMethod;
    if (status.effective == SoftwareWorld)
        return status.usesWorldSpaceProbes ? "Software world GI (voxel probes)" : "Software world GI (reflections only)";
    if (status.effective == ProbeGrid) {
        if (status.usesHardwareProbeTrace) return "DDGI (DXR probes)";
        if (status.usesScreenSpaceProbes) return "DDGI (screen-space probes)";
        return "DDGI (reflections only)";
    }
    if (status.effective == ScreenSpace && status.usesHardwareReflections)
        return "SSGI + DXR reflections";
    return giMethodLabel(status.effective);
}

[[nodiscard]] inline const char* rasterReflectionLabel(const renderer::GlobalIlluminationStatus& status) noexcept {
    if (status.usesHardwareReflections) return "DXR";
    if (status.usesScreenSpaceReflections) return "Screen space (SSR)";
    return "Off";
}

[[nodiscard]] inline bool hasActiveRasterGiQuality(const renderer::GlobalIlluminationStatus& status) noexcept {
    return status.effective == renderer::GlobalIlluminationMethod::ScreenSpace ||
        status.usesHardwareProbeTrace || status.usesScreenSpaceProbes || status.usesWorldSpaceProbes;
}

// These are current build capabilities, not a probe of the user's adapter.
// Apply dependencies only after an explicit edit, never while loading a project.
// Native D3D12 now owns a DXR path; the adapter check happens at runtime.
[[nodiscard]] inline bool projectBackendAllowsHardware(renderer::MainRenderBackend backend) noexcept {
    return backend == renderer::MainRenderBackend::D3D11Dxr ||
        backend == renderer::MainRenderBackend::D3D12;
}

inline void applyProjectRenderingEdit(project::RenderingSettings& settings, ProjectRenderingEdit edit) noexcept {
    using namespace renderer;
    const bool backendChanged = edit == ProjectRenderingEdit::Backend;
    if (backendChanged && settings.backendPreference != MainRenderBackend::D3D12)
        settings.allowFallbackToD3D11 = false;
    if ((backendChanged && !projectBackendAllowsHardware(settings.backendPreference)) ||
        (edit == ProjectRenderingEdit::HardwareRayTracing && !settings.hardwareRayTracing)) {
        settings.hardwareRayTracing = false;
        settings.hardwareRayTracingReflections = false;
        settings.hardwareRayTracingShadows = false;
        // SSGI/DDGI are raster algorithms and survive a backend that cannot run DXR.
        if (settings.globalIllumination.method != GlobalIlluminationMethod::ScreenSpace &&
            !isProbeGi(settings.globalIllumination.method))
            settings.globalIllumination.method = GlobalIlluminationMethod::Disabled;
        return;
    }
    if (edit == ProjectRenderingEdit::HardwareRayTracing && settings.hardwareRayTracing) {
        settings.hardwareRayTracingReflections = true;
        settings.hardwareRayTracingShadows = true;
    }
    if (settings.globalIllumination.method == GlobalIlluminationMethod::PathTracing) {
        if (edit == ProjectRenderingEdit::GiMethod) settings.hardwareRayTracing = true;
        settings.hardwareRayTracingReflections = false;
        if (settings.globalIllumination.mode == GlobalIlluminationMode::Realtime)
            settings.hardwareRayTracingShadows = false;
    }
}

[[nodiscard]] inline renderer::MainBackendRequest projectBackendRequest(
    const project::RenderingSettings& settings) noexcept {
    return {settings.backendPreference, settings.allowFallbackToD3D11};
}

[[nodiscard]] inline bool sameProjectStartupPolicy(const renderer::MainBackendRequest& left,
    const renderer::MainBackendRequest& right) noexcept {
    return left.preference == right.preference && (left.preference != renderer::MainRenderBackend::D3D12 ||
        left.allowFallbackToD3D11 == right.allowFallbackToD3D11);
}

enum class ProjectRestartNotice { None, SaveAndRestart, Restart, SaveToCancelRestart };

[[nodiscard]] inline ProjectRestartNotice projectRestartNotice(const project::RenderingSettings& draft,
    const project::RenderingSettings& saved, const renderer::MainBackendRequest& launched) noexcept {
    const auto next = projectBackendRequest(draft), persisted = projectBackendRequest(saved);
    if (!sameProjectStartupPolicy(next, launched))
        return sameProjectStartupPolicy(next, persisted) ? ProjectRestartNotice::Restart
                                                       : ProjectRestartNotice::SaveAndRestart;
    return sameProjectStartupPolicy(persisted, launched) ? ProjectRestartNotice::None
                                                       : ProjectRestartNotice::SaveToCancelRestart;
}

} // namespace rae::editor
