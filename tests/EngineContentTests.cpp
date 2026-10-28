#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetRegistry.h"
#include "RestArtEngine/Scene/World.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
using Json = nlohmann::json;
int checks = 0;

void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_engine_content_test_" + assetIdText(generateAssetId()));
    TemporaryTree() {
        if (!fs::create_directory(root)) throw std::runtime_error("Cannot create isolated test tree.");
    }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() &&
            root.filename().string().starts_with("rae_engine_content_test_")) fs::remove_all(root, ignored);
    }
};

std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read fixture: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void textFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    require(bool(stream), "Write isolated fixture.");
}

std::map<fs::path, std::string> treeBytes(const fs::path& root) {
    std::map<fs::path, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) result.emplace(entry.path().lexically_relative(root), bytes(entry.path()));
    return result;
}

std::string materialState(const AssetLibrary& library) {
    Json state = Json::array();
    for (const auto& material : library.materials) {
        auto item = makeMaterialAssetFile(material).manifest;
        item["testOrigin"] = material.origin == AssetOrigin::Engine ? "Engine" : "Project";
        state.push_back(std::move(item));
    }
    return state.dump();
}

void writeMaterial(const fs::path& path, AssetId id, const std::string& name) {
    MaterialAsset material;
    material.id = id; material.name = name;
    std::string error;
    const bool written = writeAssetFile(path, makeMaterialAssetFile(material), error);
    require(written, "Write material fixture: " + error);
}

void loadEngine(AssetLibrary& library, const fs::path& content) {
    std::string error;
    const bool loaded = library.loadEngineMaterials(content / "Materials", error);
    require(loaded, "Load distributed Engine Content: " + error);
}

void loadingAndCopies(const fs::path& root, const fs::path& engine) {
    std::string error;
    AssetLibrary library; library.createDefaults();
    loadEngine(library, engine);
    const auto* builtin = library.findMaterial(EngineDefaultMaterialId);
    require(builtin && builtin->origin == AssetOrigin::Engine, "Default material has engine ownership.");
    require(builtin->baseColor[0] == builtin->baseColor[1] && builtin->baseColor[1] == builtin->baseColor[2] &&
        builtin->roughness > 0 && builtin->metallic == 0 && !builtin->grid && !builtin->twoSided,
        "Distributed default is a neutral, opaque standard material.");
    const auto engineValue = makeMaterialAssetFile(*builtin).manifest.dump();
    const auto newId = library.createMaterial("New project material");
    const auto* created = library.findMaterial(newId);
    require(created && created->origin == AssetOrigin::Project && !isEngineAssetId(newId) &&
        created->baseColor[0] == .5f && created->baseColor[1] == .5f && created->baseColor[2] == .5f &&
        created->metallic == 0 && created->roughness == .6f,
        "New project materials inherit engine default parameters with independent identity.");

    const auto projectMaterials = root / "Project/Materials";
    writeMaterial(projectMaterials / "DefaultMaterial.rasset", 42, library.findMaterial(EngineDefaultMaterialId)->name);
    require(library.loadMaterials(projectMaterials, error), "Project materials load alongside Engine Content: " + error);
    require(library.findMaterial(42) && library.findMaterial(42)->origin == AssetOrigin::Project &&
        library.findMaterial(EngineDefaultMaterialId), "Same-name project and engine materials retain distinct identities.");
    require(library.loadMaterials(projectMaterials, error) && library.materials.size() == 2,
        "Repeated project reload preserves exactly one engine material.");

    const auto copiedId = library.cloneMaterial(EngineDefaultMaterialId, "Editable Copy");
    require(copiedId && copiedId != EngineDefaultMaterialId && !isEngineAssetId(copiedId),
        "Copy receives a new project identity outside the engine reservation.");
    auto* copy = library.findMaterial(copiedId);
    require(copy && copy->origin == AssetOrigin::Project && copy->name == "Editable Copy" &&
        copy->baseColor[0] == library.findMaterial(EngineDefaultMaterialId)->baseColor[0],
        "Copy preserves values and clears engine ownership.");
    copy->baseColor[0] = .1f; copy->roughness = .93f;
    const auto createdId = library.createMaterial("Another Material");
    require(!isEngineAssetId(createdId) && createdId != copiedId, "Creating project materials avoids reserved engine identities.");
    require(library.saveMaterials(projectMaterials, error), "Save editable project assets: " + error);
    for (const auto& entry : fs::recursive_directory_iterator(projectMaterials)) {
        if (!entry.is_regular_file() || !isAssetContainerFile(entry.path())) continue;
        AssetFile file;
        require(readAssetFileHeader(entry.path(), file, error) && !isEngineAssetId(file.id),
            "Project save never serializes mounted engine materials.");
    }
    require(makeMaterialAssetFile(*library.findMaterial(EngineDefaultMaterialId)).manifest.dump() == engineValue,
        "Editing the project copy leaves the engine material unchanged.");
    AssetLibrary reloaded; reloaded.createDefaults(); loadEngine(reloaded, engine);
    require(reloaded.loadMaterials(projectMaterials, error), "Reload saved project copies: " + error);
    require(reloaded.findMaterial(copiedId) && reloaded.findMaterial(copiedId)->roughness == .93f &&
        reloaded.findMaterial(copiedId)->origin == AssetOrigin::Project, "Copied material edits survive reload.");

    fs::create_directories(root / "Empty/Materials");
    require(reloaded.loadMaterials(root / "Empty/Materials", error) && reloaded.materials.size() == 1 &&
        reloaded.findMaterial(EngineDefaultMaterialId), "An empty project can rely entirely on Engine Content.");
    const auto unchanged = materialState(reloaded);
    writeMaterial(root / "Collision/Materials/Override.rasset", EngineDefaultMaterialId, "Project Override");
    require(!reloaded.loadMaterials(root / "Collision/Materials", error) && !error.empty() &&
        materialState(reloaded) == unchanged, "Project ID collision fails without replacing loaded engine values.");
    textFile(root / "Broken/Materials/Broken.rasset", "not a material container");
    require(!reloaded.loadMaterials(root / "Broken/Materials", error) && materialState(reloaded) == unchanged,
        "Broken project load preserves the library.");
    require(!reloaded.loadEngineMaterials(root / "MissingEngine/Materials", error) &&
        materialState(reloaded) == unchanged, "Missing Engine Content reports failure transactionally.");
    require(!reloaded.loadEngineMaterials(root / "Broken/Materials", error) && materialState(reloaded) == unchanged,
        "Damaged Engine Content reports failure transactionally.");
    writeMaterial(root / "NoDefault/Materials/Other.rasset", EngineDefaultMaterialId + 1, "Other Builtin");
    require(!reloaded.loadEngineMaterials(root / "NoDefault/Materials", error) && materialState(reloaded) == unchanged,
        "An engine mount without its required default is rejected.");
    fs::create_directories(root / "BadEngine");
    fs::copy(engine / "Materials", root / "BadEngine/Materials", fs::copy_options::recursive);
    writeMaterial(root / "BadEngine/Materials/ProjectId.rasset", 42, "Invalid Builtin");
    require(!reloaded.loadEngineMaterials(root / "BadEngine/Materials", error) && materialState(reloaded) == unchanged,
        "Engine files cannot claim ordinary project IDs.");
    fs::create_directories(root / "DuplicateEngine");
    fs::copy(engine / "Materials", root / "DuplicateEngine/Materials", fs::copy_options::recursive);
    writeMaterial(root / "DuplicateEngine/Materials/Duplicate.rasset", EngineDefaultMaterialId, "Duplicate Default");
    require(!reloaded.loadEngineMaterials(root / "DuplicateEngine/Materials", error) && materialState(reloaded) == unchanged,
        "Duplicate engine identities fail without partially installing assets.");
    auto invalidProject = reloaded;
    invalidProject.findMaterial(EngineDefaultMaterialId)->origin = AssetOrigin::Project;
    require(!invalidProject.saveMaterials(root / "InvalidSave/Materials", error),
        "Persistence rejects project assets that claim reserved engine IDs.");
}

bool references(const Json& scene, AssetId id) {
    return std::any_of(scene.at("materials").begin(), scene.at("materials").end(),
        [id](const auto& item) { return item.at("id").template get<AssetId>() == id; });
}

void sceneReferences(const fs::path& root, const fs::path& engine) {
    std::string error;
    AssetLibrary library; library.createDefaults(); loadEngine(library, engine);
    rae::scene::World world;
    const auto actorId = world.createActor("Engine Material User", 1, EngineDefaultMaterialId);
    const auto scenePath = root / "Scenes/EngineMaterial.rascene";
    require(world.save(scenePath, library, error), "Save scene with engine material reference: " + error);
    auto saved = Json::parse(bytes(scenePath));
    require(saved.at("version") == 3 && references(saved, EngineDefaultMaterialId),
        "Scene v3 persists the fixed engine ID without embedding a material copy.");
    AssetLibrary restored; restored.createDefaults(); loadEngine(restored, engine);
    rae::scene::World restoredWorld;
    require(restoredWorld.load(scenePath, restored, error) && restoredWorld.findActor(actorId) &&
        restoredWorld.findActor(actorId)->mesh.materialId == EngineDefaultMaterialId &&
        restored.findMaterial(EngineDefaultMaterialId)->origin == AssetOrigin::Engine,
        "Scene reload resolves the engine reference and keeps its ownership.");
    AssetLibrary missingEngine; missingEngine.createDefaults();
    rae::scene::World unchangedWorld;
    unchangedWorld.createActor("Existing Actor", 1, 1);
    require(!unchangedWorld.load(scenePath, missingEngine, error) && unchangedWorld.actors.size() == 1 &&
        unchangedWorld.actors.front().name == "Existing Actor", "Missing engine reference is an error, not a silent material replacement.");

    world.findActor(actorId)->mesh.materialId = 1;
    require(world.save(scenePath, library, error), "Save a project-only scene: " + error);
    saved = Json::parse(bytes(scenePath));
    require(!references(saved, EngineDefaultMaterialId) && references(saved, 1) && references(saved, 2),
        "Unused engine assets do not become scene dependencies; existing project reference policy is preserved.");

    // Version 1 embedded project material payloads. Loading them must preserve
    // the mounted engine while keeping the old project replacement behavior.
    saved["version"] = 1;
    saved["materials"] = Json::array({{{"id", 1}, {"name", "Legacy Copper"},
        {"baseColor", {.8f, .2f, .1f}}, {"roughness", .5f}, {"metallic", .2f}}});
    saved["meshes"] = Json::array({{{"id", 1}, {"name", "Sphere"}, {"primitive", "Sphere"}}});
    textFile(scenePath, saved.dump());
    require(restoredWorld.load(scenePath, restored, error) && restored.findMaterial(1) &&
        restored.findMaterial(1)->name == "Legacy Copper" && !restored.findMaterial(2) &&
        restored.findMaterial(EngineDefaultMaterialId) &&
        restored.findMaterial(EngineDefaultMaterialId)->origin == AssetOrigin::Engine,
        "Legacy embedded materials replace project state without unmounting engine assets.");
    const auto state = materialState(restored);
    saved["materials"][0]["id"] = EngineDefaultMaterialId;
    saved["actors"][0]["mesh"]["materialId"] = EngineDefaultMaterialId;
    textFile(scenePath, saved.dump());
    require(!restoredWorld.load(scenePath, restored, error) && materialState(restored) == state &&
        restoredWorld.findActor(actorId)->mesh.materialId == 1,
        "Legacy scene cannot overwrite engine identity; failed load keeps world and assets.");
}

void registryMounts(const fs::path& root, const fs::path& engine) {
    writeMaterial(root / "Materials/DefaultMaterial.rasset", 42, "Project Default");
    // A physical folder named Engine remains ordinary project content.
    writeMaterial(root / "Engine/Materials/Local.rasset", 43, "Local");
    AssetRegistry registry;
    registry.setContentRoot(root); registry.setEngineContentRoot(engine);
    std::string error;
    require(registry.scan(error), "Scan both content mounts: " + error);
    const auto* project = registry.find("Materials/DefaultMaterial.rasset");
    const auto* builtin = registry.find("engine://Materials/DefaultMaterial.rasset");
    require(project && builtin && project->id == 42 && builtin->id == EngineDefaultMaterialId &&
        project->origin == AssetOrigin::Project && !project->readOnly() &&
        builtin->origin == AssetOrigin::Engine && builtin->readOnly(),
        "Identical relative filenames resolve independently with trusted source and read-only status.");
    require(registry.find("Engine/Materials/Local.rasset") &&
        registry.find("Engine/Materials/Local.rasset")->origin == AssetOrigin::Project,
        "Project folder names cannot impersonate the engine mount.");
    AssetQuery query; query.recursive = true;
    auto results = registry.query(query);
    require(results.size() == 2 && std::all_of(results.begin(), results.end(), [](const auto* entry) {
        return entry->origin == AssetOrigin::Project;
    }), "Recursive project root search stays inside the project mount.");
    query.folder = "engine://";
    results = registry.query(query);
    require(!results.empty() && std::all_of(results.begin(), results.end(), [](const auto* entry) {
        return entry->origin == AssetOrigin::Engine;
    }), "Recursive engine search stays inside the engine mount.");
    const auto projectFolders = registry.childFolders("");
    require(std::find(projectFolders.begin(), projectFolders.end(), "engine://") == projectFolders.end(),
        "Engine source is a separate root, not a child of Project Content.");
    const auto engineFolders = registry.childFolders("engine://");
    require(std::find(engineFolders.begin(), engineFolders.end(), "engine://Materials") != engineFolders.end(),
        "Engine root exposes its Materials folder.");
    require(!moveContentEntry(root, "engine://Materials/DefaultMaterial.rasset", "Materials", error),
        "File move refuses engine source keys.");
    require(!moveContentEntry(root, "Materials/DefaultMaterial.rasset", "engine://Materials", error) &&
        fs::is_regular_file(root / "Materials/DefaultMaterial.rasset"), "File move refuses engine destinations without moving source.");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Pass the distributed engine Content directory as the only argument.");
        const auto engine = fs::absolute(fs::path(std::u8string(argv[1], argv[1] + std::char_traits<char>::length(argv[1]))));
        const auto originalEngine = treeBytes(engine);
        require(!originalEngine.empty(), "Engine Content is distributed as actual files.");
        TemporaryTree temporary;
        loadingAndCopies(temporary.root / "Assets", engine);
        sceneReferences(temporary.root / "Scenes", engine);
        registryMounts(temporary.root / "Registry", engine);
        require(treeBytes(engine) == originalEngine, "All save/reload/copy operations leave distributed Engine Content byte-for-byte unchanged.");
        std::cout << "Engine Content tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Engine Content tests failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
