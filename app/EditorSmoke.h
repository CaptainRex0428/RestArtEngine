#pragma once
#include <RAEngine/Editor/SceneEditor.h>
#include <RAEngine/ImGui/ImGuiLayer.h>

// Exercises the real renderer without touching the user's Content or shader files.
void runEditorSmoke(rae::renderer::SceneRenderer&, rae::imgui::ImGuiLayer&,
    rae::editor::SceneEditor&, const rae::project::ProjectPaths& project,
    const std::filesystem::path& outputDirectory);
