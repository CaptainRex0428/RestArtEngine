#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace rae::renderer {

// A lighting algorithm, independent of Forward/Deferred organization and API.
// Backends resolve requests against their capabilities without rewriting them.
enum class GlobalIlluminationMethod : std::uint32_t { Disabled = 0, PathTracing = 1, ScreenSpace = 2, ProbeGrid = 3, SoftwareWorld = 4 };

[[nodiscard]] inline constexpr bool isProbeGi(GlobalIlluminationMethod method) noexcept {
    return method == GlobalIlluminationMethod::ProbeGrid || method == GlobalIlluminationMethod::SoftwareWorld;
}

// Update policy is independent of the transport algorithm and graphics API.
enum class GlobalIlluminationMode : std::uint32_t { Progressive = 0, Realtime = 1 };

// Realtime-only reconstruction policy. Spatial keeps the legacy 5x5 resolve;
// TemporalSpatial adds history reprojection once motion vectors are available.
enum class GlobalIlluminationDenoiser : std::uint32_t { Off = 0, Spatial = 1, TemporalSpatial = 2 };

// Camera-only reprojection works without object history; PerObject also uses
// the previous object transform. Automatic falls back when history is missing.
enum class GlobalIlluminationMotionVectors : std::uint32_t { CameraOnly = 0, PerObject = 1, Automatic = 2 };
// Optional reservoir-based sampling layer over the path/light samples.
enum class GlobalIlluminationSampling : std::uint32_t { Uniform = 0, Restir = 1 };

enum class GlobalIlluminationQuality : std::uint32_t { Custom, Low, Medium, High };

inline constexpr const char* giQualityKey(GlobalIlluminationQuality value) noexcept {
    switch (value) {
    case GlobalIlluminationQuality::Custom: return "custom";
    case GlobalIlluminationQuality::Low: return "low";
    case GlobalIlluminationQuality::Medium: return "medium";
    case GlobalIlluminationQuality::High: return "high";
    }
    return nullptr;
}

inline constexpr std::uint32_t kGiMaxDenoiseIterations = 4;
inline constexpr const char* giDenoiserKey(GlobalIlluminationDenoiser value) noexcept {
    switch (value) {
    case GlobalIlluminationDenoiser::Off: return "off";
    case GlobalIlluminationDenoiser::Spatial: return "spatial";
    case GlobalIlluminationDenoiser::TemporalSpatial: return "temporalSpatial";
    }
    return "spatial";
}
inline constexpr const char* giMotionVectorsKey(GlobalIlluminationMotionVectors value) noexcept {
    switch (value) {
    case GlobalIlluminationMotionVectors::CameraOnly: return "cameraOnly";
    case GlobalIlluminationMotionVectors::PerObject: return "perObject";
    case GlobalIlluminationMotionVectors::Automatic: return "automatic";
    }
    return "automatic";
}

struct GlobalIlluminationSettings {
    GlobalIlluminationMethod method = GlobalIlluminationMethod::Disabled;
    GlobalIlluminationMode mode = GlobalIlluminationMode::Progressive;
    // Select the first surface's indirect BRDF lobes. Direct light is unaffected.
    bool indirectDiffuse = true;
    bool indirectSpecular = true;
    // Artistic gain of the receiving surface's indirect diffuse lobe only.
    // Applied once per path, never to direct light or at every bounce.
    float indirectDiffuseIntensity = 1.0f;
    std::uint32_t samplesPerFrame = 1;
    std::uint32_t targetSamples = 256;
    std::uint32_t maxBounces = 4;
    // ScreenSpace (SSGI): world-space march radius and step count. Reuses
    // indirectDiffuseIntensity as the artistic gain.
    GlobalIlluminationQuality screenSpaceQuality = GlobalIlluminationQuality::Custom;
    std::uint32_t screenSpaceRayCount = 4;
    std::uint32_t screenSpaceSteps = 8;
    float screenSpaceRadius = 2.0f;
    float screenSpaceThickness = 0.2f;
    // DDGI fits cell extents to each scene axis. Manual placement (or a scene
    // without mesh bounds) uses uniform spacing snapped around the camera target.
    GlobalIlluminationQuality probeQuality = GlobalIlluminationQuality::Custom;
    std::uint32_t probeGridX = 16, probeGridY = 8, probeGridZ = 16;
    float probeSpacing = 1.0f;
    float probeHysteresis = 0.95f;
    bool probeAutoFit = true;
    std::uint32_t probeRayCount = 16;
    std::uint32_t probeScreenSpaceSteps = 24;
    // Per-probe SH order for incident radiance: 1 = L1 (4 coefficients),
    // 2 = L2 (9 coefficients). Both paths evaluate clamped-cosine irradiance.
    std::uint32_t probeShOrder = 1;
    // Realtime reconstruction. Defaults preserve the legacy spatial resolve.
    GlobalIlluminationDenoiser denoiser = GlobalIlluminationDenoiser::Spatial;
    GlobalIlluminationMotionVectors motionVectors = GlobalIlluminationMotionVectors::Automatic;
    std::uint32_t denoiseIterations = 2;
    GlobalIlluminationSampling sampling = GlobalIlluminationSampling::Uniform;
    std::uint32_t restirCandidates = 4;
    bool restirDirect = true;
    bool restirIndirect = true;
    bool restirSpatialReuse = true;
    bool operator==(const GlobalIlluminationSettings&) const = default;
};

inline constexpr std::uint32_t kGiMaxSamplesPerFrame = 16;
inline constexpr std::uint32_t kGiMaxTargetSamples = 65536;
inline constexpr std::uint32_t kGiMaxBounces = 12;
inline constexpr std::uint32_t kGiMaxScreenSpaceSteps = 32;
inline constexpr std::uint32_t kGiMaxScreenSpaceRays = 8;
inline constexpr std::uint32_t kGiMaxProbeGridAxis = 32;
inline constexpr std::uint32_t kGiMaxProbeRays = 32;
inline constexpr std::uint32_t kGiMaxProbeScreenSpaceSteps = 24;
inline constexpr std::uint32_t kGiMaxRestirCandidates = 16;
inline constexpr float kGiMaxProbeSpacing = 20.0f;
inline constexpr float kGiMaxIndirectDiffuseIntensity = 8.0f;

// Presets resolve a frame-local copy. Switching back to Custom restores the
// authored budget, including projects saved before quality presets existed.
[[nodiscard]] inline constexpr GlobalIlluminationSettings resolveGiQuality(GlobalIlluminationSettings gi) noexcept {
    if (gi.method == GlobalIlluminationMethod::ScreenSpace) {
        struct Budget { std::uint32_t rays, steps; };
        constexpr Budget presets[] = {{2, 6}, {4, 12}, {8, 24}};
        if (gi.screenSpaceQuality >= GlobalIlluminationQuality::Low && gi.screenSpaceQuality <= GlobalIlluminationQuality::High) {
            const auto budget = presets[static_cast<unsigned>(gi.screenSpaceQuality) - 1];
            gi.screenSpaceRayCount = budget.rays;
            gi.screenSpaceSteps = budget.steps;
        } else {
            gi.screenSpaceQuality = GlobalIlluminationQuality::Custom;
            gi.screenSpaceRayCount = std::clamp(gi.screenSpaceRayCount, 1u, kGiMaxScreenSpaceRays);
            gi.screenSpaceSteps = std::clamp(gi.screenSpaceSteps, 1u, kGiMaxScreenSpaceSteps);
        }
    } else if (isProbeGi(gi.method)) {
        // DDGI's standalone SSR control uses the authored step count.
        gi.screenSpaceSteps = std::clamp(gi.screenSpaceSteps, 1u, kGiMaxScreenSpaceSteps);
        struct Budget { std::uint32_t x, y, z, rays, shOrder, screenSteps; };
        constexpr Budget presets[] = {{8, 4, 8, 8, 1, 6}, {12, 6, 12, 16, 1, 8}, {16, 8, 16, 32, 2, 12}};
        if (gi.probeQuality >= GlobalIlluminationQuality::Low && gi.probeQuality <= GlobalIlluminationQuality::High) {
            const auto budget = presets[static_cast<unsigned>(gi.probeQuality) - 1];
            gi.probeGridX = budget.x;
            gi.probeGridY = budget.y;
            gi.probeGridZ = budget.z;
            gi.probeRayCount = budget.rays;
            gi.probeShOrder = budget.shOrder;
            gi.probeScreenSpaceSteps = budget.screenSteps;
        } else {
            gi.probeQuality = GlobalIlluminationQuality::Custom;
            gi.probeGridX = std::clamp(gi.probeGridX, 2u, kGiMaxProbeGridAxis);
            gi.probeGridY = std::clamp(gi.probeGridY, 2u, kGiMaxProbeGridAxis);
            gi.probeGridZ = std::clamp(gi.probeGridZ, 2u, kGiMaxProbeGridAxis);
            gi.probeRayCount = std::clamp(gi.probeRayCount, 1u, kGiMaxProbeRays);
            gi.probeShOrder = std::clamp(gi.probeShOrder, 1u, 2u);
            gi.probeScreenSpaceSteps = std::clamp(gi.probeScreenSpaceSteps, 1u, kGiMaxProbeScreenSpaceSteps);
        }
    }
    return gi;
}

struct GlobalIlluminationStatus {
    GlobalIlluminationMethod requested = GlobalIlluminationMethod::Disabled;
    GlobalIlluminationMethod effective = GlobalIlluminationMethod::Disabled;
    GlobalIlluminationMode requestedMode = GlobalIlluminationMode::Progressive;
    GlobalIlluminationMode effectiveMode = GlobalIlluminationMode::Progressive;
    GlobalIlluminationDenoiser requestedDenoiser = GlobalIlluminationDenoiser::Spatial;
    GlobalIlluminationDenoiser effectiveDenoiser = GlobalIlluminationDenoiser::Spatial;
    GlobalIlluminationMotionVectors requestedMotionVectors = GlobalIlluminationMotionVectors::Automatic;
    GlobalIlluminationMotionVectors effectiveMotionVectors = GlobalIlluminationMotionVectors::Automatic;
    std::uint32_t accumulatedSamples = 0;
    std::uint32_t targetSamples = 256;
    std::uint64_t historyRevision = 0;
    GlobalIlluminationQuality effectiveQuality = GlobalIlluminationQuality::Custom;
    bool usesHardwareProbeTrace = false;
    bool usesHardwareReflections = false;
    bool usesScreenSpaceProbes = false;
    // Standalone screen-probe DDGI. The budget bounds actual rays because
    // optional high-change probes can decline their secondary update slot.
    std::uint32_t screenProbeUpdatePeriod = 0;
    std::uint32_t screenProbeRayBudget = 0;
    bool usesWorldSpaceProbes = false;
    bool usesScreenSpaceReflections = false;
    std::string reason;
};

} // namespace rae::renderer
