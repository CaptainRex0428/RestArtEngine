#include "RestArtEngine/Assets/AssetRegistry.h"

#include <algorithm>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace rae::assets {
namespace {

using Json = nlohmann::json;

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// Material files are JSON; read only the header fields the browser needs so a
// malformed file does not break the whole scan.
void readMaterialHeader(const std::filesystem::path& path, std::string& name, AssetId& id) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return;
        const Json value = Json::parse(stream);
        if (!value.contains("material") || !value["material"].is_object()) return;
        const auto& material = value["material"];
        if (material.contains("id") && material["id"].is_number_unsigned())
            id = material["id"].get<AssetId>();
        if (material.contains("name") && material["name"].is_string())
            name = material["name"].get<std::string>();
    } catch (const std::exception&) {
        // A broken asset is still listed under its file name; loading it reports
        // the real error later.
    }
}

} // namespace

const char* assetTypeName(AssetType type) noexcept {
    switch (type) {
    case AssetType::Scene: return "Scene";
    case AssetType::Material: return "Material";
    case AssetType::Mesh: return "Mesh";
    case AssetType::Shader: return "Shader";
    default: return "Asset";
    }
}

AssetType assetTypeFromPath(const std::filesystem::path& path) noexcept {
    const auto extension = path.extension().wstring();
    if (extension == L".rascene") return AssetType::Scene;
    if (extension == L".ramat") return AssetType::Material;
    if (extension == L".obj") return AssetType::Mesh;
    if (extension == L".hlsl" || extension == L".hlsli") return AssetType::Shader;
    return AssetType::Unknown;
}

void AssetRegistry::setContentRoot(std::filesystem::path root) {
    contentRoot_ = std::move(root);
    entries_.clear();
    folders_.clear();
}

bool AssetRegistry::scan(std::string& error) {
    error.clear();
    entries_.clear();
    folders_.clear();
    folders_.emplace_back();
    if (contentRoot_.empty()) {
        error = "The content root is not set.";
        return false;
    }
    std::error_code code;
    if (!std::filesystem::is_directory(contentRoot_, code)) return true;

    std::filesystem::recursive_directory_iterator iterator(contentRoot_,
        std::filesystem::directory_options::skip_permission_denied, code);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(code)) {
        if (code) { code.clear(); continue; }
        const auto& path = iterator->path();
        const auto fileName = path.filename().wstring();
        if (!fileName.empty() && fileName.front() == L'.') {
            if (iterator->is_directory(code)) { iterator.disable_recursion_pending(); continue; }
            continue;
        }
        if (iterator->is_directory(code)) {
            const auto relative = path.lexically_relative(contentRoot_);
            if (!relative.empty()) folders_.push_back(pathText(relative));
            continue;
        }
        if (!iterator->is_regular_file(code)) continue;
        const auto type = assetTypeFromPath(path);
        if (type == AssetType::Unknown) continue;

        AssetEntry entry;
        entry.type = type;
        entry.path = path;
        entry.relativePath = pathText(path.lexically_relative(contentRoot_));
        entry.folder = pathText(path.parent_path().lexically_relative(contentRoot_));
        entry.name = pathText(path.stem());
        std::error_code sizeCode;
        entry.sizeBytes = static_cast<std::uint64_t>(std::filesystem::file_size(path, sizeCode));
        if (sizeCode) entry.sizeBytes = 0;
        if (type == AssetType::Material) {
            std::string innerName;
            AssetId innerId = 0;
            readMaterialHeader(path, innerName, innerId);
            if (!innerName.empty()) entry.name = innerName;
            entry.id = innerId;
        }
        entries_.push_back(std::move(entry));
    }
    std::sort(folders_.begin(), folders_.end());
    std::sort(entries_.begin(), entries_.end(), [](const AssetEntry& left, const AssetEntry& right) {
        if (left.folder != right.folder) return left.folder < right.folder;
        if (left.type != right.type) return static_cast<int>(left.type) < static_cast<int>(right.type);
        return left.name < right.name;
    });
    return true;
}

std::vector<const AssetEntry*> AssetRegistry::inFolder(std::string_view folder) const {
    std::vector<const AssetEntry*> result;
    for (const auto& entry : entries_)
        if (entry.folder == folder) result.push_back(&entry);
    return result;
}

const AssetEntry* AssetRegistry::find(std::string_view relativePath) const {
    for (const auto& entry : entries_)
        if (entry.relativePath == relativePath) return &entry;
    return nullptr;
}

std::size_t AssetRegistry::count(AssetType type) const noexcept {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
        [type](const AssetEntry& entry) { return entry.type == type; }));
}


bool moveContentEntry(const std::filesystem::path& contentRoot, std::string_view relativePath,
                      std::string_view targetFolder, std::string& error) {
    error.clear();
    if (contentRoot.empty() || relativePath.empty()) { error = "Move needs a content root and a source path."; return false; }
    const std::filesystem::path source = contentRoot / std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(relativePath.data()), reinterpret_cast<const char8_t*>(relativePath.data()) + relativePath.size()));
    std::error_code code;
    if (!std::filesystem::exists(source, code)) { error = "The asset no longer exists: " + std::string(relativePath); return false; }
    const std::filesystem::path target = contentRoot / std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(targetFolder.data()), reinterpret_cast<const char8_t*>(targetFolder.data()) + targetFolder.size()));
    if (source.parent_path() == target) return true;
    std::filesystem::create_directories(target, code);
    if (code) { error = "Cannot create the target folder: " + target.generic_string(); return false; }
    const auto destination = target / source.filename();
    if (std::filesystem::exists(destination, code)) { error = "A file with that name already exists in the target folder."; return false; }

    // A moved scene keeps its imported-mesh references valid by re-basing the
    // stored relative paths onto the new folder.
    if (source.extension() == L".rascene") {
        try {
            std::ifstream input(source, std::ios::binary);
            Json value = Json::parse(input);
            input.close();
            bool changed = false;
            if (value.contains("meshes") && value["meshes"].is_array()) {
                for (auto& mesh : value["meshes"]) {
                    if (!mesh.contains("sourcePath") || !mesh["sourcePath"].is_string()) continue;
                    const auto stored = mesh["sourcePath"].get<std::string>();
                    if (stored.empty()) continue;
                    const std::filesystem::path storedPath = std::filesystem::path(std::u8string(
                        reinterpret_cast<const char8_t*>(stored.data()), reinterpret_cast<const char8_t*>(stored.data()) + stored.size()));
                    if (storedPath.is_absolute()) continue;
                    const auto absolute = std::filesystem::weakly_canonical(source.parent_path() / storedPath, code);
                    if (code) { code.clear(); continue; }
                    const auto rebased = absolute.lexically_relative(target);
                    if (rebased.empty()) continue;
                    mesh["sourcePath"] = pathText(rebased);
                    changed = true;
                }
            }
            if (changed) {
                std::ofstream output(source, std::ios::binary | std::ios::trunc);
                if (!output) { error = "Cannot rewrite the scene before moving it."; return false; }
                output << value.dump(2);
                output.close();
            }
        } catch (const std::exception& exception) {
            error = std::string("Cannot re-base the scene mesh paths: ") + exception.what();
            return false;
        }
    }

    std::filesystem::rename(source, destination, code);
    if (code) { error = "Cannot move the file: " + code.message(); return false; }
    return true;
}

} // namespace rae::assets
