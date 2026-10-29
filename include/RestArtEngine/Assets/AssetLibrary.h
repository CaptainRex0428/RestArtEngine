#pragma once

#include "RestArtEngine/Assets/AssetImportOptions.h"

#include <cstddef>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace rae::assets {
using AssetId = std::uint64_t;
enum class AssetOrigin { Project, Engine };
// Stable engine identities occupy a small reserved range. Project files cannot
// redefine them; the origin is assigned by the trusted content mount at load.
inline constexpr AssetId EngineDefaultMaterialId = 0x0010000000000001ULL;
inline constexpr AssetId EngineCubeMeshId = 0x0010000000000101ULL;
inline constexpr AssetId EngineSphereMeshId = 0x0010000000000102ULL;
inline constexpr AssetId EngineCylinderMeshId = 0x0010000000000103ULL;
inline constexpr AssetId EngineConeMeshId = 0x0010000000000104ULL;
inline constexpr AssetId EngineTorusMeshId = 0x0010000000000105ULL;
inline constexpr AssetId EnginePlaneMeshId = 0x0010000000000106ULL;
[[nodiscard]] inline constexpr bool isEngineAssetId(AssetId id) noexcept {
    return id >= 0x0010000000000000ULL && id <= 0x0010000000000fffULL;
}
enum class MeshPrimitive { Sphere, Plane, Cube, Imported };
struct MeshVertex { float position[3]; float normal[3]; };
inline constexpr std::size_t MaxMeshUvChannels = 4;
struct MeshMaterialSlot {
    std::string name;
    std::string sourceKey; // Stable import identity; distinct from an engine material GUID.
    AssetId defaultMaterialId = 0; // Zero inherits the Actor fallback material.
    bool operator==(const MeshMaterialSlot&) const = default;
};
struct MeshSection {
    std::uint32_t firstIndex = 0, indexCount = 0, materialSlot = 0;
    bool operator==(const MeshSection&) const = default;
};
struct MeshGeometryKey {
    std::uint64_t revision = 0;
    const void* vertexData = nullptr;
    const void* indexData = nullptr;
    std::size_t vertexCount = 0, indexCount = 0;
    std::array<const void*, MaxMeshUvChannels> uvData{};
    std::array<std::size_t, MaxMeshUvChannels> uvCounts{};
    const void* sectionData = nullptr;
    std::size_t sectionCount = 0;
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
    // Empty sections/slots preserve the legacy single-material mesh. Explicit
    // sections form a sorted, contiguous partition of the triangle index buffer.
    std::vector<MeshMaterialSlot> materialSlots;
    std::vector<MeshSection> sections;
    std::array<std::vector<std::array<float, 2>>, MaxMeshUvChannels> uvChannels;
    std::array<std::string, MaxMeshUvChannels> uvNames;
    [[nodiscard]] std::size_t materialSlotCount() const noexcept { return std::max<std::size_t>(1, materialSlots.size()); }
    [[nodiscard]] unsigned uvChannelMask() const noexcept {
        unsigned mask = 0;
        for (unsigned channel = 0; channel < MaxMeshUvChannels; ++channel)
            if (!uvChannels[channel].empty()) mask |= 1u << channel;
        return mask;
    }
    [[nodiscard]] std::uint32_t materialSlotForIndex(std::uint32_t offset) const noexcept {
        const auto found = std::upper_bound(sections.begin(), sections.end(), offset,
            [](std::uint32_t index, const MeshSection& section) { return index < section.firstIndex; });
        return found == sections.begin() ? 0u : std::prev(found)->materialSlot;
    }
    // Call after editing positions/normals/indices, UV values or sections in place.
    // Replacing/resizing storage is detected automatically. Slot names/default
    // materials and UV names are small metadata, checked separately by consumers.
    void markGeometryModified() noexcept { geometryRevision_ = nextGeometryRevision(); }
    [[nodiscard]] MeshGeometryKey geometryKey() const noexcept {
        MeshGeometryKey key{geometryRevision_, vertices.data(), indices.data(), vertices.size(), indices.size()};
        for (std::size_t channel = 0; channel < MaxMeshUvChannels; ++channel) {
            key.uvData[channel] = uvChannels[channel].data();
            key.uvCounts[channel] = uvChannels[channel].size();
        }
        key.sectionData = sections.data(); key.sectionCount = sections.size();
        return key;
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
struct MeshImportDiagnostics {
    std::size_t skippedDegenerateTriangles = 0;
    // Retained triangle corners repaired with geometric normals; ordinary
    // missing normals generated by the FBX reader are not counted here.
    std::size_t repairedNormals = 0;
    std::size_t droppedUvChannels = 0;
};
enum class MaterialCullMode { Off, Front, Back };
enum class MaterialDepthTest { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always, Disabled };
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
    bool cullFront = false;
    MaterialDepthTest depthTest = MaterialDepthTest::Less;
    bool depthWrite = true;
    [[nodiscard]] MaterialCullMode effectiveCullMode() const noexcept {
        return twoSided ? MaterialCullMode::Off : cullFront ? MaterialCullMode::Front : MaterialCullMode::Back;
    }
    void setCullMode(MaterialCullMode mode) noexcept {
        twoSided = mode == MaterialCullMode::Off;
        cullFront = mode == MaterialCullMode::Front;
    }
    [[nodiscard]] bool hardwareDepthCompatible() const noexcept {
        return depthWrite && (depthTest == MaterialDepthTest::Less || depthTest == MaterialDepthTest::LessEqual);
    }
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
    // Static FBX geometry; the shared import service owns project persistence.
    bool importFbx(const std::filesystem::path& path, AssetId& result, std::string& error,
        MeshImportDiagnostics* diagnostics = nullptr, const AssetImportOptions& options = {});
    bool saveMaterials(const std::filesystem::path& folder, std::string& error) const;
    // Explicit asset save: only this Project material is written. Engine assets
    // are read-only; saving a scene does not implicitly persist asset edits.
    bool saveMaterial(AssetId id, const std::filesystem::path& folder, std::string& error) const;
    bool loadMaterials(const std::filesystem::path& folder, std::string& error);
    // Loads a read-only engine mount; requires the stable default material.
    // Project reload/save never replace or copy these assets into the project.
    bool loadEngineMaterials(const std::filesystem::path& folder, std::string& error);
    // Mounts self-contained engine meshes; ownership is assigned here, never by files.
    bool loadEngineMeshes(const std::filesystem::path& folder, std::string& error);
    // Static meshes are persisted as .rasset containers; built-in primitives stay
    // procedural and are skipped. Loading keeps the built-ins already present.
    bool saveMeshes(const std::filesystem::path& folder, std::string& error) const;
    bool loadMeshes(const std::filesystem::path& folder, std::string& error);
    // Call only after successfully publishing/loading this exact mesh as a
    // modern container. Failure to cache the baseline simply leaves it dirty.
    void rememberMeshPersisted(AssetId id, const std::filesystem::path& path) const noexcept;
    [[nodiscard]] static std::string meshAssetFileName(const MeshAsset& mesh);
    // Writes the procedural primitives as .rasset files when they are missing.
    // Existing files win, so editing or deleting one is respected.
    bool ensureBuiltinMeshes(const std::filesystem::path& folder, std::string& error) const;
    // File name used when persisting a material as a .rasset container.
    [[nodiscard]] static std::string materialAssetFileName(const MaterialAsset& material);
private:
    struct MeshPersistence {
        MeshGeometryKey geometry;
        std::vector<MeshMaterialSlot> materialSlots;
        std::array<std::string, MaxMeshUvChannels> uvNames;
        std::string name, sourcePath, targetPath;
        MeshPrimitive primitive = MeshPrimitive::Imported;
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified{};
    };
    mutable std::unordered_map<AssetId, MeshPersistence> persistedMeshes_;
    bool meshPersistenceMatches(const MeshAsset& mesh, const std::filesystem::path& path) const;
    void pruneMeshPersistence() const;
};
} // namespace rae::assets
