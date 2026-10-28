#include "RestArtEngine/Assets/AssetRegistry.h"
#include "RestArtEngine/Assets/AssetFile.h"
#include "ContentFiles.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <sstream>

#include <nlohmann/json.hpp>

namespace rae::assets {
namespace {

using Json = nlohmann::json;

std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string normalizedFolder(std::string_view folder) {
    const bool engine = isEngineContentPath(folder);
    if (engine) folder.remove_prefix(EngineContentPrefix.size());
    std::string value(folder);
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto result = pathText(std::filesystem::path(std::u8string(value.begin(), value.end())).lexically_normal());
    const auto relative = result == "." ? std::string{} :
        (!result.empty() && result.back() == '/' ? result.substr(0, result.size() - 1) : result);
    return engine ? std::string(EngineContentPrefix) + relative : relative;
}

std::string folded(std::string_view text) {
    std::string result(text);
    for (char& value : result) if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
    return result;
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

namespace {

// Scene thumbnails are rendered from Actor placements: no GPU pass and no stored
// image, so they always match the saved layout.
Thumbnail sceneThumbnail(const std::filesystem::path& path) {
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return {};
        const Json value = Json::parse(stream);
        if (!value.contains("actors") || !value["actors"].is_array()) return {};
        std::vector<ScenePreviewBox> boxes;
        for (const auto& actor : value["actors"]) {
            if (!actor.contains("transform")) continue;
            const auto& transform = actor["transform"];
            if (!transform.contains("position") || !transform["position"].is_array()) continue;
            ScenePreviewBox box;
            for (int axis = 0; axis < 3; ++axis)
                box.position[axis] = transform["position"].at(static_cast<std::size_t>(axis)).get<float>();
            if (transform.contains("scale") && transform["scale"].is_array()) {
                for (int axis = 0; axis < 3; ++axis)
                    box.extent[axis] = std::max(0.25f, transform["scale"].at(static_cast<std::size_t>(axis)).get<float>());
            }
            const auto meshId = actor.contains("mesh") && actor["mesh"].contains("meshId")
                ? actor["mesh"]["meshId"].get<std::uint64_t>() : 0ull;
            const auto tint = static_cast<std::uint32_t>(meshId % 5);
            const float colors[5][3] = {{0.62f, 0.70f, 0.82f}, {0.80f, 0.55f, 0.35f}, {0.45f, 0.72f, 0.55f},
                {0.75f, 0.62f, 0.82f}, {0.72f, 0.72f, 0.42f}};
            for (int channel = 0; channel < 3; ++channel) box.color[channel] = colors[tint][channel];
            boxes.push_back(box);
        }
        return renderSceneThumbnail(boxes, 64);
    } catch (const std::exception&) {
        return {};
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

void AssetRegistry::setEngineContentRoot(std::filesystem::path root) {
    engineContentRoot_ = std::move(root);
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
    const auto scanRoot = [&](const std::filesystem::path& root, AssetOrigin origin) {
        const std::string prefix = origin == AssetOrigin::Engine ? std::string(EngineContentPrefix) : std::string{};
        if (origin == AssetOrigin::Engine) folders_.push_back(prefix);
        std::error_code code;
        if (!std::filesystem::is_directory(root, code)) return true;
        if (detail::contentLink(root)) { error = "Content root must not be a linked directory."; return false; }

        std::filesystem::recursive_directory_iterator iterator(root,
            std::filesystem::directory_options::skip_permission_denied, code);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(code)) {
            if (code) { code.clear(); continue; }
            const auto& path = iterator->path();
            if (detail::contentLink(path)) { iterator.disable_recursion_pending(); continue; }
            const auto fileName = path.filename().wstring();
            if (!fileName.empty() && fileName.front() == L'.') {
                if (iterator->is_directory(code)) { iterator.disable_recursion_pending(); continue; }
                continue;
            }
            if (iterator->is_directory(code)) {
                const auto relative = path.lexically_relative(root);
                if (!relative.empty()) folders_.push_back(prefix + pathText(relative));
                continue;
            }
            if (!iterator->is_regular_file(code)) continue;
            const auto extension = path.extension();
            AssetEntry entry;
            entry.origin = origin;
            entry.path = path;
            entry.relativePath = prefix + pathText(path.lexically_relative(root));
            entry.folder = pathText(path.parent_path().lexically_relative(root));
            if (entry.folder == ".") entry.folder.clear();
            entry.folder = prefix + entry.folder;
            entry.name = pathText(path.stem());
            if (isAssetContainerFile(path)) {
                // The container carries its own type, GUID and display name.
                AssetFile header;
                std::string headerError;
                if (readAssetFileHeader(path, header, headerError)) {
                    entry.id = header.id;
                    (void)readAssetThumbnail(path, entry.thumbnail, headerError);
                    switch (header.type) {
                    case AssetFileType::Material: entry.type = AssetType::Material; break;
                    case AssetFileType::StaticMesh: entry.type = AssetType::Mesh; break;
                    case AssetFileType::Scene: entry.type = AssetType::Scene; break;
                    default: entry.type = AssetType::Unknown; break;
                    }
                }
            } else {
                entry.type = assetTypeFromPath(path);
                if (entry.type == AssetType::Unknown) continue;
            }
            if (entry.type == AssetType::Scene) {
                // A GPU-rendered sidecar wins; the CPU layout stays as a fallback.
                Thumbnail sidecar;
                std::string thumbError;
                auto sidecarPath = path;
                sidecarPath += ".rthumb";
                if (!readSceneThumbnail(sidecarPath, sidecar, thumbError)) sidecar = sceneThumbnail(path);
                entry.thumbnail = std::move(sidecar);
            }
            std::error_code sizeCode;
            entry.sizeBytes = static_cast<std::uint64_t>(std::filesystem::file_size(path, sizeCode));
            if (sizeCode) entry.sizeBytes = 0;
            if (extension == L".ramat") {
                std::string innerName;
                AssetId innerId = 0;
                readMaterialHeader(path, innerName, innerId);
                entry.id = innerId;
            }
            entries_.push_back(std::move(entry));
        }
        return true;
    };
    if (!scanRoot(contentRoot_, AssetOrigin::Project)) return false;
    if (!engineContentRoot_.empty() && !scanRoot(engineContentRoot_, AssetOrigin::Engine)) return false;
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
    const auto normalized = normalizedFolder(folder);
    for (const auto& entry : entries_)
        if (entry.folder == normalized) result.push_back(&entry);
    return result;
}

std::vector<std::string> AssetRegistry::childFolders(std::string_view folder) const {
    const auto normalized = normalizedFolder(folder);
    std::vector<std::string> result;
    for (const auto& candidate : folders_) {
        if (candidate.empty() || candidate == EngineContentPrefix) continue;
        const auto slash = candidate.find_last_of('/');
        const auto parent = slash == std::string::npos ? std::string{} :
            (isEngineContentPath(candidate) && slash < EngineContentPrefix.size() ? std::string(EngineContentPrefix) : candidate.substr(0, slash));
        if (parent == normalized) result.push_back(candidate);
    }
    return result;
}

std::vector<const AssetEntry*> AssetRegistry::query(const AssetQuery& query) const {
    const auto folder = normalizedFolder(query.folder);
    std::istringstream terms(folded(query.search));
    std::vector<std::string> words;
    for (std::string word; terms >> word;) words.push_back(std::move(word));
    std::vector<const AssetEntry*> result;
    for (const auto& entry : entries_) {
        if (isEngineContentPath(folder) != (entry.origin == AssetOrigin::Engine)) continue;
        const bool sourceRoot = folder.empty() || folder == EngineContentPrefix;
        if (entry.folder != folder && !(query.recursive && (sourceRoot ||
            (entry.folder.size() > folder.size() && entry.folder.starts_with(folder) && entry.folder[folder.size()] == '/')))) continue;
        if (query.type != AssetType::Unknown && entry.type != query.type) continue;
        if (!query.showLegacy && entry.id != 0 && entry.path.extension() != ".rasset" &&
            (entry.path.extension() == ".ramat" || entry.path.extension() == ".raasset")) {
            const bool replaced = std::any_of(entries_.begin(), entries_.end(), [&entry](const AssetEntry& other) {
                return other.origin == entry.origin && other.id == entry.id && other.type == entry.type && other.path.extension() == ".rasset";
            });
            if (replaced) continue;
        }
        const auto searchable = folded(entry.name + " " + entry.relativePath);
        if (!std::all_of(words.begin(), words.end(), [&searchable](const std::string& word) {
            return searchable.find(word) != std::string::npos;
        })) continue;
        result.push_back(&entry);
    }
    std::sort(result.begin(), result.end(), [&query](const AssetEntry* a, const AssetEntry* b) {
        int comparison = 0;
        if (query.sort == AssetSort::Type && a->type != b->type)
            comparison = static_cast<int>(a->type) < static_cast<int>(b->type) ? -1 : 1;
        else if (query.sort == AssetSort::Size && a->sizeBytes != b->sizeBytes)
            comparison = a->sizeBytes < b->sizeBytes ? -1 : 1;
        if (comparison == 0) comparison = folded(a->name).compare(folded(b->name));
        if (comparison == 0) comparison = a->relativePath.compare(b->relativePath);
        return query.descending ? comparison > 0 : comparison < 0;
    });
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
    namespace fs = std::filesystem;
    error.clear();
    try {
        if (isEngineContentPath(relativePath) || isEngineContentPath(targetFolder))
            throw std::runtime_error("Engine Content is read-only. Copy an asset to the project before editing.");
        if (contentRoot.empty() || relativePath.empty()) throw std::runtime_error("Move needs a content root and a source path.");
        const auto checkedPath = [&](std::string_view text) {
            const auto relative = fs::path(std::u8string(text.begin(), text.end()));
            if (relative.has_root_path()) throw std::runtime_error("Content paths must be relative.");
            auto result = contentRoot;
            if (detail::contentLink(result)) throw std::runtime_error("Content root must not be linked.");
            for (const auto& part : relative) {
                if (part == "..") throw std::runtime_error("Content paths cannot leave their root.");
                if (part == "." || part.empty()) continue;
                result /= part;
                if (detail::contentLink(result)) throw std::runtime_error("Content moves cannot follow linked paths.");
            }
            return result.lexically_normal();
        };
        const auto source = checkedPath(relativePath);
        const auto target = checkedPath(targetFolder);
        if (!fs::is_regular_file(source)) throw std::runtime_error("Only asset files can be moved; folder moves are not supported.");
        if (source.parent_path() == target) return true;
        const auto destination = target / source.filename();
        auto sourceSidecar = source; sourceSidecar += ".rthumb";
        auto destinationSidecar = destination; destinationSidecar += ".rthumb";
        if (fs::exists(destination) || detail::contentLink(destination) || fs::exists(destinationSidecar) || detail::contentLink(destinationSidecar))
            throw std::runtime_error("An asset or preview with that name already exists in the target folder.");
        if (detail::contentLink(sourceSidecar)) throw std::runtime_error("Cannot move a linked preview file.");
        const bool hasSidecar = fs::exists(sourceSidecar);
        if (hasSidecar && !fs::is_regular_file(sourceSidecar)) throw std::runtime_error("The preview path is not a file.");

        // Prepare the scene payload without changing its original file. Failure
        // to create/rewrite the destination must leave the source usable.
        std::string rebasedScene;
        if (source.extension() == L".rascene") {
            std::ifstream input(source, std::ios::binary);
            Json value = Json::parse(input);
            if (value.contains("meshes") && value["meshes"].is_array()) {
                for (auto& mesh : value["meshes"]) {
                    if (!mesh.contains("sourcePath") || !mesh["sourcePath"].is_string()) continue;
                    const auto stored = mesh["sourcePath"].get<std::string>();
                    const auto storedPath = fs::path(std::u8string(stored.begin(), stored.end()));
                    if (stored.empty() || storedPath.is_absolute()) continue;
                    const auto absolute = fs::absolute(source.parent_path() / storedPath).lexically_normal();
                    const auto rebased = absolute.lexically_relative(fs::absolute(target).lexically_normal());
                    if (!rebased.empty()) mesh["sourcePath"] = pathText(rebased);
                }
            }
            rebasedScene = value.dump(2) + "\n";
        }
        fs::create_directories(target);
        bool destinationCreated = false, sidecarMoved = false;
        try {
            if (!rebasedScene.empty()) {
                // copy_file with no overwrite reserves the destination; the
                // original scene remains intact until every write succeeds.
                fs::copy_file(source, destination, fs::copy_options::none);
                destinationCreated = true;
                std::ofstream output(destination, std::ios::binary | std::ios::trunc);
                output << rebasedScene;
                output.close();
                if (!output) throw std::runtime_error("Cannot write the moved scene.");
            } else {
                fs::rename(source, destination);
                destinationCreated = true;
            }
            if (hasSidecar) { fs::rename(sourceSidecar, destinationSidecar); sidecarMoved = true; }
            if (!rebasedScene.empty() && !fs::remove(source)) throw std::runtime_error("Cannot remove the original scene after moving.");
        } catch (const std::exception& failure) {
            std::string rollback;
            std::error_code code;
            if (sidecarMoved) { fs::rename(destinationSidecar, sourceSidecar, code); if (code) rollback += " Preview rollback: " + code.message(); }
            code.clear();
            if (destinationCreated) {
                if (!rebasedScene.empty()) fs::remove(destination, code);
                else fs::rename(destination, source, code);
                if (code) rollback += " Asset rollback: " + code.message();
            }
            throw std::runtime_error(std::string(failure.what()) + rollback);
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace rae::assets
