#pragma once

#include <RAEngine/Renderer/SceneRenderer.h>
#include <RAEngine/Scene/World.h>

namespace rae::editor {

// Viewport-only input and transform tools; runtime Actors never depend on ImGui.
class ViewportInteraction {
public:
    bool draw(renderer::SceneSettings&, scene::World&, const assets::AssetLibrary&,
              scene::ActorId& selected);
    bool isUsing() const { return usingGizmo_; }
    bool isHovered() const { return hovered_; }
    bool isFocused() const { return focused_; }
private:
    int operation_ = 0; // translate, rotate, scale
    bool local_ = false, snap_ = false;
    bool usingGizmo_ = false, hovered_ = false, focused_ = false, orbiting_ = false, panning_ = false;
};

} // namespace rae::editor
