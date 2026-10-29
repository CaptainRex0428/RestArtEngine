#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <RestArtEngine/Editor/ViewportInteraction.h>
#include <RestArtEngine/Editor/EditorDockManager.h>
#include <RestArtEngine/Editor/CameraNavigation.h>
#include <RestArtEngine/ImGui/EditorTheme.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace rae::editor {
namespace {
using namespace DirectX;

XMMATRIX actorMatrix(const scene::Transform& t) {
    return XMMatrixScaling(t.scale[0], t.scale[1], t.scale[2]) *
        XMMatrixRotationRollPitchYaw(XMConvertToRadians(t.rotation[0]),
            XMConvertToRadians(t.rotation[1]), XMConvertToRadians(t.rotation[2])) *
        XMMatrixTranslation(t.position[0], t.position[1], t.position[2]);
}

bool validTransform(const scene::Transform& t) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(t.position[i]) || !std::isfinite(t.rotation[i]) ||
            !std::isfinite(t.scale[i]) || std::abs(t.scale[i]) < 0.0001f) return false;
    return true;
}

// Moves the orbit focus onto the selected Actor and fits its mesh bounds in
// view, matching the usual "frame selected" shortcut.
void focusActor(renderer::SceneSettings& settings, const scene::World& world,
                const assets::AssetLibrary& library, scene::ActorId selected) {
    const auto* actor = world.findActor(selected);
    if (!actor || !validTransform(actor->transform)) return;
    if (actor->light || actor->camera) {
        const auto previousView = camera::evaluate(renderer::resolveCamera(settings), 1);
        std::copy(std::begin(actor->transform.position), std::end(actor->transform.position), settings.cameraTarget);
        settings.cameraDistance = 6;
        if (settings.cameraPose)
            for (int i = 0; i < 3; ++i) settings.cameraPose->position[i] = settings.cameraTarget[i] - previousView.forward[i] * 6;
        return;
    }
    const auto* mesh = library.findMesh(actor->mesh.meshId);
    if (!mesh) return;
    const auto& bounds = mesh->localBounds();
    if (!bounds.valid) return;
    const auto& minimum = bounds.minimum;
    const auto& maximum = bounds.maximum;
    const float center[3] = {(minimum[0] + maximum[0]) * 0.5f, (minimum[1] + maximum[1]) * 0.5f,
        (minimum[2] + maximum[2]) * 0.5f};
    const XMMATRIX matrix = actorMatrix(actor->transform);
    const XMVECTOR worldCenter = XMVector3TransformCoord(
        XMVectorSet(center[0], center[1], center[2], 1.0f), matrix);
    settings.cameraTarget[0] = XMVectorGetX(worldCenter);
    settings.cameraTarget[1] = XMVectorGetY(worldCenter);
    settings.cameraTarget[2] = XMVectorGetZ(worldCenter);
    float worldRadius = .05f;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const auto position = XMVectorSet(corner & 1 ? maximum[0] : minimum[0],
            corner & 2 ? maximum[1] : minimum[1], corner & 4 ? maximum[2] : minimum[2], 1);
        worldRadius = std::max(worldRadius, XMVectorGetX(XMVector3Length(XMVector3TransformCoord(position, matrix) - worldCenter)));
    }
    const float aspect = settings.cameraLens.aspectRatio > 0 ? settings.cameraLens.aspectRatio :
        static_cast<float>(std::max(1, settings.viewportWidth)) / std::max(1, settings.viewportHeight);
    const float halfFov = std::atan(std::tan(XMConvertToRadians(settings.cameraLens.verticalFovDegrees * .5f)) * std::min(1.0f, aspect));
    settings.cameraDistance = std::clamp(worldRadius / std::sin(halfFov) * 1.2f, .01f, 1000000.0f);
    settings.cameraLens.orthographicHeight = std::clamp(2.4f * worldRadius / std::min(1.0f, aspect), .001f, 10000000.0f);
    settings.cameraLens.farClip = std::max(settings.cameraLens.farClip, settings.cameraDistance + worldRadius * 3);
    settings.cameraLens.farClip = std::min(settings.cameraLens.farClip, 10000000.0f);
    settings.cameraLens.nearClip = std::max(settings.cameraLens.nearClip, settings.cameraLens.farClip / 1000000.0f);
    if (settings.cameraPose) {
        const auto view = camera::evaluate(renderer::resolveCamera(settings), aspect);
        for (int i = 0; i < 3; ++i) settings.cameraPose->position[i] = settings.cameraTarget[i] - view.forward[i] * settings.cameraDistance;
    }
}

// ImGuizmo's Euler decomposition uses a different rotation order. Extract the
// engine's row-vector Rz * Rx * Ry explicitly and preserve untouched components.
bool applyMatrix(const XMFLOAT4X4& matrix, int operation, scene::Transform& transform) {
    scene::Transform next = transform;
    if (operation == 0) {
        next.position[0] = matrix._41;
        next.position[1] = matrix._42;
        next.position[2] = matrix._43;
    } else if (operation == 1) {
        XMFLOAT4X4 rotation = matrix;
        for (int axis = 0; axis < 3; ++axis) {
            const float length = std::sqrt(matrix.m[axis][0] * matrix.m[axis][0] +
                matrix.m[axis][1] * matrix.m[axis][1] + matrix.m[axis][2] * matrix.m[axis][2]);
            if (length < .0001f) return false;
            const float signedLength = std::copysign(length, transform.scale[axis]);
            for (int column = 0; column < 3; ++column) rotation.m[axis][column] /= signedLength;
        }
        const float pitch = std::asin(std::clamp(-rotation._32, -1.0f, 1.0f));
        const bool singular = std::abs(std::cos(pitch)) < .0001f;
        next.rotation[0] = XMConvertToDegrees(pitch);
        next.rotation[1] = XMConvertToDegrees(singular ? std::atan2(-rotation._13, rotation._11) :
            std::atan2(rotation._31, rotation._33));
        next.rotation[2] = singular ? 0.0f : XMConvertToDegrees(std::atan2(rotation._12, rotation._22));
    } else {
        XMFLOAT4X4 rotation;
        XMStoreFloat4x4(&rotation, XMMatrixRotationRollPitchYaw(XMConvertToRadians(transform.rotation[0]),
            XMConvertToRadians(transform.rotation[1]), XMConvertToRadians(transform.rotation[2])));
        for (int axis = 0; axis < 3; ++axis) {
            const float scale = matrix.m[axis][0] * rotation.m[axis][0] +
                matrix.m[axis][1] * rotation.m[axis][1] + matrix.m[axis][2] * rotation.m[axis][2];
            next.scale[axis] = std::copysign(std::max(std::abs(scale), .001f), scale);
        }
    }
    if (!validTransform(next)) return false;
    bool changed = false;
    for (int i = 0; i < 3; ++i)
        changed |= next.position[i] != transform.position[i] || next.rotation[i] != transform.rotation[i] ||
            next.scale[i] != transform.scale[i];
    if (changed) transform = next;
    return changed;
}

struct ViewRay {
    XMVECTOR origin;
    XMVECTOR direction;
    float length;
};

struct SurfaceHit {
    scene::ActorId actor = 0;
    std::size_t materialSlot = 0;
    XMFLOAT3 point{};
    XMFLOAT3 normal{};
    float distance = 0;
};

ViewRay viewRay(const renderer::SceneSettings& settings, const ImVec2& imageMin, const ImVec2& imageMax) {
    const auto mouse = ImGui::GetMousePos();
    const auto view = camera::evaluate(renderer::resolveCamera(settings),
        static_cast<float>(settings.viewportWidth) / static_cast<float>(settings.viewportHeight));
    const auto ray = camera::screenRay(view, 2 * (mouse.x - imageMin.x) / (imageMax.x - imageMin.x) - 1,
        1 - 2 * (mouse.y - imageMin.y) / (imageMax.y - imageMin.y));
    return {XMVectorSet(ray.origin[0], ray.origin[1], ray.origin[2], 1),
        XMVectorSet(ray.direction[0], ray.direction[1], ray.direction[2], 0), ray.maxDistance};
}
SurfaceHit closestSurface(const ViewRay& ray, const scene::World& world, const assets::AssetLibrary& assets,
                          const assets::MaterialAsset* materialOverride, assets::MeshQueryCache& queries) {
    SurfaceHit hit;
    hit.distance = ray.length;
    for (const auto& actor : world.actors) {
        if (actor.light || actor.camera || !actor.visible || !validTransform(actor.transform)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh) continue;
        std::vector<assets::MeshFaceState> faceStates(mesh->materialSlotCount());
        for (std::size_t slot = 0; slot < faceStates.size(); ++slot) {
            const auto* material = assets.findMaterial(actor.mesh.materialForSlot(*mesh, slot));
            if (material && materialOverride && materialOverride->id == material->id) material = materialOverride;
            // Test each BVH candidate's effective slot; rejecting a nearest hit
            // afterward would hide valid geometry behind a Never/culled face.
            faceStates[slot].visible = material && material->depthTest != assets::MaterialDepthTest::Never;
            if (material) faceStates[slot].cull = material->effectiveCullMode();
        }
        const XMMATRIX matrix = actorMatrix(actor.transform);
        const float determinant = XMVectorGetX(XMMatrixDeterminant(matrix));
        if (!std::isfinite(determinant) || determinant == 0) continue;
        const auto inverse = XMMatrixInverse(nullptr, matrix);
        XMFLOAT3 localOrigin, localDirection;
        XMStoreFloat3(&localOrigin, XMVector3TransformCoord(ray.origin, inverse));
        XMStoreFloat3(&localDirection, XMVector3TransformNormal(ray.direction, inverse));
        const assets::MeshRay localRay{{localOrigin.x, localOrigin.y, localOrigin.z},
            {localDirection.x, localDirection.y, localDirection.z}};
        assets::MeshRayHit meshHit;
        if (!queries.intersect(*mesh, localRay, hit.distance, false,
                1e-8f / std::abs(determinant), meshHit, false, faceStates)) continue;
        hit.actor = actor.id;
        hit.materialSlot = mesh->materialSlotForIndex(meshHit.triangleOffset);
        hit.distance = meshHit.distance;
        XMStoreFloat3(&hit.point, ray.origin + XMVectorScale(ray.direction, meshHit.distance));
        XMVECTOR vertices[3];
        for (unsigned corner = 0; corner < 3; ++corner) {
            const float* p = mesh->vertices[mesh->indices[meshHit.triangleOffset + corner]].position;
            vertices[corner] = XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1), matrix);
        }
        // Local culling already accounts for the authored front. Recover the
        // world normal from only the winning face, including mirrored Actors.
        auto normal = XMVectorScale(XMVector3Normalize(XMVector3Cross(vertices[1] - vertices[0], vertices[2] - vertices[0])),
            determinant < 0 ? -1.0f : 1.0f);
        if (XMVectorGetX(XMVector3Dot(normal, ray.direction)) > 0) normal = XMVectorNegate(normal);
        XMStoreFloat3(&hit.normal, normal);
    }
    return hit;
}

// Editor-only symbols share the scene image's exact projection and clipping.
// They are pick targets, never mesh surfaces for material drops or placement.
scene::ActorId drawLightMarkers(const scene::World& world, const std::vector<scene::ActorId>& selection,
    FXMMATRIX view, CXMMATRIX projection, const ImVec2& imageMin, const ImVec2& imageMax, float toolbarBottom) {
    auto* draw = ImGui::GetWindowDrawList();
    const auto viewProjection = view * projection;
    const auto project = [&](FXMVECTOR position, ImVec2& screen, float& depth) {
        const auto clip = XMVector4Transform(XMVectorSetW(position, 1), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= .0001f) return false;
        depth = XMVectorGetZ(clip) / w;
        if (depth < 0 || depth > 1) return false;
        screen = {imageMin.x + (XMVectorGetX(clip) / w + 1) * .5f * (imageMax.x - imageMin.x),
            imageMin.y + (1 - XMVectorGetY(clip) / w) * .5f * (imageMax.y - imageMin.y)};
        return true;
    };
    const ImVec2 clipMin{imageMin.x, std::max(imageMin.y, toolbarBottom)};
    const auto mouse = ImGui::GetMousePos();
    scene::ActorId hit = 0;
    float hitDepth = 2;
    draw->PushClipRect(clipMin, imageMax, true);
    for (const auto& actor : world.actors) {
        if ((!actor.light && !actor.camera) || !actor.visible || !validTransform(actor.transform)) continue;
        const auto position = XMVectorSet(actor.transform.position[0], actor.transform.position[1], actor.transform.position[2], 1);
        ImVec2 center;
        float depth;
        if (!project(position, center, depth) || center.x < clipMin.x || center.x > imageMax.x ||
            center.y < clipMin.y || center.y > imageMax.y) continue;
        const bool selected = std::find(selection.begin(), selection.end(), actor.id) != selection.end();
        const ImU32 color = ImGui::GetColorU32(selected ? ImGuiCol_PlotHistogram : ImGuiCol_Text);
        draw->AddCircleFilled(center, 11, IM_COL32(20, 23, 29, 220), 20);
        draw->AddCircle(center, 11, color, 20, selected ? 2.0f : 1.0f);
        const char* symbol = actor.camera ? "C" : actor.light->type == scene::LightType::Directional ? "D" : actor.light->type == scene::LightType::Sky ? "S" :
            actor.light->type == scene::LightType::Rect ? "R" : actor.light->type == scene::LightType::Spot ? "T" : "P";
        const auto textSize = ImGui::CalcTextSize(symbol);
        draw->AddText({center.x - textSize.x * .5f, center.y - textSize.y * .5f}, color, symbol);
        const float dx = mouse.x - center.x, dy = mouse.y - center.y;
        if (mouse.y >= clipMin.y && dx * dx + dy * dy <= 14 * 14 && depth < hitDepth) {
            hit = actor.id; hitDepth = depth;
        }
        if (!selected || (actor.light && (actor.light->type == scene::LightType::Sky || actor.light->type == scene::LightType::Point))) continue;
        const auto rotation = XMMatrixRotationRollPitchYaw(XMConvertToRadians(actor.transform.rotation[0]),
            XMConvertToRadians(actor.transform.rotation[1]), XMConvertToRadians(actor.transform.rotation[2]));
        const auto line = [&](FXMVECTOR a, FXMVECTOR b) {
            ImVec2 from, to; float fromDepth, toDepth;
            if (project(position + XMVector3TransformNormal(a, rotation), from, fromDepth) &&
                project(position + XMVector3TransformNormal(b, rotation), to, toDepth))
                draw->AddLine(from, to, color, 1.5f);
        };
        line(XMVectorZero(), XMVectorSet(0, 0, -1.5f, 0));
        if (actor.camera) {
            // Compact lens guide: clip planes are edited numerically; the guide
            // shows projection shape without spanning a million-unit far plane.
            const auto& lens = *actor.camera;
            const float length = std::min(3.0f, lens.farClip);
            const float halfHeight = lens.projection == camera::Projection::Orthographic ? lens.orthographicHeight * .5f :
                length * std::tan(XMConvertToRadians(lens.verticalFovDegrees * .5f));
            const float halfWidth = halfHeight * (lens.aspectRatio > 0 ? lens.aspectRatio :
                (imageMax.x - imageMin.x) / std::max(1.0f, imageMax.y - imageMin.y));
            const XMVECTOR corners[] = {XMVectorSet(-halfWidth,-halfHeight,-length,0), XMVectorSet(halfWidth,-halfHeight,-length,0),
                XMVectorSet(halfWidth,halfHeight,-length,0), XMVectorSet(-halfWidth,halfHeight,-length,0)};
            for (int i = 0; i < 4; ++i) {
                line(corners[i], corners[(i + 1) % 4]);
                line(lens.projection == camera::Projection::Perspective ? XMVectorZero() : XMVectorSetZ(corners[i], 0), corners[i]);
            }
            continue;
        }
        const auto& light = *actor.light;
        if (light.type == scene::LightType::Rect) {
            const float x = light.width * .5f, y = light.height * .5f;
            const XMVECTOR corners[] = {XMVectorSet(-x,-y,0,0), XMVectorSet(x,-y,0,0),
                XMVectorSet(x,y,0,0), XMVectorSet(-x,y,0,0)};
            for (int i = 0; i < 4; ++i) line(corners[i], corners[(i + 1) % 4]);
        } else if (light.type == scene::LightType::Spot) {
            const float length = std::min(light.range, 3.0f);
            const float radius = std::tan(XMConvertToRadians(light.outerConeAngle)) * length;
            for (int i = 0; i < 16; ++i) {
                const float a = XM_2PI * i / 16, b = XM_2PI * (i + 1) / 16;
                const auto end = XMVectorSet(radius * std::cos(a), radius * std::sin(a), -length, 0);
                line(end, XMVectorSet(radius * std::cos(b), radius * std::sin(b), -length, 0));
                if (i % 4 == 0) line(XMVectorZero(), end);
            }
        }
    }
    draw->PopClipRect();
    return hit;
}

const assets::AssetEntry* droppedAsset(const ImGuiPayload& payload, const assets::AssetRegistry& registry) {
    // Content paths have no terminating NUL. Resolve the copied byte range
    // afresh each frame instead of retaining a registry entry across a rescan.
    if (!payload.Data || payload.DataSize <= 0 || payload.DataSize > 4096) return nullptr;
    const std::string_view path(static_cast<const char*>(payload.Data), static_cast<std::size_t>(payload.DataSize));
    if (path.find('\0') != std::string_view::npos) return nullptr;
    const auto* entry = registry.find(path);
    if (!entry || !entry->id) return nullptr;
    std::error_code error;
    if (!std::filesystem::is_regular_file(entry->path, error)) return nullptr;
    return entry;
}

bool meshBounds(const assets::MeshAsset& mesh, XMFLOAT3& minimum, XMFLOAT3& maximum) {
    const auto& bounds = mesh.localBounds();
    if (!bounds.valid || mesh.indices.empty()) return false;
    minimum = {bounds.minimum[0], bounds.minimum[1], bounds.minimum[2]};
    maximum = {bounds.maximum[0], bounds.maximum[1], bounds.maximum[2]};
    return true;
}

bool dropTransform(const ViewRay& ray, const SurfaceHit& surface, const XMFLOAT3& minimum,
                   const XMFLOAT3& maximum, scene::Transform& transform, XMVECTOR& contact) {
    XMVECTOR normal;
    if (surface.actor) {
        contact = XMLoadFloat3(&surface.point);
        normal = XMLoadFloat3(&surface.normal);
    } else {
        const float vertical = XMVectorGetY(ray.direction);
        if (std::abs(vertical) < 1e-5f) return false;
        const float distance = -XMVectorGetY(ray.origin) / vertical;
        if (!std::isfinite(distance) || distance < 0 || distance > ray.length) return false;
        contact = ray.origin + XMVectorScale(ray.direction, distance);
        normal = XMVectorSet(0, 1, 0, 0);
    }
    // Keep the authored orientation/scale. Move the bounds' supporting corner
    // to the contact plane so an off-centre pivot cannot bury the new instance.
    const auto support = XMVectorSet(XMVectorGetX(normal) >= 0 ? minimum.x : maximum.x,
        XMVectorGetY(normal) >= 0 ? minimum.y : maximum.y,
        XMVectorGetZ(normal) >= 0 ? minimum.z : maximum.z, 0);
    const auto position = contact - XMVectorScale(normal, XMVectorGetX(XMVector3Dot(support, normal)));
    XMFLOAT3 placed;
    XMStoreFloat3(&placed, position);
    transform.position[0] = placed.x;
    transform.position[1] = placed.y;
    transform.position[2] = placed.z;
    return validTransform(transform);
}

void drawDropHint(const ImVec2& clipMin, const ImVec2& clipMax, const char* text, bool valid) {
    auto* drawList = ImGui::GetWindowDrawList();
    const float wrapWidth = std::max(1.0f, clipMax.x - clipMin.x - 16.0f);
    const auto textSize = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
    const ImVec2 position{clipMin.x + 8, std::max(clipMin.y + 8, clipMax.y - textSize.y - 8)};
    drawList->PushClipRect(clipMin, clipMax, true);
    drawList->AddRectFilled({position.x - 4, position.y - 4},
        {position.x + textSize.x + 4, position.y + textSize.y + 4},
        ImGui::GetColorU32(ImGuiCol_WindowBg, .92f), ImGui::GetStyle().FrameRounding);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position,
        ImGui::GetColorU32(valid ? ImGuiCol_DragDropTarget : ImGuiCol_Text), text, nullptr, wrapWidth);
    drawList->PopClipRect();
}

void drawGeometryStatistics(const renderer::SceneGeometryStatistics& statistics,
                            const ImVec2& clipMin, const ImVec2& clipMax, bool assetDragging) {
    const float wrapWidth = clipMax.x - clipMin.x - 16.0f;
    if (wrapWidth < 32.0f || clipMax.y - clipMin.y < ImGui::GetTextLineHeight() + 16.0f) return;
    std::string text = "Scene tris: " + (statistics.frameId ? std::to_string(statistics.sceneTriangles) : "pending");
    text += "\nAfter clipping: ";
    switch (statistics.status) {
    case renderer::GeometrySampleStatus::Ready: text += std::to_string(statistics.postClipTriangles); break;
    case renderer::GeometrySampleStatus::Pending: text += "pending"; break;
    case renderer::GeometrySampleStatus::NotApplicable: text += "N/A"; break;
    case renderer::GeometrySampleStatus::NoViewport: text += "no viewport"; break;
    default: text += "unavailable"; break;
    }
    const auto textSize = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapWidth);
    // Drag feedback occupies the lower edge. Move the read-only statistics
    // below the toolbar during that gesture without adding an input item.
    const ImVec2 position{std::max(clipMin.x + 8, clipMax.x - textSize.x - 8),
        assetDragging ? clipMin.y + 8 : std::max(clipMin.y + 8, clipMax.y - textSize.y - 8)};
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(clipMin, clipMax, true);
    drawList->AddRectFilled({position.x - 4, position.y - 4},
        {position.x + textSize.x + 4, position.y + textSize.y + 4},
        ImGui::GetColorU32(ImGuiCol_WindowBg, .88f), ImGui::GetStyle().FrameRounding);
    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), position,
        ImGui::GetColorU32(ImGuiCol_Text), text.c_str(), nullptr, wrapWidth);
    drawList->PopClipRect();
    if (!assetDragging && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(position, {position.x + textSize.x, position.y + textSize.y})) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        ImGui::TextUnformatted("Scene triangles count every Actor instance, including hidden instances. Unused mesh assets are excluded.");
        ImGui::TextUnformatted("After clipping is main raster geometry output, before depth and occlusion tests. Clipping can split triangles, so output can exceed input.");
        ImGui::Text("Scene frame %llu | GPU sample %llu (asynchronous)",
            static_cast<unsigned long long>(statistics.frameId), static_cast<unsigned long long>(statistics.gpuFrameId));
        if (!statistics.statusMessage.empty()) ImGui::TextUnformatted(statistics.statusMessage.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void drawDropPreview(const renderer::SceneSettings& settings, FXMMATRIX view, CXMMATRIX projection,
                     const scene::Transform& transform, FXMVECTOR contact, const XMFLOAT3& minimum,
                     const XMFLOAT3& maximum, const ImVec2& imageMin, const ImVec2& imageMax,
                     const ImVec2& clipMin, const ImVec2& clipMax) {
    const auto project = [&](FXMVECTOR point, ImVec2& screen) {
        const auto clip = XMVector4Transform(XMVectorSetW(point, 1), view * projection);
        if (XMVectorGetW(clip) <= 1e-5f) return false;
        const auto pixel = XMVector3Project(point, static_cast<float>(settings.viewportX),
            static_cast<float>(settings.viewportY), static_cast<float>(settings.viewportWidth),
            static_cast<float>(settings.viewportHeight), 0, 1, projection, view, XMMatrixIdentity());
        screen = {imageMin.x + (XMVectorGetX(pixel) - settings.viewportX) * (imageMax.x - imageMin.x) / settings.viewportWidth,
            imageMin.y + (XMVectorGetY(pixel) - settings.viewportY) * (imageMax.y - imageMin.y) / settings.viewportHeight};
        return std::isfinite(screen.x) && std::isfinite(screen.y);
    };
    ImVec2 corners[8];
    bool visible[8];
    for (int index = 0; index < 8; ++index)
        visible[index] = project(XMVectorSet((index & 1 ? maximum.x : minimum.x) + transform.position[0],
            (index & 2 ? maximum.y : minimum.y) + transform.position[1],
            (index & 4 ? maximum.z : minimum.z) + transform.position[2], 1), corners[index]);
    auto* drawList = ImGui::GetWindowDrawList();
    const auto accent = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    drawList->PushClipRect(clipMin, clipMax, true);
    for (int index = 0; index < 8; ++index)
        for (const int edge : {1, 2, 4})
            if (!(index & edge) && visible[index] && visible[index | edge])
                drawList->AddLine(corners[index], corners[index | edge], accent, 1.5f);
    ImVec2 marker;
    if (project(contact, marker)) {
        drawList->AddCircle(marker, 5.0f, accent, 16, 2.0f);
        drawList->AddLine({marker.x - 9, marker.y}, {marker.x + 9, marker.y}, accent);
        drawList->AddLine({marker.x, marker.y - 9}, {marker.x, marker.y + 9}, accent);
    }
    drawList->PopClipRect();
    drawDropHint(clipMin, clipMax, "Release to place mesh | Esc to cancel", true);
}
} // namespace

void ViewportInteraction::finishCameraGesture(const scene::World& world) {
    if (cameraGesture_) {
        if (const auto* actor = world.findActor(gestureCamera_); actor && actor->camera && onCameraEdited)
            onCameraEdited(actor->id, gestureTransform_, gestureLens_, actor->transform, *actor->camera);
    }
    cameraGesture_ = false;
    gestureCamera_ = 0;
}

void ViewportInteraction::requestFreeCamera(const renderer::SceneSettings& settings, bool restoreExposure) {
    pendingCameraAction_ = 4;
    pendingFreeCamera_ = renderer::resolveCamera(settings);
    pendingFreeHadPose_ = settings.cameraPose.has_value();
    pendingOrbit_[0] = settings.cameraYaw; pendingOrbit_[1] = settings.cameraPitch; pendingOrbit_[2] = settings.cameraDistance;
    std::copy_n(settings.cameraTarget, 3, pendingOrbit_ + 3);
    pendingRestoreExposure_ = restoreExposure;
    pendingExposure_ = settings.exposure;
}

bool ViewportInteraction::setCameraView(renderer::SceneSettings& settings, const scene::World& world,
    scene::ActorId id, bool pilot) {
    pendingCameraAction_ = 0;
    const auto* actor = world.findActor(id);
    if (!actor || !actor->camera || !actor->visible) return false;
    finishCameraGesture(world);
    if (cameraMode_ == CameraMode::Free) {
        savedFreeCamera_ = renderer::resolveCamera(settings);
        savedFreeHadPose_ = settings.cameraPose.has_value();
        savedOrbit_[0] = settings.cameraYaw; savedOrbit_[1] = settings.cameraPitch;
        savedOrbit_[2] = settings.cameraDistance;
        std::copy_n(settings.cameraTarget, 3, savedOrbit_ + 3);
    }
    cameraActor_ = id;
    cameraMode_ = pilot ? CameraMode::Pilot : CameraMode::View;
    camera::Pose pose;
    std::copy_n(actor->transform.position, 3, pose.position);
    std::copy_n(actor->transform.rotation, 3, pose.rotation);
    settings.cameraPose = pose;
    settings.cameraLens = *actor->camera;
    orbiting_ = panning_ = false;
    return true;
}

void ViewportInteraction::returnToFreeCamera(renderer::SceneSettings& settings) {
    pendingCameraAction_ = 0;
    if (cameraMode_ == CameraMode::Free) return;
    settings.cameraLens = savedFreeCamera_.lens;
    settings.cameraPose = savedFreeHadPose_ ? std::optional<camera::Pose>(savedFreeCamera_.pose) : std::nullopt;
    settings.cameraYaw = savedOrbit_[0]; settings.cameraPitch = savedOrbit_[1]; settings.cameraDistance = savedOrbit_[2];
    std::copy_n(savedOrbit_ + 3, 3, settings.cameraTarget);
    cameraActor_ = 0;
    cameraMode_ = CameraMode::Free;
    orbiting_ = panning_ = false;
}

bool ViewportInteraction::draw(renderer::SceneSettings& settings, scene::World& world,
                               const assets::AssetLibrary& assets, std::vector<scene::ActorId>& selection,
                               scene::ActorId& selected, renderer::SceneRenderer& renderer,
                               const std::function<void()>& duplicateForDrag,
                               const assets::AssetRegistry& registry,
                               const std::function<void(assets::AssetId, const scene::Transform&)>& createFromMesh,
                               const std::function<void(scene::ActorId, assets::AssetId, std::size_t)>& assignMaterial,
                               const assets::MaterialAsset* materialOverride, bool keyboardBlocked, bool mainSurfaceVisible, const Surface* surface) {
    const int pendingAction = pendingCameraAction_;
    pendingCameraAction_ = 0;
    if (pendingAction == 1 || pendingAction == 2)
        setCameraView(settings, world, pendingCameraActor_, pendingAction == 2);
    else if (pendingAction >= 3) {
        stopCameraNavigation(world);
        returnToFreeCamera(settings);
        if (pendingAction == 4) {
            settings.cameraLens = pendingFreeCamera_.lens;
            settings.cameraPose = pendingFreeHadPose_ ? std::optional<camera::Pose>(pendingFreeCamera_.pose) : std::nullopt;
            settings.cameraYaw = pendingOrbit_[0]; settings.cameraPitch = pendingOrbit_[1]; settings.cameraDistance = pendingOrbit_[2];
            std::copy_n(pendingOrbit_ + 3, 3, settings.cameraTarget);
            if (pendingRestoreExposure_) settings.exposure = pendingExposure_;
        }
    }
    imageRect_ = {};
    if (cameraMode_ != CameraMode::Free) {
        const auto* actor = world.findActor(cameraActor_);
        if (!actor || !actor->camera || !actor->visible) {
            finishCameraGesture(world);
            returnToFreeCamera(settings);
        } else {
            camera::Pose pose;
            std::copy_n(actor->transform.position, 3, pose.position);
            std::copy_n(actor->transform.rotation, 3, pose.rotation);
            settings.cameraPose = pose; settings.cameraLens = *actor->camera;
        }
    }
    meshQueries_->retain(assets);
    hovered_ = focused_ = false;
    pickedThisFrame_ = false;
    materialDragging_ = false;
    materialDropTarget_ = 0;
    settings.editorViewport = true;
    settings.viewportVisible = false;
    settings.viewportWidth = settings.viewportHeight = 0;
    if (!surface) ImGuizmo::BeginFrame();
    ImGuizmo::PushID(surface ? surface->index + 1 : 1);
    bool visible;
    if (surface) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        visible = ImGui::BeginChild(surface->name, {surface->width, surface->height}, ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
    } else {
        ImGui::SetNextWindowSize({640, 480}, ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        visible = ImGui::Begin(EditorDockManager::panelName(EditorPanel::Viewport), nullptr,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
    }
    const auto endSurface = [&] { ImGuizmo::PopID(); if (surface) ImGui::EndChild(); else ImGui::End(); };
    const auto* windowViewport = ImGui::GetWindowViewport();
    const bool inMainViewport = !surface && windowViewport == ImGui::GetMainViewport();
    const auto platformWindow = static_cast<HWND>(windowViewport->PlatformHandleRaw
        ? windowViewport->PlatformHandleRaw : windowViewport->PlatformHandle);
    // Windows hides owned native tools when their owner minimizes, without
    // necessarily marking them iconic. Keep their layout alive but skip hidden
    // scene GPU work. A newly detached viewport may not own an HWND until this
    // frame's platform update, so an absent handle is not a hidden window.
    const bool hiddenOwnedWindow = !mainSurfaceVisible && !inMainViewport && platformWindow &&
        !IsWindowVisible(platformWindow);
    if (!visible || (!mainSurfaceVisible && windowViewport == ImGui::GetMainViewport()) ||
        hiddenOwnedWindow || (windowViewport->Flags & ImGuiViewportFlags_IsMinimized)) {
        finishCameraGesture(world);
        usingGizmo_ = orbiting_ = panning_ = false;
        endSurface();
        return false;
    }
    // Docked rendering retains the main-client pixel rectangle. An independent
    // platform viewport owns a texture-local rectangle, unrelated to its desktop
    // position or to the main window's size. Input always maps from the actual
    // desktop image rectangle into that render rectangle.
    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 mainOrigin = ImGui::GetMainViewport()->Pos;
    const ImVec2 localMin = inMainViewport ? ImVec2(contentMin.x - mainOrigin.x, contentMin.y - mainOrigin.y) : ImVec2(0, 0);
    const auto mainSize = ImGui::GetMainViewport()->Size;
    const float requestedWidth = inMainViewport ? mainSize.x : available.x;
    const float requestedHeight = inMainViewport ? mainSize.y : available.y;
    if (!std::isfinite(requestedWidth) || !std::isfinite(requestedHeight) || available.x < 1 || available.y < 1 ||
        requestedWidth > 16384 || requestedHeight > 16384) {
        usingGizmo_ = orbiting_ = panning_ = false;
        ImGui::TextWrapped("Scene viewport size is unavailable or exceeds the device texture limit.");
        endSurface();
        return false;
    }
    std::string targetError;
    // A small target reserve also keeps material and asset previews usable in
    // a narrow/minimized scene panel. It does not stretch or crop the image.
    if (!surface && !renderer.ensureSceneTargetSize(std::max(256u, static_cast<unsigned>(std::ceil(requestedWidth))),
            std::max(256u, static_cast<unsigned>(std::ceil(requestedHeight))), targetError)) {
        usingGizmo_ = orbiting_ = panning_ = false;
        ImGui::TextWrapped("Scene viewport unavailable: %s", targetError.c_str());
        endSurface();
        return false;
    }
    const float targetWidth = static_cast<float>(renderer.sceneTextureWidth());
    const float targetHeight = static_cast<float>(renderer.sceneTextureHeight());
    int left = static_cast<int>(std::clamp(localMin.x, 0.0f, targetWidth));
    int top = static_cast<int>(std::clamp(localMin.y, 0.0f, targetHeight));
    int imageRight = static_cast<int>(std::clamp(localMin.x + available.x, 0.0f, targetWidth));
    int bottom = static_cast<int>(std::clamp(localMin.y + available.y, 0.0f, targetHeight));
    ImGui::GetWindowDrawList()->AddRectFilled(contentMin, {contentMin.x + available.x, contentMin.y + available.y}, IM_COL32(0, 0, 0, 255));
    if (settings.cameraLens.aspectRatio > 0 && imageRight > left && bottom > top) {
        const float requestedAspect = settings.cameraLens.aspectRatio;
        const float currentAspect = static_cast<float>(imageRight - left) / (bottom - top);
        if (currentAspect > requestedAspect) {
            const int width = std::max(1, static_cast<int>((bottom - top) * requestedAspect));
            left += (imageRight - left - width) / 2; imageRight = left + width;
        } else {
            const int height = std::max(1, static_cast<int>((imageRight - left) / requestedAspect));
            top += (bottom - top - height) / 2; bottom = top + height;
        }
    }
    settings.viewportX = left;
    settings.viewportY = top;
    settings.viewportWidth = std::max(0, imageRight - left);
    settings.viewportHeight = std::max(0, bottom - top);
    if (settings.viewportWidth < 1 || settings.viewportHeight < 1 || !renderer.sceneTexture()) {
        usingGizmo_ = orbiting_ = panning_ = false;
        endSurface();
        return false;
    }
    settings.viewportVisible = true;
    const ImVec2 imageMin = inMainViewport ? ImVec2(mainOrigin.x + left, mainOrigin.y + top) : ImVec2(contentMin.x + left, contentMin.y + top);
    const ImVec2 imageMax = inMainViewport ? ImVec2(mainOrigin.x + imageRight, mainOrigin.y + bottom)
        : ImVec2(contentMin.x + imageRight, contentMin.y + bottom);
    imageRect_ = {imageMin.x, imageMin.y, imageMax.x, imageMax.y};
    void* texture = renderer.sceneTexture();
    ImVec2 uvMin{left / targetWidth, top / targetHeight}, uvMax{imageRight / targetWidth, bottom / targetHeight};
    if (surface && !surface->active) {
        if (renderer.renderViewportPreview(settings, world, assets, materialOverride,
            static_cast<std::uint32_t>(settings.viewportWidth), static_cast<std::uint32_t>(settings.viewportHeight), targetError, surface->previewSlot))
            texture = renderer.previewTexture(surface->previewSlot);
        else texture = nullptr;
        uvMin = {0, 0}; uvMax = {1, 1};
    }
    if (texture) ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
        imageMin, imageMax, uvMin, uvMax);
    // Measure the actual three-row quad toolbar before placing its backdrop.
    // Channel zero retains the image; channel one keeps controls above the fill.
    ImDrawListSplitter toolbarLayers;
    auto* toolbarDrawList = ImGui::GetWindowDrawList();
    if (surface) {
        toolbarLayers.Split(toolbarDrawList, 2);
        toolbarLayers.SetCurrentChannel(toolbarDrawList, 1);
    }
    ImGui::SetCursorScreenPos({contentMin.x + 8, contentMin.y + 8});
    if (surface) {
        const char* titles[] = {"View 1", "View 2", "View 3", "View 4"};
        ImGui::TextColored(surface->active ? ImVec4(1,.72f,.25f,1) : ImVec4(.75f,.78f,.82f,1),
            "%s | %s | %s", titles[surface->index],
            settings.cameraLens.projection == camera::Projection::Orthographic ? "Ortho" : "Perspective", surface->active ? "Active" : "Raster");
        if (!texture) ImGui::TextWrapped("Preview unavailable: %s", targetError.c_str());
    }
    hovered_ = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(imageMin, imageMax);
    focused_ = ImGui::IsWindowFocused() && (!surface || surface->active);
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const auto* activePayload = ImGui::GetDragDropPayload();
    const bool assetDragging = activePayload && activePayload->IsDataType("RAE_ASSET");
    const auto* activeAsset = assetDragging ? droppedAsset(*activePayload, registry) : nullptr;
    materialDragging_ = activeAsset && activeAsset->type == assets::AssetType::Material;
    ImGuiIO& io = ImGui::GetIO();
    // WantTextInput is computed from the preceding frame. A click in the scene
    // can already have deactivated the text field drawn earlier this frame, so
    // use live item ownership for pointer input and retain the flag for keys.
    const bool blocked = (surface && !surface->active) || popup || assetDragging || (!usingGizmo_ && ImGui::IsAnyItemActive());
    if (!keyboardBlocked && !blocked && !io.WantTextInput && !usingGizmo_ && !isCameraNavigating() &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right) && (hovered_ || focused_)) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) operation_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) operation_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) operation_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) operation_ = (operation_ + 1) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && cameraMode_ == CameraMode::Free) focusActor(settings, world, assets, selected);
    }
    ImGui::BeginDisabled(usingGizmo_ || assetDragging || (surface && !surface->active));
    const char* labels[] = {"Move [W]", "Rotate [E]", "Scale [R]"};
    if (!surface) {
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine();
        const bool active = operation_ == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::SmallButton(labels[i])) operation_ = i;
        if (active) ImGui::PopStyleColor();
    }
    if (ImGui::GetWindowWidth() >= 620.0f) ImGui::SameLine();
    if (ImGui::SmallButton(local_ ? "Local" : "World")) local_ = !local_;
    ImGui::SameLine(); ImGui::Checkbox("Snap", &snap_);
    } else {
        ImGui::SetNextItemWidth(92);
        ImGui::Combo("##TransformTool", &operation_, "Move [W]\0Rotate [E]\0Scale [R]\0");
        ImGui::SameLine();
    }
    if (ImGui::SmallButton("Camera / View")) ImGui::OpenPopup("Camera sources");
    if (ImGui::BeginPopup("Camera sources")) {
        auto freeDraft = settings;
        bool freeChanged = false;
        if (ImGui::MenuItem("Free view", nullptr, cameraMode_ == CameraMode::Free)) {
            requestReturnToFreeCamera();
        }
        for (const auto& actor : world.actors) if (actor.camera && actor.visible) {
            ImGui::PushID(static_cast<int>(actor.id));
            if (ImGui::BeginMenu(actor.name.c_str())) {
                if (ImGui::MenuItem("View (locked)")) requestCameraView(actor.id, false);
                if (ImGui::MenuItem("Pilot (edit camera)")) requestCameraView(actor.id, true);
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::BeginDisabled(cameraMode_ != CameraMode::Free);
        if (ImGui::MenuItem("Perspective", nullptr, freeDraft.cameraLens.projection == camera::Projection::Perspective)) {
            freeDraft.cameraLens.projection = camera::Projection::Perspective; freeChanged = true;
        }
        if (ImGui::MenuItem("Orthographic", nullptr, freeDraft.cameraLens.projection == camera::Projection::Orthographic)) {
            freeDraft.cameraLens.projection = camera::Projection::Orthographic; freeChanged = true;
        }
        if (ImGui::BeginMenu("Axis view")) {
            const char* names[] = {"Front (-Z)", "Back (+Z)", "Left (+X)", "Right (-X)", "Top (-Y)", "Bottom (+Y)"};
            const float rotations[][2] = {{0,0},{0,180},{0,-90},{0,90},{-90,0},{90,0}};
            for (int direction = 0; direction < 6; ++direction) if (ImGui::MenuItem(names[direction])) {
                auto next = renderer::resolveCamera(freeDraft);
                next.pose.rotation[0] = rotations[direction][0]; next.pose.rotation[1] = rotations[direction][1]; next.pose.rotation[2] = 0;
                next.lens.projection = camera::Projection::Orthographic;
                const auto view = camera::evaluate(next, 1);
                for (int i = 0; i < 3; ++i) next.pose.position[i] = freeDraft.cameraTarget[i] - view.forward[i] * freeDraft.cameraDistance;
                freeDraft.cameraPose = next.pose; freeDraft.cameraLens = next.lens; freeChanged = true;
            }
            ImGui::EndMenu();
        }
        freeChanged |= ImGui::DragFloat("Vertical FOV", &freeDraft.cameraLens.verticalFovDegrees, .2f, 1, 175, "%.1f deg", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Ortho height", &freeDraft.cameraLens.orthographicHeight, .05f, .001f, 1000000, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Near clip", &freeDraft.cameraLens.nearClip, .01f, std::max(.001f, freeDraft.cameraLens.farClip / 1000000), freeDraft.cameraLens.farClip - .001f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Far clip", &freeDraft.cameraLens.farClip, 1, freeDraft.cameraLens.nearClip + .001f, std::min(1000000.0f, freeDraft.cameraLens.nearClip * 1000000), "%.3f", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Aspect (0 = viewport)", &freeDraft.cameraLens.aspectRatio, .01f, 0, 10, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SeparatorText("Depth of field");
        freeChanged |= ImGui::Checkbox("Enable depth of field", &freeDraft.cameraLens.depthOfField.enabled);
        freeChanged |= ImGui::DragFloat("Focus distance (m)", &freeDraft.cameraLens.depthOfField.focusDistance, .05f, .01f, 1000000, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Aperture (f-stop)", &freeDraft.cameraLens.depthOfField.fStop, .05f, .7f, 128, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        freeChanged |= ImGui::DragFloat("Sensor height (mm)", &freeDraft.cameraLens.depthOfField.sensorHeightMm, .1f, 1, 100, "%.1f", ImGuiSliderFlags_AlwaysClamp);
        if (freeDraft.cameraLens.depthOfField.enabled && !renderer::depthOfFieldEligible(freeDraft.cameraLens))
            ImGui::TextWrapped("Depth of field needs perspective projection and focus distance beyond focal length.");
        if (freeChanged && camera::valid(renderer::resolveCamera(freeDraft))) requestFreeCamera(freeDraft);
        ImGui::EndDisabled();
        ImGui::Separator();
        if (surface) {
            ImGui::Checkbox("Local transform", &local_);
            ImGui::Checkbox("Snap transform", &snap_);
        }
        ImGui::Checkbox("Fly navigation", &fly_);
        ImGui::DragFloat("Fly speed", &flySpeed_, .1f, .01f, 100000, "%.2f units/s", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TextUnformatted("Fly: hold right mouse + W/A/S/D/Q/E; Shift accelerates");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const auto* viewed = world.findActor(cameraActor_);
    ImGui::TextDisabled("%s%s%s", cameraMode_ == CameraMode::Free ? (fly_ ? "Free / Fly" : "Free / Orbit") :
        cameraMode_ == CameraMode::Pilot ? "PILOT: " : "VIEW: ", viewed ? viewed->name.c_str() : "",
        cameraMode_ == CameraMode::View ? " (locked)" : "");
    ImGui::EndDisabled();
    if (!surface) {
    if (const auto* actor = world.findActor(selected))
        ImGui::TextColored({1.0f, .75f, .2f, 1.0f}, "%s%s", actor->name.c_str(), operation_ == 2 ? " | Local scale" : "");
    else ImGui::TextDisabled("Click a scene object to select");
    }
    const float toolbarBottom = ImGui::GetCursorScreenPos().y;
    if (surface) {
        toolbarLayers.SetCurrentChannel(toolbarDrawList, 0);
        toolbarDrawList->AddRectFilled(contentMin,
            {contentMin.x + available.x, std::min(contentMin.y + available.y, toolbarBottom)},
            IM_COL32(13, 17, 23, 180));
        toolbarLayers.Merge(toolbarDrawList);
    }
    const bool overToolbar = io.MousePos.y < toolbarBottom;
    const bool canInteract = !blocked && !overToolbar;
    // Camera drags deliberately ignore ImGui item activation: clicking the
    // viewport activates the window itself, which would otherwise cancel an
    // Alt+left-drag orbit before it ever starts.
    const bool canCamera = (!surface || surface->active) && cameraMode_ != CameraMode::View && !popup && !assetDragging && !overToolbar && hovered_ && !usingGizmo_;
    // Right/middle clicks do not necessarily move ImGui's navigation focus.
    // Claim focus when a camera gesture starts so the loss-of-focus guard below
    // cancels only an actual transition away from this scene window.
    if (canCamera && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
        (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
        ImGui::SetWindowFocus();
        focused_ = true;
    }
    if (!focused_ || popup || keyboardBlocked) stopCameraNavigation(world);
    if (assetDragging || cameraMode_ == CameraMode::View) orbiting_ = panning_ = false;
    const bool altOrbiting = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (canCamera && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) orbiting_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !altOrbiting) orbiting_ = false;
    if (canCamera && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning_ = false;
    NavigationInput navigation;
    navigation.fly = fly_ && !altOrbiting;
    if (orbiting_ && hovered_) { navigation.orbitX = io.MouseDelta.x; navigation.orbitY = io.MouseDelta.y; }
    if (panning_ && hovered_) { navigation.panX = io.MouseDelta.x; navigation.panY = io.MouseDelta.y; }
    if (canCamera && canInteract) navigation.dolly = io.MouseWheel;
    if (navigation.fly && orbiting_ && hovered_ && !keyboardBlocked && !io.WantTextInput) {
        const float travel = flySpeed_ * std::min(io.DeltaTime, .1f) * (io.KeyShift ? 4.0f : 1.0f);
        navigation.forward = travel * (static_cast<float>(ImGui::IsKeyDown(ImGuiKey_W)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_S)));
        navigation.right = travel * (static_cast<float>(ImGui::IsKeyDown(ImGuiKey_D)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_A)));
        navigation.up = travel * (static_cast<float>(ImGui::IsKeyDown(ImGuiKey_E)) - static_cast<float>(ImGui::IsKeyDown(ImGuiKey_Q)));
    }
    const bool navigating = navigation.orbitX != 0 || navigation.orbitY != 0 || navigation.panX != 0 || navigation.panY != 0 ||
        navigation.dolly != 0 || navigation.forward != 0 || navigation.right != 0 || navigation.up != 0;
    bool cameraChanged = false;
    if (navigating) {
        if (cameraMode_ == CameraMode::Pilot && !cameraGesture_) {
            if (const auto* actor = world.findActor(cameraActor_); actor && actor->camera) {
                cameraGesture_ = true; gestureCamera_ = actor->id; gestureTransform_ = actor->transform; gestureLens_ = *actor->camera;
            }
        }
        applyNavigation(settings, navigation, imageMax.y - imageMin.y);
        if (cameraMode_ == CameraMode::Pilot) {
            if (auto* actor = world.findActor(cameraActor_); actor && actor->camera) {
                const auto next = renderer::resolveCamera(settings);
                std::copy_n(next.pose.position, 3, actor->transform.position);
                std::copy_n(next.pose.rotation, 3, actor->transform.rotation);
                actor->camera = next.lens; cameraChanged = true;
            }
        }
    }
    if (cameraGesture_ && (!orbiting_ && !panning_) && navigation.dolly == 0) finishCameraGesture(world);
    const auto cameraView = camera::evaluate(renderer::resolveCamera(settings),
        static_cast<float>(settings.viewportWidth) / static_cast<float>(settings.viewportHeight));
    XMFLOAT4X4 cameraViewData, cameraProjectionData;
    std::copy(cameraView.view.begin(), cameraView.view.end(), &cameraViewData._11);
    std::copy(cameraView.projection.begin(), cameraView.projection.end(), &cameraProjectionData._11);
    const XMMATRIX view = XMLoadFloat4x4(&cameraViewData);
    const XMMATRIX projection = XMLoadFloat4x4(&cameraProjectionData);
    XMFLOAT4X4 viewData, projectionData;
    XMStoreFloat4x4(&viewData, view); XMStoreFloat4x4(&projectionData, projection);
    const auto lightMarkerHit = drawLightMarkers(world, selection, view, projection, imageMin, imageMax, toolbarBottom);
    // The native negative-axis hatching is an optional, persisted viewport aid.
    ImGuizmo::GetStyle().HatchedAxisLineThickness = imgui::uiPreferences().gizmoNegativeAxisHints ? 6.0f : 0.0f;
    ImGuizmo::SetOrthographic(settings.cameraLens.projection == camera::Projection::Orthographic);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y,
        imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    ImGuizmo::SetGizmoSizeClipSpace(.18f);
    ImGuizmo::Enable(!blocked && (hovered_ || usingGizmo_) && !orbiting_ && !panning_ && (!overToolbar || usingGizmo_));
    bool changed = cameraChanged, gizmoHovered = false;
    if (auto* actor = world.findActor(selected); actor && actor->visible && validTransform(actor->transform)) {
        XMFLOAT4X4 matrix;
        XMStoreFloat4x4(&matrix, actorMatrix(actor->transform));
        const ImGuizmo::OPERATION operations[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
        const float step = operation_ == 0 ? .5f : operation_ == 1 ? 15.0f : .1f;
        const float snap[3] = {step, step, step};
        // Scale is always along the Actor's own axes: world scaling a rotated
        // object would require shear, which a position/rotation/scale cannot store.
        const XMMATRIX pivotBefore = actorMatrix(actor->transform);
        const bool manipulated = ImGuizmo::Manipulate(&viewData._11, &projectionData._11, operations[operation_],
            local_ || operation_ == 2 ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &matrix._11,
            nullptr, snap_ ? snap : nullptr);
        const bool nowUsing = ImGuizmo::IsUsing();
        if (nowUsing && !usingGizmo_) duplicateOnDrag_ = io.KeyAlt;
        scene::Transform next = actor->transform;
        if (manipulated && applyMatrix(matrix, operation_, next) &&
            (!duplicateOnDrag_ || ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            if (duplicateOnDrag_) {
                // Copy before applying the first delta, with no arbitrary
                // offset. Appending Actors may invalidate every old pointer.
                duplicateForDrag();
                actor = world.findActor(selected);
                duplicateOnDrag_ = false;
            }
            actor->transform = next;
            changed = true;
            // Group transform: every other selected Actor is moved by the same
            // world-space delta the gizmo applied to the pivot.
            if (changed && selection.size() > 1) {
                const XMMATRIX delta = XMMatrixMultiply(actorMatrix(actor->transform),
                    XMMatrixInverse(nullptr, pivotBefore));
                for (const auto id : selection) {
                    if (id == selected) continue;
                    auto* other = world.findActor(id);
                    if (!other || !other->visible || !validTransform(other->transform)) continue;
                    XMFLOAT4X4 moved;
                    XMStoreFloat4x4(&moved, XMMatrixMultiply(actorMatrix(other->transform), delta));
                    applyMatrix(moved, operation_, other->transform);
                    // Rotate and scale move the Actor as well; only translate
                    // already wrote the position inside applyMatrix.
                    if (operation_ != 0) {
                        other->transform.position[0] = moved._41;
                        other->transform.position[1] = moved._42;
                        other->transform.position[2] = moved._43;
                    }
                }
            }
        }
        gizmoHovered = ImGuizmo::IsOver();
        usingGizmo_ = nowUsing;
    } else usingGizmo_ = false;
    if (!usingGizmo_) duplicateOnDrag_ = false;
    if (canCamera && !usingGizmo_ && !gizmoHovered && io.KeyAlt &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) orbiting_ = true;
    if (canInteract && hovered_ && !usingGizmo_ && !gizmoHovered && !orbiting_ && !panning_ && !io.KeyAlt &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pickedThisFrame_ = true;
        const auto hit = lightMarkerHit ? lightMarkerHit :
            closestSurface(viewRay(settings, imageMin, imageMax), world, assets, materialOverride, *meshQueries_).actor;
        if ((io.KeyCtrl || io.KeyShift) && hit) {
            // Additive picking mirrors the Outliner: toggle membership and keep
            // the clicked Actor as the primary one.
            const auto it = std::find(selection.begin(), selection.end(), hit);
            if (it != selection.end()) selection.erase(it); else selection.push_back(hit);
            selected = selection.empty() ? 0 : selection.back();
        } else {
            selected = hit;
            selection.assign(hit ? 1 : 0, hit);
        }
    }
    // The scene image is a draw-list primitive, not an ImGui item. Give its
    // actual visible area a stable drop target without inserting an invisible
    // button that would take mouse ownership from the camera or Gizmo.
    const ImRect dropArea({imageMin.x, std::max(imageMin.y, toolbarBottom)}, imageMax);
    if ((!surface || surface->active) && assetDragging && !popup && !usingGizmo_ &&
        ImGui::GetCurrentContext()->HoveredWindowUnderMovingWindow == ImGui::GetCurrentWindow() &&
        dropArea.GetWidth() > 0 && dropArea.GetHeight() > 0 &&
        ImGui::BeginDragDropTargetCustom(dropArea, ImGui::GetID("##scene-asset-drop"))) {
        const auto* mesh = activeAsset && activeAsset->type == assets::AssetType::Mesh ? assets.findMesh(activeAsset->id) : nullptr;
        const auto* material = materialDragging_ ? assets.findMaterial(activeAsset->id) : nullptr;
        XMFLOAT3 minimum, maximum;
        scene::Transform placement;
        XMVECTOR contact;
        const auto ray = viewRay(settings, imageMin, imageMax);
        if (material && assignMaterial) {
            const auto hit = closestSurface(ray, world, assets, materialOverride, *meshQueries_);
            if (const auto* targetActor = world.findActor(hit.actor)) {
                if (const auto* accepted = ImGui::AcceptDragDropPayload("RAE_ASSET",
                        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    materialDropTarget_ = targetActor->id;
                    const auto* targetMesh = assets.findMesh(targetActor->mesh.meshId);
                    const auto slotName = targetMesh && hit.materialSlot < targetMesh->materialSlots.size() ?
                        targetMesh->materialSlots[hit.materialSlot].name : "Material";
                    const auto hint = "Assign " + material->name + " to " + targetActor->name + " / " + slotName;
                    drawDropHint(dropArea.Min, dropArea.Max, hint.c_str(), true);
                    if (accepted->IsDelivery()) assignMaterial(targetActor->id, material->id, hit.materialSlot);
                }
            } else drawDropHint(dropArea.Min, dropArea.Max, "Point at an Actor to assign this material", false);
        } else if (!mesh || !createFromMesh) {
            drawDropHint(dropArea.Min, dropArea.Max, "Drop a mesh to place it, or a material onto an Actor", false);
        } else if (!meshBounds(*mesh, minimum, maximum)) {
            drawDropHint(dropArea.Min, dropArea.Max, "This mesh has no usable geometry", false);
        } else if (!dropTransform(ray, closestSurface(ray, world, assets, materialOverride, *meshQueries_), minimum, maximum, placement, contact)) {
            drawDropHint(dropArea.Min, dropArea.Max, "No placement surface here. Aim at visible geometry or the ground", false);
        } else {
            if (const auto* accepted = ImGui::AcceptDragDropPayload("RAE_ASSET",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                drawDropPreview(settings, view, projection, placement, contact, minimum, maximum, imageMin, imageMax, dropArea.Min, dropArea.Max);
                if (accepted->IsDelivery()) createFromMesh(mesh->id, placement);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (!surface || surface->active) drawGeometryStatistics(renderer.geometryStatistics(), dropArea.Min, dropArea.Max, assetDragging);
    endSurface();
    return changed;
}

} // namespace rae::editor
