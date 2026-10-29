#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace rae::camera {

enum class Projection { Perspective, Orthographic };
struct DepthOfField {
    bool enabled = false;
    float focusDistance = 5.0f; // Scene metres, measured along camera forward.
    float fStop = 2.8f;
    float sensorHeightMm = 24.0f;
    bool operator==(const DepthOfField&) const = default;
};
struct Lens {
    Projection projection = Projection::Perspective;
    float verticalFovDegrees = 50.0f;
    float orthographicHeight = 6.0f;
    float nearClip = .05f, farClip = 100.0f;
    // Zero follows the view's image rectangle. A positive value requests framing.
    float aspectRatio = 0.0f;
    // Authored request. Orthographic projection or focus at/before the derived
    // focal length disables the effect, without invalidating the camera itself.
    DepthOfField depthOfField;
};
struct Pose {
    float position[3] = {0, 0, 0};
    // Degrees, matching scene Transform: pitch X, yaw Y, roll Z; local -Z looks forward.
    float rotation[3] = {0, 0, 0};
};
struct Camera { Pose pose; Lens lens; };
using Matrix = std::array<float, 16>;
struct View {
    // Row-major, row-vector matrices. Canonical RH clip depth is [0,1].
    Matrix view{}, projection{}, viewProjection{}, inverseViewProjection{};
    float position[3]{}, forward[3]{}, right[3]{}, up[3]{};
    Lens lens;
    float aspectRatio = 1;
};
struct Ray { float origin[3]{}, direction[3]{}; float maxDistance = 0; };

inline bool valid(const DepthOfField& settings) {
    return std::isfinite(settings.focusDistance) && settings.focusDistance >= .01f && settings.focusDistance <= 1e6f
        && std::isfinite(settings.fStop) && settings.fStop >= .7f && settings.fStop <= 128.0f
        && std::isfinite(settings.sensorHeightMm) && settings.sensorHeightMm >= 1.0f && settings.sensorHeightMm <= 100.0f;
}
inline bool valid(const Lens& lens) {
    return (lens.projection == Projection::Perspective || lens.projection == Projection::Orthographic)
        && std::isfinite(lens.verticalFovDegrees) && lens.verticalFovDegrees >= 1 && lens.verticalFovDegrees <= 179
        && std::isfinite(lens.orthographicHeight) && lens.orthographicHeight > .0001f && lens.orthographicHeight <= 1e7f
        && std::isfinite(lens.nearClip) && std::isfinite(lens.farClip)
        && lens.nearClip >= 1e-5f && lens.farClip > lens.nearClip && lens.farClip <= 1e7f
        // Keep the finite float depth projection invertible and useful for picking.
        && static_cast<double>(lens.farClip)/lens.nearClip <= 1e6
        && std::isfinite(lens.aspectRatio) && (lens.aspectRatio == 0 ||
            (lens.aspectRatio >= .001f && lens.aspectRatio <= 1000))
        && valid(lens.depthOfField);
}
inline bool valid(const Camera& camera) {
    if (!valid(camera.lens)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(camera.pose.position[i]) || !std::isfinite(camera.pose.rotation[i])
            || std::abs(camera.pose.position[i]) > 1e7f || std::abs(camera.pose.rotation[i]) > 1e7f) return false;
    return true;
}
namespace detail {
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        for (int k = 0; k < 4; ++k) result[row*4+col] += a[row*4+k]*b[k*4+col];
    return result;
}
inline Matrix inverse(const Matrix& matrix) {
    double rows[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) rows[r][c] = matrix[r*4+c];
        rows[r][r+4] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c+1; r < 4; ++r) if (std::abs(rows[r][c]) > std::abs(rows[pivot][c])) pivot = r;
        if (std::abs(rows[pivot][c]) < 1e-20) return {};
        for (int k = 0; k < 8; ++k) std::swap(rows[c][k], rows[pivot][k]);
        const double divisor = rows[c][c];
        for (int k = 0; k < 8; ++k) rows[c][k] /= divisor;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const double factor = rows[r][c];
            for (int k = 0; k < 8; ++k) rows[r][k] -= factor*rows[c][k];
        }
    }
    Matrix result{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) result[r*4+c] = static_cast<float>(rows[r][c+4]);
    return result;
}
}
inline Camera orbitCamera(float yaw, float pitch, float distance, const float* target, const Lens& lens = {}) {
    Camera result;
    result.lens = lens;
    const float cp = std::cos(pitch);
    result.pose.position[0] = target[0] + distance*cp*std::sin(yaw);
    result.pose.position[1] = target[1] + distance*std::sin(pitch);
    result.pose.position[2] = target[2] + distance*cp*std::cos(yaw);
    constexpr float degrees = 57.29577951308232f;
    result.pose.rotation[0] = -pitch*degrees;
    result.pose.rotation[1] = yaw*degrees;
    return result;
}
inline View evaluate(const Camera& requested, float viewportAspect) {
    const Camera camera = valid(requested) ? requested : Camera{};
    View result;
    result.lens = camera.lens;
    result.aspectRatio = camera.lens.aspectRatio > 0 ? camera.lens.aspectRatio
        : std::isfinite(viewportAspect) && viewportAspect > 0 ? std::clamp(viewportAspect,.001f,1000.0f) : 1;
    constexpr float radians = .017453292519943295f;
    const float x = camera.pose.rotation[0]*radians, y = camera.pose.rotation[1]*radians, z = camera.pose.rotation[2]*radians;
    const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
    const Matrix rx{1,0,0,0, 0,cx,sx,0, 0,-sx,cx,0, 0,0,0,1};
    const Matrix ry{cy,0,-sy,0, 0,1,0,0, sy,0,cy,0, 0,0,0,1};
    const Matrix rz{cz,sz,0,0, -sz,cz,0,0, 0,0,1,0, 0,0,0,1};
    const Matrix rotation = detail::multiply(detail::multiply(rz, rx), ry);
    for (int i = 0; i < 3; ++i) {
        result.position[i] = camera.pose.position[i];
        result.right[i] = rotation[i]; result.up[i] = rotation[4+i]; result.forward[i] = -rotation[8+i];
        result.view[i*4] = result.right[i]; result.view[i*4+1] = result.up[i]; result.view[i*4+2] = -result.forward[i];
        result.view[12] -= result.position[i]*result.right[i];
        result.view[13] -= result.position[i]*result.up[i];
        result.view[14] += result.position[i]*result.forward[i];
    }
    result.view[15] = 1;
    const auto& lens = camera.lens;
    if (lens.projection == Projection::Perspective) {
        const float height = 1/std::tan(lens.verticalFovDegrees*radians*.5f);
        result.projection = {height/result.aspectRatio,0,0,0, 0,height,0,0,
            0,0,lens.farClip/(lens.nearClip-lens.farClip),-1,
            0,0,lens.nearClip*lens.farClip/(lens.nearClip-lens.farClip),0};
    } else {
        result.projection = {2/(lens.orthographicHeight*result.aspectRatio),0,0,0,
            0,2/lens.orthographicHeight,0,0, 0,0,1/(lens.nearClip-lens.farClip),0,
            0,0,lens.nearClip/(lens.nearClip-lens.farClip),1};
    }
    result.viewProjection = detail::multiply(result.view, result.projection);
    result.inverseViewProjection = detail::inverse(result.viewProjection);
    return result;
}
inline Ray screenRay(const View& view, float ndcX, float ndcY) {
    Ray result;
    if (!valid(view.lens) || !std::isfinite(ndcX) || !std::isfinite(ndcY)) return result;
    const bool orthographic = view.lens.projection == Projection::Orthographic;
    const float halfHeight = orthographic ? view.lens.orthographicHeight*.5f
        : std::tan(view.lens.verticalFovDegrees*.008726646259971648f)*view.lens.nearClip;
    float lengthSquared = 0;
    for (int i = 0; i < 3; ++i) {
        const float offset = view.right[i]*(ndcX*halfHeight*view.aspectRatio)+view.up[i]*(ndcY*halfHeight);
        result.origin[i] = view.position[i]+view.forward[i]*view.lens.nearClip+offset;
        result.direction[i] = orthographic ? view.forward[i] : view.forward[i]*view.lens.nearClip+offset;
        lengthSquared += result.direction[i]*result.direction[i];
    }
    if (!std::isfinite(lengthSquared) || lengthSquared <= 0) return {};
    const float length = std::sqrt(lengthSquared);
    float forwardCosine = 0;
    for (int i = 0; i < 3; ++i) { result.direction[i] /= length; forwardCosine += result.direction[i]*view.forward[i]; }
    if (forwardCosine <= 0) return {};
    result.maxDistance = (view.lens.farClip-view.lens.nearClip)/forwardCosine;
    return result;
}
} // namespace rae::camera
