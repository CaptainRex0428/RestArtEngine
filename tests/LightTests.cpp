#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Scene/World.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::scene;
using namespace rae::assets;
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_light_tests_" + assetIdText(generateAssetId()));
    TemporaryTree() { fs::create_directory(root); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_light_tests_"))
            fs::remove_all(root, ignored);
    }
};
Json readScene(const fs::path& path) { std::ifstream stream(path); return Json::parse(stream); }
void writeScene(const fs::path& path, const Json& json) { std::ofstream stream(path); stream << json.dump(2); }
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void roundtripAndLifecycle(const fs::path& root) {
    AssetLibrary assets;
    World world;
    for (const auto type : {LightType::Directional, LightType::Sky, LightType::Rect, LightType::Spot, LightType::Point}) {
        const auto id = world.createLight({}, type);
        auto* actor = world.findActor(id);
        require(actor && actor->light && actor->light->type == type && !actor->mesh.meshId && !actor->mesh.materialId,
            "Light creation supplies a unique light Actor without asset references.");
        auto& light = *actor->light;
        light.color[0] = .25f; light.color[1] = .5f; light.color[2] = .75f;
        light.intensity = 12.5f; light.range = 7.25f;
        light.innerConeAngle = 10; light.outerConeAngle = 41;
        light.width = 3; light.height = 4; light.castShadows = false;
        actor->transform.position[0] = static_cast<float>(id);
        actor->transform.rotation[1] = 17;
        actor->transform.scale[2] = -2;
        actor->visible = type != LightType::Sky;
    }
    require(world.createLight("Invalid", static_cast<LightType>(999)) == 0 && world.actors.size() == 5,
        "Unknown light type cannot create an Actor.");
    const auto path = root / "lights.rascene";
    std::string error;
    require(world.save(path, assets, error), "Lights save without any mesh/material assets: " + error);
    const auto baseline = readScene(path);
    require(baseline.at("version") == 4 && baseline.at("materials").empty() && baseline.at("meshes").empty(),
        "Light-only v4 scene has empty asset inventories.");
    for (const auto& actor : baseline.at("actors"))
        require(actor.contains("light") && !actor.contains("mesh"), "Light Actor never serializes a dummy mesh.");
    World restored;
    require(restored.load(path, assets, error), "Five light types load: " + error);
    require(restored.save(path, assets, error) && readScene(path) == baseline,
        "All light, visibility, identity and transform fields survive roundtrip.");
    const auto original = restored.actors.front();
    const auto duplicate = restored.duplicateActor(original.id);
    require(duplicate && duplicate != original.id && restored.findActor(duplicate)->light->intensity == original.light->intensity,
        "Duplicate preserves light data and allocates a distinct Actor ID after load.");
    restored.findActor(duplicate)->light->intensity = 99;
    require(restored.findActor(original.id)->light->intensity == 12.5f, "Duplicated light components do not alias.");
    require(restored.removeActor(duplicate) && !restored.removeActor(duplicate) && restored.actors.size() == 5,
        "Light deletion uses the stable Actor lifecycle.");
    require(restored.save(path, assets, error) && readScene(path) == baseline, "Duplicate/delete leaves original scene unchanged.");
}
void legacyAndMixedScenes(const fs::path& root) {
    AssetLibrary assets; assets.createDefaults();
    World world; world.createDefaultScene();
    std::string error;
    const auto path = root / "mixed.rascene";
    require(world.save(path, assets, error), "Save existing mesh-only scene: " + error);
    const auto old = readScene(path);
    require(old.at("version") == 3, "Mesh-only scenes remain version 3.");
    const auto legacy = root / "legacy.rascene";
    writeScene(legacy, old);
    const auto id = world.createLight("Spot", LightType::Spot);
    world.findActor(id)->light->innerConeAngle = world.findActor(id)->light->outerConeAngle;
    require(world.save(path, assets, error), "Equal cone angles support a hard edge.");
    const auto mixed = readScene(path);
    require(mixed.at("version") == 4 && mixed.at("materials") == old.at("materials") && mixed.at("meshes") == old.at("meshes"),
        "Adding lights does not invent mesh/material dependencies.");
    World restored;
    require(restored.load(path, assets, error) && restored.actors.size() == 3 && restored.actors.back().light,
        "Mixed mesh/light scene roundtrips.");
    require(restored.load(legacy, assets, error) && restored.actors.size() == 2 && !restored.actors.front().light,
        "Loading an existing v3 mesh scene resets light state.");
    // v1/v2 empty scenes still follow their existing asset-loading contracts.
    for (int version : {1, 2}) {
        Json empty = {{"format","RAEngine.Scene"},{"version",version},{"actors",Json::array()},
            {"materials",Json::array()},{"meshes",Json::array()}};
        writeScene(legacy, empty);
        require(restored.load(legacy, assets, error) && restored.actors.empty(), "Legacy scene version remains supported.");
    }
}
void rejectedInputIsAtomic(const fs::path& root) {
    AssetLibrary assets; assets.createDefaults();
    World world; world.createDefaultScene();
    const auto id = world.createLight("Keep this light", LightType::Spot);
    std::string error;
    const auto path = root / "atomic.rascene";
    const auto badPath = root / "invalid.rascene";
    require(world.save(path, assets, error), "Prime atomic fixture.");
    const auto original = readScene(path);
    const auto originalBytes = bytes(path);
    const auto originalMaterial = makeMaterialAssetFile(assets.materials.front()).manifest;
    const auto originalGeometry = assets.meshes.front().geometryKey();
    const auto reject = [&](const Json& invalid, const std::string& reason) {
        writeScene(badPath, invalid);
        require(!world.load(badPath, assets, error) && !error.empty(), reason);
        require(world.save(path, assets, error) && bytes(path) == originalBytes,
            "Rejected load preserves the complete previously loaded World: " + reason);
        require(makeMaterialAssetFile(assets.materials.front()).manifest == originalMaterial &&
            assets.meshes.front().geometryKey() == originalGeometry, "Rejected light load preserves assets: " + reason);
    };
    for (const auto field : {"intensity", "range", "width", "height", "innerConeAngle", "outerConeAngle"}) {
        auto invalid = original; invalid["actors"][2]["light"][field] = -1;
        reject(invalid, std::string("Negative ") + field + " rejected.");
        invalid["actors"][2]["light"][field] = true;
        reject(invalid, std::string("Boolean ") + field + " rejected.");
    }
    for (const auto field : {"range", "width", "height"}) {
        auto invalid = original; invalid["actors"][2]["light"][field] = 0;
        reject(invalid, std::string("Zero ") + field + " rejected.");
    }
    auto invalid = original; invalid["actors"][2]["light"]["type"] = "Laser"; reject(invalid, "Unknown type rejected.");
    invalid = original; invalid["actors"][2]["light"]["color"] = {-1, 1, 1}; reject(invalid, "Negative color rejected.");
    invalid = original; invalid["actors"][2]["light"]["color"] = {1, 1}; reject(invalid, "Short color rejected.");
    invalid = original; invalid["actors"][2]["light"]["castShadows"] = 1; reject(invalid, "Numeric shadow flag rejected.");
    invalid = original; invalid["actors"][2]["light"]["innerConeAngle"] = 40; reject(invalid, "Inner cone outside outer cone rejected.");
    invalid = original; invalid["actors"][2]["light"]["outerConeAngle"] = 90; reject(invalid, "90-degree half-cone rejected.");
    invalid = original; invalid["actors"][2]["mesh"] = original["actors"][0]["mesh"]; reject(invalid, "Mixed components rejected.");
    invalid = original; invalid["version"] = 3; reject(invalid, "Light payload cannot be smuggled into version 3.");
    invalid = original; invalid["actors"][2].erase("light"); reject(invalid, "Actor missing both components rejected.");
    invalid = original; invalid["actors"][2]["transform"]["scale"] = {0,1,1}; reject(invalid, "Degenerate transform rejected.");
    const auto saved = *world.findActor(id);
    const auto rejectSave = [&](const char* reason) {
        require(!world.save(path, assets, error) && !error.empty() && bytes(path) == originalBytes, reason);
        *world.findActor(id) = saved;
    };
    world.findActor(id)->light->intensity = std::numeric_limits<float>::infinity();
    rejectSave("Infinite in-memory intensity cannot overwrite the scene.");
    world.findActor(id)->light->color[1] = std::numeric_limits<float>::quiet_NaN();
    rejectSave("NaN color cannot overwrite the scene.");
    world.findActor(id)->light->type = static_cast<LightType>(-1);
    rejectSave("Invalid in-memory enum cannot overwrite the scene.");
    world.findActor(id)->mesh.meshId = 1;
    rejectSave("In-memory light with mesh reference cannot overwrite the scene.");
    world.findActor(id)->light->outerConeAngle = std::numeric_limits<float>::quiet_NaN();
    rejectSave("NaN outer cone cannot overwrite the scene.");
}
} // namespace

int main() {
    try {
        TemporaryTree files;
        roundtripAndLifecycle(files.root);
        legacyAndMixedScenes(files.root);
        rejectedInputIsAtomic(files.root);
        std::cout << "Lighting scene data: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
