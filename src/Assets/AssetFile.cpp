#include "RestArtEngine/Assets/AssetFile.h"
#include "AssetSerialization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_set>
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
    // Project identities never occupy the engine-owned range or sentinel values.
    static std::mt19937_64 engine{std::random_device{}()};
    AssetId id = engine();
    while (id == 0 || id == std::numeric_limits<AssetId>::max() || isEngineAssetId(id)) id = engine();
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
    // Detect errors from buffered writes before a new import is published.
    stream.flush();
    stream.close();
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
            {"cullMode", detail::materialCullModeName(material.effectiveCullMode())},
            {"depthTest", detail::materialDepthTestName(material.depthTest)},
            {"depthWrite", material.depthWrite},
        }},
    };
    return file;
}

bool readMaterialAssetFile(const AssetFile& file, MaterialAsset& material, std::string& error) {
    error.clear();
    if (file.type != AssetFileType::Material) { error = "The asset is not a material."; return false; }
    try {
        const auto& payload = file.manifest.at("payload");
        MaterialAsset loaded;
        loaded.id = file.id;
        loaded.name = file.name;
        const auto& color = payload.at("baseColor");
        detail::readVector(color, loaded.baseColor);
        loaded.roughness = payload.at("roughness").get<float>();
        loaded.metallic = payload.at("metallic").get<float>();
        loaded.grid = payload.value("grid", false);
        detail::readMaterialRenderState(payload, loaded, false);
        detail::validateMaterial(loaded);
        material = std::move(loaded);
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

std::size_t meshUnsigned(const Json& value, const char* field);

bool readBlob(const AssetFile& file, const Json& range, std::size_t elementSize,
              std::size_t count, std::vector<std::uint8_t>& out, std::string& error) {
    const auto offset = meshUnsigned(range.at("offset"), "blob offset");
    const auto size = meshUnsigned(range.at("size"), "blob size");
    if (offset > file.blobs.size() || size > file.blobs.size() - offset) {
        error = "Mesh blob range is outside the asset data.";
        return false;
    }
    if (count > std::numeric_limits<std::size_t>::max() / elementSize || size != elementSize * count) {
        error = "Mesh blob size does not match its element count.";
        return false;
    }
    out.assign(file.blobs.begin() + static_cast<std::ptrdiff_t>(offset),
        file.blobs.begin() + static_cast<std::ptrdiff_t>(offset + size));
    return true;
}

std::size_t meshUnsigned(const Json& value, const char* field) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        throw std::runtime_error(std::string(field) + " must be a nonnegative integer.");
    const auto result = value.get<std::uint64_t>();
    if (result > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error(std::string(field) + " exceeds the supported range.");
    return static_cast<std::size_t>(result);
}

void validateMeshAttributes(const MeshAsset& mesh) {
    std::unordered_set<std::string> keys;
    for (const auto& slot : mesh.materialSlots) {
        if (slot.name.empty()) throw std::runtime_error("Mesh material slot names cannot be empty.");
        const auto& effectiveKey = slot.sourceKey.empty() ? slot.name : slot.sourceKey;
        if (!keys.insert(effectiveKey).second)
            throw std::runtime_error("Mesh material slot effective identities must be unique (sourceKey, or name when absent).");
        if (slot.defaultMaterialId) detail::readId(Json(slot.defaultMaterialId));
    }
    if (mesh.sections.empty() && mesh.materialSlots.size() > 1)
        throw std::runtime_error("A multi-material mesh requires explicit sections.");
    std::size_t nextIndex = 0;
    for (const auto& section : mesh.sections) {
        if (section.firstIndex != nextIndex || !section.indexCount || section.indexCount % 3 ||
            section.materialSlot >= mesh.materialSlotCount() || nextIndex > mesh.indices.size() ||
            section.indexCount > mesh.indices.size() - nextIndex)
            throw std::runtime_error("Mesh sections must be contiguous triangle ranges with valid material slots.");
        nextIndex += section.indexCount;
    }
    if (!mesh.sections.empty() && nextIndex != mesh.indices.size())
        throw std::runtime_error("Mesh sections must cover the entire index buffer.");
    for (std::size_t channel = 0; channel < MaxMeshUvChannels; ++channel) {
        const auto& uv = mesh.uvChannels[channel];
        if (uv.empty()) {
            if (!mesh.uvNames[channel].empty()) throw std::runtime_error("An absent UV channel cannot have a name.");
            continue;
        }
        if (uv.size() != mesh.vertices.size()) throw std::runtime_error("UV streams must match the mesh vertex count.");
        for (const auto& value : uv)
            for (float component : value)
                if (!std::isfinite(component)) throw std::runtime_error("Mesh UV coordinates must be finite.");
    }
}

} // namespace

AssetFile makeMeshAssetFile(const MeshAsset& mesh) {
    validateMeshAttributes(mesh);
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
    // JSON lookup/conversion for every coordinate dominated large mesh saves.
    // The geometry revision also governs the cached numeric bounds.
    const auto& localBounds = mesh.localBounds();
    if (!mesh.vertices.empty() && !localBounds.valid)
        throw std::runtime_error("Cannot serialize non-finite mesh bounds.");
    Json bounds{{"min", localBounds.minimum}, {"max", localBounds.maximum}};
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
    auto& payload = file.manifest["payload"];
    if (!mesh.sourcePath.empty()) payload["sourcePath"] = mesh.sourcePath;
    // Optional attributes leave legacy/no-attribute containers byte-compatible.
    if (!mesh.materialSlots.empty() || !mesh.sections.empty() || mesh.uvChannelMask()) payload["meshAttributesVersion"] = 1;
    if (!mesh.materialSlots.empty()) {
        payload["materialSlots"] = Json::array();
        for (const auto& slot : mesh.materialSlots)
            payload["materialSlots"].push_back({{"name",slot.name},{"sourceKey",slot.sourceKey},{"defaultMaterialId",slot.defaultMaterialId}});
    }
    if (!mesh.sections.empty()) {
        payload["sections"] = Json::array();
        for (const auto& section : mesh.sections)
            payload["sections"].push_back({{"firstIndex",section.firstIndex},{"indexCount",section.indexCount},{"materialSlot",section.materialSlot}});
    }
    if (mesh.uvChannelMask()) {
        payload["uvChannels"] = Json::array();
        for (std::size_t channel = 0; channel < MaxMeshUvChannels; ++channel) {
            const auto& uv = mesh.uvChannels[channel];
            if (uv.empty()) continue;
            const auto offset = file.blobs.size(), size = uv.size() * sizeof(uv.front());
            file.blobs.resize(offset + size);
            std::memcpy(file.blobs.data() + offset, uv.data(), size);
            payload["uvChannels"].push_back({{"channel",channel},{"name",mesh.uvNames[channel]},
                {"blob",blobRange(offset,size)}});
        }
    }
    return file;
}

bool readMeshAssetFile(const AssetFile& file, MeshAsset& mesh, std::string& error) {
    error.clear();
    if (file.type != AssetFileType::StaticMesh) { error = "The asset is not a static mesh."; return false; }
    try {
        const auto& payload = file.manifest.at("payload");
        if (payload.contains("meshAttributesVersion") && meshUnsigned(payload.at("meshAttributesVersion"), "meshAttributesVersion") != 1)
            throw std::runtime_error("Unsupported mesh attributes version.");
        const auto vertexCount = meshUnsigned(payload.at("vertexCount"), "vertexCount");
        const auto indexCount = meshUnsigned(payload.at("indexCount"), "indexCount");
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
        if (loaded.vertices.empty() || loaded.indices.empty() || loaded.indices.size() % 3 != 0) {
            error = "Mesh asset must have indexed triangle geometry.";
            return false;
        }
        for (const auto index : loaded.indices)
            if (index >= loaded.vertices.size()) throw std::runtime_error("Mesh index is outside the vertex buffer.");
        for (const auto& vertex : loaded.vertices) {
            double normalLength = 0;
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis]) || !std::isfinite(vertex.normal[axis]))
                    throw std::runtime_error("Mesh geometry must be finite.");
                normalLength += double(vertex.normal[axis]) * vertex.normal[axis];
            }
            if (normalLength < 1e-12) throw std::runtime_error("Mesh normals cannot be zero.");
        }
        if (payload.contains("materialSlots")) {
            const auto& slots = payload.at("materialSlots");
            if (!slots.is_array()) throw std::runtime_error("materialSlots must be an array.");
            for (const auto& slot : slots) {
                MeshMaterialSlot value;
                value.name = slot.at("name").get<std::string>();
                value.sourceKey = slot.value("sourceKey", std::string());
                if (slot.contains("defaultMaterialId")) {
                    const auto& id = slot.at("defaultMaterialId");
                    if (meshUnsigned(id, "defaultMaterialId")) value.defaultMaterialId = detail::readId(id);
                }
                loaded.materialSlots.push_back(std::move(value));
            }
        }
        if (payload.contains("sections")) {
            const auto& sections = payload.at("sections");
            if (!sections.is_array()) throw std::runtime_error("sections must be an array.");
            for (const auto& section : sections) {
                const auto first = meshUnsigned(section.at("firstIndex"), "firstIndex");
                const auto count = meshUnsigned(section.at("indexCount"), "indexCount");
                const auto slot = meshUnsigned(section.at("materialSlot"), "materialSlot");
                if (first > UINT32_MAX || count > UINT32_MAX || slot > UINT32_MAX)
                    throw std::runtime_error("Mesh section fields exceed the supported range.");
                loaded.sections.push_back({static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(count),static_cast<std::uint32_t>(slot)});
            }
        }
        if (payload.contains("uvChannels")) {
            const auto& channels = payload.at("uvChannels");
            if (!channels.is_array() || channels.size() > MaxMeshUvChannels)
                throw std::runtime_error("uvChannels must be an array of at most four streams.");
            unsigned mask = 0;
            for (const auto& channel : channels) {
                const auto index = meshUnsigned(channel.at("channel"), "UV channel");
                if (index >= MaxMeshUvChannels || (mask & (1u << index)))
                    throw std::runtime_error("UV channel index is duplicate or outside [0,3].");
                mask |= 1u << index;
                std::vector<std::uint8_t> uvBytes;
                if (!readBlob(file, channel.at("blob"), sizeof(std::array<float,2>), vertexCount, uvBytes, error)) return false;
                loaded.uvChannels[index].resize(vertexCount);
                if (!uvBytes.empty()) std::memcpy(loaded.uvChannels[index].data(), uvBytes.data(), uvBytes.size());
                loaded.uvNames[index] = channel.value("name", std::string());
            }
        }
        validateMeshAttributes(loaded);
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
bool isAssetContainerFile(const std::filesystem::path& path) noexcept {
    const auto extension = path.extension().string();
    return extension == kAssetExtension || extension == kLegacyAssetExtension;
}
} // namespace rae::assets
