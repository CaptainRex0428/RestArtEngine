#pragma once

#include <RestArtEngine/Project/Project.h>

namespace rae::editor {

enum class ProjectRenderingEdit { Backend, HardwareRayTracing, GiMethod, GiMode };

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
        // SSGI is a raster algorithm and survives a backend that cannot run DXR.
        if (settings.globalIllumination.method != GlobalIlluminationMethod::ScreenSpace)
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
