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
        const auto extension = detail::operationExtension(path);
        if (type == AssetFileType::Material && (extension == ".rasset" || extension == ".raasset")) {
            AssetFile file;
            std::string error;
            if (!readAssetFileHeader(path, file, error)) throw std::runtime_error(error);
            if (file.type == AssetFileType::StaticMesh && file.manifest.at("payload").contains("materialSlots")) {
                const auto& slots = file.manifest.at("payload").at("materialSlots");
                if (!slots.is_array()) throw std::runtime_error("Cannot verify mesh slot references.");
                for (const auto& slot : slots) {
                    const auto& material = slot.at("defaultMaterialId");
                    if (!material.is_number_integer()) throw std::runtime_error("Invalid mesh slot material identity.");
                    if (material != 0 && detail::readId(material) == id)
                        throw std::runtime_error("This material is referenced by mesh '" + detail::pathUtf8(path) + "'.");
                }
            }
        }
        if (detail::operationExtension(path) != ".rascene") continue;
        bool referenced = false;
        try {
            const auto scene = detail::readJson(path);
            const auto version = scene.at("version").get<int>();
            if (scene.at("format") != "RAEngine.Scene" || version < 1 || version > 7 ||
                !scene.at("actors").is_array() || !scene.at("meshes").is_array() || !scene.at("materials").is_array())
                throw std::runtime_error("Unsupported scene format or reference schema.");
            // Legacy scenes can contain embedded assets. Conservatively retain
            // a matching external identity until those scenes are resaved.
            for (const auto& item : scene.at(inventory))
                referenced |= detail::readId(item.at("id")) == id;
            for (const auto& actor : scene.at("actors")) {
                if (actor.contains("camera")) {
                    if (version < 6 || !actor.at("camera").is_object() ||
                        actor.contains("mesh") || actor.contains("light"))
                        throw std::runtime_error("Unsupported or ambiguous Camera component.");
                    continue;
                }
                if (version >= 4 && actor.contains("light") && !actor.contains("mesh")) {
                    if (!actor.at("light").is_object())
                        throw std::runtime_error("Invalid light component.");
                    continue;
                }
                if (actor.contains("light"))
                    throw std::runtime_error("Unsupported or ambiguous Actor component.");
                referenced |= detail::readId(actor.at("mesh").at(actorField)) == id;
                if (type == AssetFileType::Material && actor.at("mesh").contains("materialOverrides")) {
                    const auto& overrides = actor.at("mesh").at("materialOverrides");
                    if (version < 5 || !overrides.is_array()) throw std::runtime_error("Invalid material override schema.");
                    for (const auto& material : overrides) {
                        if (!material.is_number_integer()) throw std::runtime_error("Invalid material override GUID.");
                        if (material != 0) referenced |= detail::readId(material) == id;
                    }
                }
            }
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
            if (type == AssetFileType::Material)
                for (const auto& loadedMesh : library.meshes)
                    for (const auto& slot : loadedMesh.materialSlots)
                        if (slot.defaultMaterialId == id)
                            throw std::runtime_error("This material is referenced by a loaded mesh slot. Clear and save the mesh binding first.");
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
