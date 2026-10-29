#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Scene/World.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
using rae::scene::World;
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_scene_persistence_" + assetIdText(generateAssetId()));
    TemporaryTree() { fs::create_directory(root); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_scene_persistence_"))
            fs::remove_all(root, ignored);
    }
};
MeshAsset triangle(AssetId id) {
    MeshAsset mesh;
    mesh.id = id; mesh.name = "Authored triangle";
    mesh.vertices = {{{0,0,0},{0,0,1}}, {{1,0,0},{0,0,1}}, {{0,1,0},{0,0,1}}};
    mesh.indices = {0,1,2};
    return mesh;
}
Json readScene(const fs::path& path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
constexpr AssetId meshId = 101, materialId = 201;
struct Fixture {
    AssetLibrary library;
    World world;
    Fixture() {
        library.meshes.push_back(triangle(meshId));
        MaterialAsset material; material.id = materialId; material.name = "Test material";
        library.materials.push_back(material);
        world.createActor("Triangle actor", meshId, materialId);
    }
};
void referenceInventory(const fs::path& root) {
    Fixture fixture;
    auto unused = triangle(102);
    unused.name.clear();
    unused.indices[0] = 999;
    unused.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    unused.sourcePath = (root / "missing-source.obj").string();
    fixture.library.meshes.push_back(std::move(unused));
    auto empty = triangle(103);
    empty.vertices.clear(); empty.indices.clear(); empty.sourcePath.clear();
    fixture.library.meshes.push_back(std::move(empty));
    MaterialAsset material; material.id = 202; material.name = "Unused material";
    fixture.library.materials.push_back(material);
    fixture.world.blackBackground = true;
    auto& actor = fixture.world.actors.front();
    actor.transform.position[0] = 2.5f; actor.transform.rotation[1] = 35;
    actor.transform.scale[2] = -2; actor.visible = false;
    const auto actorId = actor.id;
    fixture.world.createActor("Another instance", meshId, materialId);
    const auto path = root / "inventory.rascene";
    std::string error;
    require(fixture.world.save(path, fixture.library, error), "Unreferenced invalid geometry/provenance must not block scene save: " + error);
    const auto json = readScene(path);
    require(json.at("meshes").size() == 1 && json.at("meshes")[0].at("id") == meshId,
        "Scene inventory contains each referenced mesh once, including hidden Actors.");
    require(json.at("materials").size() == 1 && json.at("materials")[0].at("id") == materialId,
        "Scene inventory excludes unused project materials.");
    require(json.at("meshes")[0].size() == 1, "Scene inventory contains references, not mesh geometry or provenance.");
    World loaded;
    require(loaded.load(path, fixture.library, error), "Roundtrip scene load: " + error);
    require(loaded.actors.size() == 2 && loaded.blackBackground, "Roundtrip actor count and background.");
    const auto& restored = loaded.actors.front();
    require(restored.id == actorId && restored.name == "Triangle actor" && !restored.visible &&
        restored.transform.position[0] == 2.5f && restored.transform.rotation[1] == 35 &&
        restored.transform.scale[2] == -2 && restored.mesh.meshId == meshId && restored.mesh.materialId == materialId,
        "Roundtrip actor identity, transform, visibility and asset references.");
    const auto roundtrip = root / "roundtrip.rascene";
    require(loaded.save(roundtrip, fixture.library, error), "Save roundtripped world.");
    require(readScene(roundtrip) == json, "Roundtrip preserves serialized scene content.");
    loaded.actors.clear();
    require(loaded.save(roundtrip, fixture.library, error), "Empty world ignores unused invalid geometry.");
    const auto emptyJson = readScene(roundtrip);
    require(emptyJson.at("meshes").empty() && emptyJson.at("materials").empty(), "Empty scene has no asset inventory.");
}
void sceneOnlySave(const fs::path& root) {
    Fixture fixture; std::string error;
    const auto path = root / "geometry.rascene";
    require(fixture.world.save(path, fixture.library, error), "Save initial scene references.");
    const auto baseline = bytes(path);
    auto& mesh = fixture.library.meshes.front();
    // Asset edits, including incomplete geometry, belong to Save Assets. They
    // must neither leak into scene JSON nor be repaired by saving references.
    mesh.indices[2] = 999;
    mesh.vertices[0].position[1] = std::numeric_limits<float>::infinity();
    mesh.vertices[0].normal[2] = std::numeric_limits<float>::quiet_NaN();
    mesh.markGeometryModified();
    mesh.sourcePath = (root / "unavailable-original.fbx").string();
    mesh.name.clear();
    auto& material = fixture.library.materials.front();
    material.name = "Unsaved material name";
    material.baseColor[0] = .12f; material.roughness = .91f;
    material.depthWrite = false; material.setCullMode(MaterialCullMode::Front);
    const auto geometryKey = mesh.geometryKey();
    const auto vertices = mesh.vertices;
    const auto indices = mesh.indices;
    const auto sourcePath = mesh.sourcePath;
    const auto materialBefore = makeMaterialAssetFile(material).manifest;
    require(fixture.world.save(path, fixture.library, error), "Scene save ignores unsaved asset payload edits: " + error);
    require(bytes(path) == baseline, "Geometry and material edits do not change scene JSON.");
    require(mesh.geometryKey() == geometryKey && mesh.indices == indices && mesh.name.empty() &&
        mesh.sourcePath == sourcePath &&
        std::memcmp(mesh.vertices.data(), vertices.data(), vertices.size() * sizeof(MeshVertex)) == 0,
        "Saving scene references preserves mesh storage, revision, geometry and metadata.");
    require(makeMaterialAssetFile(material).manifest == materialBefore && material.id == materialId &&
        material.origin == AssetOrigin::Project && material.name == "Unsaved material name",
        "Saving scene references leaves in-memory material edits intact.");
    mesh.vertices.clear(); mesh.indices.clear(); mesh.markGeometryModified();
    material.name.clear(); material.roughness = std::numeric_limits<float>::infinity();
    require(fixture.world.save(path, fixture.library, error), "Asset payload validation is not a scene-save responsibility.");
    require(bytes(path) == baseline && mesh.vertices.empty() && mesh.indices.empty() &&
        material.name.empty() && std::isinf(material.roughness), "Incomplete asset edits remain untouched.");
    fixture.world.actors.front().transform.position[0] = 3;
    require(fixture.world.save(path, fixture.library, error), "Actor changes save independently from asset edits.");
    require(readScene(path).at("actors")[0].at("transform").at("position")[0] == 3 &&
        mesh.vertices.empty() && std::isinf(material.roughness), "Only Actor data changed during scene save.");
}
void identityAndReferences(const fs::path& root) {
    Fixture fixture; std::string error;
    const auto path = root / "references.rascene";
    require(fixture.world.save(path, fixture.library, error), "Prime reference fixture.");
    const auto baseline = bytes(path);
    const auto reject = [&](const std::string& message) {
        require(!fixture.world.save(path, fixture.library, error) && !error.empty(), message);
        require(bytes(path) == baseline, "Rejected save preserves the previous scene: " + message);
    };
    fixture.library.meshes.push_back(triangle(meshId));
    reject("Duplicate mesh identity rejected.");
    fixture.library.meshes.pop_back();
    auto unused = triangle(102);
    fixture.library.meshes.push_back(unused); fixture.library.meshes.push_back(unused);
    reject("Duplicate unreferenced mesh identity still rejected.");
    fixture.library.meshes.resize(1);
    MaterialAsset material; material.id = 202; material.name = "Unused";
    fixture.library.materials.push_back(material); fixture.library.materials.push_back(material);
    reject("Duplicate unreferenced material identity still rejected.");
    fixture.library.materials.resize(1);
    fixture.world.actors.push_back(fixture.world.actors.front());
    reject("Duplicate Actor identity rejected.");
    fixture.world.actors.resize(1);
    fixture.world.actors.front().mesh.meshId = 999;
    reject("Missing Actor mesh reference rejected.");
    fixture.world.actors.front().mesh.meshId = meshId;
    fixture.world.actors.front().mesh.materialId = 999;
    reject("Missing Actor material reference rejected.");
    fixture.world.actors.front().mesh.materialId = materialId;
    for (const auto invalidId : {AssetId{0}, std::numeric_limits<AssetId>::max()}) {
        fixture.library.meshes.push_back(triangle(invalidId));
        reject("Invalid unreferenced mesh ID rejected.");
        fixture.library.meshes.pop_back();
        material.id = invalidId;
        fixture.library.materials.push_back(material);
        reject("Invalid unreferenced material ID rejected.");
        fixture.library.materials.pop_back();
    }
    fixture.library.meshes.front().origin = AssetOrigin::Engine;
    reject("Project mesh ID cannot impersonate Engine origin.");
    fixture.library.meshes.front().origin = AssetOrigin::Project;
    fixture.library.materials.front().origin = AssetOrigin::Engine;
    reject("Project material ID cannot impersonate Engine origin.");
    fixture.library.materials.front().origin = AssetOrigin::Project;
    fixture.library.meshes.push_back(triangle(EngineCubeMeshId));
    reject("Reserved mesh ID cannot use Project origin.");
    fixture.library.meshes.pop_back();
    material.id = EngineDefaultMaterialId;
    fixture.library.materials.push_back(material);
    reject("Reserved material ID cannot use Project origin.");
    fixture.library.materials.pop_back();
    auto& actor = fixture.world.actors.front();
    const auto actorId = actor.id;
    actor.id = 0;
    reject("Zero Actor identity rejected.");
    actor.id = actorId;
    actor.name.clear();
    reject("Empty Actor name rejected.");
    actor.name = "Triangle actor";
    for (auto* vector : {actor.transform.position, actor.transform.rotation, actor.transform.scale}) {
        const auto original = vector[0];
        vector[0] = std::numeric_limits<float>::quiet_NaN();
        reject("Non-finite Actor transform rejected.");
        vector[0] = original;
    }
    actor.transform.scale[0] = 0;
    reject("Zero Actor scale rejected.");
    actor.transform.scale[0] = 0.00001f;
    reject("Too-small Actor scale rejected.");
    actor.transform.scale[0] = 1;
    require(fixture.world.save(path, fixture.library, error), "Reference repairs succeed.");
    require(bytes(path) == baseline, "Repairing only invalid references restores the original scene.");
}
void lifecycle(const fs::path& root) {
    Fixture fixture; std::string error;
    const auto path = root / "lifecycle.rascene";
    require(fixture.world.save(path, fixture.library, error), "Save lifecycle scene.");
    const auto baseline = bytes(path);
    auto copiedLibrary = fixture.library;
    copiedLibrary.meshes.front().indices[0] = 999;
    copiedLibrary.meshes.front().markGeometryModified();
    auto copiedWorld = fixture.world;
    require(copiedWorld.save(path, copiedLibrary, error) && bytes(path) == baseline,
        "Copied World and library keep scene references independent of geometry edits.");
    auto replacement = triangle(meshId);
    replacement.indices[1] = 999;
    copiedLibrary.meshes.front() = std::move(replacement);
    require(copiedWorld.save(path, copiedLibrary, error) && bytes(path) == baseline,
        "Same-ID asset replacement does not change serialized scene references.");
    const auto geometryKey = copiedLibrary.meshes.front().geometryKey();
    require(fixture.world.load(path, copiedLibrary, error), "Reload a world after prior saves.");
    require(copiedLibrary.meshes.front().geometryKey() == geometryKey &&
        copiedLibrary.meshes.front().indices[1] == 999,
        "V3 scene load resolves identities without replacing the supplied asset library.");
    const auto bad = root / "invalid.rascene";
    { std::ofstream stream(bad); stream << "{}"; }
    require(!fixture.world.load(bad, copiedLibrary, error), "Malformed reload fails.");
    require(fixture.world.save(path, copiedLibrary, error) && bytes(path) == baseline &&
        copiedLibrary.meshes.front().geometryKey() == geometryKey,
        "Malformed load preserves all prior scene data and the asset library.");
    auto invalid = readScene(path);
    invalid["meshes"][0]["id"] = 999;
    { std::ofstream stream(bad); stream << invalid.dump(2); }
    require(!fixture.world.load(bad, copiedLibrary, error), "Reload with a missing asset reference fails.");
    require(fixture.world.save(path, copiedLibrary, error) && bytes(path) == baseline &&
        copiedLibrary.meshes.front().geometryKey() == geometryKey,
        "Reference-load failure preserves the prior World and assets.");

    // Legacy scenes still reconstruct/validate assets during load. Removing
    // geometry validation from v3 scene writes must not weaken that boundary.
    invalid = readScene(path);
    invalid["version"] = 2;
    invalid["meshes"] = Json::array();
    auto badEngine = triangle(EngineCubeMeshId);
    badEngine.origin = AssetOrigin::Engine;
    badEngine.indices[0] = 999;
    copiedLibrary.meshes.push_back(std::move(badEngine));
    { std::ofstream stream(bad); stream << invalid.dump(2); }
    require(!fixture.world.load(bad, copiedLibrary, error) && error.find("index") != std::string::npos,
        "Legacy load still validates mounted Engine geometry.");
    copiedLibrary.meshes.pop_back();
    require(fixture.world.save(path, copiedLibrary, error) && bytes(path) == baseline,
        "Legacy geometry-load failure leaves the prior World usable.");
    copiedLibrary.createDefaults();
    fixture.world.createDefaultScene(copiedLibrary);
    require(fixture.world.save(path, copiedLibrary, error), "World/default-library reset is safe.");
    const auto defaultScene = bytes(path);
    copiedLibrary.findMesh(1)->indices[0] = 999999;
    copiedLibrary.findMesh(1)->markGeometryModified();
    require(fixture.world.save(path, copiedLibrary, error) && bytes(path) == defaultScene,
        "Procedural mesh geometry is also outside scene-only persistence.");
    copiedLibrary.createDefaults();
    fixture.world.createDefaultScene();
    require(fixture.world.save(path, copiedLibrary, error), "Repeated default reset saves correctly.");
}
void presentation(const fs::path& root) {
    Fixture fixture; std::string error;
    const auto path = root / "presentation.rascene";
    require(fixture.world.save(path, fixture.library, error), "Save legacy presentation defaults.");
    const auto defaults = readScene(path);
    require(!defaults.contains("camera") && !defaults.contains("lighting"), "Default scenes retain their old schema shape.");
    fixture.world.startupCamera = rae::scene::SceneCamera{};
    fixture.world.startupCamera->target[1] = 1.7f;
    fixture.world.startupCamera->pitch = -.1f;
    fixture.world.startupCamera->exposure = .8f;
    fixture.world.environment.overrideLighting = true;
    fixture.world.environment.directionalIntensity = 0;
    fixture.world.environment.environmentIntensity = .2f;
    fixture.world.environment.areaLight.enabled = true;
    fixture.world.environment.areaLight.radiance[0] = 20;
    require(fixture.world.save(path, fixture.library, error), "Save authored camera and rectangle light: " + error);
    const auto baseline = bytes(path);
    World loaded;
    require(loaded.load(path, fixture.library, error), "Reload authored scene presentation: " + error);
    require(loaded.startupCamera && loaded.startupCamera->target[1] == 1.7f && loaded.startupCamera->pitch == -.1f &&
        loaded.startupCamera->exposure == .8f && loaded.environment.overrideLighting && loaded.environment.directionalIntensity == 0 &&
        loaded.environment.environmentIntensity == .2f && loaded.environment.areaLight.enabled && loaded.environment.areaLight.radiance[0] == 20,
        "Camera and scene light fields survive roundtrip.");
    require(loaded.save(path, fixture.library, error) && bytes(path) == baseline, "Presentation save/load is stable.");
    const auto bad = root / "bad-presentation.rascene";
    const auto reject = [&](Json invalid, const char* reason) {
        { std::ofstream stream(bad); stream << invalid.dump(2); }
        require(!loaded.load(bad, fixture.library, error), reason);
        require(loaded.save(path, fixture.library, error) && bytes(path) == baseline,
            "Rejected presentation load preserves the prior scene and camera.");
    };
    auto invalid = readScene(path); invalid["camera"]["target"] = {1, 2}; reject(invalid, "Short camera target rejected.");
    invalid = readScene(path); invalid["camera"]["pitch"] = 2; reject(invalid, "Camera pole pitch rejected.");
    invalid = readScene(path); invalid["camera"]["distance"] = 0; reject(invalid, "Zero camera distance rejected.");
    invalid = readScene(path); invalid["camera"]["yaw"] = true; reject(invalid, "Boolean is not a numeric camera value.");
    invalid = readScene(path); invalid["lighting"]["areaLight"]["radiance"] = {-1, 2, 3}; reject(invalid, "Negative light energy rejected.");
    invalid = readScene(path); invalid["lighting"]["areaLight"]["halfAxisU"] = {0, 0, 0}; reject(invalid, "Zero light axis rejected.");
    invalid = readScene(path); invalid["lighting"]["areaLight"]["halfAxisV"] = {.5, 0, .5}; reject(invalid, "Skew light axes rejected.");
    invalid = readScene(path); invalid["lighting"]["directionalIntensity"] = -1; reject(invalid, "Negative directional light rejected.");
    loaded.startupCamera->yaw = std::numeric_limits<float>::infinity();
    require(!loaded.save(path, fixture.library, error) && bytes(path) == baseline, "Nonfinite camera save preserves published file.");
    loaded.startupCamera->yaw = 0;
    loaded.environment.areaLight.halfAxisU[0] = 0;
    require(!loaded.save(path, fixture.library, error) && bytes(path) == baseline, "Invalid in-memory light cannot replace a published scene.");
    { std::ofstream stream(bad); stream << defaults.dump(2); }
    require(loaded.load(bad, fixture.library, error) && !loaded.startupCamera && !loaded.environment.overrideLighting,
        "Loading a legacy scene resets authored camera/light state without inventing defaults.");
    loaded.startupCamera = rae::scene::SceneCamera{}; loaded.environment.overrideLighting = true;
    loaded.createDefaultScene();
    require(!loaded.startupCamera && !loaded.environment.overrideLighting, "Default scene resets authored presentation.");
}
} // namespace

int main() {
    try {
        TemporaryTree files;
        referenceInventory(files.root);
        sceneOnlySave(files.root);
        identityAndReferences(files.root);
        lifecycle(files.root);
        presentation(files.root);
        std::cout << "Scene persistence: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
