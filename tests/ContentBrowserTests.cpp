#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Assets/AssetRegistry.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
using Json = nlohmann::json;
int checks = 0;
void require(bool passed, const std::string& message) {
    ++checks;
    if (!passed) throw std::runtime_error(message);
}
struct TemporaryContent {
    fs::path root;
    TemporaryContent() {
        root = fs::temp_directory_path() / ("rae_content_test_" + assetIdText(generateAssetId()));
        if (!fs::create_directory(root)) throw std::runtime_error("Cannot create unique test directory.");
    }
    ~TemporaryContent() {
        std::error_code ignored;
        // Only remove the uniquely created test tree, never project Content.
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_content_test_"))
            fs::remove_all(root, ignored);
    }
};
void writeText(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    stream.close();
    require(bool(stream), "write fixture");
}
std::string readText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void materialFile(const fs::path& path, AssetId id, const std::string& name) {
    MaterialAsset material; material.id = id; material.name = name;
    std::string error;
    require(writeAssetFile(path, makeMaterialAssetFile(material), error), "material fixture: " + error);
}
void legacyMaterial(const fs::path& path, AssetId id, const std::string& name) {
    const Json value = {{"format", "RAEngine.Material"}, {"version", 1}, {"material", {
        {"id", id}, {"name", name}, {"baseColor", {.2, .3, .4}}, {"roughness", .5}, {"metallic", .1}}}};
    writeText(path, value.dump());
}
void queryTests(const fs::path& root) {
    materialFile(root / "Root.rasset", 10, "Root");
    materialFile(root / "Materials/Blue.rasset", 11, "Blue Metal");
    materialFile(root / "Materials/Nested/Red.rasset", 12, "Red Metal");
    materialFile(root / "Materials2/Other.rasset", 13, "Other");
    legacyMaterial(root / "Materials/OldBlue.ramat", 11, "Old Blue");
    legacyMaterial(root / "Materials/Unique.ramat", 14, "Unique");
    materialFile(root / "Materials/Blue.raasset", 11, "Old Container");
    writeText(root / "Materials/Zed.hlsl", "void main() {}\n");
    writeText(root / ".hidden/hidden.hlsl", "hidden");
    AssetRegistry registry;
    registry.setContentRoot(root);
    std::string error;
    require(registry.scan(error), error);
    require(registry.inFolder("").size() == 1 && registry.inFolder(".").size() == 1, "root folder normalization");
    require(registry.childFolders("") == std::vector<std::string>{"Materials", "Materials2"}, "direct root child folders");
    require(registry.childFolders("Materials/") == std::vector<std::string>{"Materials/Nested"}, "direct nested folders");
    AssetQuery query; query.folder = "Materials";
    auto entries = registry.query(query);
    require(entries.size() == 3, "direct query hides only superseded legacy");
    require(std::any_of(entries.begin(), entries.end(), [](const auto* entry) { return entry->id == 14; }), "unique legacy remains visible");
    query.showLegacy = true;
    require(registry.query(query).size() == 5, "show legacy opt-in");
    query.showLegacy = false; query.recursive = true;
    require(registry.query(query).size() == 4, "recursive folder boundary excludes Materials2");
    query.search = "rEd nested";
    entries = registry.query(query);
    require(entries.size() == 1 && entries[0]->id == 12, "AND terms match case-folded name and path");
    query.search.clear(); query.type = AssetType::Shader;
    require(registry.query(query).size() == 1, "type filter");
    query.type = AssetType::Unknown;
    for (const auto sort : {AssetSort::Name, AssetSort::Type, AssetSort::Size}) {
        query.sort = sort; query.descending = false;
        const auto ascending = registry.query(query);
        for (std::size_t i = 1; i < ascending.size(); ++i) {
            if (sort == AssetSort::Type) require(ascending[i-1]->type <= ascending[i]->type, "type order");
            if (sort == AssetSort::Size) require(ascending[i-1]->sizeBytes <= ascending[i]->sizeBytes, "size order");
        }
        query.descending = true;
        auto descending = registry.query(query);
        std::reverse(descending.begin(), descending.end());
        require(ascending == descending, "descending is deterministic reverse order");
    }
    query = {}; query.recursive = true;
    require(registry.query(query).size() == 6, "recursive root includes all visible assets");
}
void persistenceTests(const fs::path& root) {
    const auto folder = root / "Materials";
    materialFile(folder / "Sub/Original.rasset", 101, "Display Name");
    legacyMaterial(folder / "Sub/Legacy.ramat", 102, "Legacy");
    AssetLibrary library;
    std::string error;
    require(library.loadMaterials(folder, error), error);
    require(library.materials.size() == 2, "loads nested materials");
    fs::rename(folder / "Sub/Original.rasset", folder / "Sub/Renamed.rasset");
    library.findMaterial(101)->name = "Changed Display";
    library.findMaterial(101)->roughness = .77f;
    require(library.saveMaterials(folder, error), error);
    require(fs::exists(folder / "Sub/Renamed.rasset") && !fs::exists(folder / "Changed Display.rasset"), "save retains browser filename and folder");
    require(fs::exists(folder / "Sub/Legacy.ramat") && fs::exists(folder / "Sub/Legacy.rasset"), "save upgrades without deleting legacy source");
    AssetLibrary reloaded;
    require(reloaded.loadMaterials(folder, error), error);
    require(reloaded.materials.size() == 2 && reloaded.findMaterial(101)->roughness == .77f, "current container wins over retained legacy");
    const auto firstId = reloaded.createMaterial("Same Name");
    const auto secondId = reloaded.createMaterial("Same Name");
    require(firstId != secondId && reloaded.saveMaterials(folder, error), error);
    AssetLibrary uniqueNames;
    require(uniqueNames.loadMaterials(folder, error) && uniqueNames.materials.size() == 4, "same names never overwrite distinct GUIDs");
    materialFile(folder / "Duplicate.rasset", 101, "Duplicate");
    require(!uniqueNames.loadMaterials(folder, error) && uniqueNames.materials.size() == 4 &&
        uniqueNames.findMaterial(101)->name == "Changed Display", "duplicate GUID load leaves library unchanged");
    fs::remove(folder / "Duplicate.rasset");
    writeText(folder / "Bad.rasset", "invalid");
    require(!uniqueNames.loadMaterials(folder, error) && uniqueNames.materials.size() == 4, "malformed load is transactional");

    const auto meshes = root / "Meshes";
    const auto obj = root / "source.obj";
    writeText(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetLibrary meshLibrary; meshLibrary.createDefaults();
    AssetId importedId = 0;
    require(meshLibrary.importObj(obj, importedId, error), error);
    require(meshLibrary.saveMeshes(meshes, error), error);
    fs::create_directories(meshes / "Nested");
    fs::rename(meshes / "source.rasset", meshes / "Nested/Renamed.rasset");
    meshLibrary.findMesh(importedId)->name = "Changed Mesh";
    require(meshLibrary.saveMeshes(meshes, error), error);
    require(fs::exists(meshes / "Nested/Renamed.rasset") && !fs::exists(meshes / "Changed Mesh.rasset"), "mesh saves retain GUID path");
    require(meshLibrary.loadMeshes(meshes, error) && meshLibrary.loadMeshes(meshes, error), error);
    require(meshLibrary.meshes.size() == 4, "repeated mesh loading updates without duplicates");
    require(meshLibrary.ensureBuiltinMeshes(meshes, error), error);
    fs::rename(meshes / "Sphere.rasset", meshes / "Nested/Sphere.rasset");
    require(meshLibrary.ensureBuiltinMeshes(meshes, error) && !fs::exists(meshes / "Sphere.rasset"), "moved builtin does not regenerate a root duplicate");
}
void moveTests(const fs::path& root) {
    const auto scene = root / "Scenes/Scene.rascene";
    writeText(scene, R"({"meshes":[{"sourcePath":"../Meshes/source.obj"}],"actors":[]})");
    writeText(root / "Scenes/Scene.rascene.rthumb", "preview");
    std::string error;
    require(moveContentEntry(root, "Scenes/Scene.rascene", "Scenes/Nested", error), error);
    require(!fs::exists(scene) && fs::exists(root / "Scenes/Nested/Scene.rascene.rthumb"), "scene and sidecar move together");
    const auto moved = root / "Scenes/Nested/Scene.rascene";
    require(Json::parse(readText(moved))["meshes"][0]["sourcePath"] == "../../Meshes/source.obj", "scene source path rebased");
    require(!moveContentEntry(root, "../outside.rasset", "Scenes", error), "reject source traversal");
    require(!moveContentEntry(root, "Scenes/Nested/Scene.rascene", "../outside", error) && fs::exists(moved), "reject destination traversal without modifying source");
    require(!moveContentEntry(root, "Scenes/Nested", "Scenes/Other", error), "reject directory moves");
    writeText(root / "Scenes/Occupied/Scene.rascene", "existing");
    const auto before = readText(moved);
    require(!moveContentEntry(root, "Scenes/Nested/Scene.rascene", "Scenes/Occupied", error) && readText(moved) == before, "collision leaves source unchanged");
    writeText(root / "Scenes/PreviewCollision/Scene.rascene.rthumb", "existing preview");
    require(!moveContentEntry(root, "Scenes/Nested/Scene.rascene", "Scenes/PreviewCollision", error) && readText(moved) == before, "sidecar collision leaves source unchanged");
    std::error_code code;
    fs::create_directory_symlink(root / "Scenes", root / "Linked", code);
    if (!code) {
        require(!moveContentEntry(root, "Scenes/Nested/Scene.rascene", "Linked", error), "reject linked move target");
        AssetRegistry registry; registry.setContentRoot(root);
        require(registry.scan(error) && registry.childFolders("") == std::vector<std::string>{"Scenes"}, "registry excludes linked subtree");
    } else std::cout << "SKIP symbolic-link fixture: " << code.message() << '\n';
}
}
int main() {
    try {
        TemporaryContent temporary;
        queryTests(temporary.root / "Query");
        persistenceTests(temporary.root / "Persistence");
        moveTests(temporary.root / "Move");
        std::cout << "Content browser tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Content browser test failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
