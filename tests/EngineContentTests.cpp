#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetRegistry.h"
#include "RestArtEngine/Scene/World.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <limits>
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

struct EngineMeshFixture { AssetId id; const char* name; MeshPrimitive primitive; };
constexpr std::array engineMeshes{
    EngineMeshFixture{EngineCubeMeshId, "Cube", MeshPrimitive::Cube},
    EngineMeshFixture{EngineSphereMeshId, "Sphere", MeshPrimitive::Sphere},
    EngineMeshFixture{EngineCylinderMeshId, "Cylinder", MeshPrimitive::Imported},
    EngineMeshFixture{EngineConeMeshId, "Cone", MeshPrimitive::Imported},
    EngineMeshFixture{EngineTorusMeshId, "Torus", MeshPrimitive::Imported},
    EngineMeshFixture{EnginePlaneMeshId, "Plane", MeshPrimitive::Plane}};

std::string meshState(const AssetLibrary& library) {
    std::string state;
    for (const auto& mesh : library.meshes) {
        const auto file = makeMeshAssetFile(mesh);
        state += file.manifest.dump();
        state += mesh.origin == AssetOrigin::Engine ? "Engine" : "Project";
        state.append(reinterpret_cast<const char*>(file.blobs.data()), file.blobs.size());
    }
    return state;
}

void loadEngineMeshes(AssetLibrary& library, const fs::path& content) {
    std::string error;
    require(library.loadEngineMeshes(content / "Meshes", error), "Mount distributed engine meshes: " + error);
}

void meshGeometry(const MeshAsset& mesh) {
    require(!mesh.vertices.empty() && !mesh.indices.empty() && mesh.indices.size() % 3 == 0,
        mesh.name + " has indexed triangle geometry.");
    using Point = std::array<long long, 3>;
    std::vector<Point> points;
    std::array<double, 3> minimum{1e20, 1e20, 1e20}, maximum{-1e20, -1e20, -1e20};
    double minRadial = std::numeric_limits<double>::max();
    for (const auto& vertex : mesh.vertices) {
        Point point{};
        double normalLengthSquared = 0;
        for (int axis = 0; axis < 3; ++axis) {
            require(std::isfinite(vertex.position[axis]) && std::isfinite(vertex.normal[axis]),
                mesh.name + " has finite vertices and normals.");
            normalLengthSquared += double(vertex.normal[axis]) * vertex.normal[axis];
            minimum[axis] = std::min(minimum[axis], double(vertex.position[axis]));
            maximum[axis] = std::max(maximum[axis], double(vertex.position[axis]));
            point[axis] = std::llround(double(vertex.position[axis]) * 100000.0);
        }
        require(std::abs(normalLengthSquared - 1.0) < .001, mesh.name + " has unit shading normals.");
        minRadial = std::min(minRadial, std::hypot(double(vertex.position[0]), double(vertex.position[2])));
        points.push_back(point);
    }
    std::map<std::pair<Point, Point>, std::pair<int, int>> edges;
    double signedVolume = 0;
    bool bottomCap = false, topCap = false;
    for (std::size_t offset = 0; offset < mesh.indices.size(); offset += 3) {
        const auto ia = mesh.indices[offset], ib = mesh.indices[offset + 1], ic = mesh.indices[offset + 2];
        require(ia < mesh.vertices.size() && ib < mesh.vertices.size() && ic < mesh.vertices.size(),
            mesh.name + " triangle indices stay within the vertex buffer.");
        const auto& a = mesh.vertices[ia]; const auto& b = mesh.vertices[ib]; const auto& c = mesh.vertices[ic];
        const std::array<double, 3> ab{b.position[0]-a.position[0], b.position[1]-a.position[1], b.position[2]-a.position[2]};
        const std::array<double, 3> ac{c.position[0]-a.position[0], c.position[1]-a.position[1], c.position[2]-a.position[2]};
        const std::array<double, 3> cross{ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
        double areaSquared = 0, normalDot = 0;
        for (int axis = 0; axis < 3; ++axis) {
            areaSquared += cross[axis] * cross[axis];
            normalDot += cross[axis] * (a.normal[axis] + b.normal[axis] + c.normal[axis]);
            signedVolume += double(a.position[axis]) * cross[axis] / 6.0;
        }
        require(areaSquared > 1e-14 && normalDot > 0, mesh.name + " triangles are nondegenerate with outward winding.");
        for (const auto edge : {std::pair{ia, ib}, std::pair{ib, ic}, std::pair{ic, ia}}) {
            auto start = points[edge.first], end = points[edge.second];
            const int orientation = start < end ? 1 : -1;
            if (end < start) std::swap(start, end);
            auto& incidence = edges[{start, end}];
            ++incidence.first; incidence.second += orientation;
        }
        const auto onY = [&](double y) {
            return std::abs(a.position[1] - y) < 1e-5 && std::abs(b.position[1] - y) < 1e-5 &&
                std::abs(c.position[1] - y) < 1e-5;
        };
        bottomCap |= onY(minimum[1]) && cross[1] < 0;
        topCap |= onY(maximum[1]) && cross[1] > 0;
    }
    require(signedVolume > 0, mesh.name + " encloses positive volume with outward-facing triangles.");
    require(std::all_of(edges.begin(), edges.end(), [](const auto& edge) {
        return edge.second.first == 2 && edge.second.second == 0;
    }), mesh.name + " forms a closed surface even across duplicated normal/seam vertices.");
    for (int axis = 0; axis < 3; ++axis)
        require(minimum[axis] < 0 && maximum[axis] > 0 && std::abs(minimum[axis] + maximum[axis]) < .001,
            mesh.name + " is centered at the origin with finite three-dimensional bounds.");
    if (mesh.id == EngineCylinderMeshId) require(bottomCap && topCap, "Cylinder has both outward-facing caps.");
    if (mesh.id == EngineConeMeshId) require(bottomCap, "Cone has an outward-facing base cap.");
    if (mesh.id == EngineTorusMeshId) require(minRadial > .1, "Torus retains its central opening around the Y axis.");
}

void engineMeshAssets(const fs::path& root, const fs::path& engine) {
    std::string error;
    AssetLibrary library; library.createDefaults(); loadEngineMeshes(library, engine);
    for (const auto& fixture : engineMeshes) {
        const auto* mesh = library.findMesh(fixture.id);
        require(mesh && mesh->name == fixture.name && mesh->primitive == fixture.primitive &&
            mesh->origin == AssetOrigin::Engine && mesh->sourcePath.empty(),
            std::string(fixture.name) + " has stable engine identity and self-contained geometry.");
        if (fixture.id == EnginePlaneMeshId) {
            const auto* legacyPlane = library.findMesh(2);
            require(legacyPlane && makeMeshAssetFile(*mesh).blobs == makeMeshAssetFile(*legacyPlane).blobs,
                "Engine Plane preserves the legacy 24-by-24 ground geometry, normals, and indices exactly.");
        } else meshGeometry(*mesh);
        Thumbnail thumbnail;
        require(readAssetThumbnail(engine / "Meshes" / (std::string(fixture.name) + ".rasset"), thumbnail, error) &&
            thumbnail.valid(), std::string(fixture.name) + " includes a portable Content thumbnail.");
    }
    const auto mounted = meshState(library);
    require(library.loadEngineMeshes(engine / "Meshes", error) && meshState(library) == mounted,
        "Repeated engine mesh mounts preserve identities and do not duplicate entries.");
    require(library.findMesh(1) && library.findMesh(2) && library.findMesh(3),
        "Legacy Sphere, Plane, and Cube project identities survive the engine mount.");
    const auto project = root / "Project/Meshes";
    auto copy = *library.findMesh(EngineCylinderMeshId);
    copy.id = 44; copy.origin = AssetOrigin::Project;
    library.meshes.push_back(copy);
    require(library.saveMeshes(project, error) && library.ensureBuiltinMeshes(project / "Builtin", error),
        "Project saves both imported and legacy built-in meshes alongside the engine mount: " + error);
    std::size_t savedMeshes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(project)) {
        if (!entry.is_regular_file() || !isAssetContainerFile(entry.path())) continue;
        AssetFile file;
        require(readAssetFileHeader(entry.path(), file, error) && !isEngineAssetId(file.id),
            "Neither project mesh save nor legacy built-in materialization copies Engine assets.");
        ++savedMeshes;
    }
    require(savedMeshes == 4, "Project persistence contains only its independent copy and three legacy primitives.");
    require(library.loadMeshes(project, error) && library.findMesh(44), "Project mesh copy reloads: " + error);
    for (const auto& fixture : engineMeshes)
        require(library.findMesh(fixture.id) && library.findMesh(fixture.id)->origin == AssetOrigin::Engine,
            "Project reload preserves mounted engine meshes.");
    const auto unchanged = meshState(library);
    auto writeMesh = [&](const fs::path& path, AssetId id) {
        auto mesh = *library.findMesh(EngineCylinderMeshId); mesh.id = id;
        require(writeAssetFile(path, makeMeshAssetFile(mesh), error), "Write isolated mesh fixture: " + error);
    };
    writeMesh(root / "Collision/Override.rasset", EngineCylinderMeshId);
    require(!library.loadMeshes(root / "Collision", error) && !error.empty() && meshState(library) == unchanged,
        "Reserved project mesh collision fails transactionally.");
    require(!library.loadEngineMeshes(root / "Missing", error) && meshState(library) == unchanged,
        "Missing engine mesh mount fails transactionally.");
    textFile(root / "Broken/Bad.rasset", "broken mesh");
    require(!library.loadEngineMeshes(root / "Broken", error) && meshState(library) == unchanged,
        "Damaged engine mesh mount fails transactionally.");
    fs::create_directories(root / "BadEngine");
    fs::copy(engine / "Meshes", root / "BadEngine/Meshes", fs::copy_options::recursive);
    writeMesh(root / "BadEngine/Meshes/ProjectId.rasset", 45);
    require(!library.loadEngineMeshes(root / "BadEngine/Meshes", error) && meshState(library) == unchanged,
        "Engine mesh mount rejects ordinary project identities transactionally.");
    fs::create_directories(root / "DuplicateEngine");
    fs::copy(engine / "Meshes", root / "DuplicateEngine/Meshes", fs::copy_options::recursive);
    writeMesh(root / "DuplicateEngine/Meshes/Duplicate.rasset", EngineCylinderMeshId);
    require(!library.loadEngineMeshes(root / "DuplicateEngine/Meshes", error) && meshState(library) == unchanged,
        "Duplicate engine mesh identities fail transactionally.");
    fs::create_directories(root / "IncompleteEngine");
    fs::copy(engine / "Meshes", root / "IncompleteEngine/Meshes", fs::copy_options::recursive);
    require(fs::remove(root / "IncompleteEngine/Meshes/Torus.rasset"), "Remove only the isolated Torus fixture.");
    require(!library.loadEngineMeshes(root / "IncompleteEngine/Meshes", error) && meshState(library) == unchanged,
        "A mount missing one required engine mesh fails without partially replacing the library.");
    fs::copy_file(engine / "Meshes/Torus.rasset", root / "IncompleteEngine/Meshes/Torus.rasset");
    require(fs::remove(root / "IncompleteEngine/Meshes/Plane.rasset"), "Remove only the isolated Plane fixture.");
    require(!library.loadEngineMeshes(root / "IncompleteEngine/Meshes", error) && meshState(library) == unchanged,
        "Engine Plane is required by a complete mount and missing it fails transactionally.");
    fs::create_directories(root / "InvalidGeometry");
    fs::copy(engine / "Meshes", root / "InvalidGeometry/Meshes", fs::copy_options::recursive);
    auto malformed = *library.findMesh(EngineCubeMeshId);
    malformed.indices[0] = static_cast<std::uint32_t>(malformed.vertices.size());
    require(writeAssetFile(root / "InvalidGeometry/Meshes/Cube.rasset", makeMeshAssetFile(malformed), error),
        "Write an isolated malformed geometry fixture: " + error);
    require(!library.loadEngineMeshes(root / "InvalidGeometry/Meshes", error) && meshState(library) == unchanged,
        "Out-of-range geometry in a complete engine mount fails transactionally.");
    auto invalid = library;
    invalid.findMesh(EngineCylinderMeshId)->origin = AssetOrigin::Project;
    require(!invalid.saveMeshes(root / "InvalidSave", error), "Save refuses project meshes impersonating engine identity.");

    const auto objPath = root / "Sources/Triangle.obj";
    textFile(objPath, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetId importedId = 0;
    require(library.importObj(objPath, importedId, error), "Import OBJ directly with engine meshes mounted: " + error);
    require(importedId > 3 && !isEngineAssetId(importedId) && library.findMesh(importedId) &&
        library.findMesh(importedId)->origin == AssetOrigin::Project,
        "Direct OBJ import allocates a project identity outside the mounted engine reservation.");
    require(library.saveMeshes(project, error), "Save direct OBJ import alongside engine meshes: " + error);
    require(fs::remove(objPath), "Remove only the isolated OBJ source before reloading its asset.");
    AssetLibrary importedReload; importedReload.createDefaults(); loadEngineMeshes(importedReload, engine);
    require(importedReload.loadMeshes(project, error) && importedReload.findMesh(importedId) &&
        importedReload.findMesh(importedId)->origin == AssetOrigin::Project &&
        importedReload.findMesh(importedId)->vertices.size() == 3 && importedReload.findMesh(importedId)->indices.size() == 3,
        "Directly imported mesh reloads with its project identity and geometry after its source is removed.");
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
    require(!references(saved, EngineDefaultMaterialId) && references(saved, 1) && !references(saved, 2),
        "Only referenced materials become scene dependencies, for both Engine and Project assets.");

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

void engineMeshScenes(const fs::path& root, const fs::path& engine) {
    std::string error;
    AssetLibrary library; library.createDefaults(); loadEngine(library, engine); loadEngineMeshes(library, engine);
    rae::scene::World world;
    for (const auto& fixture : engineMeshes) world.createActor(fixture.name, fixture.id, EngineDefaultMaterialId);
    const auto scenePath = root / "AllEngineMeshes.rascene";
    require(world.save(scenePath, library, error), "Save all six engine mesh references without import source files: " + error);
    auto saved = Json::parse(bytes(scenePath));
    for (const auto& fixture : engineMeshes) {
        const auto& references = saved.at("meshes");
        const auto reference = std::find_if(references.begin(), references.end(), [&](const auto& item) {
            return item.at("id").template get<AssetId>() == fixture.id;
        });
        require(reference != references.end() && reference->size() == 1,
            "Scene records only the stable Engine mesh reference, without duplicated geometry or source paths.");
    }
    AssetLibrary restored; restored.createDefaults(); loadEngine(restored, engine); loadEngineMeshes(restored, engine);
    rae::scene::World restoredWorld;
    require(restoredWorld.load(scenePath, restored, error) && restoredWorld.actors.size() == engineMeshes.size(),
        "All six Engine meshes round-trip through a scene in an otherwise empty project: " + error);
    for (std::size_t index = 0; index < engineMeshes.size(); ++index)
        require(restoredWorld.actors[index].mesh.meshId == engineMeshes[index].id &&
            restored.findMesh(engineMeshes[index].id)->origin == AssetOrigin::Engine,
            "Scene reload preserves each actor's selected engine mesh and trusted mount ownership.");
    AssetLibrary missing; missing.createDefaults(); loadEngine(missing, engine);
    rae::scene::World unchanged; unchanged.createActor("Keep Existing", 1, 1);
    const auto missingState = meshState(missing);
    require(!unchanged.load(scenePath, missing, error) && unchanged.actors.size() == 1 &&
        unchanged.actors.front().name == "Keep Existing" && meshState(missing) == missingState,
        "Missing engine mesh references fail without altering the scene or mesh library.");

    world.actors.clear(); world.createActor("Legacy Sphere", 1, 1);
    require(world.save(scenePath, library, error), "Save project-only scene with engine meshes mounted: " + error);
    saved = Json::parse(bytes(scenePath));
    require(std::none_of(saved.at("meshes").begin(), saved.at("meshes").end(), [](const auto& item) {
        return isEngineAssetId(item.at("id").template get<AssetId>());
    }), "Unused engine meshes do not become scene dependencies.");
    require(saved.at("meshes").size() == 1 && saved.at("meshes")[0].at("id").get<AssetId>() == 1,
        "A used legacy Sphere reference is retained while unused legacy Plane and Cube references are omitted.");
    const auto legacyBase = saved;
    for (const int version : {1, 2}) {
        saved = legacyBase; saved["version"] = version;
        saved["materials"] = version == 1 ? Json::array({{{"id", 1}, {"name", "Legacy"},
            {"baseColor", {.8f, .2f, .1f}}, {"roughness", .5f}, {"metallic", .2f}}}) : Json::array({{{"id", 1}}});
        saved["meshes"] = Json::array({{{"id", 1}, {"name", "Sphere"}, {"primitive", "Sphere"}}});
        textFile(scenePath, saved.dump());
        require(restoredWorld.load(scenePath, restored, error), "Legacy scene version " + std::to_string(version) + " loads: " + error);
        for (const auto& fixture : engineMeshes)
            require(restored.findMesh(fixture.id) && restored.findMesh(fixture.id)->origin == AssetOrigin::Engine,
                "Legacy scene replacement preserves all mounted engine meshes.");
        const auto before = meshState(restored);
        saved["meshes"][0]["id"] = EngineSphereMeshId;
        saved["actors"][0]["mesh"]["meshId"] = EngineSphereMeshId;
        textFile(scenePath, saved.dump());
        require(!restoredWorld.load(scenePath, restored, error) && meshState(restored) == before &&
            restoredWorld.actors.front().mesh.meshId == 1,
            "Legacy scene cannot redefine an Engine mesh; rejection preserves world and assets.");
    }
}

void engineDefaultScene(const fs::path& root, const fs::path& engine) {
    std::string error;
    AssetLibrary library; library.createDefaults(); loadEngine(library, engine); loadEngineMeshes(library, engine);
    rae::scene::World world; world.createDefaultScene(library);
    require(world.actors.size() == 2 && world.actors[0].mesh.meshId == EngineSphereMeshId &&
        world.actors[1].mesh.meshId == EnginePlaneMeshId,
        "New default scenes use the mounted Engine Sphere and Plane assets.");
    const auto content = root / "Content";
    const auto scenePath = content / "Scenes/Default.rascene";
    require(library.saveMaterials(content / "Materials", error) && library.saveMeshes(content / "Meshes", error) &&
        world.save(scenePath, library, error), "Save a default project backed by Engine meshes: " + error);
    const auto saved = Json::parse(bytes(scenePath));
    require(saved.at("meshes").size() == 2 && std::all_of(saved.at("meshes").begin(), saved.at("meshes").end(),
        [](const auto& item) {
            const auto id = item.at("id").template get<AssetId>();
            return id == EngineSphereMeshId || id == EnginePlaneMeshId;
        }), "Default scene dependencies contain only its used Engine meshes, without unused legacy IDs.");
    require(!fs::exists(content / "Meshes/Builtin"),
        "Ordinary default-project persistence does not materialize a project Meshes/Builtin folder.");
    require(std::none_of(fs::recursive_directory_iterator(content), fs::recursive_directory_iterator(),
        [](const auto& entry) { return entry.is_regular_file() && entry.path().parent_path().filename() == "Meshes"; }),
        "A default project references engine geometry without writing mesh asset copies.");
    AssetLibrary restored; restored.createDefaults(); loadEngine(restored, engine); loadEngineMeshes(restored, engine);
    rae::scene::World reloaded;
    require(reloaded.load(scenePath, restored, error) && reloaded.actors.size() == 2 &&
        reloaded.actors[0].mesh.meshId == EngineSphereMeshId && reloaded.actors[1].mesh.meshId == EnginePlaneMeshId,
        "An Engine-backed default scene reloads without project built-in mesh files: " + error);
    AssetLibrary legacy; legacy.createDefaults(); world.createDefaultScene(legacy);
    require(world.actors.size() == 2 && world.actors[0].mesh.meshId == 1 && world.actors[1].mesh.meshId == 2,
        "Legacy callers without an engine mount retain the Sphere and Plane fallback identities.");
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
    require(std::find(engineFolders.begin(), engineFolders.end(), "engine://Meshes") != engineFolders.end(),
        "Engine root exposes its Meshes folder.");
    for (const auto& fixture : engineMeshes) {
        const auto* mesh = registry.find("engine://Meshes/" + std::string(fixture.name) + ".rasset");
        require(mesh && mesh->id == fixture.id && mesh->type == AssetType::Mesh && mesh->readOnly() &&
            mesh->origin == AssetOrigin::Engine && mesh->thumbnail.valid(),
            std::string(fixture.name) + " is a read-only engine mesh with a Content thumbnail.");
    }
    query.folder = "engine://Meshes"; query.type = AssetType::Mesh;
    require(registry.query(query).size() == engineMeshes.size(), "Engine mesh browser lists all six reusable meshes.");
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
        engineMeshAssets(temporary.root / "MeshAssets", engine);
        sceneReferences(temporary.root / "Scenes", engine);
        engineMeshScenes(temporary.root / "MeshScenes", engine);
        engineDefaultScene(temporary.root / "DefaultProject", engine);
        registryMounts(temporary.root / "Registry", engine);
        require(treeBytes(engine) == originalEngine, "All save/reload/copy operations leave distributed Engine Content byte-for-byte unchanged.");
        std::cout << "Engine Content tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Engine Content tests failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
