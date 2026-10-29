#pragma once

#include <RestArtEngine/Assets/AssetLibrary.h>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace rae::renderer::meshAttributes {

using PackedUv = std::array<float, 8>;
using UvTriangle = std::array<std::uint32_t, 4>;

inline bool analytic(const assets::MeshAsset& mesh) {
    return mesh.sections.empty() && mesh.uvChannelMask() == 0 &&
        (mesh.primitive == assets::MeshPrimitive::Sphere || mesh.primitive == assets::MeshPrimitive::Plane ||
         mesh.primitive == assets::MeshPrimitive::Cube);
}

template<class Function> void sections(const assets::MeshAsset& mesh, Function&& function) {
    if (mesh.sections.empty()) function(0u, static_cast<std::uint32_t>(mesh.indices.size()), 0u);
    else for (const auto& section : mesh.sections)
        function(section.firstIndex, section.indexCount, section.materialSlot);
}

inline void validateSections(const assets::MeshAsset& mesh) {
    std::size_t expected = 0;
    for (const auto& section : mesh.sections) {
        if (section.firstIndex != expected || section.indexCount == 0 || section.indexCount % 3 != 0 ||
            section.materialSlot >= mesh.materialSlotCount() || expected > mesh.indices.size() ||
            section.indexCount > mesh.indices.size() - expected)
            throw std::runtime_error("Mesh sections must be a valid contiguous triangle partition.");
        expected += section.indexCount;
    }
    if (!mesh.sections.empty() && expected != mesh.indices.size())
        throw std::runtime_error("Mesh sections do not cover the index buffer.");
}

inline PackedUv vertexUv(const assets::MeshAsset& mesh, std::size_t vertex) {
    PackedUv result{};
    for (std::size_t channel = 0; channel < assets::MaxMeshUvChannels; ++channel) {
        const auto& values = mesh.uvChannels[channel];
        if (values.empty()) continue;
        if (values.size() != mesh.vertices.size()) throw std::runtime_error("Mesh UV count does not match vertices.");
        result[channel * 2] = values[vertex][0];
        result[channel * 2 + 1] = values[vertex][1];
        if (!std::isfinite(result[channel * 2]) || !std::isfinite(result[channel * 2 + 1]))
            throw std::runtime_error("Mesh UV values must be finite.");
    }
    return result;
}

// Only UV-bearing vertices/triangles consume storage. Vertices are shared by
// every instance of a mesh; triangle metadata follows the gathered scene order.
// RayTriangle remains 80 bytes. color.w contains an opaque bit-cast lookup
// token, zero for no UV, rather than a floating-point integer index.
struct RayUvData {
    std::vector<PackedUv> vertices;
    std::vector<UvTriangle> triangles;
    std::unordered_map<assets::AssetId, std::uint32_t> offsets;

    std::uint32_t appendMesh(const assets::MeshAsset& mesh) {
        if (!mesh.uvChannelMask()) return 0;
        if (const auto found = offsets.find(mesh.id); found != offsets.end()) return found->second;
        if (mesh.vertices.size() > (UINT32_MAX / 2u) - vertices.size())
            throw std::runtime_error("Ray UV vertex data exceeds addressable size.");
        const auto offset = static_cast<std::uint32_t>(vertices.size());
        vertices.reserve(vertices.size() + mesh.vertices.size());
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) vertices.push_back(vertexUv(mesh, i));
        offsets.emplace(mesh.id, offset);
        return offset;
    }
    float appendTriangle(const assets::MeshAsset& mesh, std::size_t firstIndex, std::uint32_t vertexOffset) {
        const auto mask = mesh.uvChannelMask();
        if (!mask) return 0;
        if (triangles.size() >= UINT32_MAX - 1u) throw std::runtime_error("Ray UV triangles exceed addressable size.");
        triangles.push_back({vertexOffset + mesh.indices[firstIndex], vertexOffset + mesh.indices[firstIndex + 1],
            vertexOffset + mesh.indices[firstIndex + 2], mask});
        return std::bit_cast<float>(static_cast<std::uint32_t>(triangles.size()));
    }
};
} // namespace rae::renderer::meshAttributes
