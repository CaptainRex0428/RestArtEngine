#pragma once
#include <filesystem>
#include <string>

namespace rae::renderer { class SceneRenderer; }
namespace rae::imgui { class ImGuiLayer; }

// Requires an initialized native D3D12 renderer. Uses temporary in-memory
// assets and public GPU readback APIs; never saves or edits project assets.
bool runNativeD3D12Smoke(rae::renderer::SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, const std::filesystem::path& artifactDirectory, std::string& error);
