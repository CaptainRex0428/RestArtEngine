#include <RAEngine/Assets/AssetLibrary.h>
#include <RAEngine/Scene/World.h>
#include <RAEngine/Project/Project.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using rae::assets::AssetLibrary;
using rae::scene::World;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(float left, float right) { return std::abs(left - right) < 0.0001f; }
std::string utf8Path(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
void write(const fs::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary);
    stream << content;
    require(static_cast<bool>(stream), "Cannot write test fixture: " + path.string());
}
struct ScratchDirectory {
    fs::path path = fs::temp_directory_path() /
        ("RAEngineSceneTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ScratchDirectory() { fs::create_directories(path); }
    ~ScratchDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

void actorLifecycle() {
    World world;
    const auto first = world.createActor("Original", 1, 1);
    const auto second = world.createActor("Other", 3, 2);
    require(first != 0 && second != first, "Actor IDs must be unique and nonzero");
    auto* actor = world.findActor(first);
    require(actor != nullptr, "Created Actor is missing");
    actor->transform.position[0] = 2.5f;
    actor->transform.rotation[1] = 37.0f;
    actor->transform.scale[2] = 1.75f;
    actor->visible = false;
    const auto copyId = world.duplicateActor(first);
    const auto* copy = world.findActor(copyId);
    require(copy && copyId != first && copyId != second, "Duplicate needs a new stable ID");
    require(copy->mesh.meshId == 1 && copy->mesh.materialId == 1 && !copy->visible,
            "Duplicate lost mesh, material, or visibility");
    require(near(copy->transform.rotation[1], 37.0f) && near(copy->transform.scale[2], 1.75f),
            "Duplicate lost rotation or scale");
    require(world.removeActor(second), "Existing Actor could not be removed");
    require(!world.findActor(second) && world.findActor(first) && world.findActor(copyId),
            "Deletion changed unrelated Actor IDs");
    require(!world.removeActor(second), "Deleting a missing Actor should fail");
    require(world.duplicateActor(second) == 0, "Duplicating a missing Actor should return 0");
    require(world.removeActor(copyId), "Cannot remove highest Actor ID");
    const auto next = world.createActor("New", 1, 1);
    require(next > copyId, "Deleted Actor ID was reused");
}

void meshImport(const fs::path& directory) {
    AssetLibrary assets;
    assets.createDefaults();
    require(assets.findMesh(1) && assets.findMesh(2) && assets.findMesh(3), "Missing built-in meshes");
    const auto obj = directory / "negative-indices.obj";
    write(obj, "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf -4 -3 -2 -1\n");
    rae::assets::AssetId id = 0;
    std::string error;
    require(assets.importObj(obj, id, error), "Negative OBJ indices should load: " + error);
    const auto* mesh = assets.findMesh(id);
    require(mesh && mesh->indices.size() == 6, "Quad was not triangulated into two triangles");
    for (const auto index : mesh->indices) require(index < mesh->vertices.size(), "OBJ index out of range");
    for (const auto& vertex : mesh->vertices) {
        const float length = vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] +
            vertex.normal[2] * vertex.normal[2];
        require(std::isfinite(length) && near(length, 1), "Missing OBJ normals were not generated");
    }
    const auto count = assets.meshes.size();
    const auto invalid = directory / "invalid.obj";
    for (const std::string& face : {"f 0 2 3\n", "f 1 2 9\n", "f -4 -2 -1\n", "f nope 2 3\n"}) {
        write(invalid, "v 0 0 0\nv 1 0 0\nv 0 1 0\n" + face);
        error.clear();
        require(!assets.importObj(invalid, id, error), "Invalid OBJ was accepted: " + face);
        require(!error.empty(), "Invalid OBJ produced no diagnostics");
        require(assets.meshes.size() == count, "Failed OBJ import mutated the asset library");
    }
}

void materialPersistence(const fs::path& directory) {
    AssetLibrary assets;
    assets.createDefaults();
    const auto id = assets.createMaterial("Ceramic test");
    auto* material = assets.findMaterial(id);
    require(material != nullptr, "Created material is missing");
    material->baseColor[0] = .13f;
    material->baseColor[1] = .47f;
    material->roughness = .72f;
    material->metallic = .03f;
    std::string error;
    const auto folder = directory / "Materials";
    require(assets.saveMaterials(folder, error), "Cannot save materials: " + error);
    AssetLibrary restored;
    restored.createDefaults();
    require(restored.loadMaterials(folder, error), "Cannot reload materials: " + error);
    const auto* loaded = restored.findMaterial(id);
    require(loaded && loaded->name == "Ceramic test", "Material ID/name did not round-trip");
    require(near(loaded->baseColor[0], .13f) && near(loaded->baseColor[1], .47f) &&
            near(loaded->roughness, .72f) && near(loaded->metallic, .03f),
            "Material parameters did not round-trip");
}

void scenePersistence(const fs::path& directory) {
    AssetLibrary assets;
    assets.createDefaults();
    World world;
    world.createDefaultScene();
    const auto materialId = assets.createMaterial("Scene material");
    assets.findMaterial(materialId)->roughness = .81f;
    const auto importedFile = directory / "scene-mesh.obj";
    write(importedFile, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    rae::assets::AssetId importedId = 0;
    std::string error;
    require(assets.importObj(importedFile, importedId, error), "Cannot import scene fixture: " + error);
    const auto actorId = world.createActor("Persisted Actor", importedId, materialId);
    auto* actor = world.findActor(actorId);
    actor->transform.position[1] = 3.5f;
    actor->transform.rotation[2] = 22.0f;
    actor->transform.scale[0] = .6f;
    actor->visible = false;
    const auto sceneFile = directory / "Scenes" / "roundtrip.scene.json";
    require(world.save(sceneFile, assets, error), "Cannot save scene: " + error);
    World restored;
    AssetLibrary restoredAssets;
    require(restored.load(sceneFile, restoredAssets, error), "Cannot reload scene: " + error);
    const auto* loaded = restored.findActor(actorId);
    require(loaded && loaded->name == "Persisted Actor" && !loaded->visible,
            "Actor identity or visibility did not round-trip");
    require(near(loaded->transform.position[1], 3.5f) && near(loaded->transform.rotation[2], 22.0f) &&
            near(loaded->transform.scale[0], .6f), "Actor transform did not round-trip");
    require(loaded->mesh.meshId == importedId && loaded->mesh.materialId == materialId,
            "Actor asset references did not round-trip");
    require(restoredAssets.findMesh(importedId) && restoredAssets.findMesh(importedId)->indices.size() == 3,
            "Imported mesh source did not reload relative to scene file");
    require(restoredAssets.findMaterial(materialId) && near(restoredAssets.findMaterial(materialId)->roughness, .81f),
            "Scene material did not round-trip");
    const auto actorCount = restored.actors.size();
    const auto materialCount = restoredAssets.materials.size();
    const auto meshCount = restoredAssets.meshes.size();
    const auto broken = directory / "broken.scene.json";
    write(broken, "{ not valid JSON");
    require(!restored.load(broken, restoredAssets, error), "Malformed scene was accepted");
    require(!error.empty(), "Scene parse failure produced no diagnostics");
    require(restored.actors.size() == actorCount && restoredAssets.materials.size() == materialCount &&
            restoredAssets.meshes.size() == meshCount && restored.findActor(actorId) &&
            restored.findActor(actorId)->name == "Persisted Actor", "Failed scene load changed the live world");
    fs::rename(importedFile, directory / "temporarily-missing.obj");
    require(!restored.load(sceneFile, restoredAssets, error), "Scene with missing mesh source was accepted");
    require(restored.actors.size() == actorCount && restoredAssets.meshes.size() == meshCount &&
            restored.findActor(actorId) && near(restored.findActor(actorId)->transform.position[1], 3.5f),
            "Missing mesh source partially replaced the live world");
    require(restored.createActor("After reload", 1, 1) > actorId, "Scene load reset the Actor ID counter");
}

void projectPaths(const fs::path& directory) {
    const auto fixture = directory / "project-paths";
    const auto engine = fixture / "Engine";
    const auto project = fixture / "Project";
    fs::create_directories(engine / "Shaders/Private");
    fs::create_directories(project / "Content/Scenes");
    fs::create_directories(project / "Shaders/Materials");
    fs::create_directories(fixture / "External");
    write(engine / "Shaders/Private/Scene.hlsl", "// fixture");
    write(engine / "Engine.raeengine",
          "{\"format\":\"RAEngine.Engine\",\"version\":1,\"name\":\"Fixture Engine\","
          "\"engineVersion\":\"1.2.3\",\"engineHash\":\"fixture-a\"}");
    write(project / "Content/Scenes/Default.rascene", "{}");
    write(fixture / "external.scene", "{}");
    const auto manifest = project / "Fixture.raproject";
    const auto manifestText = [](const std::string& content = "Content",
                                 const std::string& shaders = "Shaders",
                                 const std::string& scene = "Content/Scenes/Default.rascene",
                                 const std::string& engineRoot = "../Engine") {
        return std::string("{\"format\":\"RAEngine.Project\",\"version\":1,\"name\":\"Test project\",") +
            "\"engineRoot\":\"" + engineRoot + "\",\"contentRoot\":\"" + content +
            "\",\"shaderRoot\":\"" + shaders + "\",\"defaultScene\":\"" + scene + "\"}";
    };
    write(manifest, manifestText());
    rae::project::ProjectPaths paths;
    std::string error;
    require(rae::project::loadProject(manifest, paths, error), "Valid project manifest failed: " + error);
    require(paths.name == "Test project" && paths.projectRoot == fs::canonical(project) &&
            paths.engineRoot == fs::canonical(engine) && paths.contentRoot == fs::canonical(project / "Content") &&
            paths.shaderRoot == fs::canonical(project / "Shaders") &&
            paths.defaultScene == fs::canonical(project / "Content/Scenes/Default.rascene"),
            "Project-relative paths were not resolved to their proper absolute roots");
    require(paths.engine.manifestPresent && paths.engine.version == "1.2.3" && paths.engine.hash == "fixture-a" &&
                rae::project::checkEngineCompatibility(paths).state == rae::project::EngineCompatibility::LegacyProject,
            "Engine identity or legacy project compatibility was not detected");
    write(manifest, manifestText() + "\n");
    write(manifest, std::string("{\"format\":\"RAEngine.Project\",\"version\":2,\"name\":\"Test project\",") +
                      "\"engineRoot\":\"../Engine\",\"engineVersion\":\"1.2.3\",\"engineHash\":\"fixture-a\",\"contentRoot\":\"Content\",\"shaderRoot\":\"Shaders\",\"defaultScene\":\"Content/Scenes/Default.rascene\"}");
    require(rae::project::loadProject(manifest, paths, error), "Bound project manifest failed: " + error);
    require(rae::project::checkEngineCompatibility(paths).state == rae::project::EngineCompatibility::Compatible,
            "Matching engine identity was rejected");
    auto changedEngine = paths.engine;
    changedEngine.hash = "fixture-b";
    require(rae::project::checkEngineCompatibility(paths, changedEngine).state ==
                rae::project::EngineCompatibility::HashMismatch,
            "Engine hash mismatch was not detected");
    const bool updated = rae::project::updateProjectEngineBinding(manifest, paths.engine, error);
    require(updated, "Updating project engine binding failed: " + error);
    rae::project::ProjectPaths fromDirectory;
    require(rae::project::loadProject(project, fromDirectory, error) && fromDirectory.projectRoot == paths.projectRoot,
            "Project directory did not resolve its single manifest");
    require(fromDirectory.engineRoot == fs::canonical(engine) && fromDirectory.engineVersion == "1.2.3" &&
                fromDirectory.engineHash == "fixture-a",
            "Updating project engine binding wrote an incorrect relative engine link");
    rae::project::ProjectBuildFiles buildFiles;
    require(rae::project::writeProjectBuildFiles(fromDirectory, buildFiles, error),
            "Project-local solution generation failed: " + error);
    require(buildFiles.solution == project / "Test project.sln" &&
                buildFiles.buildManifest == project / "RAEngineProject.json" &&
                fs::is_regular_file(buildFiles.solution) && fs::is_regular_file(buildFiles.buildManifest),
            "Project-local solution files were not created");
    std::ifstream buildDescription(buildFiles.buildManifest);
    const std::string buildDescriptionText((std::istreambuf_iterator<char>(buildDescription)), {});
    require(buildDescriptionText.find("fixture-a") != std::string::npos &&
                buildDescriptionText.find("RAEngine.ProjectBuild") != std::string::npos,
            "Project build metadata did not preserve the engine identity");
    std::ifstream solutionDescription(buildFiles.solution);
    const std::string solutionText((std::istreambuf_iterator<char>(solutionDescription)), {});
    require(solutionText.find("Microsoft Visual Studio Solution File") != std::string::npos &&
                solutionText.find("Fixture.raproject") != std::string::npos,
            "Generated project solution is not a valid project container");
    const auto nestedProject = engine / "Projects" / "Nested";
    fs::create_directories(nestedProject / "Content/Scenes");
    fs::create_directories(nestedProject / "Shaders/Materials");
    write(nestedProject / "Content/Scenes/Default.rascene", "{}");
    const auto nestedManifest = nestedProject / "Nested.raproject";
    write(nestedManifest,
          "{\"format\":\"RAEngine.Project\",\"version\":2,\"name\":\"Nested\","
          "\"engineRoot\":\"../..\",\"engineVersion\":\"1.2.3\",\"engineHash\":\"fixture-a\","
          "\"contentRoot\":\"Content\",\"shaderRoot\":\"Shaders\","
          "\"defaultScene\":\"Content/Scenes/Default.rascene\"}");
    rae::project::ProjectPaths nestedPaths;
    require(rae::project::loadProject(nestedManifest, nestedPaths, error),
            "Nested project manifest failed before rebinding: " + error);
    require(rae::project::updateProjectEngineBinding(nestedManifest, nestedPaths.engine, error),
            "Nested project engine rebinding failed: " + error);
    require(rae::project::loadProject(nestedManifest, nestedPaths, error) && nestedPaths.engineRoot == fs::canonical(engine),
            "Nested project engine rebinding wrote a reversed relative path: " + error);
    auto changedVersion = paths.engine;
    changedVersion.version = "9.9.9";
    require(rae::project::checkEngineCompatibility(paths, changedVersion).state ==
                rae::project::EngineCompatibility::VersionMismatch,
            "Engine version mismatch was not detected");
    auto missingIdentity = paths.engine;
    missingIdentity.manifestPresent = false;
    require(rae::project::checkEngineCompatibility(paths, missingIdentity).state ==
                rae::project::EngineCompatibility::EngineManifestMissing,
            "Missing engine identity was not detected");
    const auto unchanged = paths;
    const auto expectFailure = [&](const fs::path& input) {
        require(!rae::project::loadProject(input, paths, error), "Invalid project input was accepted: " + input.string());
        require(!error.empty(), "Invalid project produced no diagnostic");
        require(paths.name == unchanged.name && paths.projectRoot == unchanged.projectRoot &&
                paths.engineRoot == unchanged.engineRoot && paths.contentRoot == unchanged.contentRoot &&
                paths.shaderRoot == unchanged.shaderRoot && paths.defaultScene == unchanged.defaultScene,
                "Failed project resolution changed previously resolved paths");
    };
    write(manifest, manifestText("../External"));
    expectFailure(manifest);
    write(manifest, manifestText("Content", (fixture / "External").generic_string()));
    expectFailure(manifest);
    write(manifest, manifestText("Content", "Shaders", "../external.scene"));
    expectFailure(manifest);
    write(manifest, manifestText("Content", "Shaders", "Content/Scenes/Default.rascene", "../MissingEngine"));
    expectFailure(manifest);
    write(manifest, "{ broken JSON");
    expectFailure(manifest);
    write(manifest, manifestText());
    write(project / "Ambiguous.raproject", manifestText());
    expectFailure(project);
    fs::remove(project / "Ambiguous.raproject");
    fs::remove(project / "Content/Scenes/Default.rascene");
    expectFailure(manifest);
}

void unicodePaths(const fs::path& directory) {
    const auto fixture = directory / fs::path(u8"\u4e2d\u6587\u8def\u5f84");
    const auto engine = fixture / fs::path(u8"\u5f15\u64ce");
    const auto project = fixture / fs::path(u8"\u5de5\u7a0b");
    const auto contentRelative = fs::path(u8"\u8d44\u4ea7");
    const auto shaderRelative = fs::path(u8"\u7740\u8272\u5668");
    const auto sceneRelative = contentRelative / fs::path(u8"\u573a\u666f/\u9ed8\u8ba4.rascene");
    const auto obj = project / contentRelative / fs::path(u8"\u7f51\u683c/\u6d4b\u8bd5\u6a21\u578b.obj");
    fs::create_directories(engine / "Shaders/Private");
    fs::create_directories(project / sceneRelative.parent_path());
    fs::create_directories(project / shaderRelative / "Materials");
    fs::create_directories(obj.parent_path());
    write(engine / "Shaders/Private/Scene.hlsl", "// fixture");
    write(obj, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetLibrary assets;
    assets.createDefaults();
    rae::assets::AssetId id = 0;
    std::string error;
    require(assets.importObj(obj, id, error), "Cannot import OBJ from Unicode path: " + error);
    const auto* mesh = assets.findMesh(id);
    require(mesh && mesh->name == utf8Path(obj.stem()) && mesh->sourcePath == utf8Path(fs::absolute(obj)),
            "Imported OBJ name/source path did not preserve UTF-8");
    World world;
    const auto actorId = world.createActor("Unicode mesh Actor", id, 1);
    require(world.save(project / sceneRelative, assets, error), "Cannot save scene to Unicode path: " + error);
    World restored;
    AssetLibrary restoredAssets;
    require(restored.load(project / sceneRelative, restoredAssets, error), "Cannot reload Unicode mesh reference: " + error);
    require(restored.findActor(actorId) && restoredAssets.findMesh(id) &&
            restoredAssets.findMesh(id)->name == utf8Path(obj.stem()), "Unicode mesh identity changed on scene reload");
    const auto manifest = project / fs::path(u8"\u793a\u4f8b.raproject");
    write(manifest, std::string("{\"format\":\"RAEngine.Project\",\"version\":1,\"name\":\"Unicode project\",") +
        "\"engineRoot\":\"../" + utf8Path(engine.filename()) + "\",\"contentRoot\":\"" + utf8Path(contentRelative) +
        "\",\"shaderRoot\":\"" + utf8Path(shaderRelative) + "\",\"defaultScene\":\"" + utf8Path(sceneRelative) + "\"}");
    rae::project::ProjectPaths paths;
    require(rae::project::loadProject(project, paths, error), "Cannot resolve Unicode project paths: " + error);
    require(paths.engineRoot == fs::canonical(engine) && paths.contentRoot == fs::canonical(project / contentRelative) &&
            paths.shaderRoot == fs::canonical(project / shaderRelative) &&
            paths.defaultScene == fs::canonical(project / sceneRelative), "UTF-8 project manifest paths resolved incorrectly");
}

void engineRegistryAndAssociation(const fs::path& directory) {
    const auto fixture = directory / "engine-registry";
    const auto engineA = fixture / "Engine-0.2.0";
    const auto engineB = fixture / "Engine-0.3.0";
    const auto project = fixture / "Project";
    fs::create_directories(engineA / "Shaders/Private");
    fs::create_directories(engineB / "Shaders/Private");
    fs::create_directories(project / "Content/Scenes");
    fs::create_directories(project / "Shaders/Materials");
    write(engineA / "Shaders/Private/Scene.hlsl", "// fixture A");
    write(engineB / "Shaders/Private/Scene.hlsl", "// fixture B");
    write(engineA / "Engine.raeengine",
          "{\"format\":\"RAEngine.Engine\",\"version\":1,\"name\":\"Engine A\","
          "\"engineVersion\":\"0.2.0\",\"engineHash\":\"hash-a\"}");
    write(engineB / "Engine.raeengine",
          "{\"format\":\"RAEngine.Engine\",\"version\":1,\"name\":\"Engine B\","
          "\"engineVersion\":\"0.3.0\",\"engineHash\":\"hash-b\"}");
    write(project / "Content/Scenes/Default.rascene", "{}");

    rae::project::EngineRegistry registry;
    rae::project::registerEngineInRegistry(registry, "0.2.0", engineA);
    rae::project::registerEngineInRegistry(registry, "0.3.0", engineB);
    rae::project::registerEngineInRegistry(registry, "0.2.0", engineA); // dedupe
    require(registry.engines.size() == 2, "Registry should keep one entry per association.");
    const auto registryPath = fixture / "engines.json";
    std::string error;
    require(rae::project::saveEngineRegistry(registryPath, registry, error), "Cannot save engine registry: " + error);
    rae::project::EngineRegistry restored;
    require(rae::project::loadEngineRegistry(registryPath, restored, error), "Cannot reload engine registry: " + error);
    require(restored.engines.size() == 2, "Registry round-trip lost entries.");
    rae::project::EngineRegistryEntry found;
    require(rae::project::findEngineByAssociation(restored, "0.3.0", found), "findEngineByAssociation failed.");
    require(found.root == fs::absolute(engineB), "Registry resolved the wrong engine root.");
    require(!rae::project::findEngineByAssociation(restored, "9.9.9", found), "Unknown association should not resolve.");

    write(project / "Demo.raproject",
          "{\"format\":\"RAEngine.Project\",\"version\":3,\"name\":\"Demo\","
          "\"engineRoot\":\"../Engine-0.2.0\",\"engineVersion\":\"0.2.0\",\"engineHash\":\"hash-a\","
          "\"engineAssociation\":\"0.2.0\",\"contentRoot\":\"Content\",\"shaderRoot\":\"Shaders\","
          "\"defaultScene\":\"Content/Scenes/Default.rascene\"}");
    rae::project::ProjectPaths paths;
    require(rae::project::loadProject(project / "Demo.raproject", paths, error), "v3 manifest failed: " + error);
    require(paths.engineAssociation == "0.2.0", "engineAssociation did not round-trip.");
    rae::project::EngineIdentity identityB;
    require(rae::project::loadEngineIdentity(engineB, identityB, error), "Cannot load engine B identity: " + error);
    require(rae::project::updateProjectEngineBinding(project / "Demo.raproject", identityB, error),
            "Rebinding to a different engine version failed: " + error);
    rae::project::ProjectPaths rebind;
    require(rae::project::loadProject(project / "Demo.raproject", rebind, error), "Rebound project failed: " + error);
    require(rebind.engineVersion == "0.3.0" && rebind.engineHash == "hash-b" && rebind.engineAssociation == "0.3.0",
            "Rebinding did not write the new version/association.");
    require(rebind.engineRoot == fs::canonical(engineB), "Rebinding did not rewrite engineRoot.");

    // A stale engine root must not load directly, but its manifest info still
    // exposes the association so the launcher can recover via the registry.
    write(project / "Stale.raproject",
          "{\"format\":\"RAEngine.Project\",\"version\":3,\"name\":\"Stale\","
          "\"engineRoot\":\"../Missing-Engine\",\"engineVersion\":\"0.2.0\",\"engineHash\":\"hash-a\","
          "\"engineAssociation\":\"0.2.0\",\"contentRoot\":\"Content\",\"shaderRoot\":\"Shaders\","
          "\"defaultScene\":\"Content/Scenes/Default.rascene\"}");
    rae::project::ProjectPaths stalePaths;
    require(!rae::project::loadProject(project / "Stale.raproject", stalePaths, error),
            "A project with a missing engine root should not load directly.");
    rae::project::ProjectManifestInfo info;
    require(rae::project::loadProjectManifestInfo(project / "Stale.raproject", info, error),
            "loadProjectManifestInfo should read a project with a stale engine root: " + error);
    require(info.engineAssociation == "0.2.0" && info.name == "Stale", "Manifest info lost association/name.");
    write(project / "Broken.raproject", "{ not json");
    require(!rae::project::loadProjectManifestInfo(project / "Broken.raproject", info, error),
            "Broken manifest should not yield manifest info.");
}

void projectSolutionReferences(const fs::path& directory) {
    const auto fixture = directory / "project-solution";
    const auto engine = fixture / "Engine";
    const auto project = fixture / "Project";
    fs::create_directories(engine / "Shaders/Private");
    fs::create_directories(project / "Content/Scenes");
    fs::create_directories(project / "Shaders/Materials");
    write(engine / "Shaders/Private/Scene.hlsl", "// fixture");
    write(engine / "Engine.raeengine",
          "{\"format\":\"RAEngine.Engine\",\"version\":1,\"name\":\"Engine\","
          "\"engineVersion\":\"0.2.0\",\"engineHash\":\"hash\"}");
    write(engine / "RAEngine.vcxproj",
          "<?xml version=\"1.0\"?><Project><PropertyGroup><ProjectGuid>{6B43E54C-05F1-46E9-A697-BEF48B3D74E8}</ProjectGuid></PropertyGroup></Project>");
    write(project / "Content/Scenes/Default.rascene", "{}");
    write(project / "Demo.raproject",
          "{\"format\":\"RAEngine.Project\",\"version\":3,\"name\":\"Demo\","
          "\"engineRoot\":\"../Engine\",\"engineVersion\":\"0.2.0\",\"engineHash\":\"hash\","
          "\"engineAssociation\":\"0.2.0\",\"contentRoot\":\"Content\",\"shaderRoot\":\"Shaders\","
          "\"defaultScene\":\"Content/Scenes/Default.rascene\"}");
    rae::project::ProjectPaths paths;
    std::string error;
    require(rae::project::loadProject(project / "Demo.raproject", paths, error), "Project failed: " + error);
    rae::project::ProjectBuildFiles files;
    require(rae::project::writeProjectBuildFiles(paths, files, error), "Cannot generate build files: " + error);
    std::ifstream solution(files.solution);
    const std::string text((std::istreambuf_iterator<char>(solution)), {});
    require(text.find("RAEngine.vcxproj") != std::string::npos &&
            text.find("{6B43E54C-05F1-46E9-A697-BEF48B3D74E8}") != std::string::npos &&
            text.find("RAEngineRuntime.vcxproj") == std::string::npos,
            "Generated solution should reference only the single RAEngine project.");
    std::ifstream buildDescription(files.buildManifest);
    const std::string buildText((std::istreambuf_iterator<char>(buildDescription)), {});
    require(buildText.find("\"engineSources\": true") != std::string::npos &&
            buildText.find("0.2.0") != std::string::npos,
            "Build metadata did not record engine sources or the association.");
}
} // namespace

int main() {
    try {
        ScratchDirectory scratch;
        actorLifecycle();
        meshImport(scratch.path);
        materialPersistence(scratch.path);
        scenePersistence(scratch.path);
        projectPaths(scratch.path);
        unicodePaths(scratch.path);
        engineRegistryAndAssociation(scratch.path);
        projectSolutionReferences(scratch.path);
        std::cout << "PASS: Actor lifecycle, OBJ validation, material persistence, scene transactionality, project paths, Unicode paths, engine registry/association, manifest-info recovery, project solution references\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
