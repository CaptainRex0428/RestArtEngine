#include "RestArtEngine/Assets/AssetOperations.h"
#include "AssetOperationPaths.h"

#include <algorithm>
#include <type_traits>

namespace rae::assets {
namespace {

static_assert(std::is_nothrow_move_assignable_v<MeshAsset>);
static_assert(std::is_nothrow_move_assignable_v<MaterialAsset>);

void rejectSavedReferences(const std::vector<std::filesystem::path>& files,
    AssetId id, AssetFileType type) {
    const char* inventory = type == AssetFileType::StaticMesh ? "meshes" : "materials";
    const char* actorField = type == AssetFileType::StaticMesh ? "meshId" : "materialId";
    for (const auto& path : files) {
        if (detail::operationExtension(path) != ".rascene") continue;
        bool referenced = false;
        try {
            const auto scene = detail::readJson(path);
            const auto version = scene.at("version").get<int>();
            if (scene.at("format") != "RAEngine.Scene" || version < 1 || version > 3 ||
                !scene.at("actors").is_array() || !scene.at("meshes").is_array() || !scene.at("materials").is_array())
                throw std::runtime_error("Unsupported scene format or reference schema.");
            // Legacy scenes can contain embedded assets. Conservatively retain
            // a matching external identity until those scenes are resaved.
            for (const auto& item : scene.at(inventory))
                referenced |= detail::readId(item.at("id")) == id;
            for (const auto& actor : scene.at("actors"))
                referenced |= detail::readId(actor.at("mesh").at(actorField)) == id;
        } catch (const std::exception& failure) {
            throw std::runtime_error("Cannot verify references in scene '" + detail::pathUtf8(path) + "': " + failure.what());
        }
        if (referenced)
            throw std::runtime_error("This asset is referenced by saved scene '" + detail::pathUtf8(path) + "'. Remove its references and save that scene first.");
    }
}

} // namespace

bool deleteContentAsset(AssetLibrary& library, const std::filesystem::path& contentRoot,
    std::string_view relativePath, std::string& error) {
    try {
        const auto target = detail::checkedContentFile(contentRoot, relativePath);
        const auto extension = detail::operationExtension(target);
        if (extension == ".hlsl" || extension == ".hlsli")
            throw std::runtime_error("Shader files cannot be deleted until their include dependencies can be checked.");
        const auto originalSize = std::filesystem::file_size(target);
        const auto originalModified = std::filesystem::last_write_time(target);
        AssetId id = 0;
        AssetFileType type = AssetFileType::Unknown;
        if (extension == ".rasset" || extension == ".raasset") {
            AssetFile file;
            if (!readAssetFileHeader(target, file, error)) throw std::runtime_error(error);
            id = file.id;
            type = file.type;
            if (type != AssetFileType::StaticMesh && type != AssetFileType::Material)
                throw std::runtime_error("Deletion is not supported for this asset container type.");
        } else if (extension == ".ramat") {
            id = detail::contentFileIdentity(target);
            type = AssetFileType::Material;
        }
        if (id) {
            if (isEngineAssetId(id)) throw std::runtime_error("Engine assets are read-only.");
            const auto* mesh = library.findMesh(id);
            const auto* material = library.findMaterial(id);
            if ((mesh && (mesh->origin != AssetOrigin::Project || mesh->primitive != MeshPrimitive::Imported)) ||
                (material && material->origin != AssetOrigin::Project))
                throw std::runtime_error("Engine and built-in assets cannot be deleted.");
            if ((type == AssetFileType::StaticMesh && material) || (type == AssetFileType::Material && mesh))
                throw std::runtime_error("The disk asset type does not match its loaded identity.");
            if (std::count_if(library.meshes.begin(), library.meshes.end(), [&](const auto& x) { return x.id == id; }) > 1 ||
                std::count_if(library.materials.begin(), library.materials.end(), [&](const auto& x) { return x.id == id; }) > 1)
                throw std::runtime_error("The asset GUID is ambiguous in the loaded library.");
            const auto files = detail::checkedContentFiles(contentRoot);
            detail::requireUniqueContentIdentity(files, id);
            rejectSavedReferences(files, id, type);
            // Recheck identity and type after scanning. A same-GUID replacement
            // must not cause us to erase a different in-memory asset category.
            detail::rejectOperationLinks(target);
            if (extension == ".rasset" || extension == ".raasset") {
                AssetFile current;
                if (!readAssetFileHeader(target, current, error) || current.id != id || current.type != type)
                    throw std::runtime_error("The asset file changed during deletion. Refresh Content and retry.");
            } else if (detail::contentFileIdentity(target) != id)
                throw std::runtime_error("The asset file changed during deletion. Refresh Content and retry.");
        }
        detail::rejectOperationLinks(target);
        if (!std::filesystem::is_regular_file(target) || std::filesystem::file_size(target) != originalSize ||
            std::filesystem::last_write_time(target) != originalModified)
            throw std::runtime_error("The asset file changed during deletion. Refresh Content and retry.");
        if (!std::filesystem::remove(target)) throw std::runtime_error("The asset file no longer exists.");
        // Erasing these value types cannot throw; publication/removal always
        // precedes the in-memory commit so failed IO leaves the library intact.
        if (type == AssetFileType::StaticMesh)
            std::erase_if(library.meshes, [&](const auto& mesh) { return mesh.id == id; });
        else if (type == AssetFileType::Material)
            std::erase_if(library.materials, [&](const auto& material) { return material.id == id; });
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace rae::assets
