#pragma once

#include <cstddef>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rae::assets {
using AssetId = std::uint64_t;
enum class AssetOrigin { Project, Engine };
// Stable engine identities occupy a small reserved range. Project files cannot
// redefine them; the origin is assigned by the trusted content mount at load.
inline constexpr AssetId EngineDefaultMaterialId = 0x0010000000000001ULL;
[[nodiscard]] inline constexpr bool isEngineAssetId(AssetId id) noexcept {
    return id >= 0x0010000000000000ULL && id <= 0x0010000000000fffULL;
}
enum class MeshPrimitive { Sphere, Plane, Cube, Imported };
struct MeshVertex { float position[3]; float normal[3]; };
struct MeshGeometryKey {
    std::uint64_t revision = 0;
    const void* vertexData = nullptr;
    const void* indexData = nullptr;
    std::size_t vertexCount = 0, indexCount = 0;
    bool operator==(const MeshGeometryKey&) const = default;
};
struct MeshBounds {
    std::array<float, 3> minimum{}, maximum{};
    bool valid = false;
};
struct MeshAsset {
    AssetId id = 0;
    std::string name; // UTF-8; names displayed by the editor use UTF-8 throughout.
    MeshPrimitive primitive = MeshPrimitive::Imported;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::string sourcePath; // UTF-8 absolute path; decode via std::u8string when constructing filesystem paths.
    AssetOrigin origin = AssetOrigin::Project;
    // Call after editing existing positions/normals/indices in place. Loading,
    // replacing or resizing storage is detected by geometryKey automatically.
    void markGeometryModified() noexcept { geometryRevision_ = nextGeometryRevision(); }
    [[nodiscard]] MeshGeometryKey geometryKey() const noexcept {
        return {geometryRevision_, vertices.data(), indices.data(), vertices.size(), indices.size()};
    }
    [[nodiscard]] const MeshBounds& localBounds() const noexcept {
        const auto key = geometryKey();
        if (boundsKey_ == key) return bounds_;
        bounds_ = {};
        if (!vertices.empty()) {
            bounds_.minimum.fill(std::numeric_limits<float>::max());
            bounds_.maximum.fill(std::numeric_limits<float>::lowest());
            for (const auto& vertex : vertices)
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(vertex.position[axis])) {
                        bounds_ = {}; boundsKey_ = key;
                        return bounds_;
                    }
                    bounds_.minimum[axis] = std::min(bounds_.minimum[axis], vertex.position[axis]);
                    bounds_.maximum[axis] = std::max(bounds_.maximum[axis], vertex.position[axis]);
                }
            bounds_.valid = true;
        }
        boundsKey_ = key;
        return bounds_;
    }
private:
    static std::uint64_t nextGeometryRevision() noexcept {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t geometryRevision_ = nextGeometryRevision();
    mutable MeshGeometryKey boundsKey_;
    mutable MeshBounds bounds_;
};
struct MaterialAsset {
    AssetId id = 0;
    std::string name;
    float baseColor[3] = { .85f, .28f, .08f };
    float roughness = .25f;
    float metallic = .65f;
    bool grid = false;
    // New materials cull back faces; legacy files without this field retain
    // their previously implicit two-sided visibility when deserialized.
    bool twoSided = false;
    AssetOrigin origin = AssetOrigin::Project;
};
class AssetLibrary {
public:
    std::vector<MeshAsset> meshes;
    std::vector<MaterialAsset> materials;
    void createDefaults();
    MeshAsset* findMesh(AssetId id);
    const MeshAsset* findMesh(AssetId id) const;
    MaterialAsset* findMaterial(AssetId id);
    const MaterialAsset* findMaterial(AssetId id) const;
    AssetId createMaterial(std::string name);
    // Copies parameters to a new project identity; throws if source is absent.
    AssetId cloneMaterial(AssetId sourceId, std::string name);
    bool importObj(const std::filesystem::path& path, AssetId& result, std::string& error);
    bool saveMaterials(const std::filesystem::path& folder, std::string& error) const;
    bool loadMaterials(const std::filesystem::path& folder, std::string& error);
    // Loads a read-only engine mount; requires the stable default material.
    // Project reload/save never replace or copy these assets into the project.
    bool loadEngineMaterials(const std::filesystem::path& folder, std::string& error);
    // Static meshes are persisted as .rasset containers; built-in primitives stay
    // procedural and are skipped. Loading keeps the built-ins already present.
    bool saveMeshes(const std::filesystem::path& folder, std::string& error) const;
    bool loadMeshes(const std::filesystem::path& folder, std::string& error);
    [[nodiscard]] static std::string meshAssetFileName(const MeshAsset& mesh);
    // Writes the procedural primitives as .rasset files when they are missing.
    // Existing files win, so editing or deleting one is respected.
    bool ensureBuiltinMeshes(const std::filesystem::path& folder, std::string& error) const;
    // File name used when persisting a material as a .rasset container.
    [[nodiscard]] static std::string materialAssetFileName(const MaterialAsset& material);
private:
    AssetId nextMeshId_ = 4;
};
} // namespace rae::assets
