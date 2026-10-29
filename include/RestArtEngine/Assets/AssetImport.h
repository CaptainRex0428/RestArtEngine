#pragma once

#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Assets/AssetImportOptions.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rae::assets {

// File dialogs and external file drops use this same list of source formats.
// Extensions include their leading dot and are matched case-insensitively.
struct AssetImportFormat {
    std::string_view extension;
    std::string_view description;
};
[[nodiscard]] std::span<const AssetImportFormat> supportedImportFormats() noexcept;

struct AssetImportResult {
    std::filesystem::path sourcePath;
    // Absolute path of the newly created, self-contained project asset.
    std::filesystem::path assetPath;
    AssetId id = 0;
    std::string name;
    std::string error;
    // Recoverable cleanup on successful imports is visible to both UI entries.
    std::vector<std::string> warnings;
    [[nodiscard]] bool succeeded() const noexcept { return id != 0; }
};

struct AssetImportReport {
    std::vector<AssetImportResult> items;
    [[nodiscard]] std::size_t importedCount() const noexcept;
    [[nodiscard]] std::size_t rejectedCount() const noexcept;
    [[nodiscard]] std::size_t warningCount() const noexcept;
    [[nodiscard]] std::string summary() const;
};

// Each source is an independent transaction. Unsupported/corrupt sources leave
// no asset behind; a mixed batch retains its successful imports and reports all
// rejections. New assets never overwrite existing files. No Actor is created.
// destinationFolder is UTF-8 relative to the writable project Content mount;
// an empty string selects its root. Absolute, escaping, and linked targets and
// the read-only engine:// mount are rejected.
[[nodiscard]] AssetImportReport importAssets(AssetLibrary& library,
    const std::filesystem::path& contentRoot, std::string_view destinationFolder,
    std::span<const std::filesystem::path> sources, const AssetImportOptions& options = {});

} // namespace rae::assets
