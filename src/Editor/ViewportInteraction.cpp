#include <RestArtEngine/Editor/ViewportInteraction.h>
#include <RestArtEngine/Editor/EditorDockManager.h>
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

struct ViewRay {
    XMVECTOR origin;
    XMVECTOR direction;
    float length;
};

struct SurfaceHit {
    scene::ActorId actor = 0;
    XMFLOAT3 point{};
    XMFLOAT3 normal{};
    float distance = 0;
};

ViewRay viewRay(const renderer::SceneSettings& settings, FXMMATRIX view, CXMMATRIX projection) {
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
    return {origin, XMVector3Normalize(farPoint - origin), XMVectorGetX(XMVector3Length(farPoint - origin))};
}

SurfaceHit closestSurface(const ViewRay& ray, const scene::World& world, const assets::AssetLibrary& assets,
                          const assets::MaterialAsset* materialOverride, assets::MeshQueryCache& queries) {
    SurfaceHit hit;
    hit.distance = ray.length;
    for (const auto& actor : world.actors) {
        if (!actor.visible || !validTransform(actor.transform)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        const auto* material = assets.findMaterial(actor.mesh.materialId);
        if (!mesh || !material) continue;
        if (materialOverride && materialOverride->id == material->id) material = materialOverride;
        // Never cannot contribute a visible surface. Other custom depth modes
        // retain geometric nearest-hit picking; this CPU query does not replay
        // the raster submission order or depth-buffer contents.
        if (material->depthTest == assets::MaterialDepthTest::Never) continue;
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
        const auto cullMode = material->effectiveCullMode();
        if (!queries.intersect(*mesh, localRay, hit.distance, cullMode == assets::MaterialCullMode::Off,
                1e-8f / std::abs(determinant), meshHit, cullMode == assets::MaterialCullMode::Front)) continue;
        hit.actor = actor.id;
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
                     const XMFLOAT3& maximum, const ImVec2& clipMin, const ImVec2& clipMax) {
    const auto desktopOrigin = ImGui::GetMainViewport()->Pos;
    const auto project = [&](FXMVECTOR point, ImVec2& screen) {
        const auto clip = XMVector4Transform(XMVectorSetW(point, 1), view * projection);
        if (XMVectorGetW(clip) <= 1e-5f) return false;
        const auto pixel = XMVector3Project(point, static_cast<float>(settings.viewportX),
            static_cast<float>(settings.viewportY), static_cast<float>(settings.viewportWidth),
            static_cast<float>(settings.viewportHeight), 0, 1, projection, view, XMMatrixIdentity());
        screen = {desktopOrigin.x + XMVectorGetX(pixel), desktopOrigin.y + XMVectorGetY(pixel)};
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

bool ViewportInteraction::draw(renderer::SceneSettings& settings, scene::World& world,
                               const assets::AssetLibrary& assets, std::vector<scene::ActorId>& selection,
                               scene::ActorId& selected, renderer::SceneRenderer& renderer,
                               const std::function<void()>& duplicateForDrag,
                               const assets::AssetRegistry& registry,
                               const std::function<void(assets::AssetId, const scene::Transform&)>& createFromMesh,
                               const std::function<void(scene::ActorId, assets::AssetId)>& assignMaterial,
                               const assets::MaterialAsset* materialOverride, bool keyboardBlocked) {
    meshQueries_.retain(assets);
    hovered_ = focused_ = false;
    pickedThisFrame_ = false;
    materialDragging_ = false;
    materialDropTarget_ = 0;
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
    const auto* activePayload = ImGui::GetDragDropPayload();
    const bool assetDragging = activePayload && activePayload->IsDataType("RAE_ASSET");
    const auto* activeAsset = assetDragging ? droppedAsset(*activePayload, registry) : nullptr;
    materialDragging_ = activeAsset && activeAsset->type == assets::AssetType::Material;
    ImGuiIO& io = ImGui::GetIO();
    // WantTextInput is computed from the preceding frame. A click in the scene
    // can already have deactivated the text field drawn earlier this frame, so
    // use live item ownership for pointer input and retain the flag for keys.
    const bool blocked = popup || assetDragging || (!usingGizmo_ && ImGui::IsAnyItemActive());
    if (!keyboardBlocked && !blocked && !io.WantTextInput && !usingGizmo_ && (hovered_ || focused_)) {
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) operation_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) operation_ = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) operation_ = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) operation_ = (operation_ + 1) % 3;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) focusActor(settings, world, assets, selected);
    }
    ImGui::BeginDisabled(usingGizmo_ || assetDragging);
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
    const bool canCamera = !popup && !assetDragging && !overToolbar && hovered_ && !usingGizmo_;
    if (assetDragging) orbiting_ = panning_ = false;

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
    // The native negative-axis hatching is an optional, persisted viewport aid.
    ImGuizmo::GetStyle().HatchedAxisLineThickness = imgui::uiPreferences().gizmoNegativeAxisHints ? 6.0f : 0.0f;
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
        const auto hit = closestSurface(viewRay(settings, view, projection), world, assets, materialOverride, meshQueries_).actor;
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
    if (assetDragging && !popup && !usingGizmo_ &&
        ImGui::GetCurrentContext()->HoveredWindowUnderMovingWindow == ImGui::GetCurrentWindow() &&
        dropArea.GetWidth() > 0 && dropArea.GetHeight() > 0 &&
        ImGui::BeginDragDropTargetCustom(dropArea, ImGui::GetID("##scene-asset-drop"))) {
        const auto* mesh = activeAsset && activeAsset->type == assets::AssetType::Mesh ? assets.findMesh(activeAsset->id) : nullptr;
        const auto* material = materialDragging_ ? assets.findMaterial(activeAsset->id) : nullptr;
        XMFLOAT3 minimum, maximum;
        scene::Transform placement;
        XMVECTOR contact;
        const auto ray = viewRay(settings, view, projection);
        if (material && assignMaterial) {
            const auto hit = closestSurface(ray, world, assets, materialOverride, meshQueries_);
            if (const auto* targetActor = world.findActor(hit.actor)) {
                if (const auto* accepted = ImGui::AcceptDragDropPayload("RAE_ASSET",
                        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    materialDropTarget_ = targetActor->id;
                    const auto hint = "Assign " + material->name + " to " + targetActor->name;
                    drawDropHint(dropArea.Min, dropArea.Max, hint.c_str(), true);
                    if (accepted->IsDelivery()) assignMaterial(targetActor->id, material->id);
                }
            } else drawDropHint(dropArea.Min, dropArea.Max, "Point at an Actor to assign this material", false);
        } else if (!mesh || !createFromMesh) {
            drawDropHint(dropArea.Min, dropArea.Max, "Drop a mesh to place it, or a material onto an Actor", false);
        } else if (!meshBounds(*mesh, minimum, maximum)) {
            drawDropHint(dropArea.Min, dropArea.Max, "This mesh has no usable geometry", false);
        } else if (!dropTransform(ray, closestSurface(ray, world, assets, materialOverride, meshQueries_), minimum, maximum, placement, contact)) {
            drawDropHint(dropArea.Min, dropArea.Max, "No placement surface here. Aim at visible geometry or the ground", false);
        } else {
            if (const auto* accepted = ImGui::AcceptDragDropPayload("RAE_ASSET",
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                drawDropPreview(settings, view, projection, placement, contact, minimum, maximum, dropArea.Min, dropArea.Max);
                if (accepted->IsDelivery()) createFromMesh(mesh->id, placement);
            }
        }
        ImGui::EndDragDropTarget();
    }
    drawGeometryStatistics(renderer.geometryStatistics(), dropArea.Min, dropArea.Max, assetDragging);
    ImGui::End();
    return changed;
}

} // namespace rae::editor
