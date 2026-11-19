#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace rae::renderer {

// Hybrid mode is a first-class choice: D3D11 raster with optional D3D12/DXR.
// Projects without an explicit preference retain this existing behavior.
enum class MainRenderBackend : std::uint8_t { D3D11Dxr, Auto, D3D11, D3D12 };

[[nodiscard]] inline const char* mainRenderBackendKey(MainRenderBackend value) noexcept {
    switch (value) {
    case MainRenderBackend::D3D11Dxr: return "d3d11Dxr";
    case MainRenderBackend::Auto: return "auto";
    case MainRenderBackend::D3D11: return "d3d11";
    case MainRenderBackend::D3D12: return "d3d12";
    }
    return "invalid";
}

[[nodiscard]] inline std::optional<MainRenderBackend> parseMainRenderBackend(std::string_view value) noexcept {
    if (value == "d3d11Dxr" || value == "legacyHybrid") return MainRenderBackend::D3D11Dxr;
    if (value == "auto") return MainRenderBackend::Auto;
    if (value == "d3d11" || value == "dx11") return MainRenderBackend::D3D11;
    if (value == "d3d12" || value == "dx12") return MainRenderBackend::D3D12;
    return std::nullopt;
}

struct MainBackendRequest {
    MainRenderBackend preference = MainRenderBackend::D3D11Dxr;
    bool allowFallbackToD3D11 = true;
    bool operator==(const MainBackendRequest&) const = default;
};

// Startup plan only. A usable plan is not evidence that a native device exists.
// SceneRenderer owns device creation and reports readiness only after it succeeds.
struct MainBackendSelection {
    MainBackendRequest requested;
    MainRenderBackend effective = MainRenderBackend::D3D11;
    bool startupAllowed = false;
    bool fallback = false;
    bool allowD3D12Bridge = false;
    const char* reason = "Renderer has not selected a main backend.";
};

[[nodiscard]] inline MainBackendSelection selectMainBackend(const MainBackendRequest& request) noexcept {
    MainBackendSelection result;
    result.requested = request;
    // Prefer the validated D3D11 path for Auto. Explicit D3D12 requests must
    // actually attempt the experimental native device before fallback is decided.
    switch (request.preference) {
    case MainRenderBackend::D3D11Dxr:
        result.startupAllowed = true;
        result.allowD3D12Bridge = true;
        result.reason = "D3D11 raster with optional D3D12/DXR ray tracing.";
        break;
    case MainRenderBackend::Auto:
        result.startupAllowed = true;
        result.reason = "Auto selected D3D11; the experimental native D3D12 backend is opt-in.";
        break;
    case MainRenderBackend::D3D11:
        result.startupAllowed = true;
        result.reason = "Strict D3D11; D3D12 and the DXR bridge are not initialized.";
        break;
    case MainRenderBackend::D3D12:
        result.effective = MainRenderBackend::D3D12;
        result.startupAllowed = true;
        result.reason = "Experimental native D3D12 rendering; raster and DXR share the same device, no D3D11 bridge.";
        break;
    default:
        result.reason = "Invalid main render backend preference.";
        break;
    }
    return result;
}

// Call only after an actual D3D12 device/target initialization failure. Shader,
// asset and other general rendering failures are not reasons to hide a bug by
// changing APIs. The caller retains the original native initialization error.
[[nodiscard]] inline MainBackendSelection selectD3D11Fallback(const MainBackendRequest& request) noexcept {
    MainBackendSelection result;
    result.requested = request;
    result.startupAllowed = request.preference == MainRenderBackend::D3D12 && request.allowFallbackToD3D11;
    result.fallback = result.startupAllowed;
    result.reason = result.startupAllowed
        ? "Native D3D12 initialization failed; falling back to strict D3D11 without the DXR bridge."
        : "D3D11 fallback is disabled or the request did not select D3D12.";
    return result;
}

} // namespace rae::renderer
