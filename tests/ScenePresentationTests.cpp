#include <RestArtEngine/Assets/AssetFile.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() /
        ("rae_scene_presentation_" + rae::assets::assetIdText(rae::assets::generateAssetId()));
    TemporaryTree() { fs::create_directories(root); }
    ~TemporaryTree() { std::error_code ignored; fs::remove_all(root, ignored); }
};
Json read(const fs::path& path) {
    std::ifstream stream(path);
    return Json::parse(stream);
}
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void verify(const fs::path& root) {
    rae::assets::AssetLibrary assets;
    assets.createDefaults();
    rae::scene::World world;
    world.createDefaultScene(assets);
    const auto path = root / "presentation.rascene";
    const auto invalidPath = root / "invalid.rascene";
    std::string error;
    require(world.save(path, assets, error), "Save default scene: " + error);
    const auto defaults = read(path);
    require(!defaults.contains("camera") && !defaults.contains("lighting"),
        "Default scenes omit the optional presentation fields.");

    world.startupCamera = rae::scene::SceneCamera{};
    world.startupCamera->target[1] = 1.7f;
    world.startupCamera->pitch = -.1f;
    world.startupCamera->distance = 7.5f;
    world.startupCamera->exposure = .8f;
    world.environment.overrideLighting = true;
    world.environment.directionalIntensity = 0;
    world.environment.environmentIntensity = .2f;
    world.environment.areaLight.enabled = true;
    world.environment.areaLight.center[1] = 3.8f;
    world.environment.areaLight.halfAxisU[0] = .65f;
    world.environment.areaLight.radiance[0] = 20;
    require(world.save(path, assets, error), "Save authored presentation: " + error);
    const auto baseline = bytes(path);
    rae::scene::World loaded;
    require(loaded.load(path, assets, error), "Load authored presentation: " + error);
    require(loaded.startupCamera && loaded.startupCamera->target[1] == 1.7f &&
        loaded.startupCamera->pitch == -.1f && loaded.startupCamera->distance == 7.5f &&
        loaded.startupCamera->exposure == .8f, "Camera bookmark survives roundtrip.");
    require(loaded.environment.overrideLighting && loaded.environment.directionalIntensity == 0 &&
        loaded.environment.environmentIntensity == .2f && loaded.environment.areaLight.enabled &&
        loaded.environment.areaLight.center[1] == 3.8f && loaded.environment.areaLight.halfAxisU[0] == .65f &&
        loaded.environment.areaLight.radiance[0] == 20, "Lighting values survive roundtrip.");
    require(loaded.save(path, assets, error) && bytes(path) == baseline,
        "Presentation serialization is stable after reload.");

    const auto reject = [&](Json invalid, const char* reason) {
        { std::ofstream stream(invalidPath); stream << invalid.dump(2); }
        require(!loaded.load(invalidPath, assets, error), reason);
        require(loaded.save(path, assets, error) && bytes(path) == baseline,
            "Rejected load retains the previous scene, opening view and light.");
    };
    auto invalid = read(path);
    invalid["camera"]["target"] = {1, 2}; reject(invalid, "Short target vector rejected.");
    invalid = read(path); invalid["camera"]["pitch"] = 2; reject(invalid, "Camera pole pitch rejected.");
    invalid = read(path); invalid["camera"]["distance"] = 0; reject(invalid, "Zero distance rejected.");
    invalid = read(path); invalid["camera"]["yaw"] = true; reject(invalid, "Boolean camera value rejected.");
    invalid = read(path); invalid["camera"]["exposure"] = nullptr; reject(invalid, "Null camera value rejected.");
    invalid = read(path); invalid["lighting"]["areaLight"]["radiance"] = {-1, 2, 3}; reject(invalid, "Negative radiance rejected.");
    invalid = read(path); invalid["lighting"]["areaLight"]["halfAxisU"] = {0, 0, 0}; reject(invalid, "Zero light axis rejected.");
    invalid = read(path); invalid["lighting"]["areaLight"]["halfAxisV"] = {.5, 0, .5}; reject(invalid, "Skew light axes rejected.");
    invalid = read(path); invalid["lighting"]["areaLight"]["enabled"] = 1; reject(invalid, "Nonboolean light switch rejected.");
    invalid = read(path); invalid["lighting"]["directionalIntensity"] = -1; reject(invalid, "Negative directional intensity rejected.");
    invalid = read(path); invalid["lighting"]["environmentIntensity"] = 1e30; reject(invalid, "Excessive environment intensity rejected.");

    loaded.startupCamera->yaw = std::numeric_limits<float>::infinity();
    require(!loaded.save(path, assets, error) && bytes(path) == baseline,
        "Nonfinite in-memory camera cannot replace a published scene.");
    loaded.startupCamera->yaw = 0;
    loaded.environment.areaLight.halfAxisU[0] = 0;
    require(!loaded.save(path, assets, error) && bytes(path) == baseline,
        "Invalid in-memory area light cannot replace a published scene.");

    { std::ofstream stream(invalidPath); stream << defaults.dump(2); }
    require(loaded.load(invalidPath, assets, error) && !loaded.startupCamera && !loaded.environment.overrideLighting,
        "Loading an older scene clears authored presentation.");
    for (bool withLibrary : {false, true}) {
        loaded.startupCamera = rae::scene::SceneCamera{};
        loaded.environment.overrideLighting = true;
        loaded.environment.areaLight.enabled = true;
        if (withLibrary) loaded.createDefaultScene(assets);
        else loaded.createDefaultScene();
        require(!loaded.startupCamera && !loaded.environment.overrideLighting && !loaded.environment.areaLight.enabled,
            "Both default-scene overloads clear authored presentation.");
    }
}
} // namespace

int main() {
    try {
        TemporaryTree files;
        verify(files.root);
        std::cout << "Scene presentation: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
