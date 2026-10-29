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
using namespace rae::assets;
using namespace rae::scene;
using Json = nlohmann::json;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_camera_persistence_" + assetIdText(generateAssetId()));
    TemporaryTree() { fs::create_directory(root); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_camera_persistence_"))
            fs::remove_all(root, ignored);
    }
};
Json readScene(const fs::path& path) { std::ifstream input(path); return Json::parse(input); }
void writeScene(const fs::path& path, const Json& value) { std::ofstream output(path); output << value.dump(2); }
std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void roundtripAndLifecycle(const fs::path& root) {
    AssetLibrary assets;
    World world;
    const auto perspective = world.createCamera("Perspective camera");
    const auto orthographic = world.createCamera({});
    auto* actor = world.findActor(orthographic);
    require(actor && actor->camera && !actor->light && !actor->mesh.meshId && !actor->mesh.materialId,
        "Camera creation supplies a lens without light or dummy asset references.");
    actor->camera->projection = rae::camera::Projection::Orthographic;
    actor->camera->verticalFovDegrees = 67;
    actor->camera->orthographicHeight = 12;
    actor->camera->nearClip = .3f;
    actor->camera->farClip = 500;
    actor->camera->aspectRatio = 16.f / 9;
    actor->transform.position[0] = 12;
    actor->transform.rotation[2] = 27;
    actor->transform.scale[0] = -2;
    actor->visible = false;
    world.defaultCameraActor = orthographic;
    world.startupCamera.emplace();
    world.startupCamera->target[1] = 4;
    const auto path = root / "cameras.rascene";
    std::string error;
    require(world.save(path, assets, error), "Camera-only scene saves without assets: " + error);
    const auto baseline = readScene(path);
    require(baseline.at("version") == 6 && baseline.at("defaultCameraActor") == orthographic &&
        baseline.at("meshes").empty() && baseline.at("materials").empty(), "V6 camera-only scene has no asset dependencies.");
    for (const auto& item : baseline.at("actors"))
        require(item.contains("camera") && !item.contains("mesh") && !item.contains("light"),
            "Camera Actor serializes only its camera component.");
    World loaded;
    require(loaded.load(path, assets, error), "Load Camera Actors: " + error);
    require(loaded.save(path, assets, error) && readScene(path) == baseline,
        "Camera lens, transform, visibility, default reference and legacy opening view roundtrip.");
    const auto duplicate = loaded.duplicateActor(orthographic);
    require(duplicate && duplicate != perspective && duplicate != orthographic &&
        loaded.defaultCameraActor == orthographic && loaded.findActor(duplicate)->camera->orthographicHeight == 12,
        "Duplicating a camera copies the lens without changing the authored default.");
    loaded.findActor(duplicate)->camera->orthographicHeight = 2;
    require(loaded.findActor(orthographic)->camera->orthographicHeight == 12, "Camera copies have independent lens values.");
    require(loaded.removeActor(duplicate) && loaded.defaultCameraActor == orthographic,
        "Deleting another camera preserves the default reference.");
    require(loaded.removeActor(orthographic) && loaded.defaultCameraActor == 0 && !loaded.removeActor(orthographic),
        "Deleting the default camera clears its reference once.");
    require(loaded.save(path, assets, error) && !readScene(path).contains("defaultCameraActor"),
        "Cleared default is omitted from serialized output.");
    assets.createDefaults();
    loaded.defaultCameraActor = loaded.createCamera("Reset me");
    loaded.createDefaultScene(assets);
    require(!loaded.defaultCameraActor && !loaded.startupCamera, "Creating a default scene clears camera presentation state.");
}
void legacyAndOpeningViews(const fs::path& root) {
    AssetLibrary assets;
    World world;
    const auto path = root / "legacy.rascene";
    std::string error;
    for (int version = 1; version <= 5; ++version) {
        Json value = {{"format", "RAEngine.Scene"}, {"version", version}, {"actors", Json::array()},
            {"materials", Json::array()}, {"meshes", Json::array()},
            {"camera", {{"yaw", .25}, {"pitch", -.2}, {"distance", 8}, {"target", {1,2,3}}, {"exposure", .8}}}};
        writeScene(path, value);
        world.defaultCameraActor = world.createCamera("Old camera");
        require(world.load(path, assets, error) && world.actors.empty() && !world.defaultCameraActor &&
            world.startupCamera && !world.startupCamera->pose && world.startupCamera->lens.verticalFovDegrees == 50,
            "Legacy v1-v5 views load with default lens and no Actor binding.");
    }
    require(world.save(path, assets, error), "Save legacy opening view.");
    require(readScene(path).at("version") == 3 && !readScene(path).at("camera").contains("lens"),
        "Unchanged legacy orbit bookmark keeps its old serialized shape.");
    world.startupCamera->distance = 250;
    world.startupCamera->lens.projection = rae::camera::Projection::Orthographic;
    world.startupCamera->lens.orthographicHeight = 180;
    world.startupCamera->lens.farClip = 2000;
    world.startupCamera->pose.emplace();
    world.startupCamera->pose->position[1] = 120;
    world.startupCamera->pose->rotation[0] = 90;
    require(world.save(path, assets, error) && readScene(path).at("version") == 6, "Extended opening view selects scene v6.");
    const auto authored = readScene(path);
    World loaded;
    require(loaded.load(path, assets, error) && loaded.startupCamera->pose &&
        loaded.startupCamera->pose->position[1] == 120 && loaded.startupCamera->lens.orthographicHeight == 180 &&
        loaded.startupCamera->distance == 250, "Large-distance orthographic/free-flight bookmark roundtrips.");
    require(loaded.save(path, assets, error) && readScene(path) == authored, "Extended opening view is serialization stable.");
    auto oldVersion = authored; oldVersion["version"] = 5;
    const auto bad = root / "old-extension.rascene";
    writeScene(bad, oldVersion);
    require(!loaded.load(bad, assets, error) && loaded.save(path, assets, error) && readScene(path) == authored,
        "Old version cannot silently discard new lens/pose fields or replace the active view.");
    loaded.startupCamera->pose->rotation[0] = std::numeric_limits<float>::infinity();
    require(!loaded.save(path, assets, error) && readScene(path) == authored, "Nonfinite bookmark pose cannot overwrite a valid scene.");
    for (const float distance : {.001f, 1000000.f}) for (const float pitch : {-1.5707964f, 1.5707964f}) {
        World boundary;
        boundary.startupCamera.emplace();
        boundary.startupCamera->distance = distance;
        boundary.startupCamera->pitch = pitch;
        require(boundary.save(path, assets, error) && readScene(path).at("version") == 6,
            "Extended near/far orbit and exact pole bookmarks select v6.");
        require(loaded.load(path, assets, error) && loaded.startupCamera->distance == distance &&
            loaded.startupCamera->pitch == pitch, "Navigation distance and pitch boundaries survive a roundtrip.");
    }
}
void mixedSceneCompatibility(const fs::path& root) {
    AssetLibrary assets; assets.createDefaults();
    World world; world.createDefaultScene();
    world.actors.front().mesh.materialOverrides = {2};
    world.createLight("Existing light", LightType::Spot);
    const auto path = root / "mixed.rascene";
    std::string error;
    require(world.save(path, assets, error), "Save mesh slots and light before adding a camera: " + error);
    const auto previous = readScene(path);
    require(previous.at("version") == 5, "Material slot scene retains version 5.");
    const auto oldPath = root / "previous-v5.rascene";
    writeScene(oldPath, previous);
    world.defaultCameraActor = world.createCamera("Camera alongside slots and lights");
    require(world.save(path, assets, error), "Save camera alongside mesh slots and lights: " + error);
    const auto mixed = readScene(path);
    require(mixed.at("version") == 6 && mixed.at("meshes") == previous.at("meshes") &&
        mixed.at("materials") == previous.at("materials"), "Camera upgrades the schema without altering asset inventory.");
    World restored;
    require(restored.load(path, assets, error) && restored.actors.size() == 4 && restored.actors[2].light &&
        restored.actors[3].camera && restored.actors.front().mesh.materialOverrides == std::vector<AssetId>{2},
        "Mesh overrides, Light and Camera coexist after loading v6.");
    require(restored.save(path, assets, error) && readScene(path) == mixed, "Mixed scene serialization remains stable.");
    require(restored.load(oldPath, assets, error) && !restored.defaultCameraActor && restored.actors.size() == 3 &&
        restored.actors.front().mesh.materialOverrides == std::vector<AssetId>{2}, "Loading v5 replaces prior camera state and retains slots.");
}
void invalidDataIsAtomic(const fs::path& root) {
    AssetLibrary assets; assets.createDefaults();
    World world; world.createDefaultScene();
    const auto id = world.createCamera("Keep this camera");
    world.defaultCameraActor = id;
    const auto path = root / "atomic.rascene";
    const auto bad = root / "invalid.rascene";
    std::string error;
    require(world.save(path, assets, error), "Prime atomic camera fixture.");
    const auto baseline = readScene(path);
    const auto baselineBytes = bytes(path);
    const auto material = makeMaterialAssetFile(assets.materials.front()).manifest;
    const auto geometry = assets.meshes.front().geometryKey();
    const auto reject = [&](const Json& value, const std::string& reason) {
        writeScene(bad, value);
        require(!world.load(bad, assets, error) && !error.empty(), reason);
        require(world.save(path, assets, error) && bytes(path) == baselineBytes &&
            makeMaterialAssetFile(assets.materials.front()).manifest == material &&
            assets.meshes.front().geometryKey() == geometry, "Rejected load preserves World and assets: " + reason);
    };
    for (const auto field : {"verticalFovDegrees", "orthographicHeight", "nearClip", "farClip", "aspectRatio"}) {
        auto invalid = baseline; invalid["actors"][2]["camera"][field] = -1; reject(invalid, std::string("Negative ") + field);
        invalid["actors"][2]["camera"][field] = true; reject(invalid, std::string("Boolean ") + field);
        invalid["actors"][2]["camera"].erase(field); reject(invalid, std::string("Missing ") + field);
    }
    auto invalid = baseline; invalid["actors"][2]["camera"]["projection"] = "fisheye"; reject(invalid, "Unknown projection.");
    invalid = baseline; invalid["actors"][2]["camera"]["verticalFovDegrees"] = 180; reject(invalid, "Degenerate FOV.");
    invalid = baseline; invalid["actors"][2]["camera"]["farClip"] = .05; reject(invalid, "Far must exceed near.");
    invalid = baseline; invalid["actors"][2]["camera"]["nearClip"] = 0; reject(invalid, "Near must be positive.");
    invalid = baseline; invalid["actors"][2]["mesh"] = baseline["actors"][0]["mesh"]; reject(invalid, "Camera plus mesh.");
    invalid = baseline; invalid["actors"][2]["light"] = Json::object(); reject(invalid, "Camera plus light.");
    invalid = baseline; invalid["actors"][2]["transform"]["position"][0] = 1e20; reject(invalid, "Extreme finite Camera position.");
    invalid = baseline; invalid["actors"][2]["transform"]["rotation"][1] = -1e20; reject(invalid, "Extreme finite Camera rotation.");
    invalid = baseline; invalid["version"] = 5; reject(invalid, "Camera in an old version.");
    for (const Json value : {Json(99999), Json(world.actors.front().id), Json(-1), Json(true), Json(1.5), Json("1")}) {
        invalid = baseline; invalid["defaultCameraActor"] = value; reject(invalid, "Invalid default camera reference.");
    }
    const auto actor = *world.findActor(id);
    const auto rejectSave = [&](const char* reason) {
        require(!world.save(path, assets, error) && !error.empty() && bytes(path) == baselineBytes, reason);
        *world.findActor(id) = actor; world.defaultCameraActor = id;
    };
    world.findActor(id)->camera->nearClip = std::numeric_limits<float>::quiet_NaN(); rejectSave("NaN lens cannot overwrite a scene.");
    world.findActor(id)->camera->farClip = std::numeric_limits<float>::infinity(); rejectSave("Infinite lens cannot overwrite a scene.");
    world.findActor(id)->camera->projection = static_cast<rae::camera::Projection>(42); rejectSave("Invalid enum cannot overwrite a scene.");
    world.findActor(id)->transform.position[0] = 1e20f; rejectSave("Extreme finite Camera position cannot overwrite a scene.");
    world.findActor(id)->transform.rotation[2] = -1e20f; rejectSave("Extreme finite Camera rotation cannot overwrite a scene.");
    world.findActor(id)->camera->nearClip = 1e20f;
    world.findActor(id)->camera->farClip = 2e20f; rejectSave("Overflowing near/far lens cannot overwrite a scene.");
    world.findActor(id)->mesh.meshId = 1; rejectSave("Camera cannot acquire hidden mesh references.");
    world.findActor(id)->light.emplace(); rejectSave("Camera cannot acquire a light component.");
    world.defaultCameraActor = world.actors.front().id; rejectSave("Default must point to a Camera Actor.");
    world.defaultCameraActor = 99999; rejectSave("Default cannot dangle.");
    invalid = baseline; invalid["defaultCameraActor"] = 0; writeScene(bad, invalid);
    require(world.load(bad, assets, error) && !world.defaultCameraActor, "Explicit zero default camera is accepted.");
}
void depthOfFieldRoundtrip(const fs::path& root) {
    AssetLibrary assets;
    World world;
    const auto id = world.createCamera("Depth of field camera");
    world.defaultCameraActor = id;
    const auto path = root / "dof.rascene";
    std::string error;
    require(world.save(path, assets, error), "Save old-compatible camera.");
    const auto legacy = readScene(path);
    require(legacy.at("version") == 6 && !legacy.at("actors")[0].at("camera").contains("depthOfField"),
        "Default disabled DOF preserves existing v6 camera schema.");
    World restored;
    require(restored.load(path, assets, error) && restored.findActor(id)->camera->depthOfField == rae::camera::DepthOfField{},
        "Old v6 Camera Actor defaults to disabled DOF.");
    auto& dof = world.findActor(id)->camera->depthOfField;
    dof.enabled = true; dof.focusDistance = 12; dof.fStop = 1.4f; dof.sensorHeightMm = 36;
    require(world.save(path, assets, error), "Save authored DOF camera: " + error);
    const auto authored = readScene(path);
    require(authored.at("version") == 7 && authored.at("actors")[0].at("camera").at("depthOfField").at("enabled") == true,
        "Authored DOF selects v7 and writes a complete effect object.");
    require(restored.load(path, assets, error) && restored.findActor(id)->camera->depthOfField == dof &&
        restored.defaultCameraActor == id, "DOF values and default output camera survive roundtrip.");
    require(restored.save(path, assets, error) && readScene(path) == authored, "Authored DOF roundtrip has stable JSON.");
    dof.enabled = false;
    require(world.save(path, assets, error) && readScene(path).at("version") == 7 && restored.load(path, assets, error) &&
        !restored.findActor(id)->camera->depthOfField.enabled && restored.findActor(id)->camera->depthOfField.fStop == 1.4f,
        "Disabling DOF preserves authored tuning and keeps v7 protection.");
    world.findActor(id)->camera->projection = rae::camera::Projection::Orthographic;
    dof.enabled = true;
    dof.focusDistance = .01f;
    require(world.save(path, assets, error) && restored.load(path, assets, error) &&
        restored.findActor(id)->camera->projection == rae::camera::Projection::Orthographic &&
        restored.findActor(id)->camera->depthOfField == dof,
        "Orthographic and focus-before-focal-length requests remain authored data; rendering determines effective support.");
    writeScene(path, legacy);
    require(restored.load(path, assets, error) && restored.findActor(id)->camera->depthOfField == rae::camera::DepthOfField{},
        "Loading legacy v6 clears a previous DOF request instead of retaining stale state.");

    World bookmark;
    bookmark.startupCamera.emplace();
    bookmark.startupCamera->lens.depthOfField.enabled = true;
    bookmark.startupCamera->lens.depthOfField.focusDistance = 7.5f;
    require(bookmark.save(path, assets, error), "Save DOF-only change to opening view: " + error);
    const auto opening = readScene(path);
    require(opening.at("version") == 7 && opening.at("actors").empty() &&
        opening.at("camera").at("lens").at("depthOfField").at("focusDistance") == 7.5f,
        "DOF-only opening view causes lens serialization without creating a Camera Actor.");
    require(restored.load(path, assets, error) && restored.startupCamera &&
        restored.startupCamera->lens.depthOfField == bookmark.startupCamera->lens.depthOfField,
        "Opening view DOF roundtrips through shared lens parsing.");
    bookmark.startupCamera->lens.depthOfField.enabled = false;
    require(bookmark.save(path, assets, error) && readScene(path).at("version") == 7 && restored.load(path, assets, error) &&
        restored.startupCamera->lens.depthOfField == bookmark.startupCamera->lens.depthOfField,
        "An opening bookmark retains disabled but nondefault DOF tuning.");
    bookmark.startupCamera->lens.depthOfField = {};
    require(bookmark.save(path, assets, error) && readScene(path).at("version") == 3 &&
        !readScene(path).at("camera").contains("lens"), "Restoring all defaults returns an old-compatible orbit bookmark.");
}
void depthOfFieldInvalidData(const fs::path& root) {
    AssetLibrary assets;
    World world;
    const auto id = world.createCamera("Keep authored focus");
    world.findActor(id)->camera->depthOfField.enabled = true;
    world.startupCamera.emplace();
    world.startupCamera->lens.depthOfField.enabled = true;
    const auto path = root / "dof-atomic.rascene", bad = root / "dof-invalid.rascene";
    std::string error;
    require(world.save(path, assets, error), "Prime DOF atomic fixture.");
    const auto baseline = readScene(path);
    const auto baselineBytes = bytes(path);
    const auto reject = [&](const Json& value, const std::string& reason) {
        writeScene(bad, value);
        require(!world.load(bad, assets, error) && !error.empty(), reason);
        require(world.save(path, assets, error) && bytes(path) == baselineBytes,
            "Rejected DOF input preserves the prior World: " + reason);
    };
    for (const bool opening : {false, true}) {
        const auto lens = [&](Json& scene) -> Json& { return opening ? scene["camera"]["lens"] : scene["actors"][0]["camera"]; };
        for (const auto field : {"focusDistance", "fStop", "sensorHeightMm"}) {
            auto invalid = baseline; lens(invalid)["depthOfField"][field] = 0; reject(invalid, std::string("Zero DOF ") + field);
            invalid = baseline; lens(invalid)["depthOfField"][field] = true; reject(invalid, std::string("Boolean DOF ") + field);
            invalid = baseline; lens(invalid)["depthOfField"][field] = 1e30; reject(invalid, std::string("Oversized DOF ") + field);
            invalid = baseline; lens(invalid)["depthOfField"].erase(field); reject(invalid, std::string("Missing DOF ") + field);
        }
        auto invalid = baseline; lens(invalid)["depthOfField"]["enabled"] = 1; reject(invalid, "Numeric DOF enabled is rejected.");
        invalid = baseline; lens(invalid)["depthOfField"].erase("enabled"); reject(invalid, "Missing DOF enabled is rejected.");
        invalid = baseline; lens(invalid)["depthOfField"] = Json::array(); reject(invalid, "DOF must be an object.");
        invalid = baseline; lens(invalid)["depthOfField"]["focusDistance"] = nullptr; reject(invalid, "Null DOF distance is rejected.");
        invalid = baseline; invalid["version"] = 6;
        if (opening) invalid["actors"][0]["camera"].erase("depthOfField");
        else invalid["camera"]["lens"].erase("depthOfField");
        reject(invalid, "DOF cannot be silently parsed as v6.");
        // Removing only one object still leaves the other v7 object present;
        // a clean absent object on v7 must load the default values for that lens.
        invalid = baseline; lens(invalid).erase("depthOfField"); writeScene(bad, invalid);
        World absent;
        require(absent.load(bad, assets, error), "V7 lenses may omit default DOF.");
        const auto& settings = opening ? absent.startupCamera->lens.depthOfField : absent.findActor(id)->camera->depthOfField;
        require(settings == rae::camera::DepthOfField{}, "Omitted v7 DOF uses explicit defaults.");
    }
    for (const bool opening : {false, true}) {
        auto& lens = opening ? world.startupCamera->lens : *world.findActor(id)->camera;
        const auto before = lens;
        for (int field = 0; field < 3; ++field) {
            float& value = field == 0 ? lens.depthOfField.focusDistance : field == 1 ? lens.depthOfField.fStop : lens.depthOfField.sensorHeightMm;
            value = field == 1 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
            require(!world.save(path, assets, error) && !error.empty() && bytes(path) == baselineBytes,
                "Nonfinite in-memory DOF cannot overwrite a saved scene.");
            lens = before;
        }
    }
}
}

int main() {
    try {
        TemporaryTree files;
        roundtripAndLifecycle(files.root);
        legacyAndOpeningViews(files.root);
        mixedSceneCompatibility(files.root);
        invalidDataIsAtomic(files.root);
        depthOfFieldRoundtrip(files.root);
        depthOfFieldInvalidData(files.root);
        std::cout << "Camera persistence: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
