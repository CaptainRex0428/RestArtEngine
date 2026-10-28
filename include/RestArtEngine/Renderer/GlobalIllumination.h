#pragma once

#include <cstdint>
#include <string>

namespace rae::renderer {

// A lighting algorithm, independent of Forward/Deferred organization and API.
// Backends resolve requests against their capabilities without rewriting them.
enum class GlobalIlluminationMethod : std::uint32_t { Disabled = 0, PathTracing = 1 };

struct GlobalIlluminationSettings {
    GlobalIlluminationMethod method = GlobalIlluminationMethod::Disabled;
    std::uint32_t samplesPerFrame = 1;
    std::uint32_t targetSamples = 256;
    std::uint32_t maxBounces = 4;
    bool operator==(const GlobalIlluminationSettings&) const = default;
};

inline constexpr std::uint32_t kGiMaxSamplesPerFrame = 16;
inline constexpr std::uint32_t kGiMaxTargetSamples = 65536;
inline constexpr std::uint32_t kGiMaxBounces = 12;

struct GlobalIlluminationStatus {
    GlobalIlluminationMethod requested = GlobalIlluminationMethod::Disabled;
    GlobalIlluminationMethod effective = GlobalIlluminationMethod::Disabled;
    std::uint32_t accumulatedSamples = 0;
    std::uint32_t targetSamples = 256;
    std::uint64_t historyRevision = 0;
    std::string reason;
};

} // namespace rae::renderer
