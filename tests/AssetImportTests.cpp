#include "RestArtEngine/Assets/AssetImport.h"
#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
int checks = 0;

void require(bool value, const std::string& message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

struct TemporaryFiles {
    fs::path root = fs::temp_directory_path() / ("rae_import_test_" + assetIdText(generateAssetId()));
    TemporaryFiles() { require(fs::create_directory(root), "Create an isolated import test directory."); }
    ~TemporaryFiles() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_import_test_"))
            fs::remove_all(root, ignored);
    }
};

void writeText(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    require(bool(stream), "Write source fixture.");
}

std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

constexpr std::string_view objTriangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";

// Authored here rather than copied from a third-party model corpus. One
// centimeter-based FBX triangle with a translated mesh node.
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
        Properties70: {
            P: "Lcl Translation", "Lcl Translation", "", "A", 100,0,0
        }
    }
}
Connections: {
    C: "OO", 100, 200
    C: "OO", 200, 0
}
)fbx";

void replaceFixtureText(std::string& source, std::string_view before, std::string_view after) {
    const auto offset = source.find(before);
    require(offset != std::string::npos, "Find the authored FBX fixture field.");
    source.replace(offset, before.size(), after);
}

// Keep all regression geometry self-authored and independent of downloaded
// models. These variants retain the same unit and axis settings as fbxTriangle.
std::string fbxGeometry(std::string_view vertices, std::string_view indices,
    std::string_view normals = {}) {
    auto source = std::string(fbxTriangle);
    const auto replaceArray = [&](std::string_view name, std::string_view values) {
        const auto begin = source.find(std::string(name) + ": *");
        const auto end = source.find('}', begin);
        require(begin != std::string::npos && end != std::string::npos, "Find the authored FBX array.");
        const auto count = std::count(values.begin(), values.end(), ',') + 1;
        source.replace(begin, end - begin + 1, std::string(name) + ": *" + std::to_string(count) +
            " {\n            a: " + std::string(values) + "\n        }");
    };
    replaceArray("Vertices", vertices);
    replaceArray("PolygonVertexIndex", indices);
    replaceFixtureText(source,
        "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 100,0,0",
        "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 0,0,0");
    if (!normals.empty()) {
        const auto count = std::count(normals.begin(), normals.end(), ',') + 1;
        const auto normalLayer = std::string("        LayerElementNormal: 0 {\n") +
            "            Version: 101\n            Name: \"\"\n" +
            "            MappingInformationType: \"ByPolygonVertex\"\n" +
            "            ReferenceInformationType: \"Direct\"\n" +
            "            Normals: *" + std::to_string(count) + " {\n                a: " +
            std::string(normals) + "\n            }\n        }\n" +
            "        Layer: 0 {\n            Version: 100\n            LayerElement: {\n" +
            "                Type: \"LayerElementNormal\"\n                TypedIndex: 0\n" +
            "            }\n        }\n    }\n    Model:";
        replaceFixtureText(source, "    }\n    Model:", normalLayer);
    }
    return source;
}

void requireAlignedNormals(const MeshAsset& mesh) {
    for (std::size_t offset = 0; offset < mesh.indices.size(); offset += 3) {
        const auto& a = mesh.vertices[mesh.indices[offset]];
        const auto& b = mesh.vertices[mesh.indices[offset + 1]];
        const auto& c = mesh.vertices[mesh.indices[offset + 2]];
        const std::array u{double(b.position[0]) - a.position[0], double(b.position[1]) - a.position[1],
            double(b.position[2]) - a.position[2]};
        const std::array v{double(c.position[0]) - a.position[0], double(c.position[1]) - a.position[1],
            double(c.position[2]) - a.position[2]};
        const std::array geometric{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
            u[0] * v[1] - u[1] * v[0]};
        require(std::hypot(geometric[0], geometric[1], geometric[2]) > 0.0,
            "Every retained FBX triangle has positive stored-float area.");
        for (const auto index : {offset, offset + 1, offset + 2}) {
            const auto& normal = mesh.vertices[mesh.indices[index]].normal;
            require(std::abs(std::hypot(normal[0], normal[1], normal[2]) - 1.0) < 1e-5 &&
                geometric[0] * normal[0] + geometric[1] * normal[1] + geometric[2] * normal[2] > 0.0,
                "Retained FBX normals are unit length and agree with geometric winding.");
        }
    }
}

AssetImportReport importOne(AssetLibrary& library, const fs::path& content,
    std::string_view folder, const fs::path& source, const AssetImportOptions& options = {}) {
    const std::array paths{source};
    return importAssets(library, content, folder, paths, options);
}

std::size_t fileCount(const fs::path& root) {
    if (!fs::exists(root)) return 0;
    return static_cast<std::size_t>(std::distance(fs::recursive_directory_iterator(root), fs::recursive_directory_iterator{}));
}

void supportedAndMixed(const fs::path& root) {
    const auto formats = supportedImportFormats();
    require(formats.size() == 2 && formats[0].extension == ".obj" && formats[1].extension == ".fbx",
        "Dialogs and file drops expose the same supported formats.");
    const auto first = root / "Sources/Triangle.OBJ";
    const auto second = root / "Sources/Second.FbX";
    const auto unsupported = root / "Sources/Texture.png";
    const auto invalidObj = root / "Sources/Broken.obj";
    const auto invalidFbx = root / "Sources/Broken.fbx";
    writeText(first, objTriangle);
    writeText(second, fbxTriangle);
    writeText(unsupported, objTriangle);
    writeText(invalidObj, "v 0 0 0\nf 1 2 3\n");
    writeText(invalidFbx, "This is not FBX.\n");
    const std::array paths{first, unsupported, second, invalidObj, invalidFbx};
    AssetLibrary library;
    library.createDefaults();
    const auto content = root / "Content";
    auto report = importAssets(library, content, "Imported/Nested", paths);
    require(report.importedCount() == 2 && report.rejectedCount() == 3 && report.items.size() == paths.size(), report.summary());
    require(report.items[0].succeeded() && report.items[2].succeeded() && library.meshes.size() == 5,
        "A mixed batch retains each valid source and reports each invalid source.");
    require(report.warningCount() == 0 && report.items[0].warnings.empty() && report.items[2].warnings.empty(),
        "Ordinary OBJ and FBX imports succeed without cleanup warnings.");
    require(report.summary().find("Texture.png") != std::string::npos &&
        report.summary().find("Broken.obj") != std::string::npos && report.summary().find("Broken.fbx") != std::string::npos,
        "The batch diagnostic identifies every rejected source.");
    require(report.items[1].assetPath.empty() && report.items[3].assetPath.empty() && report.items[4].assetPath.empty(),
        "Rejected sources have no published destination.");
    for (const auto index : {0u, 2u}) {
        const auto& item = report.items[index];
        require(item.assetPath.is_absolute() && item.assetPath.parent_path() == fs::absolute(content / "Imported/Nested") &&
            fs::is_regular_file(item.assetPath) && item.id > 3 && !isEngineAssetId(item.id),
            "A successful import has a new project GUID and uses the requested folder.");
        AssetFile stored;
        MeshAsset mesh;
        std::string error;
        require(readAssetFile(item.assetPath, stored, error) && readMeshAssetFile(stored, mesh, error), error);
        require(mesh.id == item.id && mesh.indices.size() == 3 && mesh.vertices.size() == 3 &&
            stored.manifest.contains("thumbnail"), "Imported geometry and its thumbnail are self-contained.");
    }
    const auto* fbx = library.findMesh(report.items[2].id);
    require(fbx && std::any_of(fbx->vertices.begin(), fbx->vertices.end(), [](const auto& vertex) {
        return std::abs(vertex.position[0] - 2.0f) < 1e-4f;
    }), "FBX source centimeters and node translation are baked into meter geometry.");
    const auto ids = std::array{report.items[0].id, report.items[2].id};
    fs::remove_all(root / "Sources");
    AssetLibrary loaded;
    loaded.createDefaults();
    std::string error;
    require(loaded.loadMeshes(content, error) && loaded.findMesh(ids[0]) && loaded.findMesh(ids[1]),
        "Root-wide mesh reload works after the original OBJ and FBX are removed: " + error);
    require(loaded.findMesh(ids[0])->indices.size() == 3 && loaded.findMesh(ids[1])->indices.size() == 3,
        "Reloaded assets retain geometry without source files.");
    AssetRegistry registry;
    registry.setContentRoot(content);
    require(registry.scan(error) && registry.count(AssetType::Mesh) == 2, "New assets appear in Content.");
    require(fileCount(content) == 4, "Successful imports leave only their two directory levels and two asset files.");
}

void collisionsAndPersistence(const fs::path& root) {
    const auto source = root / "Sources/Same.obj";
    writeText(source, objTriangle);
    const auto content = root / "Content";
    AssetLibrary library;
    library.createDefaults();
    auto first = importOne(library, content, "", source);
    require(first.importedCount() == 1 && first.items[0].assetPath == fs::absolute(content / "Same.rasset"), first.summary());
    const auto originalBytes = bytes(first.items[0].assetPath);
    auto second = importOne(library, content, "", source);
    require(second.importedCount() == 1 && second.items[0].assetPath.filename() == "Same-1.rasset" &&
        second.items[0].id != first.items[0].id && bytes(first.items[0].assetPath) == originalBytes,
        "Repeated import allocates a distinct file and identity without overwriting the first.");
    writeText(content / "Same-2.rasset", "An unrelated existing file must not change.");
    const auto unrelated = bytes(content / "Same-2.rasset");
    auto third = importOne(library, content, "", source);
    require(third.importedCount() == 1 && third.items[0].assetPath.filename() == "Same-3.rasset" &&
        bytes(content / "Same-2.rasset") == unrelated, "A malformed existing container also reserves its filename.");
    fs::remove(content / "Same-2.rasset");
    fs::create_directories(content / "Moved/Deep");
    fs::rename(first.items[0].assetPath, content / "Moved/Deep/Renamed.rasset");
    std::string error;
    require(library.saveMeshes(content, error) && fs::exists(content / "Moved/Deep/Renamed.rasset") &&
        !fs::exists(content / "Same.rasset"), "Save All preserves the GUID location of imported and moved assets: " + error);
    AssetLibrary reloaded;
    reloaded.createDefaults();
    require(reloaded.loadMeshes(content, error) && reloaded.meshes.size() == library.meshes.size(),
        "Root and nested imported assets reload without duplicate GUIDs: " + error);
    writeText(root / "Sources/.hidden.obj", objTriangle);
    auto hidden = importOne(library, content, "", root / "Sources/.hidden.obj");
    require(hidden.importedCount() == 1 && hidden.items[0].assetPath.filename().native().front() != '.',
        "Dot-prefixed source names produce assets visible to Content scanning.");
    AssetRegistry registry;
    registry.setContentRoot(content);
    require(registry.scan(error) && registry.count(AssetType::Mesh) == 4, "Every imported asset remains visible.");

    const auto unicodeSource = root / fs::path(u8"Sources/模型.OBJ");
    writeText(unicodeSource, objTriangle);
    const auto unicodeReport = importOne(library, content, "导入/模型", unicodeSource);
    require(unicodeReport.importedCount() == 1 && unicodeReport.items[0].assetPath ==
        fs::absolute(content / fs::path(u8"导入/模型/模型.rasset")), "Unicode source names and UTF-8 destination folders survive import.");
    AssetLibrary unicodeReload;
    require(unicodeReload.loadMeshes(content, error) && unicodeReload.findMesh(unicodeReport.items[0].id),
        "Unicode imported assets reload by GUID: " + error);
}

void fbxTransforms(const fs::path& root) {
    auto mirroredSource = std::string(fbxTriangle);
    const std::string translation = "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 100,0,0";
    const auto offset = mirroredSource.find(translation);
    require(offset != std::string::npos, "Find the authored FBX transform property.");
    mirroredSource.insert(offset + translation.size(),
        "\n            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1,2,1");
    const auto path = root / "Mirrored.fbx";
    writeText(path, mirroredSource);
    AssetLibrary library;
    auto report = importOne(library, root / "Content", "", path);
    require(report.importedCount() == 1, report.summary());
    const auto* mesh = library.findMesh(report.items[0].id);
    require(mesh && mesh->vertices.size() == 3 && std::any_of(mesh->vertices.begin(), mesh->vertices.end(), [](const auto& vertex) {
        return std::abs(vertex.position[0]) < 1e-4f;
    }) && std::any_of(mesh->vertices.begin(), mesh->vertices.end(), [](const auto& vertex) {
        return std::abs(vertex.position[1] - 2.0f) < 1e-4f;
    }), "FBX mirrored/nonuniform node scale is baked together with unit conversion.");
    const auto& a = mesh->vertices[mesh->indices[0]];
    const auto& b = mesh->vertices[mesh->indices[1]];
    const auto& c = mesh->vertices[mesh->indices[2]];
    const std::array u{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
    const std::array v{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
    const std::array geometric{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    for (const auto& vertex : mesh->vertices) {
        const auto dot = geometric[0] * vertex.normal[0] + geometric[1] * vertex.normal[1] + geometric[2] * vertex.normal[2];
        require(dot > 0.0f, "Mirrored FBX winding stays aligned with transformed vertex normals.");
    }
}

void fbxDegenerateTriangles(const fs::path& root) {
    // The duplicate-index and collinear triangles also have zero generated
    // normals. Filtering must happen before those normals are normalized.
    const auto source = root / "Sources/MixedDegenerate.fbx";
    writeText(source, fbxGeometry("0,0,0,100,0,0,0,100,0,50,0,0", "0,1,-3,0,1,-2,0,3,-2"));
    AssetLibrary parsed;
    AssetId parsedId = 0;
    MeshImportDiagnostics diagnostics;
    std::string error;
    require(parsed.importFbx(source, parsedId, error, &diagnostics), error);
    require(diagnostics.skippedDegenerateTriangles == 2 && diagnostics.repairedNormals == 0,
        "Duplicate and collinear triangles are counted exactly; discarded normals need no repair.");
    const auto* parsedMesh = parsed.findMesh(parsedId);
    require(parsedMesh && parsedMesh->vertices.size() == 3 && parsedMesh->indices.size() == 3,
        "Only the valid triangle is emitted, with no orphan corners from discarded triangles.");
    requireAlignedNormals(*parsedMesh);

    const auto content = root / "Content";
    AssetLibrary library;
    library.createDefaults();
    const auto originalCount = library.meshes.size();
    const auto strictReport = importOne(library, content, "Must/Not/Exist", source,
        AssetImportOptions{.skipDegenerateTriangles = false});
    require(strictReport.importedCount() == 0 && strictReport.rejectedCount() == 1 && strictReport.warningCount() == 0 &&
        !strictReport.items[0].error.empty() && strictReport.items[0].assetPath.empty() &&
        library.meshes.size() == originalCount && !fs::exists(content),
        "Disabling degenerate cleanup rejects the whole mixed source without files or library changes: " + strictReport.summary());
    const auto report = importOne(library, content, "Cleaned", source,
        AssetImportOptions{.skipDegenerateTriangles = true});
    require(report.importedCount() == 1 && report.rejectedCount() == 0 && report.warningCount() == 1 &&
        report.items[0].error.empty() && report.items[0].warnings.size() == 1,
        "A partially degenerate FBX succeeds and carries one cleanup warning: " + report.summary());
    require(report.items[0].warnings[0].find("2 degenerate triangle") != std::string::npos &&
        report.summary().find(report.items[0].warnings[0]) != std::string::npos,
        "The shared import report exposes the exact number of skipped triangles.");
    require(library.findMesh(report.items[0].id)->vertices.size() == 3 &&
        library.findMesh(report.items[0].id)->indices.size() == 3,
        "The published library asset contains only valid geometry.");
    AssetFile stored;
    MeshAsset storedMesh;
    require(readAssetFile(report.items[0].assetPath, stored, error) && readMeshAssetFile(stored, storedMesh, error), error);
    require(storedMesh.vertices.size() == 3 && storedMesh.indices.size() == 3,
        "The self-contained asset stores only the valid remainder.");
    requireAlignedNormals(storedMesh);
    fs::remove(source);
    require(library.saveMeshes(content, error), error);
    AssetLibrary reloaded;
    reloaded.createDefaults();
    require(reloaded.loadMeshes(content, error) && reloaded.findMesh(report.items[0].id) &&
        reloaded.findMesh(report.items[0].id)->indices.size() == 3,
        "Cleaned geometry survives Save All and source-independent reload: " + error);

    // A tiny but positive area remains renderable. No arbitrary epsilon may
    // turn it into a cleanup warning.
    const auto tiny = root / "Sources/Tiny.fbx";
    writeText(tiny, fbxGeometry("0,0,0,0.000001,0,0,0,0.000001,0", "0,1,-3"));
    const auto tinyReport = importOne(library, content, "", tiny);
    require(tinyReport.importedCount() == 1 && tinyReport.warningCount() == 0,
        "A tiny positive-area FBX triangle is preserved without warnings: " + tinyReport.summary());
    const auto* tinyMesh = library.findMesh(tinyReport.items[0].id);
    require(tinyMesh && tinyMesh->indices.size() == 3, "The tiny triangle remains in the output.");
    requireAlignedNormals(*tinyMesh);

    // The second triangle is valid in source doubles, but the first edge
    // collapses when centimeter positions near one million meters become float.
    const auto collapsed = root / "Sources/FloatCollapsed.fbx";
    writeText(collapsed, fbxGeometry(
        "0,0,0,100,0,0,0,100,0,100000000,0,0,100000001,0,0,100000000,100,0", "0,1,-3,3,4,-6"));
    const auto collapsedReport = importOne(library, content, "", collapsed);
    require(collapsedReport.importedCount() == 1 && collapsedReport.warningCount() == 1 &&
        collapsedReport.items[0].warnings[0].find("1 degenerate triangle") != std::string::npos &&
        library.findMesh(collapsedReport.items[0].id)->indices.size() == 3,
        "A triangle collapsed by stored-float conversion is skipped while its valid sibling imports: " + collapsedReport.summary());
}

void fbxNormalRepair(const fs::path& root) {
    // Use an oblique plane and mirrored/nonuniform scale so a hardcoded axis
    // normal or normals computed before the final winding change would fail.
    auto fixture = fbxGeometry("0,0,0,100,0,100,0,100,100", "0,1,-3,0,1,-2",
        "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    const std::string translation = "P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\", 0,0,0";
    replaceFixtureText(fixture, translation, translation +
        "\n            P: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\", -1,2,1");
    const auto source = root / "Sources/ZeroNormals.fbx";
    writeText(source, fixture);
    AssetLibrary parsed;
    AssetId id = 0;
    MeshImportDiagnostics diagnostics;
    std::string error;
    require(parsed.importFbx(source, id, error, &diagnostics), error);
    require(diagnostics.skippedDegenerateTriangles == 1 && diagnostics.repairedNormals == 3,
        "Only the retained triangle's three unusable normals are rebuilt.");
    const auto* mesh = parsed.findMesh(id);
    require(mesh && mesh->indices.size() == 3 && mesh->vertices.size() == 3,
        "Normal repair preserves the valid mirrored triangle and removes the duplicate one.");
    requireAlignedNormals(*mesh);
    AssetLibrary library;
    const auto report = importOne(library, root / "Content", "", source);
    require(report.importedCount() == 1 && report.warningCount() == 2 && report.items[0].warnings.size() == 2 &&
        report.items[0].warnings[0].find("1 degenerate triangle") != std::string::npos &&
        report.items[0].warnings[1].find("3 unusable vertex normal") != std::string::npos,
        "Triangle cleanup and normal repair remain separate counted warnings: " + report.summary());
}

void fbxInvalidGeometry(const fs::path& root) {
    AssetLibrary library;
    library.createDefaults();
    const auto meshCount = library.meshes.size();
    const auto content = root / "Content";
    const auto existing = content / "Existing.txt";
    writeText(existing, "Existing project bytes must survive rejected FBX import.");
    const auto originalBytes = bytes(existing);
    const auto originalFiles = fileCount(content);
    const std::array fixtures{
        std::pair{"OnlyDegenerate.fbx", fbxGeometry("0,0,0,100,0,0,50,0,0", "0,1,-2,0,2,-2")},
        std::pair{"InvalidIndex.fbx", fbxGeometry("0,0,0,100,0,0,0,100,0", "0,0,-1,0,1,-10")},
        std::pair{"Nonfinite.fbx", fbxGeometry("0,0,0,100,0,0,0,100,0,1e309,0,0", "0,1,-3,3,3,-4")},
    };
    for (const auto& [name, fixture] : fixtures) {
        const auto source = root / "Sources" / name;
        writeText(source, fixture);
        for (const bool skip : {false, true}) {
            const auto report = importOne(library, content, "Must/Not/Exist", source,
                AssetImportOptions{.skipDegenerateTriangles = skip});
            require(report.importedCount() == 0 && report.rejectedCount() == 1 && report.warningCount() == 0 &&
                report.items[0].id == 0 && report.items[0].assetPath.empty() && !report.items[0].error.empty(),
                "Empty or corrupt FBX is rejected with either cleanup choice: " + report.summary());
            require(library.meshes.size() == meshCount && fileCount(content) == originalFiles &&
                bytes(existing) == originalBytes && !fs::exists(content / "Must"),
                "Rejected FBX leaves the library, existing files, and destination folders unchanged.");
            if (std::string_view(name) == "OnlyDegenerate.fbx")
                require(report.items[0].error.find("degenerate") != std::string::npos ||
                    report.items[0].error.find("zero area") != std::string::npos,
                    "An entirely degenerate FBX explains why no valid triangles remain.");
        }
    }

    const auto ordinarySource = root / "Sources/Ordinary.fbx";
    writeText(ordinarySource, fbxTriangle);
    const auto strict = importOne(library, content, "", ordinarySource,
        AssetImportOptions{.skipDegenerateTriangles = false});
    const auto permissive = importOne(library, content, "", ordinarySource,
        AssetImportOptions{.skipDegenerateTriangles = true});
    require(strict.importedCount() == 1 && permissive.importedCount() == 1 &&
        strict.warningCount() == 0 && permissive.warningCount() == 0,
        "Ordinary geometry imports without warnings for both cleanup choices.");
    const auto* strictMesh = library.findMesh(strict.items[0].id);
    const auto* permissiveMesh = library.findMesh(permissive.items[0].id);
    require(strictMesh && permissiveMesh && strictMesh->indices == permissiveMesh->indices &&
        strictMesh->vertices.size() == permissiveMesh->vertices.size(),
        "Cleanup choices leave ordinary FBX topology unchanged.");
    for (std::size_t corner = 0; corner < strictMesh->vertices.size(); ++corner)
        for (std::size_t axis = 0; axis < 3; ++axis)
            require(strictMesh->vertices[corner].position[axis] == permissiveMesh->vertices[corner].position[axis] &&
                strictMesh->vertices[corner].normal[axis] == permissiveMesh->vertices[corner].normal[axis],
                "Cleanup choices leave ordinary FBX positions and normals unchanged.");
}

void failuresAndBoundaries(const fs::path& root) {
    const auto source = root / "Sources/Valid.obj";
    writeText(source, objTriangle);
    AssetLibrary library;
    library.createDefaults();
    const auto count = library.meshes.size();
    const auto content = root / "Content";
    const std::array rejectedFolders{
        std::string("../Escape"), std::string("Meshes/../../Escape"), std::string("engine://Meshes"),
        std::string("/absolute"), std::string("C:\\absolute"), std::string("Meshes/../Other"),
        std::string("Meshes\0Invisible", 16), fs::absolute(root / "Outside").generic_string(),
#ifdef _WIN32
        std::string(".. /Escape"), std::string(".../Escape"), std::string("Nested./Escape"),
#endif
    };
    for (const auto& folder : rejectedFolders) {
        const auto report = importOne(library, content, folder, source);
        require(report.rejectedCount() == 1 && !report.items[0].error.empty() && library.meshes.size() == count &&
            !fs::exists(content), "An invalid destination rejects import without creating files or changing assets.");
    }
    const auto badSource = root / "Sources/Bad.obj";
    writeText(badSource, "v 0 0 0\nf 1 2 3\n");
    auto report = importOne(library, content, "Will/Not/Exist", badSource);
    require(report.rejectedCount() == 1 && !fs::exists(content) && library.meshes.size() == count,
        "A parse failure creates no destination directories or partial asset.");
    const auto wrongType = root / "Sources/NotSupported.png";
    writeText(wrongType, objTriangle);
    report = importOne(library, content, "", wrongType);
    require(report.rejectedCount() == 1 && report.items[0].error.find("Unsupported") != std::string::npos && !fs::exists(content),
        "A known-looking payload with an unsupported extension is rejected.");
    writeText(root / "Sources/Disguised.fbx", objTriangle);
    report = importOne(library, content, "", root / "Sources/Disguised.fbx");
    require(report.rejectedCount() == 1 && !fs::exists(content),
        "An OBJ renamed to FBX cannot bypass format validation.");
    fs::create_directory(root / "Sources/Directory.obj");
    report = importOne(library, content, "", root / "Sources/Directory.obj");
    require(report.rejectedCount() == 1 && !fs::exists(content), "Directories are rejected rather than recursively imported.");
    report = importOne(library, content, "", root / "Sources/Missing.obj");
    require(report.rejectedCount() == 1 && !fs::exists(content), "Missing files leave project state unchanged.");
    writeText(content / "Occupied", "Keep the existing destination file.");
    const auto original = bytes(content / "Occupied");
    report = importOne(library, content, "Occupied", source);
    require(report.rejectedCount() == 1 && bytes(content / "Occupied") == original && library.meshes.size() == count,
        "A destination file cannot be replaced by a directory.");

    const auto outside = root / "Outside";
    fs::create_directory(outside);
    std::error_code code;
    fs::create_directory_symlink(outside, content / "Linked", code);
    if (!code) {
        report = importOne(library, content, "Linked/Nested", source);
        require(report.rejectedCount() == 1 && fs::is_empty(outside) && library.meshes.size() == count,
            "Linked destination ancestors cannot route imports outside Content.");
        report = importOne(library, content / "Linked", "", source);
        require(report.rejectedCount() == 1 && fs::is_empty(outside), "A linked Content root is rejected.");
    } else std::cout << "SKIP symbolic-link import fixture: " << code.message() << '\n';
    const std::array<fs::path, 0> noSources{};
    report = importAssets(library, content, "", noSources);
    require(report.items.empty() && report.importedCount() == 0 && report.rejectedCount() == 0,
        "An empty selection is an empty report.");
}

} // namespace

int main() {
    try {
        TemporaryFiles temporary;
        supportedAndMixed(temporary.root / "Mixed");
        collisionsAndPersistence(temporary.root / "Collision");
        fbxTransforms(temporary.root / "Transforms");
        fbxDegenerateTriangles(temporary.root / "Degenerate");
        fbxNormalRepair(temporary.root / "NormalRepair");
        fbxInvalidGeometry(temporary.root / "InvalidGeometry");
        failuresAndBoundaries(temporary.root / "Failure");
        std::cout << "Asset import tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Asset import test failed after " << checks << " checks: " << failure.what() << '\n';
        return 1;
    }
}
