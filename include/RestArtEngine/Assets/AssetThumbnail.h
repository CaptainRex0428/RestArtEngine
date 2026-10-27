#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RestArtEngine/Assets/AssetLibrary.h"

namespace rae::assets {

// Small RGBA8 preview rendered on the CPU so saving an asset never needs a GPU
// pass or an image encoder. Stored in the .rasset blob region.
struct Thumbnail {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, straight alpha

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

// Orthographic preview of a mesh with simple lambert shading.
[[nodiscard]] Thumbnail renderMeshThumbnail(const MeshAsset& mesh, std::uint32_t size = 64);
// Shaded sphere preview of a material's parameters.
// Layout preview for a scene: one box per Actor, drawn top-down with a fixed
// yaw so scenes have a recognizable thumbnail without a GPU pass.
struct ScenePreviewBox {
    float position[3] = {0, 0, 0};
    float extent[3] = {1, 1, 1};
    float color[3] = {0.6f, 0.7f, 0.8f};
};
[[nodiscard]] Thumbnail renderSceneThumbnail(const std::vector<ScenePreviewBox>& boxes, std::uint32_t size = 64);
[[nodiscard]] Thumbnail renderMaterialThumbnail(const MaterialAsset& material, std::uint32_t size = 64);

} // namespace rae::assets
