#pragma once
#include <DirectXCollision.h>
#include <algorithm>
#include <array>
#include <cmath>

namespace rae::renderer::worldVoxels {

// All coordinates are in voxel units. The caller has already rejected
// degenerate triangles; the caller supplies their normalized geometric normal.
template<class Visitor>
void visitTriangle(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b, DirectX::FXMVECTOR c,
    DirectX::GXMVECTOR normal, const std::array<int, 3>& dimensions, Visitor&& visit) {
    using namespace DirectX;
    XMFLOAT3 minimum, maximum;
    XMStoreFloat3(&minimum, XMVectorFloor(XMVectorMin(a, XMVectorMin(b, c))));
    XMStoreFloat3(&maximum, XMVectorFloor(XMVectorMax(a, XMVectorMax(b, c))));
    const std::array<int, 3> lo{std::max(0, int(minimum.x)), std::max(0, int(minimum.y)), std::max(0, int(minimum.z))};
    const std::array<int, 3> hi{std::min(dimensions[0] - 1, int(maximum.x)),
        std::min(dimensions[1] - 1, int(maximum.y)), std::min(dimensions[2] - 1, int(maximum.z))};
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;
    const auto intersects = [&](int x, int y, int z) {
        BoundingBox box(XMFLOAT3(float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f),
            XMFLOAT3(0.50001f, 0.50001f, 0.50001f));
        if (box.Intersects(a, b, c)) visit(x, y, z);
    };
    // Dense meshes typically have many triangles entirely within one cell.
    if (minimum.x == maximum.x && minimum.y == maximum.y && minimum.z == maximum.z) {
        visit(lo[0], lo[1], lo[2]);
        return;
    }
    if ((hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) <= 16) {
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    intersects(x, y, z);
        return;
    }

    // Project along the dominant normal axis. A unit square on the projected
    // plane spans at most two units of depth. Clip the candidate columns to
    // that slab before running exact triangle/box tests, even on long diagonals.
    XMFLOAT3 n;
    XMStoreFloat3(&n, normal);
    const std::array<float, 3> axisNormal{n.x, n.y, n.z};
    const auto dominant = std::max_element(axisNormal.begin(), axisNormal.end(),
        [](float x, float y) { return std::abs(x) < std::abs(y); });
    const int depth = int(dominant - axisNormal.begin()), u = (depth + 1) % 3, v = (depth + 2) % 3;
    const float uSlope = axisNormal[u] / *dominant, vSlope = axisNormal[v] / *dominant;
    const float centerIntercept = XMVectorGetX(XMVector3Dot(normal, a)) / *dominant - 0.5f * (uSlope + vSlope);
    const float radius = 0.5f * (std::abs(uSlope) + std::abs(vSlope)) + 0.0001f;
    std::array<int, 3> cell{};
    for (cell[u] = lo[u]; cell[u] <= hi[u]; ++cell[u]) {
        for (cell[v] = lo[v]; cell[v] <= hi[v]; ++cell[v]) {
            const float center = centerIntercept - uSlope * float(cell[u]) - vSlope * float(cell[v]);
            const int begin = std::max(lo[depth], int(std::floor(center - radius)));
            const int end = std::min(hi[depth], int(std::floor(center + radius)));
            for (cell[depth] = begin; cell[depth] <= end; ++cell[depth])
                intersects(cell[0], cell[1], cell[2]);
        }
    }
}
} // namespace rae::renderer::worldVoxels
