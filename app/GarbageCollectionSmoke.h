#pragma once

#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>

// GPU/cache reconstruction and real ImGui control checks. Uses isolated captures/settings.
void runGarbageCollectionSmoke(rae::renderer::SceneRenderer&, rae::imgui::ImGuiLayer&,
    rae::editor::SceneEditor&, const std::filesystem::path& outputDirectory);
