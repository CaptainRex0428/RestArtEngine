#pragma once

// Three-layer feature policy, shared by rendering, the editor and diagnostics.
//
// The layers are deliberately separate because the same request can fail at
// three different heights:
//   1. build availability  - is this implementation compiled into the binary?
//   2. device/backend      - can the selected adapter and API run it?
//   3. frame preconditions - is every dependency of this particular frame met?
// A request is never rewritten by a capability check. The renderer records the
// request and the effective state side by side and reports the blocking layer,
// so switching a device on cannot silently overwrite the user's preference.
//
// This table is a policy surface, not an execution description. Pass order and
// resource edges live in RenderPipeline.h; see docs/RenderPipeline.md.

#include <RestArtEngine/Renderer/GlobalIllumination.h>
#include <RestArtEngine/Renderer/RenderPipeline.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rae::renderer {

enum class RenderFeature : std::uint8_t {
    ForwardPath,
    DeferredPath,
    ShadowMapPCF,
    ShadowMapHard,
    ShadowMapPCF9,
    ShadowMapPCSS,
    ShadowMapCSM2,
    AmbientOcclusionGTAO,
    ContactShadows,
    AnalyticRayTracing,
    HardwareRayTracing,
    HardwareReflections,
    HardwareShadows,
    PathTracingGI,
    RealtimeGI,
    DepthOfField,
    SelectionOutline,
    Count,
};

inline constexpr std::size_t renderFeatureCount =
    static_cast<std::size_t>(RenderFeature::Count);

enum class FeatureState : std::uint8_t {
    Off,                 // intent is off; not an error
    Active,              // requested and used by this frame
    Degraded,            // requested; a different implementation resolves part of it
    UnavailableInBuild,  // layer 1: this build has no implementation
    UnsupportedByDevice, // layer 2: the selected device or API cannot run it
    BlockedByDependency, // layer 3: a precondition of this frame is not met
};

struct FeatureStatus {
    RenderFeature feature = RenderFeature::ForwardPath;
    FeatureState state = FeatureState::Off;
    // Empty for Off and Active; otherwise names the blocking layer or the
    // implementation that takes over.
    std::string reason;
};

// User / project intent for one frame. Values are copied from SceneSettings and
// project configuration; capability checks never edit them.
struct FeatureRequests {
    RenderPath path = RenderPath::Deferred;
    ShadowMethod shadowMethod = ShadowMethod::ShadowMap;
    bool rayTracing = false;
    bool hardwareRayTracing = false;
    bool hardwareRayTracingReflections = true;
    bool hardwareRayTracingShadows = true;
    GlobalIlluminationMethod globalIllumination = GlobalIlluminationMethod::Disabled;
    GlobalIlluminationMode globalIlluminationMode = GlobalIlluminationMode::Progressive;
    bool depthOfField = false;
    bool selectionOutline = false;
};

// Resolved capabilities for the current frame. The renderer fills these after
// device creation and after it has inspected the visible materials.
struct FeatureCapabilities {
    bool shaderPipelineReady = true;
    bool hardwareRayTracingBuilt = true;
    bool hardwareRayTracingDevice = false;
    bool hardwarePipelineReady = false;
    bool hardwareDepthCompatible = true;
    // The pipeline exists but the last dispatch failed; the request is kept and
    // the raster image is retained until the user retries.
    bool hardwareFrameFailed = false;
    bool editorSelection = false;
    // Progressive path tracing resolves depth of field inside the path itself,
    // so the screen-space pass is skipped even though the lens asks for it.
    bool thinLensDepthOfField = false;
};

struct RenderFeatureMatrix {
    std::array<FeatureStatus, renderFeatureCount> features{};

    [[nodiscard]] const FeatureStatus& at(RenderFeature feature) const {
        return features[static_cast<std::size_t>(feature)];
    }
    [[nodiscard]] bool isActive(RenderFeature feature) const {
        return at(feature).state == FeatureState::Active;
    }
    // Active plus degraded features, i.e. features that contribute pixels.
    [[nodiscard]] std::size_t effectiveCount() const;
    // One line per feature that is not Off: state, name and reason.
    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] RenderFeatureMatrix evaluateRenderFeatures(
    const FeatureRequests& requests, const FeatureCapabilities& capabilities);
[[nodiscard]] const char* toString(RenderFeature feature) noexcept;
[[nodiscard]] const char* toString(FeatureState state) noexcept;

} // namespace rae::renderer
