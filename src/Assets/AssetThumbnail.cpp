#include "RestArtEngine/Assets/AssetThumbnail.h"

#include <algorithm>
#include <fstream>
#include <array>
#include <cmath>
#include <limits>

namespace rae::assets {
namespace {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(const Vec3& value) {
    const float length = std::sqrt(dot(value, value));
    return length > 1e-6f ? value * (1.0f / length) : Vec3{0, 0, 1};
}

std::uint8_t toByte(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void putPixel(Thumbnail& thumbnail, int x, int y, float r, float g, float b) {
    if (x < 0 || y < 0 || x >= static_cast<int>(thumbnail.width) || y >= static_cast<int>(thumbnail.height)) return;
    const auto index = static_cast<std::size_t>(y) * thumbnail.width + static_cast<std::size_t>(x);
    auto* pixel = thumbnail.rgba.data() + index * 4;
    pixel[0] = toByte(r);
    pixel[1] = toByte(g);
    pixel[2] = toByte(b);
    pixel[3] = 255;
}

} // namespace

Thumbnail renderMeshThumbnail(const MeshAsset& mesh, std::uint32_t size) {
    Thumbnail thumbnail;
    thumbnail.width = size;
    thumbnail.height = size;
    thumbnail.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    if (size < 4 || mesh.vertices.empty() || mesh.indices.size() < 3) return thumbnail;

    Vec3 minimum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    Vec3 maximum{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max()};
    for (const auto& vertex : mesh.vertices) {
        const Vec3 position{vertex.position[0], vertex.position[1], vertex.position[2]};
        minimum.x = std::min(minimum.x, position.x);
        minimum.y = std::min(minimum.y, position.y);
        minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x);
        maximum.y = std::max(maximum.y, position.y);
        maximum.z = std::max(maximum.z, position.z);
    }
    const Vec3 extent = maximum - minimum;
    // Project along the smallest extent so flat meshes (a ground plane) do not
    // collapse to a line.
    const float extents[3] = {extent.x, extent.y, extent.z};
    int order[3] = {0, 1, 2};
    std::sort(order, order + 3, [&extents](int left, int right) { return extents[left] > extents[right]; });
    const int axisU = order[0], axisV = order[1], axisW = order[2];
    const float extentMax = std::max({extent.x, extent.y, extent.z, 1e-4f});
    const float scale = (static_cast<float>(size) - 3.0f) / extentMax;
    const float minValues[3] = {minimum.x, minimum.y, minimum.z};
    const float maxValues[3] = {maximum.x, maximum.y, maximum.z};
    const float centerU = (minValues[axisU] + maxValues[axisU]) * 0.5f;
    const float centerV = (minValues[axisV] + maxValues[axisV]) * 0.5f;
    std::vector<float> depth(static_cast<std::size_t>(size) * size, -std::numeric_limits<float>::max());
    const Vec3 light = normalize(Vec3{0.35f, 0.55f, 0.75f});

    const auto project = [&](const MeshVertex& vertex) {
        const float x = (vertex.position[axisU] - centerU) * scale + static_cast<float>(size) * 0.5f;
        const float y = static_cast<float>(size) * 0.5f - (vertex.position[axisV] - centerV) * scale;
        return std::array<float, 3>{x, y, vertex.position[axisW] * scale};
    };

    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const auto i0 = mesh.indices[index], i1 = mesh.indices[index + 1], i2 = mesh.indices[index + 2];
        if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) continue;
        const auto& v0 = mesh.vertices[i0];
        const auto& v1 = mesh.vertices[i1];
        const auto& v2 = mesh.vertices[i2];
        const auto p0 = project(v0), p1 = project(v1), p2 = project(v2);
        const float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
        if (std::abs(area) < 1e-5f) continue;
        const Vec3 faceNormal = normalize(cross(
            Vec3{v1.position[0] - v0.position[0], v1.position[1] - v0.position[1], v1.position[2] - v0.position[2]},
            Vec3{v2.position[0] - v0.position[0], v2.position[1] - v0.position[1], v2.position[2] - v0.position[2]}));
        const float shading = 0.25f + 0.75f * std::clamp(std::abs(dot(faceNormal, light)), 0.0f, 1.0f);

        const int minX = std::max(0, static_cast<int>(std::floor(std::min({p0[0], p1[0], p2[0]}))));
        const int maxX = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({p0[0], p1[0], p2[0]}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({p0[1], p1[1], p2[1]}))));
        const int maxY = std::min(static_cast<int>(size) - 1, static_cast<int>(std::ceil(std::max({p0[1], p1[1], p2[1]}))));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float px = static_cast<float>(x) + 0.5f;
                const float py = static_cast<float>(y) + 0.5f;
                const float w0 = ((p1[0] - px) * (p2[1] - py) - (p2[0] - px) * (p1[1] - py)) / area;
                const float w1 = ((p2[0] - px) * (p0[1] - py) - (p0[0] - px) * (p2[1] - py)) / area;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
                const float z = w0 * p0[2] + w1 * p1[2] + w2 * p2[2];
                const auto pixel = static_cast<std::size_t>(y) * size + static_cast<std::size_t>(x);
                if (z <= depth[pixel]) continue;
                depth[pixel] = z;
                putPixel(thumbnail, x, y, 0.62f * shading, 0.68f * shading, 0.78f * shading);
            }
        }
    }
    return thumbnail;
}

Thumbnail renderMaterialThumbnail(const MaterialAsset& material, std::uint32_t size) {
    Thumbnail thumbnail;
    thumbnail.width = size;
    thumbnail.height = size;
    thumbnail.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    if (size < 4) return thumbnail;
    const Vec3 light = normalize(Vec3{-0.4f, 0.5f, 0.75f});
    const float radius = static_cast<float>(size) * 0.5f - 1.5f;
    const float center = static_cast<float>(size) * 0.5f;
    const float shininess = 4.0f + (1.0f - std::clamp(material.roughness, 0.0f, 1.0f)) * 60.0f;
    for (int y = 0; y < static_cast<int>(size); ++y) {
        for (int x = 0; x < static_cast<int>(size); ++x) {
            const float nx = (static_cast<float>(x) + 0.5f - center) / radius;
            const float ny = (center - static_cast<float>(y) - 0.5f) / radius;
            const float distance = nx * nx + ny * ny;
            if (distance > 1.0f) continue;
            const float nz = std::sqrt(1.0f - distance);
            const Vec3 normal = normalize(Vec3{nx, ny, nz});
            const float lambert = std::clamp(dot(normal, light), 0.0f, 1.0f);
            const Vec3 view{0, 0, 1};
            const Vec3 halfVector = normalize(light + view);
            const float specular = std::pow(std::clamp(dot(normal, halfVector), 0.0f, 1.0f), shininess) *
                (0.15f + 0.6f * std::clamp(material.metallic, 0.0f, 1.0f));
            const float ambient = 0.12f + 0.18f * (1.0f - std::clamp(material.metallic, 0.0f, 1.0f));
            const float rim = std::pow(1.0f - nz, 2.0f) * 0.10f;
            putPixel(thumbnail, x, y,
                material.baseColor[0] * (ambient + lambert * 0.85f) + specular + rim,
                material.baseColor[1] * (ambient + lambert * 0.85f) + specular + rim,
                material.baseColor[2] * (ambient + lambert * 0.85f) + specular + rim);
        }
    }
    return thumbnail;
}


Thumbnail renderSceneThumbnail(const std::vector<ScenePreviewBox>& boxes, std::uint32_t size) {
    Thumbnail thumbnail;
    thumbnail.width = size;
    thumbnail.height = size;
    thumbnail.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
    if (size < 4) return thumbnail;

    // Fixed three-quarter view: yaw 35 degrees around Y, then a top-down layout.
    const float yaw = 0.610865f; // 35 degrees
    const float cosine = std::cos(yaw), sine = std::sin(yaw);
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    std::vector<std::array<float, 4>> projected; // u, v, radius, height
    projected.reserve(boxes.size());
    for (const auto& box : boxes) {
        const float x = box.position[0], z = box.position[2];
        const float u = x * cosine - z * sine;
        const float v = x * sine + z * cosine;
        const float radius = 0.5f * std::max({box.extent[0], box.extent[1], box.extent[2]});
        projected.push_back({u, v, radius, box.position[1]});
        minU = std::min(minU, u - radius);
        maxU = std::max(maxU, u + radius);
        minV = std::min(minV, v - radius);
        maxV = std::max(maxV, v + radius);
    }
    if (projected.empty()) return thumbnail;
    const float span = std::max({maxU - minU, maxV - minV, 1e-3f});
    const float scale = (static_cast<float>(size) - 6.0f) / span;
    const float centerU = (minU + maxU) * 0.5f;
    const float centerV = (minV + maxV) * 0.5f;

    // Painter's order: farther boxes first so nearer ones overlap them.
    std::sort(projected.begin(), projected.end(),
        [](const auto& left, const auto& right) { return left[1] < right[1]; });
    std::size_t boxIndex = 0;
    for (const auto& item : projected) {
        const auto& source = boxes[std::min(boxIndex++, boxes.size() - 1)];
        const float centerX = (item[0] - centerU) * scale + static_cast<float>(size) * 0.5f;
        const float centerY = (item[1] - centerV) * scale + static_cast<float>(size) * 0.5f;
        const float half = std::clamp(item[2] * scale, 6.0f, static_cast<float>(size) * 0.22f);
        const float lift = std::clamp(item[3] * 0.25f, -0.2f, 0.3f);
        for (int y = static_cast<int>(centerY - half); y <= static_cast<int>(centerY + half); ++y) {
            for (int x = static_cast<int>(centerX - half); x <= static_cast<int>(centerX + half); ++x) {
                putPixel(thumbnail, x, y,
                    std::clamp(source.color[0] * (1.0f + lift), 0.0f, 1.0f),
                    std::clamp(source.color[1] * (1.0f + lift), 0.0f, 1.0f),
                    std::clamp(source.color[2] * (1.0f + lift), 0.0f, 1.0f));
            }
        }
    }
    return thumbnail;
}

bool writeSceneThumbnail(const std::filesystem::path& path, const Thumbnail& thumbnail, std::string& error) {
    error.clear();
    if (!thumbnail.valid()) { error = "Cannot write an empty scene thumbnail."; return false; }
    std::error_code code;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) { error = "Cannot write the scene thumbnail: " + path.generic_string(); return false; }
    const std::uint32_t header[4] = {0x42485452u, 1u, thumbnail.width, thumbnail.height};
    stream.write(reinterpret_cast<const char*>(header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(thumbnail.rgba.data()),
        static_cast<std::streamsize>(thumbnail.rgba.size()));
    if (!stream) { error = "Writing the scene thumbnail failed."; return false; }
    return true;
}

bool readSceneThumbnail(const std::filesystem::path& path, Thumbnail& thumbnail, std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { error = "Cannot open the scene thumbnail."; return false; }
    std::uint32_t header[4]{};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    if (stream.gcount() != static_cast<std::streamsize>(sizeof(header)) || header[0] != 0x42485452u || header[1] != 1u)
        { error = "Invalid scene thumbnail header."; return false; }
    Thumbnail loaded;
    loaded.width = header[2];
    loaded.height = header[3];
    if (loaded.width == 0 || loaded.height == 0 || loaded.width > 4096 || loaded.height > 4096)
        { error = "Invalid scene thumbnail dimensions."; return false; }
    loaded.rgba.resize(static_cast<std::size_t>(loaded.width) * loaded.height * 4);
    stream.read(reinterpret_cast<char*>(loaded.rgba.data()), static_cast<std::streamsize>(loaded.rgba.size()));
    if (stream.gcount() != static_cast<std::streamsize>(loaded.rgba.size()))
        { error = "Scene thumbnail is truncated."; return false; }
    thumbnail = std::move(loaded);
    return true;
}
} // namespace rae::assets
