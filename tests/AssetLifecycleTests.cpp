#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetImport.h"
#include "RestArtEngine/Assets/AssetOperations.h"
#include "RestArtEngine/Assets/AssetRegistry.h"
#include "RestArtEngine/Scene/World.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
using Json = nlohmann::json;
int checks = 0;
void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_asset_lifecycle_" + assetIdText(generateAssetId()));
    TemporaryTree() { require(fs::create_directory(root), "Create isolated lifecycle fixture."); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_asset_lifecycle_"))
            fs::remove_all(root, ignored);
    }
};
std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
void writeText(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    require(bool(stream), "Write fixture: " + utf8(path));
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
constexpr std::string_view triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view widerTriangle = "v 0 0 0\nv 4 0 0\nv 0 2 0\nf 1 2 3\n";
// Authored ASCII FBX fixture; no external model or application dependency.
constexpr std::string_view fbxTriangle = R"fbx(; FBX 7.4.0 project file
FBXHeaderExtension: {
    FBXHeaderVersion: 1003
    FBXVersion: 7400
}
GlobalSettings: {
    Version: 1000
    Properties70: {
        P: "UpAxis", "int", "Integer", "", 1
        P: "UpAxisSign", "int", "Integer", "", 1
        P: "FrontAxis", "int", "Integer", "", 2
        P: "FrontAxisSign", "int", "Integer", "", -1
        P: "CoordAxis", "int", "Integer", "", 0
        P: "CoordAxisSign", "int", "Integer", "", 1
        P: "UnitScaleFactor", "double", "Number", "", 1
    }
}
Objects: {
    Geometry: 100, "Geometry::Triangle", "Mesh" {
        Vertices: *9 {
            a: 0,0,0,100,0,0,0,100,0
        }
        PolygonVertexIndex: *3 {
            a: 0,1,-3
        }
    }
    Model: 200, "Model::Triangle", "Mesh" {
        Version: 232
    }
}
Connections: {
    C: "OO", 100, 200
    C: "OO", 200, 0
}
)fbx";

struct Fixture {
    fs::path root, content, source, asset;
    std::string relative;
    AssetLibrary library;
    AssetId id = 0;
    explicit Fixture(const fs::path& folder) : root(folder), content(folder / "Content"), source(folder / "Sources/Original.obj") {
        writeText(source, triangle);
        const std::array sources{source};
        auto report = importAssets(library, content, "Meshes", sources);
        require(report.importedCount() == 1, "Create source mesh: " + report.summary());
        id = report.items.front().id;
        asset = report.items.front().assetPath;
        relative = utf8(asset.lexically_relative(content));
    }
    AssetId addMaterial() {
        const auto result = library.createMaterial("Lifecycle material");
        std::string error;
        require(library.saveMaterials(content / "Materials", error), "Publish material fixture: " + error);
        return result;
    }
};

void requireUnchanged(const Fixture& f, const std::string& before, MeshGeometryKey key,
                      std::string_view provenance, std::size_t count) {
    const auto* mesh = f.library.findMesh(f.id);
    require(mesh && mesh->geometryKey() == key && mesh->sourcePath == provenance &&
        f.library.meshes.size() == count && bytes(f.asset) == before,
        "Rejected operation preserves disk, in-memory geometry, provenance and library size.");
}

void reimportIdentityAndPersistence(const fs::path& root) {
    Fixture f(root);
    auto* mesh = f.library.findMesh(f.id);
    mesh->name = "Stable display name";
    std::string error;
    require(f.library.saveMeshes(f.content, error), "Persist display name: " + error);
    const auto moved = f.content / "Custom/Nested/Renamed.rasset";
    fs::create_directories(moved.parent_path());
    fs::rename(f.asset, moved);
    f.asset = moved; f.relative = utf8(moved.lexically_relative(f.content));
    const auto oldKey = mesh->geometryKey();
    require(mesh->localBounds().maximum[0] == 1, "Initial local bounds are cached.");
    const auto originalName = mesh->name;
    const auto sourcePath = mesh->sourcePath;
    writeText(f.source, widerTriangle);
    const auto sourceBytes = bytes(f.source);
    auto result = reimportAsset(f.library, f.content, f.relative);
    require(result.succeeded() && result.id == f.id && result.assetPath == f.asset && result.name == originalName,
        "Original-path reimport retains GUID, moved file path and authored name: " + result.error);
    mesh = f.library.findMesh(f.id);
    require(mesh && mesh->sourcePath == sourcePath && mesh->name == originalName && mesh->geometryKey() != oldKey &&
        mesh->vertices.size() == 3 && mesh->indices.size() == 3 && mesh->localBounds().maximum[0] == 4,
        "Same-count replacement changes the geometry key and invalidates cached bounds.");
    require(bytes(f.source) == sourceBytes && f.library.meshes.size() == 1,
        "Reimport neither modifies the source nor appends an extra asset.");
    AssetLibrary loaded;
    require(loaded.loadMeshes(f.content, error), "Reload reimported mesh: " + error);
    require(loaded.findMesh(f.id) && loaded.findMesh(f.id)->name == originalName &&
        loaded.findMesh(f.id)->sourcePath == sourcePath && loaded.findMesh(f.id)->localBounds().maximum[0] == 4,
        "Reimported geometry and identity survive a fresh library load.");
    const auto published = bytes(f.asset);
    require(f.library.saveMeshes(f.content, error) && bytes(f.asset) == published,
        "Saving a reimported mesh preserves the published geometry and identity.");

    const auto newSource = root / fs::path(u8"Sources/替换模型.fbx");
    writeText(newSource, fbxTriangle);
    result = reimportAsset(f.library, f.content, f.relative, newSource);
    require(result.succeeded() && result.id == f.id && result.assetPath == f.asset,
        "User-selected FBX can replace an OBJ-derived mesh without changing its identity: " + result.error);
    mesh = f.library.findMesh(f.id);
    require(mesh->name == originalName && mesh->sourcePath == utf8(fs::absolute(newSource).lexically_normal()) &&
        bytes(newSource) == fbxTriangle, "Successful source replacement persists the new UTF-8 path and preserves the source.");
    fs::remove(f.source);
    std::string enlarged(fbxTriangle);
    const auto position = enlarged.find("0,0,0,100,0,0,0,100,0");
    require(position != std::string::npos, "Locate authored FBX positions.");
    enlarged.replace(position, std::string_view("0,0,0,100,0,0,0,100,0").size(), "0,0,0,200,0,0,0,100,0");
    writeText(newSource, enlarged);
    const auto fbxKey = mesh->geometryKey();
    const auto fbxBytes = bytes(f.asset);
    result = reimportAsset(f.library, f.content, f.relative);
    require(result.succeeded() && f.library.findMesh(f.id)->geometryKey() != fbxKey && bytes(f.asset) != fbxBytes,
        "Later original-path reimport uses the replacement source, even after the first source is removed.");
}

void reimportFailures(const fs::path& root) {
    Fixture f(root);
    const auto before = bytes(f.asset);
    const auto key = f.library.findMesh(f.id)->geometryKey();
    const auto provenance = f.library.findMesh(f.id)->sourcePath;
    const auto count = f.library.meshes.size();
    const auto bad = root / "Sources/Bad.obj", unsupported = root / "Sources/Bad.png";
    const auto disguised = root / "Sources/Disguised.fbx", directory = root / "Sources/Directory.obj";
    writeText(bad, "v 0 0 0\nf 1 2 3\n");
    writeText(unsupported, triangle); writeText(disguised, triangle);
    fs::create_directory(directory);
    for (const auto& source : {bad, unsupported, disguised, directory, root / "Missing.obj"}) {
        const auto result = reimportAsset(f.library, f.content, f.relative, source);
        require(!result.succeeded() && !result.error.empty(), "Invalid replacement source is rejected with a reason.");
        requireUnchanged(f, before, key, provenance, count);
    }
    fs::remove(f.source);
    const auto missing = reimportAsset(f.library, f.content, f.relative);
    require(!missing.succeeded() && !missing.error.empty(), "Missing stored source is reported rather than creating an empty mesh.");
    requireUnchanged(f, before, key, provenance, count);
    writeText(f.source, widerTriangle);

    const auto duplicate = f.content / "Other/Legacy.raasset";
    fs::create_directories(duplicate.parent_path()); fs::copy_file(f.asset, duplicate);
    const auto duplicateResult = reimportAsset(f.library, f.content, f.relative);
    require(!duplicateResult.succeeded(), "Reimport refuses duplicate identities, including a legacy fallback.");
    requireUnchanged(f, before, key, provenance, count);
    fs::remove(duplicate);

#ifdef _WIN32
    struct ReadOnlyFile {
        fs::path path;
        DWORD attributes;
        explicit ReadOnlyFile(fs::path value) : path(std::move(value)), attributes(GetFileAttributesW(path.c_str())) {
            require(attributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY),
                "Set replacement target read-only.");
        }
        ~ReadOnlyFile() { SetFileAttributesW(path.c_str(), attributes); }
    } readOnly(f.asset);
    const auto result = reimportAsset(f.library, f.content, f.relative);
    require(!result.succeeded() && !result.error.empty(), "Read-only target reports a publish failure.");
    requireUnchanged(f, before, key, provenance, count);
#endif
}

void deletionAndReload(const fs::path& root) {
    Fixture f(root);
    const auto materialId = f.addMaterial();
    const auto materialPath = f.content / "Materials" / AssetLibrary::materialAssetFileName(*f.library.findMaterial(materialId));
    const auto materialRelative = utf8(materialPath.lexically_relative(f.content));
    const auto sourceBefore = bytes(f.source);
    std::string error;
    require(deleteContentAsset(f.library, f.content, f.relative, error), "Delete unused loaded mesh: " + error);
    require(!fs::exists(f.asset) && !f.library.findMesh(f.id) && bytes(f.source) == sourceBefore,
        "Mesh deletion removes container and live entry, preserving the external source.");
    require(deleteContentAsset(f.library, f.content, materialRelative, error), "Delete unused loaded material: " + error);
    require(!fs::exists(materialPath) && !f.library.findMaterial(materialId), "Material deletion removes disk and live entry.");
    require(f.library.saveMeshes(f.content, error) && f.library.saveMaterials(f.content / "Materials", error),
        "Save after deletion succeeds: " + error);
    require(!fs::exists(f.asset) && !fs::exists(materialPath), "Save cannot resurrect deleted assets.");
    AssetLibrary loaded;
    require(loaded.loadMeshes(f.content, error) && loaded.loadMaterials(f.content / "Materials", error),
        "Reload after deletion succeeds: " + error);
    require(!loaded.findMesh(f.id) && !loaded.findMaterial(materialId), "Deleted identities stay absent after restart.");
    AssetRegistry registry; registry.setContentRoot(f.content);
    require(registry.scan(error) && !registry.find(f.relative) && !registry.find(materialRelative),
        "A fresh registry scan contains neither deleted asset.");
    require(!deleteContentAsset(f.library, f.content, f.relative, error) && !error.empty(),
        "Deleting a missing file is a reported failure.");
    const auto scene = f.content / "Scenes/Unused.rascene";
    writeText(scene, R"({"format":"RAEngine.Scene","version":3,"actors":[],"materials":[],"meshes":[]})");
    require(deleteContentAsset(f.library, f.content, "Scenes/Unused.rascene", error) && !fs::exists(scene),
        "An ordinary saved scene can be deleted; active/startup protection belongs to the editor.");
}

void deletionReferencesAndFailures(const fs::path& root) {
    Fixture f(root);
    const auto materialId = f.addMaterial();
    const auto materialPath = f.content / "Materials" / AssetLibrary::materialAssetFileName(*f.library.findMaterial(materialId));
    const auto materialRelative = utf8(materialPath.lexically_relative(f.content));
    const auto original = bytes(f.asset), materialBytes = bytes(materialPath);
    const auto key = f.library.findMesh(f.id)->geometryKey();
    const auto provenance = f.library.findMesh(f.id)->sourcePath;
    std::string error;
    rae::scene::World world;
    world.createActor("Saved reference", f.id, materialId);
    const auto scenePath = f.content / "Scenes/Nested/References.rascene";
    require(world.save(scenePath, f.library, error), "Save referenced scene: " + error);
    require(!deleteContentAsset(f.library, f.content, f.relative, error) && !error.empty(),
        "A saved scene prevents deleting its mesh.");
    requireUnchanged(f, original, key, provenance, 1);
    require(!deleteContentAsset(f.library, f.content, materialRelative, error) && !error.empty() &&
        f.library.findMaterial(materialId) && bytes(materialPath) == materialBytes,
        "A saved scene prevents deleting its material.");

    auto scene = Json::parse(bytes(scenePath));
    scene["actors"] = Json::array();
    writeText(scenePath, scene.dump());
    require(!deleteContentAsset(f.library, f.content, f.relative, error),
        "Saved mesh inventory references are checked even when no Actor currently uses the ID.");
    scene["meshes"] = Json::array(); scene["materials"] = Json::array();
    scene["actors"] = Json::array({{{"mesh", {{"meshId", f.id}, {"materialId", materialId}}}}});
    writeText(scenePath, scene.dump());
    require(!deleteContentAsset(f.library, f.content, f.relative, error),
        "Actor references cannot bypass deletion protection through an incomplete inventory.");
    writeText(scenePath, "{broken JSON");
    require(!deleteContentAsset(f.library, f.content, f.relative, error) && !error.empty(),
        "An unreadable scene refuses deletion instead of assuming no references.");
    requireUnchanged(f, original, key, provenance, 1);
    fs::remove(scenePath);

    const auto duplicate = f.content / "Legacy.raasset";
    fs::copy_file(f.asset, duplicate);
    require(!deleteContentAsset(f.library, f.content, f.relative, error) && fs::exists(duplicate),
        "A same-GUID legacy file cannot silently resurrect a deleted modern asset.");
    requireUnchanged(f, original, key, provenance, 1);
    fs::remove(duplicate);
    const auto materialDuplicate = f.content / "Materials/Legacy.raasset";
    fs::copy_file(materialPath, materialDuplicate);
    require(!deleteContentAsset(f.library, f.content, materialRelative, error) &&
        f.library.findMaterial(materialId) && bytes(materialPath) == materialBytes,
        "Duplicate material identities also refuse deletion.");
    fs::remove(materialDuplicate);

}

void boundaries(const fs::path& root) {
    Fixture f(root);
    const auto before = bytes(f.asset);
    const auto key = f.library.findMesh(f.id)->geometryKey();
    const auto provenance = f.library.findMesh(f.id)->sourcePath;
    std::string error;
    const std::array invalid{
        std::string("engine://Meshes/Original.rasset"), std::string("../Outside.rasset"),
        std::string("Meshes/../../Outside.rasset"), std::string("/absolute.rasset"),
        std::string("C:\\absolute.rasset"), std::string("Meshes/../Meshes/Original.rasset"),
        std::string("Meshes\0Hidden/Original.rasset", 29),
#ifdef _WIN32
        std::string(".. /Outside.rasset"), std::string("Meshes./Original.rasset"),
#endif
    };
    for (const auto& path : invalid) {
        require(!reimportAsset(f.library, f.content, path, f.source).succeeded(), "Reimport rejects invalid or Engine-qualified paths.");
        require(!deleteContentAsset(f.library, f.content, path, error) && !error.empty(),
            "Deletion rejects invalid or Engine-qualified paths.");
        requireUnchanged(f, before, key, provenance, 1);
    }
    require(!deleteContentAsset(f.library, f.content, "Meshes", error), "Deletion never recursively removes folders.");
    const auto shader = f.content / "Shaders/Shared.hlsl";
    writeText(shader, "float4 main() : SV_Target { return 1; }\n");
    const auto shaderBytes = bytes(shader);
    require(!deleteContentAsset(f.library, f.content, "Shaders/Shared.hlsl", error) && bytes(shader) == shaderBytes,
        "Shader dependencies are not modeled; shader deletion remains protected.");

    MeshAsset engine = *f.library.findMesh(f.id);
    engine.id = EngineCubeMeshId; engine.origin = AssetOrigin::Engine; engine.name = "Engine forged into project";
    const auto enginePath = f.content / "Engine.rasset";
    require(writeAssetFile(enginePath, makeMeshAssetFile(engine), error), "Write ownership test fixture.");
    f.library.meshes.push_back(std::move(engine));
    const auto engineBytes = bytes(enginePath);
    require(!reimportAsset(f.library, f.content, "Engine.rasset", f.source).succeeded() &&
        !deleteContentAsset(f.library, f.content, "Engine.rasset", error) && bytes(enginePath) == engineBytes &&
        f.library.findMesh(EngineCubeMeshId), "Engine identity is protected even behind a Project-looking path.");

    const auto outside = root / "Outside";
    fs::create_directories(outside);
    const auto outsideAsset = outside / "Outside.rasset";
    fs::copy_file(f.asset, outsideAsset);
    std::error_code code;
    fs::create_directory_symlink(outside, f.content / "Linked", code);
    if (!code) {
        require(!reimportAsset(f.library, f.content, "Linked/Outside.rasset", f.source).succeeded(),
            "Reimport refuses linked destination ancestors.");
        require(!deleteContentAsset(f.library, f.content, "Linked/Outside.rasset", error) && bytes(outsideAsset) == before,
            "Deletion cannot follow a directory link outside Content.");
        require(!deleteContentAsset(f.library, f.content / "Linked", "Outside.rasset", error),
            "A linked Content root is also refused.");
    } else std::cout << "SKIP symbolic-link lifecycle fixture: " << code.message() << '\n';
}
} // namespace

int main() {
    try {
        TemporaryTree temporary;
        reimportIdentityAndPersistence(temporary.root / "Reimport");
        reimportFailures(temporary.root / "ReimportFailure");
        deletionAndReload(temporary.root / "Delete");
        deletionReferencesAndFailures(temporary.root / "References");
        boundaries(temporary.root / "Boundaries");
        std::cout << "Asset lifecycle tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Asset lifecycle test failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
