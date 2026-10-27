#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "RestArtEngine/Assets/AssetLibrary.h"

namespace rae::assets {

// Self-contained asset container (".raasset"). Layout:
//   0  magic "RAAS" | 4 version u16 | 6 flags u16 | 8 manifestSize u64
//  16  manifest JSON (UTF-8) | 16 + manifestSize: blob region
// Integers are little-endian. Blobs are referenced from the manifest.
inline constexpr std::uint16_t kAssetFileVersion = 1;
inline constexpr std::uint64_t kAssetFileHeaderSize = 16;
inline constexpr std::uint16_t kAssetFlagThumbnail = 1u << 0;

enum class AssetFileType { Unknown, Material, StaticMesh, Scene, Texture };

[[nodiscard]] const char* assetFileTypeName(AssetFileType type) noexcept;
[[nodiscard]] AssetFileType assetFileTypeFromName(std::string_view name) noexcept;

// 64-bit GUID rendered as 16 lowercase hex digits in files, so the identity is
// readable in JSON while the engine keeps its existing uint64 AssetId.
[[nodiscard]] AssetId generateAssetId();
[[nodiscard]] std::string assetIdText(AssetId id);
[[nodiscard]] bool parseAssetId(std::string_view text, AssetId& result) noexcept;

struct AssetFile {
    AssetFileType type = AssetFileType::Unknown;
    AssetId id = 0;
    std::string name;
    nlohmann::json manifest = nlohmann::json::object();
    // Raw bytes of the blob region. Payload offsets are relative to its start.
    std::vector<std::uint8_t> blobs;
};

// Reads only the header and manifest; blobs stay empty. Returns false with a
// message for a missing file, bad magic, unsupported version or truncated data.
bool readAssetFileHeader(const std::filesystem::path& path, AssetFile& file, std::string& error);
// Reads the header, manifest and blob region.
bool readAssetFile(const std::filesystem::path& path, AssetFile& file, std::string& error);
bool writeAssetFile(const std::filesystem::path& path, const AssetFile& file, std::string& error);

// Material payload helpers: a material asset stores its parameters in the
// manifest, so browsers never need the blob region.
[[nodiscard]] AssetFile makeMaterialAssetFile(const MaterialAsset& material);
bool readMaterialAssetFile(const AssetFile& file, MaterialAsset& material, std::string& error);

// Static mesh payload: vertex/index blobs plus counts. Built-in primitives are
// stored the same way, so a scene never needs engine-side geometry tables.
[[nodiscard]] AssetFile makeMeshAssetFile(const MeshAsset& mesh);
bool readMeshAssetFile(const AssetFile& file, MeshAsset& mesh, std::string& error);

// File name for a material asset: a sanitized display name plus the extension.
// Collisions are resolved by the caller appending the GUID.
[[nodiscard]] std::string sanitizeAssetName(std::string_view name);

} // namespace rae::assets
