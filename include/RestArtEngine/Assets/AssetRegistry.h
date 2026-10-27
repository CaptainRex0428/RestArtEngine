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

[[nodiscard]] const char* assetTypeName(AssetType type) noexcept;
// Extension based classification, independent from any in-memory asset library.
[[nodiscard]] AssetType assetTypeFromPath(const std::filesystem::path& path) noexcept;

struct AssetEntry {
    AssetType type = AssetType::Unknown;
    // Display name: the file stem for file-backed assets.
    std::string name;
    // Path of the containing folder relative to the content root, '/'-separated.
    // The root itself is the empty string.
    std::string folder;
    // Path relative to the content root, '/'-separated, stable across machines.
    std::string relativePath;
    std::filesystem::path path;
    std::uint64_t sizeBytes = 0;
    // Preview decoded from the container, when the asset carries one.
    Thumbnail thumbnail;
    // Material files carry their own AssetId; other types report 0 until the
    // asset format lands.
    AssetId id = 0;
};

// Caches the content tree so the editor can render a folder view without hitting
// the disk every frame. `scan` is the only mutator; call it after any file move,
// create or delete.
class AssetRegistry {
public:
    void setContentRoot(std::filesystem::path root);
    [[nodiscard]] const std::filesystem::path& contentRoot() const noexcept { return contentRoot_; }

    // Full rescan of the content root. Missing roots are not an error: the
    // registry simply becomes empty.
    bool scan(std::string& error);

    [[nodiscard]] const std::vector<AssetEntry>& entries() const noexcept { return entries_; }
    // Every folder under the root, root ("") first, then sorted depth-first.
    [[nodiscard]] const std::vector<std::string>& folders() const noexcept { return folders_; }
    // Entries directly inside `folder` (no recursion), sorted by type then name.
    [[nodiscard]] std::vector<const AssetEntry*> inFolder(std::string_view folder) const;
    [[nodiscard]] const AssetEntry* find(std::string_view relativePath) const;
    [[nodiscard]] std::size_t count(AssetType type) const noexcept;

private:
    std::filesystem::path contentRoot_;
    std::vector<AssetEntry> entries_;
    std::vector<std::string> folders_;
};

// Moves one file-backed asset (or folder) inside the content root. Materials and
// scenes stay valid: a moved scene has its relative mesh source paths rewritten
// so imported OBJ references still resolve from the new folder.
bool moveContentEntry(const std::filesystem::path& contentRoot, std::string_view relativePath,
                      std::string_view targetFolder, std::string& error);

} // namespace rae::assets
