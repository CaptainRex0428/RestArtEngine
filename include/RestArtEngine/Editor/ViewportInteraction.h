#pragma once

#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <RestArtEngine/Assets/AssetRegistry.h>
#include <RestArtEngine/Assets/MeshQuery.h>
#include <vector>
#include <functional>

namespace rae::editor {

// Viewport-only input and transform tools; runtime Actors never depend on ImGui.
class ViewportInteraction {
public:
    // selection is the full editor selection; selected is its primary Actor
    // (the one the gizmo pivots on). Picking replaces the selection.
    // duplicateForDrag copies that selection in place and selects its copies;
    // the editor owns creation/undo, while this class owns gesture arbitration.
    // A content drop stays a transient bounds preview until delivery; the
    // editor creates and records the final instance through createFromMesh.
    // keyboardBlocked is this frame's external tool keyboard ownership. Mouse
    // drags keep their existing camera / gizmo / asset ownership independently.
    // A minimized main surface does not hide an independent scene window.
    bool draw(renderer::SceneSettings&, scene::World&, const assets::AssetLibrary&,
              std::vector<scene::ActorId>& selection, scene::ActorId& selected,
              renderer::SceneRenderer& renderer, const std::function<void()>& duplicateForDrag,
              const assets::AssetRegistry& registry,
              const std::function<void(assets::AssetId, const scene::Transform&)>& createFromMesh,
              const std::function<void(scene::ActorId, assets::AssetId)>& assignMaterial,
              const assets::MaterialAsset* materialOverride = nullptr,
              bool keyboardBlocked = false, bool mainSurfaceVisible = true);
    bool isUsing() const { return usingGizmo_; }
    bool isHovered() const { return hovered_; }
    bool isFocused() const { return focused_; }
    // True only for this draw's actual scene-picking path, including reselect.
    bool pickedThisFrame() const { return pickedThisFrame_; }
    // Transient drag feedback, independent of the editor's actual selection.
    bool materialDragging() const { return materialDragging_; }
    scene::ActorId materialDropTarget() const { return materialDropTarget_; }
private:
    assets::MeshQueryCache meshQueries_;
    int operation_ = 0; // translate, rotate, scale
    bool local_ = false, snap_ = false;
    bool usingGizmo_ = false, hovered_ = false, focused_ = false, orbiting_ = false, panning_ = false;
    bool duplicateOnDrag_ = false;
    bool pickedThisFrame_ = false;
    bool materialDragging_ = false;
    scene::ActorId materialDropTarget_ = 0;
};

} // namespace rae::editor
