#include "WorldVoxelCoverage.h"
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
using namespace DirectX;

void compareCoverage(const std::array<int, 3>& dim, XMFLOAT3 first, XMFLOAT3 second, XMFLOAT3 third) {
    const auto a = XMLoadFloat3(&first), b = XMLoadFloat3(&second), c = XMLoadFloat3(&third);
    const auto normal = XMVector3Normalize(XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a)));
    std::vector<bool> projected(dim[0] * dim[1] * dim[2]);
    const auto index = [&](int x, int y, int z) { return (y * dim[2] + z) * dim[0] + x; };
    rae::renderer::worldVoxels::visitTriangle(a, b, c, normal, dim, [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= dim[0] || y >= dim[1] || z >= dim[2] || projected[index(x, y, z)])
            throw std::runtime_error("Duplicate or out-of-range voxel.");
        projected[index(x, y, z)] = true;
    });
    const auto lo = XMVectorFloor(XMVectorMin(a, XMVectorMin(b, c)));
    const auto hi = XMVectorFloor(XMVectorMax(a, XMVectorMax(b, c)));
    for (int y = 0; y < dim[1]; ++y)
        for (int z = 0; z < dim[2]; ++z)
            for (int x = 0; x < dim[0]; ++x) {
                const auto cell = XMVectorSet(float(x), float(y), float(z), 0);
                const bool withinBounds = XMVector3GreaterOrEqual(cell, lo) && XMVector3LessOrEqual(cell, hi);
                const BoundingBox box({float(x) + .5f, float(y) + .5f, float(z) + .5f}, {.50001f, .50001f, .50001f});
                if (projected[index(x, y, z)] != (withinBounds && box.Intersects(a, b, c)))
                    throw std::runtime_error("Projected voxel coverage differs from exhaustive triangle/box tests.");
            }
}
}

int main() {
    try {
        compareCoverage({64, 64, 64}, {1.1f, 1.1f, 1.1f}, {62.5f, 16.2f, 42.9f}, {25.2f, 63.0f, 63.1f});
        compareCoverage({19, 13, 22}, {9, 3, 0}, {9, 21, 24}, {9, 7, 22});
        compareCoverage({9, 7, 5}, {1.45f, 1.49f, 1.5f}, {1.46f, 1.53f, 1.54f}, {1.51f, 1.5f, 1.52f});
        compareCoverage({9, 7, 5}, {-1.5f, 1, 2}, {-1.4f, 1.1f, 2.1f}, {-1.4f, 1.2f, 2.1f});
        std::mt19937 generator(1739);
        std::uniform_real_distribution<float> coordinate(-1.0f, 19.0f);
        const auto point = [&] { return XMFLOAT3{coordinate(generator), coordinate(generator), coordinate(generator)}; };
        for (int i = 0; i < 64; ++i)
            compareCoverage({17, 11, 19}, point(), point(), point());
        std::cout << "World voxel coverage matches exhaustive overlap tests.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
