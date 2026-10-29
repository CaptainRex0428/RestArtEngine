#pragma once

#include "RestArtEngine/Assets/AssetLibrary.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rae::assets {

// Direction need not be normalized. An inverse-transformed world ray therefore
// keeps its original distance parameter, including under non-uniform scales.
struct MeshRay { std::array<float, 3> origin{}, direction{}; };
struct MeshRayHit { float distance = 0; std::uint32_t triangleOffset = 0; };

// CPU-only acceleration; stores no borrowed vertex buffers. Geometry edits must
// follow MeshAsset's markGeometryModified() contract. Each editor owns a cache.
class MeshQueryCache {
public:
    explicit MeshQueryCache(std::size_t byteBudget = 256u * 1024u * 1024u);
    ~MeshQueryCache();
    MeshQueryCache(const MeshQueryCache&) = delete;
    MeshQueryCache& operator=(const MeshQueryCache&) = delete;
    bool intersect(const MeshAsset&, const MeshRay&, float maximumDistance,
                   bool twoSided, float determinantEpsilon, MeshRayHit&, bool cullFront = false);
    // Discard caches for removed/replaced geometry, without scanning vertices.
    void retain(const AssetLibrary&);
    void clear();
    [[nodiscard]] std::uint64_t buildCount() const;
    [[nodiscard]] std::size_t lastTriangleTests() const;
    [[nodiscard]] std::size_t residentBytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace rae::assets
