#include "RestArtEngine/Assets/MeshQuery.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

namespace rae::assets {
namespace {
using Vec3 = std::array<float, 3>;
struct Bounds {
    Vec3 minimum{INFINITY, INFINITY, INFINITY};
    Vec3 maximum{-INFINITY, -INFINITY, -INFINITY};
    void include(const Bounds& other) {
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], other.minimum[axis]);
            maximum[axis] = std::max(maximum[axis], other.maximum[axis]);
        }
    }
};
struct MortonKey { std::uint32_t code, offset; };
struct Node { Bounds bounds; std::uint32_t first = 0, count = 0, left = 0, right = 0; };
struct Bvh {
    MeshGeometryKey key;
    std::vector<Node> nodes;
    std::vector<std::uint32_t> triangles;
    std::uint64_t used = 0;
    std::size_t bytes() const { return nodes.capacity() * sizeof(Node) + triangles.capacity() * sizeof(std::uint32_t); }
};

bool boxHit(const Bounds& box, const MeshRay& ray, float farLimit, float& entry) {
    double nearValue = 0, farValue = farLimit;
    for (int axis = 0; axis < 3; ++axis) {
        const double origin = ray.origin[axis], direction = ray.direction[axis];
        if (direction == 0) {
            if (origin < box.minimum[axis] || origin > box.maximum[axis]) return false;
        } else {
            double a = (box.minimum[axis] - origin) / direction;
            double b = (box.maximum[axis] - origin) / direction;
            if (a > b) std::swap(a, b);
            nearValue = std::max(nearValue, a);
            farValue = std::min(farValue, b);
            if (nearValue > farValue) return false;
        }
    }
    entry = static_cast<float>(nearValue);
    return true;
}
bool triangleHit(const MeshAsset& mesh, std::uint32_t offset, const MeshRay& ray,
                 bool twoSided, bool cullFront, float epsilon, float& distance) {
    const auto& a = mesh.vertices[mesh.indices[offset]].position;
    const auto& b = mesh.vertices[mesh.indices[offset + 1]].position;
    const auto& c = mesh.vertices[mesh.indices[offset + 2]].position;
    double edge1[3], edge2[3], delta[3];
    for (int axis = 0; axis < 3; ++axis) {
        edge1[axis] = double(b[axis]) - a[axis];
        edge2[axis] = double(c[axis]) - a[axis];
        delta[axis] = double(ray.origin[axis]) - a[axis];
    }
    const double p[3] = {ray.direction[1] * edge2[2] - ray.direction[2] * edge2[1],
        ray.direction[2] * edge2[0] - ray.direction[0] * edge2[2],
        ray.direction[0] * edge2[1] - ray.direction[1] * edge2[0]};
    const double determinant = edge1[0] * p[0] + edge1[1] * p[1] + edge1[2] * p[2];
    if (std::abs(determinant) < epsilon || determinant == 0 ||
        (!twoSided && (cullFront ? determinant >= 0 : determinant <= 0))) return false;
    const double inverse = 1.0 / determinant;
    const double u = (delta[0] * p[0] + delta[1] * p[1] + delta[2] * p[2]) * inverse;
    if (u < 0 || u > 1) return false;
    const double q[3] = {delta[1] * edge1[2] - delta[2] * edge1[1],
        delta[2] * edge1[0] - delta[0] * edge1[2], delta[0] * edge1[1] - delta[1] * edge1[0]};
    const double v = (ray.direction[0] * q[0] + ray.direction[1] * q[1] + ray.direction[2] * q[2]) * inverse;
    if (v < 0 || u + v > 1) return false;
    const double t = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) * inverse;
    if (!std::isfinite(t) || t < 0 || t > std::numeric_limits<float>::max()) return false;
    distance = static_cast<float>(t);
    return true;
}

std::uint32_t spreadMortonBits(std::uint32_t value) {
    value = (value | (value << 16)) & 0x030000ffu;
    value = (value | (value << 8)) & 0x0300f00fu;
    value = (value | (value << 4)) & 0x030c30c3u;
    return (value | (value << 2)) & 0x09249249u;
}

void radixSort(std::vector<MortonKey>& keys) {
    std::vector<MortonKey> scratch(keys.size());
    // Four linear stable passes, rather than comparison sorting large bounds
    // records at each tree level. Raw contiguous access also avoids Debug
    // iterator validation dominating million-triangle import/picking workloads.
    for (unsigned shift = 0; shift < 32; shift += 8) {
        std::array<std::uint32_t, 256> counts{};
        auto* source = keys.data();
        auto* destination = scratch.data();
        for (std::size_t i = 0; i < keys.size(); ++i) ++counts[(source[i].code >> shift) & 255u];
        std::uint32_t prefix = 0;
        for (auto& count : counts) { const auto next = prefix + count; count = prefix; prefix = next; }
        for (std::size_t i = 0; i < keys.size(); ++i)
            destination[counts[(source[i].code >> shift) & 255u]++] = source[i];
        keys.swap(scratch);
    }
}

Bvh build(const MeshAsset& mesh) {
    Bvh result;
    result.key = mesh.geometryKey();
    const auto& local = mesh.localBounds();
    if (!local.valid) return result;
    double scale[3];
    for (int axis = 0; axis < 3; ++axis) {
        const double extent = double(local.maximum[axis]) - local.minimum[axis];
        scale[axis] = extent > 0 ? 1023.0 / extent : 0;
    }
    std::vector<MortonKey> keys;
    keys.reserve(mesh.indices.size() / 3);
    const auto* vertices = mesh.vertices.data();
    const auto* indices = mesh.indices.data();
    for (std::size_t offset = 0; offset + 2 < mesh.indices.size() && offset <= UINT32_MAX - 2; offset += 3) {
        const auto a = indices[offset], b = indices[offset + 1], c = indices[offset + 2];
        if (a >= mesh.vertices.size() || b >= mesh.vertices.size() || c >= mesh.vertices.size()) continue;
        std::uint32_t code = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const double center = (double(vertices[a].position[axis]) + vertices[b].position[axis] + vertices[c].position[axis]) / 3;
            const auto quantized = static_cast<std::uint32_t>(std::clamp((center - local.minimum[axis]) * scale[axis], 0.0, 1023.0));
            code |= spreadMortonBits(quantized) << axis;
        }
        keys.push_back({code, static_cast<std::uint32_t>(offset)});
    }
    if (keys.empty()) return result;
    radixSort(keys);
    // A balanced median tree has at most this many leaves. For large meshes,
    // reserving by the leaf bound avoids the former half-a-node-per-triangle
    // allocation (roughly twice the actual persistent nodes for the R8).
    std::size_t leafBound = 1;
    while (leafBound < (keys.size() + 7) / 8) leafBound *= 2;
    result.nodes.reserve(leafBound * 2 - 1);
    result.triangles.resize(keys.size());
    const auto* sortedKeys = keys.data();
    auto* orderedTriangles = result.triangles.data();
    const auto split = [&](auto&& self, std::uint32_t first, std::uint32_t count) -> std::uint32_t {
        const auto index = static_cast<std::uint32_t>(result.nodes.size());
        result.nodes.emplace_back();
        if (count <= 8) {
            Bounds combined;
            auto* minimum = combined.minimum.data();
            auto* maximum = combined.maximum.data();
            for (std::uint32_t i = first; i < first + count; ++i) {
                const auto offset = sortedKeys[i].offset;
                orderedTriangles[i] = offset;
                for (unsigned corner = 0; corner < 3; ++corner) {
                    const auto* position = vertices[indices[offset + corner]].position;
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        if (position[axis] < minimum[axis]) minimum[axis] = position[axis];
                        if (position[axis] > maximum[axis]) maximum[axis] = position[axis];
                    }
                }
            }
            result.nodes[index].bounds = combined;
            result.nodes[index].first = first;
            result.nodes[index].count = count;
            return index;
        }
        const auto middle = first + count / 2;
        const auto left = self(self, first, count / 2);
        const auto right = self(self, middle, count - count / 2);
        result.nodes[index].left = left;
        result.nodes[index].right = right;
        result.nodes[index].bounds = result.nodes[left].bounds;
        result.nodes[index].bounds.include(result.nodes[right].bounds);
        return index;
    };
    split(split, 0, static_cast<std::uint32_t>(keys.size()));
    return result;
}
} // namespace

struct MeshQueryCache::Impl {
    explicit Impl(std::size_t budget) : budget(budget) {}
    std::unordered_map<AssetId, Bvh> entries;
    std::size_t budget, triangleTests = 0;
    std::uint64_t epoch = 0, builds = 0;
    std::size_t bytes() const {
        std::size_t total = 0;
        for (const auto& [id, entry] : entries) { (void)id; total += entry.bytes(); }
        return total;
    }
};

MeshQueryCache::MeshQueryCache(std::size_t budget) : impl_(std::make_unique<Impl>(budget)) {}
MeshQueryCache::~MeshQueryCache() = default;
void MeshQueryCache::clear() { impl_->entries.clear(); }
std::uint64_t MeshQueryCache::buildCount() const { return impl_->builds; }
std::size_t MeshQueryCache::lastTriangleTests() const { return impl_->triangleTests; }
std::size_t MeshQueryCache::residentBytes() const { return impl_->bytes(); }
void MeshQueryCache::retain(const AssetLibrary& library) {
    std::erase_if(impl_->entries, [&](const auto& pair) {
        const auto mesh = std::find_if(library.meshes.begin(), library.meshes.end(),
            [&](const auto& candidate) { return candidate.id == pair.first; });
        return mesh == library.meshes.end() || mesh->geometryKey() != pair.second.key;
    });
}
bool MeshQueryCache::intersect(const MeshAsset& mesh, const MeshRay& ray, float maximumDistance,
                              bool twoSided, float epsilon, MeshRayHit& hit, bool cullFront,
                              std::span<const MeshFaceState> slotStates) {
    impl_->triangleTests = 0;
    if (!std::isfinite(maximumDistance) || maximumDistance < 0 || !std::isfinite(epsilon) || epsilon < 0) return false;
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(ray.origin[axis]) || !std::isfinite(ray.direction[axis])) return false;
    const auto& localBounds = mesh.localBounds();
    float entryDistance = 0;
    if (!localBounds.valid || !boxHit({localBounds.minimum, localBounds.maximum}, ray, maximumDistance, entryDistance)) return false;
    auto found = impl_->entries.find(mesh.id);
    if (found == impl_->entries.end() || found->second.key != mesh.geometryKey()) {
        auto candidate = build(mesh);
        found = impl_->entries.insert_or_assign(mesh.id, std::move(candidate)).first;
        ++impl_->builds;
    }
    auto& bvh = found->second;
    bvh.used = ++impl_->epoch;
    // Keep one queried mesh even if it alone exceeds the budget; other entries
    // are evicted by recency. No borrowed geometry survives this call.
    while (impl_->entries.size() > 1 && impl_->bytes() > impl_->budget) {
        auto oldest = impl_->entries.end();
        for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it)
            if (it->first != mesh.id && (oldest == impl_->entries.end() || it->second.used < oldest->second.used)) oldest = it;
        impl_->entries.erase(oldest);
    }
    if (bvh.nodes.empty()) return false;
    std::array<std::uint32_t, 64> stack{};
    std::size_t stackSize = 1;
    bool any = false;
    float closest = maximumDistance;
    std::uint32_t triangleOffset = UINT32_MAX;
    while (stackSize) {
        const auto& node = bvh.nodes[stack[--stackSize]];
        if (!boxHit(node.bounds, ray, closest, entryDistance)) continue;
        if (node.count) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i) {
                ++impl_->triangleTests;
                float distance = 0;
                const auto offset = bvh.triangles[i];
                bool triangleTwoSided = twoSided, triangleCullFront = cullFront;
                if (!slotStates.empty()) {
                    const auto slot = mesh.materialSlotForIndex(offset);
                    if (slot >= slotStates.size() || !slotStates[slot].visible) continue;
                    triangleTwoSided = slotStates[slot].cull == MaterialCullMode::Off;
                    triangleCullFront = slotStates[slot].cull == MaterialCullMode::Front;
                }
                if (triangleHit(mesh, offset, ray, triangleTwoSided, triangleCullFront, epsilon, distance) &&
                    (distance < closest || (any && distance == closest && offset < triangleOffset))) {
                    any = true; closest = distance; triangleOffset = offset;
                }
            }
        } else {
            float leftDistance = 0, rightDistance = 0;
            const bool left = boxHit(bvh.nodes[node.left].bounds, ray, closest, leftDistance);
            const bool right = boxHit(bvh.nodes[node.right].bounds, ray, closest, rightDistance);
            if (left && right) {
                // Median splits limit depth to < 32 for 32-bit triangle offsets.
                stack[stackSize++] = leftDistance < rightDistance ? node.right : node.left;
                stack[stackSize++] = leftDistance < rightDistance ? node.left : node.right;
            } else if (left || right) stack[stackSize++] = left ? node.left : node.right;
        }
    }
    if (any) hit = {closest, triangleOffset};
    return any;
}
} // namespace rae::assets
