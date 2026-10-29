#pragma once

#include "RestArtEngine/Assets/AssetImportOptions.h"

#include <filesystem>
#include <string>

namespace rae::assets {
struct MeshAsset;
struct MeshImportDiagnostics;

namespace detail {
// Reads static FBX mesh instances into a single mesh without assigning an asset
// identity or mutating the library. The output is replaced only on success;
// diagnostics are reset on entry and populated only on success.
bool readFbxMesh(const std::filesystem::path& path, MeshAsset& output, std::string& error,
    MeshImportDiagnostics* diagnostics = nullptr, const AssetImportOptions& options = {});
} // namespace detail
} // namespace rae::assets
