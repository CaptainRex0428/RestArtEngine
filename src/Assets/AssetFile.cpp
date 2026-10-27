#include "RestArtEngine/Assets/AssetFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <random>

namespace rae::assets {
namespace {

using Json = nlohmann::json;

constexpr std::array<char, 4> kMagic{'R', 'A', 'A', 'S'};

void writeU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void writeU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) out.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF));
}

std::uint16_t readU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint64_t readU64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(data[index]) << (index * 8);
    return value;
}

} // namespace

const char* assetFileTypeName(AssetFileType type) noexcept {
    switch (type) {
    case AssetFileType::Material: return "material";
    case AssetFileType::StaticMesh: return "staticMesh";
    case AssetFileType::Scene: return "scene";
    case AssetFileType::Texture: return "texture";
    default: return "unknown";
    }
}

AssetFileType assetFileTypeFromName(std::string_view name) noexcept {
    if (name == "material") return AssetFileType::Material;
    if (name == "staticMesh") return AssetFileType::StaticMesh;
    if (name == "scene") return AssetFileType::Scene;
    if (name == "texture") return AssetFileType::Texture;
    return AssetFileType::Unknown;
}

AssetId generateAssetId() {
    // 64 random bits are enough for per-project uniqueness; the zero value stays
    // reserved for "no asset".
    static std::mt19937_64 engine{std::random_device{}()};
    AssetId id = engine();
    while (id == 0) id = engine();
    return id;
}

std::string assetIdText(AssetId id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] = digits[id & 0xF];
        id >>= 4;
    }
    return text;
}

bool parseAssetId(std::string_view text, AssetId& result) noexcept {
    if (text.size() != 16) return false;
    AssetId value = 0;
    for (const char character : text) {
        value <<= 4;
        if (character >= '0' && character <= '9') value |= static_cast<AssetId>(character - '0');
        else if (character >= 'a' && character <= 'f') value |= static_cast<AssetId>(character - 'a' + 10);
        else if (character >= 'A' && character <= 'F') value |= static_cast<AssetId>(character - 'A' + 10);
        else return false;
    }
    result = value;
    return value != 0;
}

std::string sanitizeAssetName(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const char character : name) {
        const bool invalid = character == '/' || character == '\\' || character == ':' || character == '*' ||
            character == '?' || character == '"' || character == '<' || character == '>' || character == '|';
        result.push_back(invalid ? '_' : character);
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) result.pop_back();
    if (result.empty()) result = "Asset";
    return result;
}

bool readAssetFileHeader(const std::filesystem::path& path, AssetFile& file, std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = "Cannot open the asset file: " + path.generic_string(); return false; }
    std::array<std::uint8_t, kAssetFileHeaderSize> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size())) {
        error = "Asset file is shorter than its header: " + path.generic_string();
        return false;
    }
    if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
        error = "Asset file has an invalid header (expected RAAS): " + path.generic_string();
        return false;
    }
    const std::uint16_t version = readU16(header.data() + 4);
    if (version == 0 || version > kAssetFileVersion) {
        error = "Unsupported asset version " + std::to_string(version) + ": " + path.generic_string();
        return false;
    }
    const std::uint64_t manifestSize = readU64(header.data() + 8);
    if (manifestSize == 0 || manifestSize > (1ull << 24)) {
        error = "Asset manifest size is invalid: " + path.generic_string();
        return false;
    }
    std::string manifestText(static_cast<std::size_t>(manifestSize), '\0');
    stream.read(manifestText.data(), static_cast<std::streamsize>(manifestSize));
    if (stream.gcount() != static_cast<std::streamsize>(manifestSize)) {
        error = "Asset manifest is truncated: " + path.generic_string();
        return false;
    }
    try {
        file.manifest = Json::parse(manifestText);
    } catch (const std::exception& exception) {
        error = std::string("Asset manifest is not valid JSON: ") + exception.what();
        return false;
    }
    if (!file.manifest.contains("format") || file.manifest.at("format") != "RAEngine.Asset") {
        error = "Asset manifest has an unexpected format: " + path.generic_string();
        return false;
    }
    file.type = assetFileTypeFromName(file.manifest.value("type", std::string()));
    if (file.type == AssetFileType::Unknown) {
        error = "Asset manifest has an unknown type: " + path.generic_string();
        return false;
    }
    const auto idText = file.manifest.value("id", std::string());
    if (!parseAssetId(idText, file.id)) {
        error = "Asset manifest has an invalid GUID: " + path.generic_string();
        return false;
    }
    file.name = file.manifest.value("name", std::string());
    if (file.name.empty()) file.name = path.stem().generic_string();
    return true;
}

bool readAssetFile(const std::filesystem::path& path, AssetFile& file, std::string& error) {
    if (!readAssetFileHeader(path, file, error)) return false;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) { error = "Cannot open the asset file: " + path.generic_string(); return false; }
    const auto size = static_cast<std::uint64_t>(stream.tellg());
    // Re-read the stored manifest length from the header (offset 8) instead of
    // re-serializing the parsed JSON.
    stream.seekg(8);
    std::array<std::uint8_t, 8> sizeBytes{};
    stream.read(reinterpret_cast<char*>(sizeBytes.data()), 8);
    const std::uint64_t storedManifestSize = readU64(sizeBytes.data());
    const std::uint64_t blobStart = kAssetFileHeaderSize + storedManifestSize;
    if (size < blobStart) { error = "Asset file is truncated: " + path.generic_string(); return false; }
    const auto blobSize = static_cast<std::size_t>(size - blobStart);
    file.blobs.assign(blobSize, 0);
    if (blobSize > 0) {
        stream.seekg(static_cast<std::streamoff>(blobStart));
        stream.read(reinterpret_cast<char*>(file.blobs.data()), static_cast<std::streamsize>(blobSize));
        if (stream.gcount() != static_cast<std::streamsize>(blobSize)) {
            error = "Asset blob region is truncated: " + path.generic_string();
            return false;
        }
    }
    return true;
}

bool writeAssetFile(const std::filesystem::path& path, const AssetFile& file, std::string& error) {
    error.clear();
    if (file.type == AssetFileType::Unknown) { error = "Cannot write an asset with an unknown type."; return false; }
    if (file.id == 0) { error = "Cannot write an asset without a GUID."; return false; }
    Json manifest = file.manifest;
    manifest["format"] = "RAEngine.Asset";
    manifest["version"] = kAssetFileVersion;
    manifest["type"] = assetFileTypeName(file.type);
    manifest["id"] = assetIdText(file.id);
    manifest["name"] = file.name;
    const auto manifestText = manifest.dump(2);

    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(kAssetFileHeaderSize) + manifestText.size() + file.blobs.size());
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    writeU16(bytes, kAssetFileVersion);
    writeU16(bytes, 0);
    writeU64(bytes, manifestText.size());
    bytes.insert(bytes.end(), manifestText.begin(), manifestText.end());
    bytes.insert(bytes.end(), file.blobs.begin(), file.blobs.end());

    std::error_code code;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { error = "Cannot write the asset file: " + path.generic_string(); return false; }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream) { error = "Writing the asset file failed: " + path.generic_string(); return false; }
    return true;
}

AssetFile makeMaterialAssetFile(const MaterialAsset& material) {
    AssetFile file;
    file.type = AssetFileType::Material;
    file.id = material.id;
    file.name = material.name;
    file.manifest = Json{
        {"payload", Json{
            {"baseColor", material.baseColor},
            {"roughness", material.roughness},
            {"metallic", material.metallic},
            {"grid", material.grid},
            {"twoSided", material.twoSided},
        }},
    };
    return file;
}

bool readMaterialAssetFile(const AssetFile& file, MaterialAsset& material, std::string& error) {
    error.clear();
    if (file.type != AssetFileType::Material) { error = "The asset is not a material."; return false; }
    try {
        const auto& payload = file.manifest.at("payload");
        material = MaterialAsset{};
        material.id = file.id;
        material.name = file.name;
        const auto& color = payload.at("baseColor");
        for (std::size_t index = 0; index < 3; ++index) material.baseColor[index] = color.at(index).get<float>();
        material.roughness = payload.at("roughness").get<float>();
        material.metallic = payload.at("metallic").get<float>();
        material.grid = payload.value("grid", false);
        material.twoSided = payload.value("twoSided", false);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Material payload is invalid: ") + exception.what();
        return false;
    }
}

} // namespace rae::assets
