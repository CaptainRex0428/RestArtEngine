#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetOperations.h"
#include "RestArtEngine/Assets/MeshQuery.h"
#include "RestArtEngine/Scene/World.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rae;
namespace fs = std::filesystem;
using Json = nlohmann::json;
unsigned checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Temp {
    fs::path path = fs::temp_directory_path() / ("rae_slots_" + assets::assetIdText(assets::generateAssetId()));
    Temp() { fs::create_directories(path); }
    ~Temp() { std::error_code ignored; fs::remove_all(path, ignored); }
};
assets::MeshAsset meshFixture() {
    assets::MeshAsset mesh;
    mesh.id = 101; mesh.name = "Layered mesh";
    mesh.vertices = {{{-1,-1,1},{0,0,1}}, {{1,-1,1},{0,0,1}}, {{0,1,1},{0,0,1}},
                     {{-1,-1,0},{0,0,1}}, {{1,-1,0},{0,0,1}}, {{0,1,0},{0,0,1}}};
    mesh.indices = {0,1,2,3,4,5};
    mesh.materialSlots = {{"Front paint", "source/front", 202}, {"Rear paint", "source/rear", 203}};
    mesh.sections = {{0,3,0}, {3,3,1}};
    return mesh;
}
Json read(const fs::path& path) { std::ifstream stream(path); return Json::parse(stream); }
void write(const fs::path& path, const Json& value) { std::ofstream stream(path); stream << value.dump(2); }
void picking() {
    auto mesh = meshFixture();
    assets::MeshQueryCache cache;
    assets::MeshRayHit hit;
    const assets::MeshRay ray{{0,0,3},{0,0,-1}};
    std::array<assets::MeshFaceState,2> states{{{assets::MaterialCullMode::Back,false},{assets::MaterialCullMode::Back,true}}};
    require(cache.intersect(mesh, ray, 10, false, 1e-8f, hit, false, states) && hit.triangleOffset == 3 && hit.distance == 3,
        "Never on nearer slot must not hide a farther valid BVH hit.");
    states[0] = {assets::MaterialCullMode::Front,true};
    require(cache.intersect(mesh, ray, 10, false, 1e-8f, hit, false, states) && hit.triangleOffset == 3,
        "Per-slot front culling must continue to a valid farther triangle.");
    states[0].cull = assets::MaterialCullMode::Off;
    require(cache.intersect(mesh, ray, 10, false, 1e-8f, hit, false, states) && hit.triangleOffset == 0 && hit.distance == 2,
        "Changing slot state reveals the nearest surface immediately.");
    require(cache.buildCount() == 1, "Slot material edits reuse geometry acceleration.");
    require(mesh.materialSlotForIndex(0) == 0 && mesh.materialSlotForIndex(3) == 1,
        "Hit triangle offsets resolve the exact material slot.");
}
void sceneAndReferences(const fs::path& root) {
    assets::AssetLibrary library;
    library.meshes.push_back(meshFixture());
    for (assets::AssetId id : {201u,202u,203u,204u,205u}) {
        assets::MaterialAsset material;
        material.id = id; material.name = "Material " + std::to_string(id);
        library.materials.push_back(material);
    }
    scene::World world;
    const auto actorId = world.createActor("Two slots", 101, 201);
    auto& component = world.findActor(actorId)->mesh;
    const auto& mesh = library.meshes.front();
    require(component.materialForSlot(mesh, 0) == 202 && component.materialForSlot(mesh, 1) == 203,
        "Mesh slot defaults override the legacy Actor fallback.");
    component.materialOverrides = {204,0};
    require(component.materialForSlot(mesh, 0) == 204 && component.materialForSlot(mesh, 1) == 203,
        "Instance override wins, zero inherits its mesh default.");
    const auto duplicateId = world.duplicateActor(actorId);
    world.findActor(duplicateId)->mesh.materialOverrides[0] = 205;
    require(world.findActor(actorId)->mesh.materialOverrides[0] == 204, "Actor duplication owns its override values.");
    world.removeActor(duplicateId);
    const auto path = root / "Slots.rascene";
    std::string error;
    require(world.save(path, library, error), error);
    auto document = read(path);
    require(document.at("version") == 5 && document.at("materials").size() == 4,
        "Scene v5 records overrides, mesh defaults and fallback references without unused materials.");
    require(document.at("actors")[0].at("mesh").at("materialOverrideKeys")[0] == "source/front",
        "Scene binds stable source slot keys, not only numeric indices.");
    scene::World loaded;
    require(loaded.load(path, library, error) && loaded.actors[0].mesh.materialOverrides == world.findActor(actorId)->mesh.materialOverrides,
        "Slot overrides survive save and reload: " + error);
    std::swap(library.meshes[0].materialSlots[0], library.meshes[0].materialSlots[1]);
    require(loaded.load(path, library, error) && loaded.actors[0].mesh.materialOverrides == std::vector<assets::AssetId>({0,204}),
        "Reimport slot reorder must restore bindings by source identity.");
    library.meshes[0].materialSlots.erase(library.meshes[0].materialSlots.begin()+1);
    require(loaded.load(path, library, error) && loaded.actors[0].mesh.materialOverrides == std::vector<assets::AssetId>({0}),
        "Removed source slots cannot bind a different remaining slot.");
    library.meshes[0] = meshFixture();
    const auto reject = [&](Json invalid) {
        write(path, invalid);
        const auto original = loaded.actors[0].mesh.materialOverrides;
        require(!loaded.load(path, library, error) && loaded.actors[0].mesh.materialOverrides == original,
            "Invalid slot payload must preserve the loaded World.");
    };
    auto invalid = document; invalid["actors"][0]["mesh"]["materialOverrides"][0] = -1; reject(invalid);
    invalid = document; invalid["actors"][0]["mesh"]["materialOverrides"][0] = 9999; reject(invalid);
    invalid = document; invalid["actors"][0]["mesh"]["materialOverrides"][0] = true; reject(invalid);
    invalid = document; invalid["actors"][0]["mesh"]["materialOverrideKeys"][1] = "source/front"; reject(invalid);
    invalid = document; invalid["version"] = 4; reject(invalid);
    write(path, document);
    library.meshes[0].materialSlots = {{"Paint", "", 202}, {"Paint", "", 203}};
    require(!world.save(path, library, error) && read(path) == document,
        "Ambiguous fallback slot names fail before replacing a valid scene file.");
    library.meshes[0].materialSlots = {{"Paint", "", 202}, {"Other", "Paint", 203}};
    require(!world.save(path, library, error) && read(path) == document,
        "Explicit source key colliding with fallback name cannot produce an unloadable scene.");
    library.meshes[0] = meshFixture();
    for (const auto& material : library.materials)
        require(assets::writeAssetFile(root/(std::to_string(material.id)+".rasset"), assets::makeMaterialAssetFile(material), error), error);
    require(!assets::deleteContentAsset(library, root, "204.rasset", error) && fs::exists(root/"204.rasset"),
        "Saved slot-only material reference prevents deletion.");
    fs::remove(path);
    require(assets::writeAssetFile(root/"mesh.rasset", assets::makeMeshAssetFile(library.meshes[0]), error), error);
    require(!assets::deleteContentAsset(library, root, "202.rasset", error), "Loaded mesh default material prevents deletion.");
    library.meshes.clear();
    require(!assets::deleteContentAsset(library, root, "202.rasset", error) && fs::exists(root/"202.rasset"),
        "Unloaded saved mesh default material also prevents deletion.");
    require(assets::deleteContentAsset(library, root, "205.rasset", error), "Unused material remains deletable: "+error);
}
}
int main() {
    try {
        Temp temp;
        picking(); sceneAndReferences(temp.path);
        std::cout << "Mesh slot scene tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Mesh slot scene tests failed: " << failure.what() << '\n';
        return 1;
    }
}
