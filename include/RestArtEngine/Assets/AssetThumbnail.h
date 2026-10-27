#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RestArtEngine/Assets/AssetLibrary.h"

namespace rae::assets {

// Small RGBA8 preview rendered on the CPU so saving an asset never needs a GPU
// pass or an image encoder. Stored in the .raasset blob region.
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
[[nodiscard]] Thumbnail renderMaterialThumbnail(const MaterialAsset& material, std::uint32_t size = 64);

} // namespace rae::assets
