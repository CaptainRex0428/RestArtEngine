#pragma once

#include "RestArtEngine/Assets/AssetLibrary.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace rae::assets {

// Deletes one writable Project Content file and evicts its loaded identity.
// Mesh/material deletion rejects references from saved scenes, duplicate GUIDs,
// Engine identities and unverifiable dependencies. External sources are never
// removed. The editor must additionally protect its live/unsaved references,
// open/startup scene and active drafts, and invalidate undo closures after success.
// Shader deletion is unavailable until include dependencies can be checked.
bool deleteContentAsset(AssetLibrary& library, const std::filesystem::path& contentRoot,
    std::string_view relativePath, std::string& error);

} // namespace rae::assets
