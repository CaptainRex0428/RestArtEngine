#include "RestArtEngine/Assets/AssetFile.h"
#include "RestArtEngine/Assets/AssetImport.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace rae::assets;
int checks = 0;
void require(bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct TemporaryTree {
    fs::path root = fs::temp_directory_path() / ("rae_mesh_persistence_" + assetIdText(generateAssetId()));
    TemporaryTree() { fs::create_directory(root); }
    ~TemporaryTree() {
        std::error_code ignored;
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().starts_with("rae_mesh_persistence_"))
            fs::remove_all(root, ignored);
    }
};
MeshAsset triangle() {
    MeshAsset mesh;
    mesh.id = generateAssetId(); mesh.name = "Triangle";
    mesh.vertices = {{{0,0,0},{0,0,1}}, {{1,0,0},{0,0,1}}, {{0,1,0},{0,0,1}}};
    mesh.indices = {0,1,2};
    return mesh;
}
MeshAsset readMesh(const fs::path& path) {
    AssetFile file; MeshAsset mesh; std::string error;
    require(readAssetFile(path, file, error), "Read persisted container: " + error);
    require(readMeshAssetFile(file, mesh, error), "Read persisted mesh: " + error);
    return mesh;
}
void writeText(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    require(bool(stream), "Write isolated fixture.");
}
void checksForPersistence(const fs::path& root) {
    std::string error;
    AssetLibrary library;
    library.meshes.push_back(triangle());
    const auto id = library.meshes.front().id;
    const auto folder = root / "Meshes";
    const auto path = folder / "Triangle.rasset";
    require(library.saveMeshes(folder, error), "Save new unreferenced mesh: " + error);
    const auto oldTime = fs::file_time_type::clock::now() - std::chrono::hours(24);
    fs::last_write_time(path, oldTime);
    AssetLibrary loaded;
    require(loaded.loadMeshes(folder, error), "Load modern mesh: " + error);
    require(loaded.saveMeshes(folder / ".", error), "Save loaded mesh: " + error);
    require(fs::last_write_time(path) == oldTime, "Loaded modern geometry must not be serialized again.");
    require(loaded.saveMeshes(folder, error), "Repeat save.");
    require(fs::last_write_time(path) == oldTime, "Repeated save preserves exact timestamp.");

    // A deep-copied library cannot trust pointers stored in its copied baseline.
    auto copy = loaded;
    copy.meshes.front().vertices[1].position[0] = 2;
    require(copy.saveMeshes(folder, error), "Save deep-copied library.");
    require(readMesh(path).vertices[1].position[0] == 2, "Copied geometry invalidates persistence baseline.");
    loaded.meshes.front().vertices[1].position[0] = 3;
    loaded.meshes.front().markGeometryModified();
    require(loaded.saveMeshes(folder, error), "Save equal-sized geometry revision.");
    require(readMesh(path).vertices[1].position[0] == 3, "In-place marked geometry is persisted.");
    loaded.meshes.front().name = "Renamed";
    loaded.meshes.front().sourcePath = "authored/source.obj";
    require(loaded.saveMeshes(folder, error), "Save serialized metadata change.");
    auto renamed = readMesh(path);
    require(renamed.name == "Renamed" && renamed.sourcePath == "authored/source.obj", "Metadata change keeps GUID path and updates payload.");

    const auto movedPath = folder / "Moved.rasset";
    fs::rename(path, movedPath);
    fs::last_write_time(movedPath, oldTime);
    require(loaded.saveMeshes(folder, error), "Save moved file.");
    require(!fs::exists(path) && readMesh(movedPath).id == id, "Moved file remains the GUID target.");
    require(fs::last_write_time(movedPath) != oldTime, "Moved target invalidates path baseline.");
    fs::last_write_time(movedPath, oldTime);
    require(loaded.saveMeshes(folder, error), "Save externally touched file.");
    require(fs::last_write_time(movedPath) != oldTime, "Changed file timestamp invalidates baseline.");
    // Change file length while preserving mtime: timestamp alone is insufficient.
    const auto beforeAppend = fs::last_write_time(movedPath);
    const auto beforeAppendSize = fs::file_size(movedPath);
    { std::ofstream append(movedPath, std::ios::binary | std::ios::app); append << "extra"; }
    fs::last_write_time(movedPath, beforeAppend);
    require(loaded.saveMeshes(folder, error), "Repair externally lengthened file.");
    require(fs::file_size(movedPath) == beforeAppendSize, "Changed file size forces a rewrite even when mtime is identical.");
    require(readMesh(movedPath).vertices.size() == 3, "Changed file size still saves valid geometry.");

    fs::remove(movedPath);
    require(loaded.saveMeshes(folder, error), "Recreate missing file.");
    const auto renamedPath = folder / "Renamed.rasset";
    require(fs::is_regular_file(renamedPath), "Deleted target is recreated.");
    const auto other = root / "Other";
    require(loaded.saveMeshes(other, error), "Save to another destination.");
    require(readMesh(other / "Renamed.rasset").id == id, "Another destination cannot reuse clean baseline.");

    // Failure leaves dirty data available for a real retry.
    loaded.meshes.front().vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    loaded.meshes.front().markGeometryModified();
    require(!loaded.saveMeshes(other, error), "Reject invalid changed geometry.");
    loaded.meshes.front().vertices[0].position[0] = 4;
    loaded.meshes.front().markGeometryModified();
    require(loaded.saveMeshes(other, error), "Retry serialization after repair.");
    require(readMesh(other / "Renamed.rasset").vertices[0].position[0] == 4, "Retried data reaches disk.");
    const auto blocker = root / "Blocked";
    writeText(blocker, "not a directory");
    require(!loaded.saveMeshes(blocker, error), "Report failed destination.");
    fs::remove(blocker);
    require(loaded.saveMeshes(blocker, error), "Retry destination after fixing failure.");

    // Loading failure must preserve the live mesh and its clean baseline.
    const auto stablePath = blocker / "Renamed.rasset";
    fs::last_write_time(stablePath, oldTime);
    AssetLibrary stable;
    require(stable.loadMeshes(blocker, error), "Load stable baseline.");
    const auto key = stable.meshes.front().geometryKey();
    const auto broken = blocker / "Broken.rasset";
    writeText(broken, "broken container");
    require(!stable.loadMeshes(blocker, error), "Reject malformed reload.");
    require(stable.meshes.front().geometryKey() == key, "Failed load preserves old mesh storage.");
    fs::remove(broken);
    require(stable.saveMeshes(blocker, error), "Save after failed load.");
    require(fs::last_write_time(stablePath) == oldTime, "Failed load preserves old clean baseline.");
    auto removed = std::move(stable.meshes.front());
    stable.meshes.clear();
    require(stable.saveMeshes(blocker, error), "Prune removed mesh cache.");
    removed.vertices[0].position[0] = 5; // same allocation; old cache must be gone
    stable.meshes.push_back(std::move(removed));
    require(stable.saveMeshes(blocker, error), "Reinsert previously removed mesh.");
    require(readMesh(stablePath).vertices[0].position[0] == 5, "Removal really evicts persistence baseline.");

    const auto legacyFolder = root / "Legacy";
    fs::create_directory(legacyFolder);
    auto legacyMesh = triangle();
    require(writeAssetFile(legacyFolder / "Triangle.raasset", makeMeshAssetFile(legacyMesh), error), "Write legacy-extension fixture.");
    AssetLibrary legacy;
    require(legacy.loadMeshes(legacyFolder, error), "Load legacy fixture.");
    require(legacy.saveMeshes(legacyFolder, error), "Migrate legacy fixture.");
    require(readMesh(legacyFolder / "Triangle.rasset").id == legacyMesh.id, "Legacy load stays dirty until modern file exists.");

    const auto source = root / "Imported.obj";
    writeText(source, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetLibrary imported;
    const std::array sources{source};
    const auto report = importAssets(imported, root / "Content", "Meshes", sources);
    require(report.importedCount() == 1, "Import persists the fixture.");
    const auto importPath = report.items.front().assetPath;
    const auto importTime = fs::last_write_time(importPath);
    require(imported.saveMeshes(root / "Content", error), "Save immediately after import.");
    require(fs::last_write_time(importPath) == importTime, "Published import starts clean.");
}
void benchmark(const fs::path& source, const fs::path& root) {
    const auto destination = root / "Benchmark";
    fs::create_directory(destination);
    const auto path = destination / "Mesh.rasset";
    fs::copy_file(source, path);
    AssetLibrary library; std::string error;
    require(library.loadMeshes(destination, error), "Load optional benchmark asset: " + error);
    const auto begin = std::chrono::steady_clock::now();
    require(library.saveMeshes(destination, error), "Save optional benchmark asset: " + error);
    const auto end = std::chrono::steady_clock::now();
    std::cout << "Loaded mesh saveMeshes: " << std::chrono::duration<double, std::milli>(end - begin).count() << " ms\n";
}
void invalidGeometryDoesNotOverwrite(const fs::path& root) {
    AssetLibrary library; library.meshes.push_back(triangle());
    std::string error;
    const auto folder = root / "Validation";
    const auto path = folder / "Triangle.rasset";
    require(library.saveMeshes(folder, error), "Persist valid geometry before malformed edits.");
    const auto originalTime = fs::last_write_time(path);
    const auto readBytes = [&] {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    };
    const auto originalBytes = readBytes();
    auto& mesh = library.meshes.front();
    const auto reject = [&] {
        mesh.markGeometryModified();
        require(!library.saveMeshes(folder, error), "Malformed dirty mesh must fail before overwrite.");
        require(fs::last_write_time(path) == originalTime && readBytes() == originalBytes,
            "Rejected mesh edit preserves bytes and mtime.");
    };
    mesh.indices[0] = 999; reject(); mesh.indices[0] = 0;
    mesh.indices.pop_back(); reject(); mesh.indices.push_back(2);
    mesh.vertices[0].position[0] = std::numeric_limits<float>::infinity(); reject(); mesh.vertices[0].position[0] = 0;
    mesh.vertices[0].normal[2] = std::numeric_limits<float>::quiet_NaN(); reject();
    mesh.vertices[0].normal[2] = 0; reject(); mesh.vertices[0].normal[2] = 1;
    mesh.name.clear(); reject(); mesh.name = "Triangle";
    mesh.markGeometryModified();
    require(library.saveMeshes(folder, error), "Repaired dirty geometry can retry asset persistence.");
    require(readMesh(path).indices == mesh.indices, "Repaired geometry is readable on disk.");
}
} // namespace

int main(int argc, char** argv) {
    try {
        TemporaryTree files;
        checksForPersistence(files.root);
        invalidGeometryDoesNotOverwrite(files.root);
        if (argc > 1) benchmark(fs::path(argv[1]), files.root);
        std::cout << "Mesh persistence: " << checks << " checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
