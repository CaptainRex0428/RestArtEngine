#include <RestArtEngine/Editor/EditorDockManager.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace rae::editor {

const std::array<EditorDockManager::Panel, static_cast<unsigned>(EditorPanel::Count)>
EditorDockManager::panels_ = {{
    {"World Outliner", Slot::LeftTop},
    {"Details", Slot::LeftBottom},
    {"Viewport settings", Slot::Right},
    {"Content & Editors", Slot::Bottom},
    {"Scene Viewport##Interaction", Slot::Center},
}};

const char* EditorDockManager::panelName(EditorPanel panel) {
    return panels_[static_cast<unsigned>(panel)].name;
}

void EditorDockManager::buildDefault(unsigned dockId, float width, float height) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, {width, height});
    ImGuiID center = dockId;
    const float side = std::min(width * .28f, width < 1200 ? 230.0f : 280.0f);
    const float bottomHeight = std::min(height * .45f, std::clamp((height + 90) * .34f, 220.0f, 360.0f));
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, bottomHeight / height, nullptr, &center);
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, side / width, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, side / (width - side), nullptr, &center);
    const ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, .42f, nullptr, &left);
    for (const auto& panel : panels_) {
        ImGuiID target = center;
        switch (panel.slot) {
        case Slot::LeftTop: target = leftTop; break;
        case Slot::LeftBottom: target = left; break;
        case Slot::Right: target = right; break;
        case Slot::Bottom: target = bottom; break;
        case Slot::Center: break;
        }
        ImGui::DockBuilderDockWindow(panel.name, target);
    }
    ImGui::DockBuilderFinish(dockId);
}

void EditorDockManager::beginFrame(float toolbarHeight) {
    const auto* viewport = ImGui::GetMainViewport();
    const ImVec2 position{viewport->Pos.x, viewport->Pos.y + toolbarHeight};
    const ImVec2 size{std::max(1.0f, viewport->Size.x), std::max(1.0f, viewport->Size.y - toolbarHeight)};
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("Editor workspace", nullptr, flags);
    ImGui::PopStyleVar(3);
    // A new root ID migrates old free-window layouts once. Later starts retain
    // native docking state; the tree is only rebuilt by the explicit reset.
    const ImGuiID dockId = ImHashStr("RestArtEngine.DockSpace.v1");
    if (resetRequested_ || !ImGui::DockBuilderGetNode(dockId)) {
        buildDefault(dockId, size.x, size.y);
        resetRequested_ = false;
    }
    ImGui::DockSpace(dockId, {0, 0});
    ImGui::End();
}

void EditorDockManager::drawPanel(EditorPanel panel, const std::function<void()>& contents) {
    // Placement is owned by DockSpace and floating window settings. Never set
    // positions every frame: that would prevent undocking and native reflow.
    if (ImGui::Begin(panelName(panel), nullptr, ImGuiWindowFlags_NoCollapse)) contents();
    ImGui::End();
}

void EditorDockManager::drawLayoutMenu() {
    if (ImGui::SmallButton("Layout")) ImGui::OpenPopup("Editor layout");
    if (!ImGui::BeginPopup("Editor layout")) return;
    ImGui::TextDisabled("Drag tabs to split, merge, or float panels");
    ImGui::Separator();
    if (ImGui::MenuItem("Restore default layout")) resetLayout();
    ImGui::Separator();
    for (const auto& panel : panels_) {
        const char* label = panel.slot == Slot::Center ? "Scene Viewport" : panel.name;
        if (ImGui::MenuItem(label)) ImGui::SetWindowFocus(panel.name);
    }
    ImGui::EndPopup();
}

} // namespace rae::editor
