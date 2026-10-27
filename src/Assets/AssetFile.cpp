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


namespace {

const char* primitiveName(MeshPrimitive primitive) {
    switch (primitive) {
    case MeshPrimitive::Sphere: return "Sphere";
    case MeshPrimitive::Plane: return "Plane";
    case MeshPrimitive::Cube: return "Cube";
    default: return "Imported";
    }
}

MeshPrimitive primitiveFromName(std::string_view name) {
    if (name == "Sphere") return MeshPrimitive::Sphere;
    if (name == "Plane") return MeshPrimitive::Plane;
    if (name == "Cube") return MeshPrimitive::Cube;
    return MeshPrimitive::Imported;
}

Json blobRange(std::size_t offset, std::size_t size) {
    return Json{{"offset", offset}, {"size", size}};
}

bool readBlob(const AssetFile& file, const Json& range, std::size_t elementSize,
              std::size_t count, std::vector<std::uint8_t>& out, std::string& error) {
    const auto offset = range.at("offset").get<std::size_t>();
    const auto size = range.at("size").get<std::size_t>();
    if (offset > file.blobs.size() || size > file.blobs.size() - offset) {
        error = "Mesh blob range is outside the asset data.";
        return false;
    }
    if (size != elementSize * count) {
        error = "Mesh blob size does not match its element count.";
        return false;
    }
    out.assign(file.blobs.begin() + static_cast<std::ptrdiff_t>(offset),
        file.blobs.begin() + static_cast<std::ptrdiff_t>(offset + size));
    return true;
}

} // namespace

AssetFile makeMeshAssetFile(const MeshAsset& mesh) {
    AssetFile file;
    file.type = AssetFileType::StaticMesh;
    file.id = mesh.id;
    file.name = mesh.name;
    const auto vertexBytes = mesh.vertices.size() * sizeof(MeshVertex);
    const auto indexBytes = mesh.indices.size() * sizeof(std::uint32_t);
    file.blobs.resize(vertexBytes + indexBytes);
    if (vertexBytes) std::memcpy(file.blobs.data(), mesh.vertices.data(), vertexBytes);
    if (indexBytes)
        std::memcpy(file.blobs.data() + vertexBytes, mesh.indices.data(), indexBytes);
    Json bounds{{"min", {0.0f, 0.0f, 0.0f}}, {"max", {0.0f, 0.0f, 0.0f}}};
    if (!mesh.vertices.empty()) {
        for (int axis = 0; axis < 3; ++axis) {
            bounds["min"][axis] = mesh.vertices.front().position[axis];
            bounds["max"][axis] = mesh.vertices.front().position[axis];
        }
        for (const auto& vertex : mesh.vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                bounds["min"][axis] = std::min(bounds["min"][axis].get<float>(), vertex.position[axis]);
                bounds["max"][axis] = std::max(bounds["max"][axis].get<float>(), vertex.position[axis]);
            }
        }
    }
    file.manifest = Json{
        {"payload", Json{
            {"primitive", primitiveName(mesh.primitive)},
            {"vertexCount", mesh.vertices.size()},
            {"indexCount", mesh.indices.size()},
            {"vertexStride", sizeof(MeshVertex)},
            {"indexFormat", "u32"},
            {"bounds", bounds},
            {"blobs", Json{
                {"vertices", blobRange(0, vertexBytes)},
                {"indices", blobRange(vertexBytes, indexBytes)},
            }},
        }},
    };
    if (!mesh.sourcePath.empty()) file.manifest["payload"]["sourcePath"] = mesh.sourcePath;
    return file;
}

bool readMeshAssetFile(const AssetFile& file, MeshAsset& mesh, std::string& error) {
    error.clear();
    if (file.type != AssetFileType::StaticMesh) { error = "The asset is not a static mesh."; return false; }
    try {
        const auto& payload = file.manifest.at("payload");
        const auto vertexCount = payload.at("vertexCount").get<std::size_t>();
        const auto indexCount = payload.at("indexCount").get<std::size_t>();
        if (payload.at("vertexStride").get<std::size_t>() != sizeof(MeshVertex)) {
            error = "Mesh vertex stride is not supported.";
            return false;
        }
        if (payload.at("indexFormat").get<std::string>() != "u32") {
            error = "Mesh index format is not supported.";
            return false;
        }
        std::vector<std::uint8_t> vertexBytes;
        std::vector<std::uint8_t> indexBytes;
        const auto& blobs = payload.at("blobs");
        if (!readBlob(file, blobs.at("vertices"), sizeof(MeshVertex), vertexCount, vertexBytes, error)) return false;
        if (!readBlob(file, blobs.at("indices"), sizeof(std::uint32_t), indexCount, indexBytes, error)) return false;
        MeshAsset loaded;
        loaded.id = file.id;
        loaded.name = file.name;
        loaded.primitive = primitiveFromName(payload.value("primitive", std::string()));
        loaded.sourcePath = payload.value("sourcePath", std::string());
        loaded.vertices.resize(vertexCount);
        loaded.indices.resize(indexCount);
        if (!vertexBytes.empty()) std::memcpy(loaded.vertices.data(), vertexBytes.data(), vertexBytes.size());
        if (!indexBytes.empty()) std::memcpy(loaded.indices.data(), indexBytes.data(), indexBytes.size());
        if (loaded.vertices.empty() || loaded.indices.empty()) {
            error = "Mesh asset has no geometry.";
            return false;
        }
        mesh = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Mesh payload is invalid: ") + exception.what();
        return false;
    }
}

void attachThumbnail(AssetFile& file, const Thumbnail& thumbnail) {
    if (!thumbnail.valid()) return;
    const auto offset = file.blobs.size();
    file.blobs.insert(file.blobs.end(), thumbnail.rgba.begin(), thumbnail.rgba.end());
    file.manifest["thumbnail"] = Json{
        {"offset", offset},
        {"size", thumbnail.rgba.size()},
        {"width", thumbnail.width},
        {"height", thumbnail.height},
        {"format", "rgba8"},
    };
}

bool extractThumbnail(const AssetFile& file, Thumbnail& thumbnail, std::string& error) {
    error.clear();
    if (!file.manifest.contains("thumbnail")) { error = "The asset has no thumbnail."; return false; }
    try {
        const auto& entry = file.manifest.at("thumbnail");
        const auto offset = entry.at("offset").get<std::size_t>();
        const auto size = entry.at("size").get<std::size_t>();
        const auto width = entry.at("width").get<std::uint32_t>();
        const auto height = entry.at("height").get<std::uint32_t>();
        if (entry.value("format", std::string()) != "rgba8") { error = "Unsupported thumbnail format."; return false; }
        if (offset > file.blobs.size() || size > file.blobs.size() - offset ||
            size != static_cast<std::size_t>(width) * height * 4) {
            error = "Thumbnail blob range is invalid.";
            return false;
        }
        Thumbnail loaded;
        loaded.width = width;
        loaded.height = height;
        loaded.rgba.assign(file.blobs.begin() + static_cast<std::ptrdiff_t>(offset),
            file.blobs.begin() + static_cast<std::ptrdiff_t>(offset + size));
        thumbnail = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Thumbnail metadata is invalid: ") + exception.what();
        return false;
    }
}

bool readAssetThumbnail(const std::filesystem::path& path, Thumbnail& thumbnail, std::string& error) {
    AssetFile header;
    if (!readAssetFileHeader(path, header, error)) return false;
    if (!header.manifest.contains("thumbnail")) { error = "The asset has no thumbnail."; return false; }
    try {
        const auto& entry = header.manifest.at("thumbnail");
        const auto offset = entry.at("offset").get<std::uint64_t>();
        const auto size = entry.at("size").get<std::uint64_t>();
        const auto width = entry.at("width").get<std::uint32_t>();
        const auto height = entry.at("height").get<std::uint32_t>();
        if (entry.value("format", std::string()) != "rgba8") { error = "Unsupported thumbnail format."; return false; }
        if (size != static_cast<std::uint64_t>(width) * height * 4) { error = "Thumbnail size is invalid."; return false; }
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) { error = "Cannot open the asset file: " + path.generic_string(); return false; }
        const auto fileSize = static_cast<std::uint64_t>(stream.tellg());
        std::array<std::uint8_t, 8> sizeBytes{};
        stream.seekg(8);
        stream.read(reinterpret_cast<char*>(sizeBytes.data()), 8);
        const std::uint64_t manifestSize = readU64(sizeBytes.data());
        const std::uint64_t absolute = kAssetFileHeaderSize + manifestSize + offset;
        if (absolute > fileSize || size > fileSize - absolute) { error = "Thumbnail is outside the file."; return false; }
        Thumbnail loaded;
        loaded.width = width;
        loaded.height = height;
        loaded.rgba.assign(static_cast<std::size_t>(size), 0);
        stream.seekg(static_cast<std::streamoff>(absolute));
        stream.read(reinterpret_cast<char*>(loaded.rgba.data()), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size)) { error = "Thumbnail read failed."; return false; }
        thumbnail = std::move(loaded);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Thumbnail metadata is invalid: ") + exception.what();
        return false;
    }
}
} // namespace rae::assets
