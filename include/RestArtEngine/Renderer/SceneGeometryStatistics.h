#pragma once

#include <cstdint>
#include <string>

namespace rae::renderer {

enum class GeometrySampleStatus { Pending, Ready, Unavailable, NotApplicable, NoViewport };

// Scene topology and the main raster view have different counting boundaries.
// Shadow, selection, fullscreen and tool-preview draws are never included.
struct SceneGeometryStatistics {
    // Monotonic main-scene render sequence; tool previews do not advance it.
    std::uint64_t frameId = 0;
    // Per Actor instance, including hidden Actors; unused Mesh assets excluded.
    std::uint64_t sceneTriangles = 0;
    // Asynchronous GPU sample sequence. Counts are valid only when Ready.
    std::uint64_t gpuFrameId = 0;
    std::uint64_t inputTriangles = 0;
    // Output of GPU primitive clipping, before depth/occlusion visibility.
    // Clipping can split triangles, so this can exceed inputTriangles.
    std::uint64_t postClipTriangles = 0;
    GeometrySampleStatus status = GeometrySampleStatus::NoViewport;
    std::string statusMessage = "No main scene viewport has been rendered.";
};

} // namespace rae::renderer
