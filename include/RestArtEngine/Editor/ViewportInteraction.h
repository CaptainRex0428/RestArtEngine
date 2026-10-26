#pragma once

#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
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
    bool draw(renderer::SceneSettings&, scene::World&, const assets::AssetLibrary&,
              std::vector<scene::ActorId>& selection, scene::ActorId& selected,
              renderer::SceneRenderer& renderer, const std::function<void()>& duplicateForDrag,
              const assets::MaterialAsset* materialOverride = nullptr);
    bool isUsing() const { return usingGizmo_; }
    bool isHovered() const { return hovered_; }
    bool isFocused() const { return focused_; }
    // True only for this draw's actual scene-picking path, including reselect.
    bool pickedThisFrame() const { return pickedThisFrame_; }
private:
    int operation_ = 0; // translate, rotate, scale
    bool local_ = false, snap_ = false;
    bool usingGizmo_ = false, hovered_ = false, focused_ = false, orbiting_ = false, panning_ = false;
    bool duplicateOnDrag_ = false;
    bool pickedThisFrame_ = false;
};

} // namespace rae::editor
