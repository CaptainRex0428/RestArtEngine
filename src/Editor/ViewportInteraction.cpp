#include <RestArtEngine/Editor/ViewportInteraction.h>
#include <RestArtEngine/Editor/EditorDockManager.h>

#include <imgui.h>
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
    const auto* mesh = library.findMesh(actor->mesh.meshId);
    if (!mesh || mesh->vertices.empty()) return;
    float minimum[3] = {1e30f, 1e30f, 1e30f};
    float maximum[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& vertex : mesh->vertices)
        for (int axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
        }
    const float center[3] = {(minimum[0] + maximum[0]) * 0.5f, (minimum[1] + maximum[1]) * 0.5f,
        (minimum[2] + maximum[2]) * 0.5f};
    float radius = 0.0f;
    for (const auto& vertex : mesh->vertices) {
        const float dx = vertex.position[0] - center[0];
        const float dy = vertex.position[1] - center[1];
        const float dz = vertex.position[2] - center[2];
        radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    const XMMATRIX matrix = actorMatrix(actor->transform);
    const XMVECTOR worldCenter = XMVector3TransformCoord(
        XMVectorSet(center[0], center[1], center[2], 1.0f), matrix);
    settings.cameraTarget[0] = XMVectorGetX(worldCenter);
    settings.cameraTarget[1] = XMVectorGetY(worldCenter);
    settings.cameraTarget[2] = XMVectorGetZ(worldCenter);
    const float scale = std::max({std::abs(actor->transform.scale[0]),
        std::abs(actor->transform.scale[1]), std::abs(actor->transform.scale[2])});
    const float worldRadius = std::max(radius * scale, 0.05f);
    const float fit = worldRadius / std::tan(XMConvertToRadians(25.0f)) * 1.45f;
    settings.cameraDistance = std::clamp(fit, 2.0f, 30.0f);
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

bool intersectTriangle(FXMVECTOR origin, FXMVECTOR direction, FXMVECTOR a,
                       GXMVECTOR b, HXMVECTOR c, float& distance, bool twoSided, float orientation) {
    const XMVECTOR edge1 = b - a, edge2 = c - a;
    const XMVECTOR p = XMVector3Cross(direction, edge2);
    const float determinant = XMVectorGetX(XMVector3Dot(edge1, p));
    if (std::abs(determinant) < 1e-8f || (!twoSided && determinant * orientation <= 0)) return false;
    const float inverse = 1.0f / determinant;
    const XMVECTOR delta = origin - a;
    const float u = XMVectorGetX(XMVector3Dot(delta, p)) * inverse;
    if (u < 0 || u > 1) return false;
    const XMVECTOR q = XMVector3Cross(delta, edge1);
    const float v = XMVectorGetX(XMVector3Dot(direction, q)) * inverse;
    if (v < 0 || u + v > 1) return false;
    distance = XMVectorGetX(XMVector3Dot(edge2, q)) * inverse;
    return distance >= 0;
}

scene::ActorId pick(const renderer::SceneSettings& settings, const scene::World& world,
                   const assets::AssetLibrary& assets, FXMMATRIX view, CXMMATRIX projection,
                   const assets::MaterialAsset* materialOverride) {
    const auto screenMouse = ImGui::GetMousePos();
    const auto mainOrigin = ImGui::GetMainViewport()->Pos;
    const ImVec2 mouse{screenMouse.x - mainOrigin.x, screenMouse.y - mainOrigin.y};
    const auto unproject = [&](float depth) {
        return XMVector3Unproject(XMVectorSet(mouse.x, mouse.y, depth, 1),
            static_cast<float>(settings.viewportX), static_cast<float>(settings.viewportY),
            static_cast<float>(settings.viewportWidth), static_cast<float>(settings.viewportHeight),
            0, 1, projection, view, XMMatrixIdentity());
    };
    const XMVECTOR origin = unproject(0), farPoint = unproject(1);
    const XMVECTOR direction = XMVector3Normalize(farPoint - origin);
    float nearest = XMVectorGetX(XMVector3Length(farPoint - origin));
    scene::ActorId hit = 0;
    for (const auto& actor : world.actors) {
        if (!actor.visible || !validTransform(actor.transform)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        const auto* material = assets.findMaterial(actor.mesh.materialId);
        if (!mesh || !material) continue;
        if (materialOverride && materialOverride->id == material->id) material = materialOverride;
        const XMMATRIX matrix = actorMatrix(actor.transform);
        const float orientation = XMVectorGetX(XMMatrixDeterminant(matrix)) < 0 ? -1.0f : 1.0f;
        for (std::size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            XMVECTOR vertices[3];
            bool valid = true;
            for (int j = 0; j < 3; ++j) {
                const auto index = mesh->indices[i + j];
                if (index >= mesh->vertices.size()) { valid = false; break; }
                const float* p = mesh->vertices[index].position;
                vertices[j] = XMVector3TransformCoord(XMVectorSet(p[0], p[1], p[2], 1), matrix);
            }
            float distance = 0;
            if (valid && intersectTriangle(origin, direction, vertices[0], vertices[1], vertices[2], distance,
                    material->twoSided, orientation) &&
                distance < nearest) { nearest = distance; hit = actor.id; }
        }
    }
    return hit;
}
} // namespace

bool ViewportInteraction::draw(renderer::SceneSettings& settings, scene::World& world,
                               const assets::AssetLibrary& assets, std::vector<scene::ActorId>& selection,
                               scene::ActorId& selected, renderer::SceneRenderer& renderer,
                               const std::function<void()>& duplicateForDrag,
                               const assets::MaterialAsset* materialOverride) {
    hovered_ = focused_ = false;
    pickedThisFrame_ = false;
    settings.editorViewport = true;
    settings.viewportVisible = false;
    settings.viewportWidth = settings.viewportHeight = 0;
    ImGuizmo::BeginFrame();
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowSize({640, 480}, ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible = ImGui::Begin(EditorDockManager::panelName(EditorPanel::Viewport), nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!visible) {
        usingGizmo_ = orbiting_ = panning_ = false;
        ImGui::End();
        return false;
    }
    // Rendering uses main-client/texture pixels. ImGui, mouse input and Gizmo
    // use desktop coordinates when platform viewports are enabled.
    const ImVec2 contentMin = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 mainOrigin = ImGui::GetMainViewport()->Pos;
    const ImVec2 localMin{contentMin.x - mainOrigin.x, contentMin.y - mainOrigin.y};
    const float targetWidth = static_cast<float>(renderer.sceneTextureWidth());
    const float targetHeight = static_cast<float>(renderer.sceneTextureHeight());
    const int left = static_cast<int>(std::clamp(localMin.x, 0.0f, targetWidth));
    const int top = static_cast<int>(std::clamp(localMin.y, 0.0f, targetHeight));
    const int imageRight = static_cast<int>(std::clamp(localMin.x + available.x, 0.0f, targetWidth));
    const int bottom = static_cast<int>(std::clamp(localMin.y + available.y, 0.0f, targetHeight));
    settings.viewportX = left;
    settings.viewportY = top;
    settings.viewportWidth = std::max(0, imageRight - left);
    settings.viewportHeight = std::max(0, bottom - top);
    if (settings.viewportWidth < 1 || settings.viewportHeight < 1 || !renderer.sceneTexture()) {
        usingGizmo_ = orbiting_ = panning_ = false;
        ImGui::End();
        return false;
    }
    settings.viewportVisible = true;
    const ImVec2 imageMin{mainOrigin.x + left, mainOrigin.y + top};
    const ImVec2 imageMax{mainOrigin.x + imageRight, mainOrigin.y + bottom};
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(renderer.sceneTexture())),
        imageMin, imageMax, {left / targetWidth, top / targetHeight},
        {imageRight / targetWidth, bottom / targetHeight});
    ImGui::SetCursorScreenPos({imageMin.x + 8, imageMin.y + 8});
    hovered_ = ImGui::IsWindowHovered();
    focused_ = ImGui::IsWindowFocused();
    const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImGuiIO& io = ImGui::GetIO();
    // WantTextInput is computed from the preceding frame. A click in the scene
    // can already have deactivated the text field drawn earlier this frame, so
    // use live item ownership for pointer input and retain the flag for keys.
    const bool blocked = popup || (!usingGizmo_ && ImGui::IsAnyItemActive());
    if (!blocked && !io.WantTextInput && !usingGizmo_ && (hovered_ || focused_)) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) operation_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) operation_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) operation_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) operation_ = (operation_ + 1) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusActor(settings, world, assets, selected);
    }
    ImGui::BeginDisabled(usingGizmo_);
    const char* labels[] = {"Move [W]", "Rotate [E]", "Scale [R]"};
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
    ImGui::EndDisabled();
    if (const auto* actor = world.findActor(selected))
        ImGui::TextColored({1.0f, .75f, .2f, 1.0f}, "%s%s", actor->name.c_str(), operation_ == 2 ? " | Local scale" : "");
    else ImGui::TextDisabled("Click a scene object to select");
    const float toolbarBottom = ImGui::GetCursorScreenPos().y;
    const bool overToolbar = io.MousePos.y < toolbarBottom;
    const bool canInteract = !blocked && !overToolbar;
    // Camera drags deliberately ignore ImGui item activation: clicking the
    // viewport activates the window itself, which would otherwise cancel an
    // Alt+left-drag orbit before it ever starts.
    const bool canCamera = !popup && !overToolbar && hovered_ && !usingGizmo_;

    // Right-drag orbits; Alt+left-drag is the DCC-style alternative.
    const bool altOrbiting = io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (canCamera && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) orbiting_ = true;
    // Alt+left is claimed after this frame's gizmo hit test. A handle owns
    // duplication; otherwise the same gesture starts camera orbiting.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !altOrbiting) orbiting_ = false;
    if (orbiting_ && hovered_) {
        settings.cameraYaw -= io.MouseDelta.x * .008f;
        settings.cameraPitch = std::clamp(settings.cameraPitch + io.MouseDelta.y * .008f,
            renderer::SceneSettings::minPitch, renderer::SceneSettings::maxPitch);
    }
    // Middle-mouse drag pans the orbit focus along the camera's right/up axes so
    // the camera can move freely instead of only orbiting the default target.
    if (canCamera && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle)) panning_ = false;
    if (panning_ && hovered_) {
        const float p = std::clamp(settings.cameraPitch, renderer::SceneSettings::minPitch, renderer::SceneSettings::maxPitch);
        const float d = std::clamp(settings.cameraDistance, 2.0f, 30.0f);
        const float cosYaw = std::cos(settings.cameraYaw), sinYaw = std::sin(settings.cameraYaw);
        const float cosPitch = std::cos(p), sinPitch = std::sin(p);
        // Camera basis: right is pitch independent; up leans back as pitch rises.
        const XMVECTOR right = XMVectorSet(cosYaw, 0.0f, -sinYaw, 0.0f);
        const XMVECTOR up = XMVectorSet(-sinYaw * sinPitch, cosPitch, -cosYaw * sinPitch, 0.0f);
        const float worldPerPixel = 2.0f * d * std::tan(XMConvertToRadians(25.0f)) /
            std::max(1.0f, static_cast<float>(settings.viewportHeight));
        const float dx = io.MouseDelta.x * worldPerPixel;
        const float dy = io.MouseDelta.y * worldPerPixel;
        // Grab style: the scene follows the cursor, so the camera moves opposite.
        const XMVECTOR delta = XMVectorAdd(XMVectorScale(right, -dx), XMVectorScale(up, dy));
        const XMVECTOR panTarget = XMVectorAdd(
            XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 0.0f), delta);
        settings.cameraTarget[0] = XMVectorGetX(panTarget);
        settings.cameraTarget[1] = XMVectorGetY(panTarget);
        settings.cameraTarget[2] = XMVectorGetZ(panTarget);
    }
    if (canInteract && hovered_ && !usingGizmo_ && io.MouseWheel != 0)
        settings.cameraDistance = std::clamp(settings.cameraDistance * std::pow(.88f, io.MouseWheel), 2.0f, 30.0f);

    const float pitch = std::clamp(settings.cameraPitch, renderer::SceneSettings::minPitch, renderer::SceneSettings::maxPitch);
    const float distance = std::clamp(settings.cameraDistance, 2.0f, 30.0f);
    const XMVECTOR target = XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 1.0f);
    const XMVECTOR eye = XMVectorAdd(target, XMVectorSet(
        distance * std::cos(pitch) * std::sin(settings.cameraYaw),
        distance * std::sin(pitch),
        distance * std::cos(pitch) * std::cos(settings.cameraYaw), 0.0f));
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(50),
        static_cast<float>(settings.viewportWidth) / static_cast<float>(settings.viewportHeight), .05f, 100);
    XMFLOAT4X4 viewData, projectionData;
    XMStoreFloat4x4(&viewData, view); XMStoreFloat4x4(&projectionData, projection);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(imageMin.x, imageMin.y,
        static_cast<float>(settings.viewportWidth), static_cast<float>(settings.viewportHeight));
    ImGuizmo::SetGizmoSizeClipSpace(.18f);
    ImGuizmo::Enable(!blocked && (hovered_ || usingGizmo_) && !orbiting_ && !panning_ && (!overToolbar || usingGizmo_));
    bool changed = false, gizmoHovered = false;
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
        const auto hit = pick(settings, world, assets, view, projection, materialOverride);
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
    ImGui::End();
    return changed;
}

} // namespace rae::editor
