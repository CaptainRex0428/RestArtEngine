#include "RestArtEngine/Assets/AssetImport.h"

#include "RestArtEngine/Assets/AssetFile.h"
#include "AssetSerialization.h"
#include "ContentFiles.h"
#include "AssetOperationPaths.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unordered_set>

namespace rae::assets {
namespace {

constexpr std::array formats{
    AssetImportFormat{".obj", "Wavefront OBJ"},
    AssetImportFormat{".fbx", "Autodesk FBX"},
};
static_assert(std::is_nothrow_move_constructible_v<MeshAsset>);
static_assert(std::is_nothrow_move_assignable_v<MeshAsset>);

std::string extensionOf(const std::filesystem::path& path) {
    auto extension = detail::pathUtf8(path.extension());
    for (char& character : extension)
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    return extension;
}

void rejectLinkedPath(const std::filesystem::path& path) {
    std::filesystem::path checked;
    for (const auto& component : path) {
        checked /= component;
        if (detail::contentLink(checked))
            throw std::runtime_error("Import destination must not contain linked files or directories.");
    }
}

std::filesystem::path importFolder(const std::filesystem::path& contentRoot,
    std::string_view destinationFolder) {
    if (contentRoot.empty()) throw std::runtime_error("Project Content root is not set.");
    if (destinationFolder.starts_with("engine://"))
        throw std::runtime_error("Engine Content is read-only. Choose a Project Content folder.");
    std::string folder(destinationFolder);
    std::replace(folder.begin(), folder.end(), '\\', '/');
    if (folder.find('\0') != std::string::npos || folder.find(':') != std::string::npos)
        throw std::runtime_error("Import destination must be a relative Project Content folder.");
    const auto relative = detail::pathFromUtf8(folder);
    if (relative.has_root_path())
        throw std::runtime_error("Import destination must be relative to Project Content.");
    // Reject traversal components even when a later component would cancel it.
    for (const auto& component : relative) {
        if (component == "..") throw std::runtime_error("Import destination cannot leave Project Content.");
#ifdef _WIN32
        // Win32 removes trailing spaces/dots: a component such as ".. "
        // would otherwise bypass lexical traversal checks and name the parent.
        const auto& name = component.native();
        if (component != "." && !name.empty() && (name.back() == L' ' || name.back() == L'.'))
            throw std::runtime_error("Import folder names cannot end with a space or dot.");
#endif
    }
    const auto root = std::filesystem::absolute(contentRoot).lexically_normal();
    const auto destination = (root / relative).lexically_normal();
    rejectLinkedPath(destination);
    if (std::filesystem::exists(destination) && !std::filesystem::is_directory(destination))
        throw std::runtime_error("Import destination is not a directory.");
    return destination;
}

// Only directories created by this transaction are candidates for cleanup.
// Existing folders and files are never removed, even after a write failure.
struct ImportFiles {
    std::filesystem::path temporary;
    std::filesystem::path temporaryFolder;
    std::filesystem::path published;
    std::vector<std::filesystem::path> createdFolders;
    bool committed = false;
    ~ImportFiles() {
        std::error_code ignored;
        if (!temporary.empty()) std::filesystem::remove(temporary, ignored);
        if (!temporaryFolder.empty()) std::filesystem::remove(temporaryFolder, ignored);
        if (committed) return;
        if (!published.empty()) std::filesystem::remove(published, ignored);
        for (auto it = createdFolders.rbegin(); it != createdFolders.rend(); ++it)
            std::filesystem::remove(*it, ignored); // remove() only succeeds on empty directories.
    }
    void createFolder(const std::filesystem::path& destination) {
        std::vector<std::filesystem::path> missing;
        for (auto path = destination; !path.empty() && !std::filesystem::exists(path); path = path.parent_path())
            missing.push_back(path);
        createdFolders.reserve(missing.size());
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            rejectLinkedPath(*it);
            if (std::filesystem::create_directory(*it)) createdFolders.push_back(std::move(*it));
        }
        rejectLinkedPath(destination);
    }
};

// Unlike rename() on POSIX, publishing this way always refuses an existing
// destination. Source and destination live in the same Content directory.
bool publishNewFile(const std::filesystem::path& temporary,
    const std::filesystem::path& destination, std::error_code& code) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        code.clear();
        return true;
    }
    code = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::create_hard_link(temporary, destination, code);
    return !code;
#endif
}

std::unordered_set<AssetId> contentIdentities(const std::filesystem::path& root) {
    std::unordered_set<AssetId> ids;
    for (const auto& path : detail::contentFiles(root)) {
        if (!isAssetContainerFile(path)) continue;
        AssetFile file;
        std::string error;
        if (readAssetFileHeader(path, file, error)) ids.insert(file.id);
    }
    return ids;
}

} // namespace

std::span<const AssetImportFormat> supportedImportFormats() noexcept { return formats; }

std::size_t AssetImportReport::importedCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(items.begin(), items.end(),
        [](const auto& item) { return item.succeeded(); }));
}

std::size_t AssetImportReport::rejectedCount() const noexcept { return items.size() - importedCount(); }

std::size_t AssetImportReport::warningCount() const noexcept {
    std::size_t count = 0;
    for (const auto& item : items) count += item.warnings.size();
    return count;
}

std::string AssetImportReport::summary() const {
    if (items.empty()) return "No files to import.";
    std::string result = "Imported " + std::to_string(importedCount()) + " file(s)";
    if (const auto rejected = rejectedCount()) result += "; rejected " + std::to_string(rejected) + " file(s)";
    if (const auto warnings = warningCount()) result += "; " + std::to_string(warnings) + " warning(s)";
    result += ".";
    for (const auto& item : items) {
        if (!item.succeeded()) result += "\n" + detail::pathUtf8(item.sourcePath.filename()) + ": " + item.error;
        for (const auto& warning : item.warnings)
            result += "\n" + detail::pathUtf8(item.sourcePath.filename()) + ": Warning: " + warning;
    }
    return result;
}

AssetImportReport importAssets(AssetLibrary& library, const std::filesystem::path& contentRoot,
    std::string_view destinationFolder, std::span<const std::filesystem::path> sources,
    const AssetImportOptions& options) {
    AssetImportReport report;
    report.items.reserve(sources.size());
    std::filesystem::path destination;
    std::unordered_set<AssetId> usedIds;
    std::string destinationError;
    try {
        destination = importFolder(contentRoot, destinationFolder);
        usedIds = contentIdentities(std::filesystem::absolute(contentRoot).lexically_normal());
        for (const auto& mesh : library.meshes) usedIds.insert(mesh.id);
        for (const auto& material : library.materials) usedIds.insert(material.id);
    } catch (const std::exception& failure) { destinationError = failure.what(); }

    for (const auto& source : sources) {
        AssetImportResult result;
        result.sourcePath = source;
        try {
            if (!destinationError.empty()) throw std::runtime_error(destinationError);
            const auto extension = extensionOf(source);
            if (std::none_of(formats.begin(), formats.end(), [&](const auto& format) { return format.extension == extension; }))
                throw std::runtime_error("Unsupported source format. Supported formats: OBJ, FBX.");
            if (!std::filesystem::is_regular_file(source))
                throw std::runtime_error("Import source is not a readable regular file.");

            // Parse into a small, isolated library: the existing project meshes
            // are neither copied nor modified while validating this source.
            AssetLibrary parsed;
            AssetId parsedId = 0;
            std::string error;
            MeshImportDiagnostics diagnostics;
            const bool valid = extension == ".obj" ? parsed.importObj(source, parsedId, error)
                : parsed.importFbx(source, parsedId, error, &diagnostics, options);
            if (!valid) throw std::runtime_error(error);
            if (diagnostics.skippedDegenerateTriangles)
                result.warnings.push_back("Skipped " + std::to_string(diagnostics.skippedDegenerateTriangles) +
                    " degenerate triangle(s) with zero area. Valid geometry was retained.");
            if (diagnostics.repairedNormals)
                result.warnings.push_back("Rebuilt " + std::to_string(diagnostics.repairedNormals) +
                    " unusable vertex normal(s) from valid triangles.");
            if (diagnostics.droppedUvChannels)
                result.warnings.push_back("Ignored " + std::to_string(diagnostics.droppedUvChannels) +
                    " UV channel(s) beyond UV0-UV3. The first four channels were retained.");
            auto mesh = std::move(*parsed.findMesh(parsedId));
            do { mesh.id = generateAssetId(); } while (usedIds.contains(mesh.id));
            auto file = makeMeshAssetFile(mesh);
            attachThumbnail(file, renderMeshThumbnail(mesh));
            // All potentially allocating result/library work is prepared before
            // publishing. With capacity available, moving MeshAsset is noexcept.
            library.meshes.reserve(library.meshes.size() + 1);
            result.name = mesh.name;
            auto baseName = sanitizeAssetName(mesh.name);
            // Content scanning intentionally omits dot-prefixed metadata.
            if (baseName.front() == '.') baseName.insert(baseName.begin(), '_');
            auto assetPath = destination / detail::pathFromUtf8(baseName + ".rasset");
            unsigned suffix = 0;
            while (std::filesystem::exists(assetPath) || detail::contentLink(assetPath)) {
                ++suffix;
                assetPath = destination / detail::pathFromUtf8(baseName + "-" + std::to_string(suffix) + ".rasset");
            }
            result.assetPath = assetPath;
            usedIds.insert(mesh.id);

            ImportFiles files;
            files.createFolder(destination);
            auto temporaryFolder = destination / detail::pathFromUtf8(".ra-import-" + assetIdText(mesh.id) + ".tmp");
            if (!std::filesystem::create_directory(temporaryFolder))
                throw std::runtime_error("A temporary import file already exists. Retry the import.");
            files.temporaryFolder = std::move(temporaryFolder);
            files.temporary = files.temporaryFolder / "asset.rasset";
            if (!writeAssetFile(files.temporary, file, error)) throw std::runtime_error(error);
            rejectLinkedPath(destination);
            std::error_code code;
            if (!publishNewFile(files.temporary, assetPath, code))
                throw std::runtime_error("Cannot publish imported asset without overwriting a file: " + code.message());
            files.published = std::move(assetPath);
            library.meshes.push_back(std::move(mesh));
            result.id = library.meshes.back().id;
            files.committed = true;
            library.rememberMeshPersisted(result.id, result.assetPath);
        } catch (const std::exception& failure) {
            result.id = 0;
            result.assetPath.clear();
            result.name.clear();
            result.warnings.clear();
            result.error = failure.what();
        }
        report.items.push_back(std::move(result));
    }
    return report;
}

AssetImportResult reimportAsset(AssetLibrary& library, const std::filesystem::path& contentRoot,
    std::string_view relativePath, const std::filesystem::path& sourceOverride, const AssetImportOptions& options) {
    AssetImportResult result;
    result.sourcePath = sourceOverride;
    try {
        const auto target = detail::checkedContentFile(contentRoot, relativePath);
        if (extensionOf(target) != ".rasset")
            throw std::runtime_error("Reimport requires a modern .rasset mesh asset.");
        AssetFile original;
        std::string error;
        if (!readAssetFileHeader(target, original, error)) throw std::runtime_error(error);
        if (original.type != AssetFileType::StaticMesh || isEngineAssetId(original.id))
            throw std::runtime_error("Only Project imported meshes can be reimported.");
        auto* existing = library.findMesh(original.id);
        if (!existing || existing->origin != AssetOrigin::Project || existing->primitive != MeshPrimitive::Imported)
            throw std::runtime_error("Reimport requires a loaded Project imported mesh.");
        if (std::count_if(library.meshes.begin(), library.meshes.end(),
                [&](const auto& mesh) { return mesh.id == original.id; }) != 1 || library.findMaterial(original.id))
            throw std::runtime_error("The asset GUID is ambiguous in the loaded library.");
        detail::requireUniqueContentIdentity(detail::checkedContentFiles(contentRoot), original.id);
        const auto originalSize = std::filesystem::file_size(target);
        const auto originalModified = std::filesystem::last_write_time(target);
        if (result.sourcePath.empty()) result.sourcePath = detail::pathFromUtf8(existing->sourcePath);
        if (result.sourcePath.empty()) throw std::runtime_error("No source file is recorded. Use Reimport from file.");
        const auto extension = extensionOf(result.sourcePath);
        if (std::none_of(formats.begin(), formats.end(), [&](const auto& format) { return format.extension == extension; }))
            throw std::runtime_error("Unsupported source format. Supported formats: OBJ, FBX.");
        if (!std::filesystem::is_regular_file(result.sourcePath))
            throw std::runtime_error("The source file is missing or unreadable. Use Reimport from file to choose another source.");
        AssetLibrary parsed;
        AssetId parsedId = 0;
        MeshImportDiagnostics diagnostics;
        const bool valid = extension == ".obj" ? parsed.importObj(result.sourcePath, parsedId, error)
            : parsed.importFbx(result.sourcePath, parsedId, error, &diagnostics, options);
        if (!valid) throw std::runtime_error(error);
        if (diagnostics.skippedDegenerateTriangles)
            result.warnings.push_back("Skipped " + std::to_string(diagnostics.skippedDegenerateTriangles) +
                " degenerate triangle(s) with zero area. Valid geometry was retained.");
        if (diagnostics.repairedNormals)
            result.warnings.push_back("Rebuilt " + std::to_string(diagnostics.repairedNormals) +
                " unusable vertex normal(s) from valid triangles.");
        if (diagnostics.droppedUvChannels)
            result.warnings.push_back("Ignored " + std::to_string(diagnostics.droppedUvChannels) +
                " UV channel(s) beyond UV0-UV3. The first four channels were retained.");
        auto mesh = std::move(*parsed.findMesh(parsedId));
        mesh.id = existing->id;
        mesh.name = existing->name;
        mesh.origin = AssetOrigin::Project;
        // ufbx element indices are not stable across re-exports. Distinct FBX
        // materials with equal names therefore cannot be matched safely: an
        // ordinal match could silently exchange Actor overrides or defaults.
        const auto ambiguousFbxSlots = [](const MeshAsset& value) {
            std::unordered_set<std::string> names;
            for (const auto& slot : value.materialSlots)
                if (slot.sourceKey.starts_with("fbx:") && slot.sourceKey != "fbx:default" && !names.insert(slot.name).second)
                    return true;
            return false;
        };
        if (ambiguousFbxSlots(*existing) || ambiguousFbxSlots(mesh))
            throw std::runtime_error("Cannot safely match duplicate FBX material names during reimport. "
                "Give source materials unique names, import as a new asset, and reassign its material slots.");
        // Reimport may reorder sections. Preserve defaults by source identity,
        // never by the current numeric slot position.
        for (auto& slot : mesh.materialSlots) {
            if (slot.sourceKey.empty()) continue;
            const auto oldSlot = std::find_if(existing->materialSlots.begin(), existing->materialSlots.end(),
                [&](const auto& old) { return old.sourceKey == slot.sourceKey; });
            if (oldSlot != existing->materialSlots.end()) slot.defaultMaterialId = oldSlot->defaultMaterialId;
        }
        auto file = makeMeshAssetFile(mesh);
        attachThumbnail(file, renderMeshThumbnail(mesh));
        result.assetPath = target;
        result.name = mesh.name;

        // This cleanup owns temporary files only, never the existing target.
        ImportFiles files;
        auto staging = target.parent_path() / detail::pathFromUtf8(".ra-reimport-" + assetIdText(generateAssetId()) + ".tmp");
        if (!std::filesystem::create_directory(staging)) throw std::runtime_error("Cannot create reimport staging directory.");
        files.temporaryFolder = std::move(staging);
        files.temporary = files.temporaryFolder / "asset.rasset";
        if (!writeAssetFile(files.temporary, file, error)) throw std::runtime_error(error);
        detail::rejectOperationLinks(target);
        AssetFile current;
        if (!readAssetFileHeader(target, current, error) || current.id != original.id || current.type != original.type ||
            std::filesystem::file_size(target) != originalSize || std::filesystem::last_write_time(target) != originalModified)
            throw std::runtime_error("The asset file changed during reimport. Refresh Content and retry.");
#ifdef _WIN32
        if (!MoveFileExW(files.temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace the asset file: " + std::system_category().message(static_cast<int>(GetLastError())));
#else
        std::filesystem::rename(files.temporary, target);
#endif
        *existing = std::move(mesh);
        result.id = existing->id;
        library.rememberMeshPersisted(result.id, target);
    } catch (const std::exception& failure) {
        result.id = 0;
        result.assetPath.clear();
        result.name.clear();
        result.warnings.clear();
        result.error = failure.what();
    }
    return result;
}

} // namespace rae::assets
