#pragma once

#include <cstdint>
#include <string>

namespace rae::renderer {

// A lighting algorithm, independent of Forward/Deferred organization and API.
// Backends resolve requests against their capabilities without rewriting them.
enum class GlobalIlluminationMethod : std::uint32_t { Disabled = 0, PathTracing = 1, ScreenSpace = 2, ProbeGrid = 3 };

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
    std::uint32_t screenSpaceSteps = 8;
    float screenSpaceRadius = 2.0f;
    float screenSpaceThickness = 0.2f;
    // DDGI probe grid. The grid is camera-target centred and rebuilt per frame;
    // probe hysteresis controls how quickly the update pass replaces history.
    std::uint32_t probeGridX = 16, probeGridY = 8, probeGridZ = 16;
    float probeSpacing = 1.0f;
    float probeHysteresis = 0.95f;
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
inline constexpr std::uint32_t kGiMaxProbeGridAxis = 32;
inline constexpr std::uint32_t kGiMaxRestirCandidates = 16;
inline constexpr float kGiMaxProbeSpacing = 20.0f;
inline constexpr float kGiMaxIndirectDiffuseIntensity = 8.0f;

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
    std::string reason;
};

} // namespace rae::renderer
