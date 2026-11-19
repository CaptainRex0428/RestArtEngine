#pragma once

#include <cstdint>
#include <string>

namespace rae::renderer {

enum class AntiAliasingMethod : std::uint32_t { Disabled = 0, FXAA = 1, TAA = 2 };

struct AntiAliasingSettings {
    AntiAliasingMethod method = AntiAliasingMethod::FXAA;
    // Current-frame contribution; higher values respond faster to changes.
    float temporalCurrentWeight = 0.1f;
    bool operator==(const AntiAliasingSettings&) const = default;
};

struct AntiAliasingStatus {
    AntiAliasingMethod requested = AntiAliasingMethod::Disabled;
    AntiAliasingMethod effective = AntiAliasingMethod::Disabled;
    std::uint32_t accumulatedFrames = 0;
    std::uint64_t historyRevision = 0;
    std::string reason;
};

inline const char* antiAliasingMethodName(AntiAliasingMethod method) {
    switch (method) {
    case AntiAliasingMethod::Disabled: return "Off";
    case AntiAliasingMethod::FXAA: return "FXAA";
    case AntiAliasingMethod::TAA: return "TAA";
    }
    return "Unknown";
}

} // namespace rae::renderer
