#pragma once

#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <functional>

// Exercises native WM_DROPFILES through the real editor in its isolated project.
void runImportSmoke(rae::editor::SceneEditor&, rae::imgui::ImGuiLayer&, rae::renderer::SceneRenderer&,
    const std::filesystem::path& output,
    const std::function<void(const char*)>& frame,
    const std::function<void(bool)>& setImportOption);
