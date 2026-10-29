#include "RestArtEngine/Assets/MeshQuery.h"
#include "RestArtEngine/Assets/AssetFile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
using namespace rae::assets;
int checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
MeshAsset grid(unsigned side) {
    MeshAsset mesh;
    mesh.id = 42;
    for (unsigned y = 0; y < side; ++y)
        for (unsigned x = 0; x < side; ++x) {
            // A real opening rather than an AABB-shaped hit surface.
            if (x >= side / 3 && x < side * 2 / 3 && y >= side / 3 && y < side * 2 / 3) continue;
            const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
            for (const auto& point : {std::array<float, 2>{float(x), float(y)},
                    {float(x + 1), float(y)}, {float(x + 1), float(y + 1)}, {float(x), float(y + 1)}})
                mesh.vertices.push_back({{point[0], point[1], 0}, {0, 0, 1}});
            mesh.indices.insert(mesh.indices.end(), {first, first + 1, first + 2, first, first + 2, first + 3});
        }
    return mesh;
}
// Independent reference for the planar test fixture: every quad in the grid
// lies at Z=0. It avoids repeating the BVH's triangle intersection algorithm.
bool gridReference(float x, float y, unsigned side) {
    if (x < 0 || y < 0 || x >= side || y >= side) return false;
    return !(x >= side / 3 && x < side * 2 / 3 && y >= side / 3 && y < side * 2 / 3);
}
void geometryAndReuse() {
    constexpr unsigned side = 96;
    auto mesh = grid(side);
    MeshQueryCache cache;
    MeshRayHit hit;
    require(cache.intersect(mesh, {{.25f, .75f, 2}, {0, 0, -1}}, 100, false, 1e-8f, hit) && hit.distance == 2,
        "Front ray hits the grid at its geometric distance.");
    require(cache.buildCount() == 1 && cache.lastTriangleTests() < 32,
        "Dense mesh uses a localized BVH leaf instead of testing every triangle.");
    std::mt19937 random(17);
    std::uniform_real_distribution<float> position(-4.0f, float(side) + 4.0f);
    for (int ray = 0; ray < 500; ++ray) {
        const float x = position(random), y = position(random);
        const bool expected = gridReference(x, y, side);
        const bool found = cache.intersect(mesh, {{x, y, 3}, {0, 0, -1}}, 100, false, 1e-8f, hit);
        require(found == expected, "BVH agrees with independent grid/opening reference.");
        if (found) {
            require(hit.distance == 3, "BVH returns the nearest surface distance.");
            require(cache.lastTriangleTests() < 64, "Repeated dense-mesh queries remain sublinear.");
        }
    }
    require(cache.buildCount() == 1, "Unchanged queries reuse the acceleration structure.");
    require(!cache.intersect(mesh, {{1.25f, 1.75f, -2}, {0, 0, 1}}, 100, false, 1e-8f, hit),
        "One-sided mesh rejects its back face.");
    require(cache.intersect(mesh, {{1.25f, 1.75f, -2}, {0, 0, 1}}, 100, true, 1e-8f, hit) && hit.distance == 2,
        "Two-sided override accepts the same back-face ray without rebuilding.");
    require(!cache.intersect(mesh, {{1.25f, 1.75f, 2}, {0, 0, -1}}, 100, false, 1e-8f, hit, true),
        "Front culling rejects the authored front face.");
    require(cache.intersect(mesh, {{1.25f, 1.75f, -2}, {0, 0, 1}}, 100, false, 1e-8f, hit, true) && hit.distance == 2,
        "Front culling accepts the authored back face.");
    require(cache.intersect(mesh, {{1.25f, 1.75f, 2}, {0, 0, -1}}, 100, true, 1e-8f, hit, true),
        "Cull Off takes precedence over a stale front-cull flag.");
    require(cache.buildCount() == 1, "Material sidedness is a query option, not a geometry rebuild.");
    require(cache.intersect(mesh, {{1.25f, 1.75f, 5}, {0, 0, -.25f}}, 30, false, 1e-8f, hit) && hit.distance == 20,
        "Unnormalized inverse-transformed rays preserve the world distance parameter.");
    require(!cache.intersect(mesh, {{1.25f, 1.75f, 5}, {0, 0, -.25f}}, 19, false, 1e-8f, hit),
        "World maximum distance remains effective with scaled local rays.");
    // Actor scale (-2, .5, 4): world origin (-2.5, .875, 20)
    // inverse-transforms to this ray. Mirroring must not reverse authored culling.
    require(cache.intersect(mesh, {{1.25f, 1.75f, 5}, {0, 0, -.25f}}, 30, false, 1e-8f / 4, hit) && hit.distance == 20,
        "Mirrored non-uniform Actor rays still hit the authored front at world distance.");
    require(!cache.intersect(mesh, {{1.25f, 1.75f, -5}, {0, 0, .25f}}, 30, false, 1e-8f / 4, hit),
        "Mirrored non-uniform Actor rays still reject the authored back face.");
    require(!cache.intersect(mesh, {{1.25f, 1.75f, 5}, {1, 0, 0}}, 100, true, 1e-8f, hit),
        "Parallel ray above the surface misses.");
    for (auto& vertex : mesh.vertices) vertex.position[2] = 1;
    mesh.markGeometryModified();
    require(cache.intersect(mesh, {{.25f, .75f, 2}, {0, 0, -1}}, 100, false, 1e-8f, hit) && hit.distance == 1,
        "In-place geometry revision invalidates both bounds and BVH.");
    require(cache.buildCount() == 2, "Geometry edit rebuilds once.");
    auto replacement = grid(side);
    require(cache.intersect(replacement, {{.25f, .75f, 2}, {0, 0, -1}}, 100, false, 1e-8f, hit) && hit.distance == 2,
        "Replacing geometry under the same asset ID does not reuse stale triangles.");
    require(cache.buildCount() == 3, "Same-ID replacement rebuilds once.");
}
void nearestAndLifetime() {
    auto mesh = grid(3);
    const auto vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto originalVertices = mesh.vertices;
    const auto originalIndices = mesh.indices;
    for (auto vertex : originalVertices) { vertex.position[2] = -2; mesh.vertices.push_back(vertex); }
    for (auto index : originalIndices) mesh.indices.push_back(vertexCount + index);
    MeshQueryCache cache;
    MeshRayHit hit;
    require(cache.intersect(mesh, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit) && hit.distance == 3,
        "Overlapping depth layers choose the nearest actual triangle.");
    require(cache.intersect(mesh, {{.2f, .3f, -3}, {0, 0, 1}}, 100, true, 1e-8f, hit) && hit.distance == 1,
        "Reverse rays choose the near back layer rather than node order.");
    AssetLibrary library;
    library.meshes.push_back(std::move(mesh));
    cache.retain(library);
    require(cache.residentBytes() > 0, "Moving a mesh into its library preserves matching cached geometry.");
    library.meshes.clear();
    cache.retain(library);
    require(cache.residentBytes() == 0, "Removing assets releases their acceleration memory.");
    auto first = grid(3), second = grid(3); second.id = 43;
    MeshQueryCache small(1);
    require(small.intersect(first, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit), "Cache first mesh.");
    require(small.intersect(second, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit), "Cache second mesh.");
    require(small.buildCount() == 2, "Distinct IDs build distinct geometry.");
    require(small.intersect(first, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit) && small.buildCount() == 3,
        "Memory budget evicts the least recently used other mesh.");
    small.clear(); require(small.residentBytes() == 0, "Explicit clear releases cache memory.");
    auto invalid = grid(3);
    for (auto& index : invalid.indices) index = UINT32_MAX;
    require(!small.intersect(invalid, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit),
        "Malformed mesh indices cannot be dereferenced by the query.");
    auto nonfinite = grid(3);
    require(nonfinite.localBounds().valid, "Finite geometry has valid cached bounds.");
    nonfinite.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    nonfinite.markGeometryModified();
    require(!nonfinite.localBounds().valid &&
        !small.intersect(nonfinite, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit),
        "Nonfinite geometry invalidates old cached bounds and cannot be queried.");
    nonfinite.vertices[0].position[0] = 0;
    nonfinite.markGeometryModified();
    require(nonfinite.localBounds().valid &&
        small.intersect(nonfinite, {{.2f, .3f, 3}, {0, 0, -1}}, 100, true, 1e-8f, hit),
        "Repairing geometry recomputes valid bounds and acceleration.");
}

void spatialOrderingAndRepeatedKeys() {
    auto mesh = grid(48);
    std::vector<std::array<std::uint32_t, 3>> faces;
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
        faces.push_back({mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]});
    std::mt19937 random(71);
    std::shuffle(faces.begin(), faces.end(), random);
    mesh.indices.clear();
    for (const auto& face : faces) mesh.indices.insert(mesh.indices.end(), face.begin(), face.end());
    mesh.markGeometryModified();
    MeshQueryCache cache;
    MeshRayHit hit;
    for (unsigned y = 0; y < 48; y += 3)
        for (unsigned x = 0; x < 48; x += 3) {
            const float px = float(x) + .25f, py = float(y) + .75f;
            const bool found = cache.intersect(mesh, {{px, py, 2}, {0, 0, -1}}, 100, true, 0, hit);
            require(found == gridReference(px, py, 48), "Spatial sorting preserves shuffled triangle coverage and holes.");
            require(cache.lastTriangleTests() < mesh.indices.size() / 12,
                "Shuffled triangle order still yields localized spatial queries.");
        }
    MeshAsset repeated;
    repeated.id = 43;
    // Identical centroids intentionally produce identical Morton keys. The tree
    // must still terminate, preserve all faces and retain original-order ties.
    for (unsigned face = 0; face < 257; ++face) {
        const float radius = 1.0f + float(face) / 1024.0f;
        const auto first = static_cast<std::uint32_t>(repeated.vertices.size());
        repeated.vertices.push_back({{-radius, -radius, 0}, {0, 0, 1}});
        repeated.vertices.push_back({{radius, -radius, 0}, {0, 0, 1}});
        repeated.vertices.push_back({{0, 2 * radius, 0}, {0, 0, 1}});
        repeated.indices.insert(repeated.indices.end(), {first, first + 1, first + 2});
    }
    require(cache.intersect(repeated, {{0, 0, 2}, {0, 0, -1}}, 100, false, 0, hit) &&
        hit.distance == 2 && hit.triangleOffset == 0,
        "Repeated spatial keys retain all geometry and deterministic coincident-hit ordering.");
}

void benchmarkFile(const std::filesystem::path& path) {
    using Clock = std::chrono::steady_clock;
    const auto milliseconds = [](auto start, auto stop) {
        return std::chrono::duration<double, std::milli>(stop - start).count();
    };
    MeshAsset mesh;
    {
        AssetFile file;
        std::string error;
        if (!readAssetFile(path, file, error) || !readMeshAssetFile(file, mesh, error))
            throw std::runtime_error("Cannot load benchmark mesh: " + error);
    } // Release the container bytes before allocating BVH build scratch space.
    const auto triangleCount = mesh.indices.size() / 3;
    require(triangleCount != 0, "Benchmark mesh has triangles.");
    const auto boundsStart = Clock::now();
    const auto bounds = mesh.localBounds();
    const auto boundsStop = Clock::now();
    require(bounds.valid, "Benchmark mesh has finite bounds.");
    double diagonalSquared = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const double size = double(bounds.maximum[axis]) - bounds.minimum[axis];
        diagonalSquared += size * size;
    }
    const float offsetDistance = static_cast<float>(std::sqrt(diagonalSquared) + 1.0);
    std::vector<MeshRay> rays;
    // Use reproducible rays aimed at sampled actual triangle centroids, rather
    // than an arbitrary bounding-box center that may lie in a car's empty space.
    for (std::size_t sample = 0; sample < 128; ++sample) {
        const auto offset = ((sample * triangleCount) / 128) * 3;
        const auto& a = mesh.vertices[mesh.indices[offset]].position;
        const auto& b = mesh.vertices[mesh.indices[offset + 1]].position;
        const auto& c = mesh.vertices[mesh.indices[offset + 2]].position;
        double e1[3], e2[3];
        for (int axis = 0; axis < 3; ++axis) {
            e1[axis] = double(b[axis]) - a[axis];
            e2[axis] = double(c[axis]) - a[axis];
        }
        const double normal[3] = {e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const double length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (!std::isfinite(length) || length == 0) continue;
        MeshRay ray;
        for (int axis = 0; axis < 3; ++axis) {
            const double direction = normal[axis] / length;
            const double center = (double(a[axis]) + b[axis] + c[axis]) / 3;
            ray.origin[axis] = static_cast<float>(center + direction * offsetDistance);
            ray.direction[axis] = static_cast<float>(-direction);
        }
        rays.push_back(ray);
    }
    require(!rays.empty(), "Benchmark contains a usable sampled triangle.");
    MeshQueryCache cache;
    MeshRayHit hit;
    const auto firstStart = Clock::now();
    const bool firstHit = cache.intersect(mesh, rays.front(), offsetDistance * 2, true, 0, hit);
    const auto firstStop = Clock::now();
    const auto firstCandidates = cache.lastTriangleTests();
    require(cache.buildCount() == 1, "First benchmark query constructs one BVH.");
    std::size_t candidates = 0, hits = 0;
    const auto warmStart = Clock::now();
    for (const auto& ray : rays) {
        hits += cache.intersect(mesh, ray, offsetDistance * 2, true, 0, hit) ? 1u : 0u;
        candidates += cache.lastTriangleTests();
    }
    const auto warmStop = Clock::now();
    require(hits > 0 && cache.buildCount() == 1, "Warm real-mesh queries hit geometry and reuse one BVH.");
    std::cout << std::fixed << std::setprecision(3)
        << "Mesh query benchmark: " << mesh.name << '\n'
        << "triangles=" << triangleCount << " vertices=" << mesh.vertices.size() << '\n'
        << "bounds_ms=" << milliseconds(boundsStart, boundsStop)
        << " first_query_ms=" << milliseconds(firstStart, firstStop)
        << " first_hit=" << firstHit << " first_triangle_candidates=" << firstCandidates << '\n'
        << "warm_rays=" << rays.size() << " warm_hits=" << hits
        << " warm_total_ms=" << milliseconds(warmStart, warmStop)
        << " warm_mean_ms=" << milliseconds(warmStart, warmStop) / rays.size() << '\n'
        << "warm_triangle_candidates_total=" << candidates
        << " warm_triangle_candidates_mean=" << double(candidates) / rays.size()
        << " full_scan_candidates_per_ray=" << triangleCount << '\n'
        << "bvh_build_count=" << cache.buildCount() << " bvh_resident_bytes=" << cache.residentBytes() << '\n';
}
} // namespace
int main(int argc, char** argv) {
    try {
        geometryAndReuse(); nearestAndLifetime(); spatialOrderingAndRepeatedKeys();
        std::cout << "Mesh query tests passed: " << checks << " checks\n";
        if (argc == 2) benchmarkFile(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[1]))));
        else require(argc == 1, "Usage: MeshQueryTests [mesh.rasset]");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Mesh query tests failed: " << error.what() << '\n';
        return 1;
    }
}
