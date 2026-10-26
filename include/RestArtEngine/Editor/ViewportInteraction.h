#pragma once

#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <vector>

namespace rae::editor {

// Viewport-only input and transform tools; runtime Actors never depend on ImGui.
class ViewportInteraction {
public:
    // selection is the full editor selection; selected is its primary Actor
    // (the one the gizmo pivots on). Picking replaces the selection.
    bool draw(renderer::SceneSettings&, scene::World&, const assets::AssetLibrary&,
              std::vector<scene::ActorId>& selection, scene::ActorId& selected,
              renderer::SceneRenderer& renderer,
              const assets::MaterialAsset* materialOverride = nullptr);
    bool isUsing() const { return usingGizmo_; }
    bool isHovered() const { return hovered_; }
    bool isFocused() const { return focused_; }
private:
    int operation_ = 0; // translate, rotate, scale
    bool local_ = false, snap_ = false;
    bool usingGizmo_ = false, hovered_ = false, focused_ = false, orbiting_ = false, panning_ = false;
};

} // namespace rae::editor
