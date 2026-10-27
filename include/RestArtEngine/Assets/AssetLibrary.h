#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rae::assets {
using AssetId = std::uint64_t;
enum class MeshPrimitive { Sphere, Plane, Cube, Imported };
struct MeshVertex { float position[3]; float normal[3]; };
struct MeshAsset {
    AssetId id = 0;
    std::string name; // UTF-8; names displayed by the editor use UTF-8 throughout.
    MeshPrimitive primitive = MeshPrimitive::Imported;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::string sourcePath; // UTF-8 absolute path; decode via std::u8string when constructing filesystem paths.
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
    bool importObj(const std::filesystem::path& path, AssetId& result, std::string& error);
    bool saveMaterials(const std::filesystem::path& folder, std::string& error) const;
    bool loadMaterials(const std::filesystem::path& folder, std::string& error);
    // File name used when persisting a material as a .raasset container.
    [[nodiscard]] static std::string materialAssetFileName(const MaterialAsset& material);
private:
    AssetId nextMeshId_ = 4;
    AssetId nextMaterialId_ = 3;
};
} // namespace rae::assets
