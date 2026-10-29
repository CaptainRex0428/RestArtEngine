#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetRegistry.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
int checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_material_persistence_" + assetIdText(generateAssetId()));
    TemporaryTree() { fs::create_directory(root); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_material_persistence_"))
            fs::remove_all(root, ignored);
    }
};
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
MaterialAsset readMaterial(const fs::path& path) {
    AssetFile file; MaterialAsset material; std::string error;
    require(readAssetFile(path, file, error), "Read material container: " + error);
    require(readMaterialAssetFile(file, material, error), "Read material payload: " + error);
    return material;
}
void persistence(const fs::path& root) {
    AssetLibrary library;
    MaterialAsset a; a.id = 101; a.name = "First";
    MaterialAsset b; b.id = 102; b.name = "Second";
    library.materials = {a,b};
    const auto folder = root / "Content";
    const auto first = folder / "First.rasset", second = folder / "Second.rasset";
    std::string error;
    require(library.saveMaterials(folder, error), "Save initial materials: " + error);
    const auto oldTime = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(first, oldTime); fs::last_write_time(second, oldTime);
    const auto firstBytes = bytes(first), secondBytes = bytes(second);
    require(library.saveMaterial(a.id, folder, error), "Save unchanged selected material.");
    require(library.saveMaterials(folder, error), "Save unchanged material collection.");
    require(fs::last_write_time(first) == oldTime && bytes(first) == firstBytes &&
        fs::last_write_time(second) == oldTime && bytes(second) == secondBytes,
        "Unchanged material saves preserve bytes and exact mtime.");
    library.materials[0].roughness = .51f; library.materials[1].roughness = .73f;
    require(library.saveMaterial(a.id, folder, error), "Save only first dirty material.");
    require(readMaterial(first).roughness == .51f, "Selected material changes reach disk.");
    require(fs::last_write_time(second) == oldTime && bytes(second) == secondBytes,
        "Selected save leaves another dirty material untouched.");
    library.materials[1].roughness = std::numeric_limits<float>::quiet_NaN();
    require(library.saveMaterial(a.id, folder, error), "Invalid unrelated material cannot block selected save.");
    require(!library.saveMaterial(b.id, folder, error), "Invalid selected material fails.");
    require(bytes(second) == secondBytes, "Invalid selected save preserves existing file.");
    library.materials[1].roughness = .73f;
    require(library.saveMaterial(b.id, folder, error), "Repaired selected material retries successfully.");
    require(readMaterial(second).roughness == .73f, "Repair is persisted.");

    fs::create_directory(folder / "Moved");
    const auto moved = folder / "Moved" / "RenamedOnDisk.rasset";
    fs::rename(first, moved);
    library.materials[0].name = "New display name";
    library.materials[0].metallic = .2f;
    require(library.saveMaterial(a.id, folder, error), "Save moved GUID material.");
    require(!fs::exists(first) && readMaterial(moved).name == "New display name", "GUID path survives file/display rename.");
    MaterialAsset c = a; c.id = 103; c.name = "Second";
    library.materials.push_back(c);
    const auto preservedSecond = bytes(second);
    require(library.saveMaterial(c.id, folder, error), "Save new colliding-name asset.");
    require(bytes(second) == preservedSecond && fs::is_regular_file(folder / ("Second-" + assetIdText(c.id) + ".rasset")),
        "New asset uses GUID suffix without overwriting existing material.");
    MaterialAsset engine = a; engine.id = EngineDefaultMaterialId; engine.origin = AssetOrigin::Engine; engine.name = "EngineDefault";
    library.materials.push_back(engine);
    require(!library.saveMaterial(engine.id, folder, error), "Explicit Engine save is rejected as read-only.");
    require(library.saveMaterials(folder, error), "Batch skips Engine materials.");
    require(!fs::exists(folder / "EngineDefault.rasset"), "Engine material is not copied to Project Content.");
    require(!library.saveMaterial(999, folder, error), "Missing selected material fails.");

    const auto legacyFolder = root / "Legacy";
    fs::create_directory(legacyFolder);
    require(writeAssetFile(legacyFolder / "First.raasset", makeMaterialAssetFile(a), error), "Write legacy fixture.");
    AssetLibrary legacy; legacy.materials.push_back(a);
    require(legacy.saveMaterial(a.id, legacyFolder, error), "Explicit save migrates legacy asset.");
    require(fs::exists(legacyFolder / "First.raasset") && readMaterial(legacyFolder / "First.rasset").id == a.id,
        "Legacy migration preserves source and publishes modern asset.");

    // Metadata-only indexing must not load container thumbnails or render the
    // scene fallback, while the default browser scan retains both behaviors.
    const auto scene = folder / "Layout.rascene";
    { std::ofstream stream(scene); stream << R"({"actors":[{"transform":{"position":[0,0,0],"scale":[1,1,1]},"mesh":{"meshId":101}}]})"; }
    AssetRegistry registry; registry.setContentRoot(folder);
    require(registry.scan(error, false), "Index assets without preview work.");
    require(!registry.entries().empty(), "Metadata scan retains entries.");
    bool foundMaterial = false;
    for (const auto& entry : registry.entries()) {
        require(!entry.thumbnail.valid() && entry.thumbnail.rgba.empty(), "Metadata scan leaves all preview buffers empty.");
        if (entry.id == a.id) foundMaterial = entry.type == AssetType::Material;
    }
    require(foundMaterial, "Metadata scan retains material identity and type.");
    require(registry.scan(error), "Default scan loads previews.");
    require(registry.find("Layout.rascene") && registry.find("Layout.rascene")->thumbnail.valid(), "Default browser scan renders scene fallback.");
    require(registry.find("Second.rasset") && registry.find("Second.rasset")->thumbnail.valid(), "Default browser scan reads material thumbnail.");
}
} // namespace
int main() {
    try {
        TemporaryTree files;
        persistence(files.root);
        std::cout << "Material persistence: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
