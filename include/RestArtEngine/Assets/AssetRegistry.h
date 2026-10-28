#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Assets/AssetThumbnail.h"

namespace rae::assets {

// Kinds of content the editor can show in the Content browser. The list is kept
// deliberately small: it mirrors what the engine can actually load today.
enum class AssetType { Unknown, Scene, Material, Mesh, Shader };
enum class AssetSort { Name, Type, Size };

inline constexpr std::string_view EngineContentPrefix = "engine://";
[[nodiscard]] inline bool isEngineContentPath(std::string_view path) noexcept {
    return path.starts_with(EngineContentPrefix);
}

struct AssetQuery {
    std::string folder;
    std::string search;
    // Unknown means all supported types, including unreadable containers.
    AssetType type = AssetType::Unknown;
    bool recursive = false;
    // Only superseded legacy files are hidden; a unique .ramat remains visible.
    bool showLegacy = false;
    AssetSort sort = AssetSort::Name;
    bool descending = false;
};

[[nodiscard]] const char* assetTypeName(AssetType type) noexcept;
// Extension based classification, independent from any in-memory asset library.
[[nodiscard]] AssetType assetTypeFromPath(const std::filesystem::path& path) noexcept;

struct AssetEntry {
    AssetType type = AssetType::Unknown;
    AssetOrigin origin = AssetOrigin::Project;
    [[nodiscard]] bool readOnly() const noexcept { return origin == AssetOrigin::Engine; }
    // Display name: the file stem for file-backed assets.
    std::string name;
    // Path of the containing folder relative to the content root, '/'-separated.
    // The root itself is the empty string.
    std::string folder;
    // Project-relative path, or engine:// plus the engine-content-relative path.
    // This source-qualified key is stable across machines and is also the drag payload.
    std::string relativePath;
    std::filesystem::path path;
    std::uint64_t sizeBytes = 0;
    // Preview decoded from the container, when the asset carries one.
    Thumbnail thumbnail;
    // Material and mesh containers carry a GUID; unsupported/invalid files use 0.
    AssetId id = 0;
};

// Caches the content tree so the editor can render a folder view without hitting
// the disk every frame. `scan` is the only mutator; call it after any file move,
// create or delete.
class AssetRegistry {
public:
    void setContentRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& contentRoot() const noexcept { return contentRoot_; }
    void setEngineContentRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& engineContentRoot() const noexcept { return engineContentRoot_; }

    // Full rescan of the content root. Missing roots are not an error: the
    // registry simply becomes empty.
    bool scan(std::string& error);

    [[nodiscard]] const std::vector<AssetEntry>& entries() const noexcept { return entries_; }
    // Every folder under the root, root ("") first, then sorted depth-first.
    [[nodiscard]] const std::vector<std::string>& folders() const noexcept { return folders_; }
    // Entries directly inside `folder` (no recursion), sorted by type then name.
    [[nodiscard]] std::vector<const AssetEntry*> inFolder(std::string_view folder) const;
    // Search matches every whitespace-separated term against name or relative
    // path. ASCII case-insensitive; UTF-8 non-ASCII bytes are preserved.
    [[nodiscard]] std::vector<const AssetEntry*> query(const AssetQuery& query) const;
    [[nodiscard]] std::vector<std::string> childFolders(std::string_view folder) const;
    [[nodiscard]] const AssetEntry* find(std::string_view relativePath) const;
    [[nodiscard]] std::size_t count(AssetType type) const noexcept;

private:
    std::filesystem::path contentRoot_;
    std::filesystem::path engineContentRoot_;
    std::vector<AssetEntry> entries_;
    std::vector<std::string> folders_;
};

// Moves one file-backed asset inside the content root; folders are rejected. Materials and
// scenes stay valid: a moved scene has its relative mesh source paths rewritten
// so imported OBJ references still resolve from the new folder.
bool moveContentEntry(const std::filesystem::path& contentRoot, std::string_view relativePath,
                      std::string_view targetFolder, std::string& error);

} // namespace rae::assets
