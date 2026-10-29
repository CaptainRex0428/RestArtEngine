// Offline, deterministic authoring tool for the checked-in Path Tracing sample.
// Refuses a nonempty destination so regeneration cannot overwrite user edits.
#include <RestArtEngine/Assets/AssetFile.h>
#include <RestArtEngine/Assets/AssetThumbnail.h>
#include <RestArtEngine/Scene/World.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rae;
using Vec3 = std::array<float, 3>;
using Json = nlohmann::json;
constexpr assets::AssetId FirstMaterial = 0x43424f5800000001ULL;
void require(bool ok, const std::string& error) { if (!ok) throw std::runtime_error(error); }
void writeJson(const std::filesystem::path& path, const Json& value) {
    std::ofstream stream(path, std::ios::binary);
    stream << value.dump(2) << '\n';
    require(static_cast<bool>(stream), "Cannot write " + path.generic_string());
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: GenerateCornellBox <engine-root> <new-project-directory>\n";
        return 2;
    }
    try {
        const auto engine = std::filesystem::absolute(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(argv[1]))));
        const auto output = std::filesystem::absolute(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(argv[2]))));
        require(!std::filesystem::exists(output) || std::filesystem::is_empty(output),
            "Destination must be absent or empty; existing sample edits are never overwritten.");
        Json engineManifest;
        std::ifstream(engine / "Engine.raeengine") >> engineManifest;
        assets::AssetLibrary library;
        std::string error;
        require(library.loadEngineMaterials(engine / "Content/Materials", error), error);
        require(library.loadEngineMeshes(engine / "Content/Meshes", error), error);
        std::filesystem::create_directories(output / "Content/Materials");
        std::filesystem::create_directories(output / "Content/Scenes");
        std::filesystem::create_directories(output / "Shaders/Materials");
        std::filesystem::copy_file(engine / "Shaders/Materials/Standard.hlsli",
            output / "Shaders/Materials/Standard.hlsli");
        assets::AssetId materialOrdinal = 0;
        const auto material = [&](const char* name, Vec3 color, float roughness, float metallic) {
            assets::MaterialAsset asset;
            asset.id = FirstMaterial + materialOrdinal++;
            asset.name = name;
            std::copy(color.begin(), color.end(), asset.baseColor);
            asset.roughness = roughness;
            asset.metallic = metallic;
            asset.grid = false;
            asset.twoSided = true;
            auto file = assets::makeMaterialAssetFile(asset);
            assets::attachThumbnail(file, assets::renderMaterialThumbnail(asset));
            require(assets::writeAssetFile(output / "Content/Materials" / (asset.name + ".rasset"), file, error), error);
            library.materials.push_back(asset);
            return asset.id;
        };
        const auto plaster = material("Neutral plaster", {.64f,.64f,.62f}, .85f, 0);
        const auto red = material("Red wall", {.55f,.035f,.025f}, .85f, 0);
        const auto green = material("Green wall", {.035f,.38f,.055f}, .85f, 0);
        const auto silver = material("Polished silver", {.82f,.84f,.86f}, .07f, 1);
        const auto satin = material("Satin metal", {.58f,.60f,.62f}, .29f, 1);
        const auto mirror = material("Mirror metal", {.94f,.94f,.94f}, .025f, 1);
        scene::World world;
        world.blackBackground = true;
        const auto actor = [&](const char* name, assets::AssetId mesh, assets::AssetId surface,
                               Vec3 position, Vec3 rotation, Vec3 scale) {
            auto* instance = world.findActor(world.createActor(name, mesh, surface));
            std::copy(position.begin(), position.end(), instance->transform.position);
            std::copy(rotation.begin(), rotation.end(), instance->transform.rotation);
            std::copy(scale.begin(), scale.end(), instance->transform.scale);
        };
        constexpr float halfWidth = 2.6f, roomHeight = 4.0f;
        constexpr float planeHorizontal = halfWidth / 12.0f, planeVertical = roomHeight / 24.0f;
        const auto plane = assets::EnginePlaneMeshId;
        actor("Floor", plane, plaster, {0,0,0}, {0,0,0}, {planeHorizontal,1,planeHorizontal});
        actor("Ceiling", plane, plaster, {0,roomHeight,0}, {180,0,0}, {planeHorizontal,1,planeHorizontal});
        actor("Back wall", plane, plaster, {0,2,-halfWidth}, {90,0,0}, {planeHorizontal,1,planeVertical});
        actor("Red wall", plane, red, {-halfWidth,2,0}, {0,0,-90}, {planeVertical,1,planeHorizontal});
        actor("Green wall", plane, green, {halfWidth,2,0}, {0,0,90}, {planeVertical,1,planeHorizontal});
        actor("Silver sphere", assets::EngineSphereMeshId, silver, {-1.28f,2,-.25f}, {0,0,0}, {.60f,.60f,.60f});
        actor("Satin sphere", assets::EngineSphereMeshId, satin, {0,2,-.25f}, {0,0,0}, {.60f,.60f,.60f});
        actor("Mirror sphere", assets::EngineSphereMeshId, mirror, {1.28f,2,-.25f}, {0,0,0}, {.60f,.60f,.60f});
        world.environment.overrideLighting = true;
        world.environment.directionalIntensity = 0;
        world.environment.environmentIntensity = 0;
        auto& light = world.environment.areaLight;
        light.enabled = true;
        light.center[0] = 0; light.center[1] = 3.96f; light.center[2] = -.35f;
        light.halfAxisU[0] = .65f; light.halfAxisU[1] = 0; light.halfAxisU[2] = 0;
        light.halfAxisV[0] = 0; light.halfAxisV[1] = 0; light.halfAxisV[2] = .50f;
        std::fill(std::begin(light.radiance), std::end(light.radiance), 15.0f);
        world.startupCamera = scene::SceneCamera{};
        world.startupCamera->distance = 6.0f;
        world.startupCamera->target[1] = 2.0f;
        world.startupCamera->exposure = .38f;
        require(world.save(output / "Content/Scenes/CornellBox.rascene", library, error), error);
        const auto relativeEngine = std::filesystem::relative(engine, output).generic_string();
        writeJson(output / "PathTracing_CornellBox.raproject", {
            {"format","RAEngine.Project"}, {"version",3}, {"name","PathTracing_CornellBox"},
            {"engineRoot",relativeEngine}, {"engineAssociation",engineManifest.at("engineVersion")},
            {"engineVersion",engineManifest.at("engineVersion")}, {"engineHash",engineManifest.at("engineHash")},
            {"contentRoot","Content"}, {"shaderRoot","Shaders"}, {"defaultScene","Content/Scenes/CornellBox.rascene"},
            {"rendering",{{"hardwareRayTracing",true}, {"globalIllumination",{
                {"method","pathTracing"}, {"mode","progressive"}, {"indirectDiffuse",true}, {"indirectSpecular",true},
                {"samplesPerFrame",4}, {"targetSamples",1024}, {"maxBounces",8}}}}}});
        // Validate the serialized scene, actual container bytes and Engine GUIDs.
        assets::AssetLibrary loaded;
        require(loaded.loadEngineMaterials(engine / "Content/Materials", error), error);
        require(loaded.loadEngineMeshes(engine / "Content/Meshes", error), error);
        require(loaded.loadMaterials(output / "Content/Materials", error), error);
        scene::World roundtrip;
        require(roundtrip.load(output / "Content/Scenes/CornellBox.rascene", loaded, error), error);
        require(roundtrip.actors.size() == 8 && roundtrip.environment.areaLight.enabled && roundtrip.startupCamera.has_value(),
            "Sample roundtrip lost its scene, light or camera.");
        std::cout << "Generated Cornell Box: 8 Actors, 6 materials, Engine meshes, one rectangular light.\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
