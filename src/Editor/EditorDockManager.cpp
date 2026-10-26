#include <RestArtEngine/Editor/EditorDockManager.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>

namespace rae::editor {

const std::array<EditorDockManager::Panel, static_cast<unsigned>(EditorPanel::Count)>
EditorDockManager::panels_ = {{
    {"World Outliner", Slot::LeftTop},
    {"Scene", Slot::LeftBottom},
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
    if (auto* node = ImGui::DockBuilderGetNode(left))
        node->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr(panelName(EditorPanel::Scene)));
    requestFocus(EditorPanel::Scene);
}

void EditorDockManager::migrateScenePanel() {
    const auto sceneId = ImHashStr(panelName(EditorPanel::Scene));
    if (ImGui::FindWindowByID(sceneId) || ImGui::FindWindowSettingsByID(sceneId)) return;

    // The original Actor inspector used the Details identity. Reuse that
    // placement for Scene without replacing any of the user's split nodes.
    const auto detailsId = ImHashStr(panelName(EditorPanel::Details));
    const auto* details = ImGui::FindWindowByID(detailsId);
    const auto* saved = ImGui::FindWindowSettingsByID(detailsId);
    const ImGuiID existingDock = details ? details->DockId : (saved ? saved->DockId : 0);
    ImGuiID target = existingDock;
    if (!target || !ImGui::DockBuilderGetNode(target)) {
        if (!details && !saved) return;
        ImVec2 position, size;
        if (details) {
            position = details->Pos;
            size = details->SizeFull;
        } else {
            ImVec2 origin = ImGui::GetMainViewport()->Pos;
            if (saved->ViewportId)
                origin = {static_cast<float>(saved->ViewportPos.x), static_cast<float>(saved->ViewportPos.y)};
            position = {origin.x + saved->Pos.x, origin.y + saved->Pos.y};
            size = {static_cast<float>(saved->Size.x), static_cast<float>(saved->Size.y)};
        }
        const auto* viewport = ImGui::GetMainViewport();
        if (size.x < 1 || size.y < 1) {
            position = {viewport->WorkPos.x + 20, viewport->WorkPos.y + 20};
            size = {std::min(360.0f, viewport->WorkSize.x), std::min(480.0f, viewport->WorkSize.y)};
        }
        target = ImGui::DockBuilderAddNode(0);
        ImGui::DockBuilderSetNodePos(target, position);
        ImGui::DockBuilderSetNodeSize(target, size);
        ImGui::DockBuilderDockWindow(panelName(EditorPanel::Details), target);
    }
    ImGui::DockBuilderDockWindow(panelName(EditorPanel::Scene), target);
    auto* root = ImGui::DockBuilderGetNode(target);
    while (root && root->ParentNode) root = root->ParentNode;
    if (root) ImGui::DockBuilderFinish(root->ID);
    if (auto* node = ImGui::DockBuilderGetNode(target)) {
        node->SelectedTabId = ImHashStr("#TAB", 0, sceneId);
        if (node->TabBar) node->TabBar->NextSelectedTabId = node->SelectedTabId;
    }
    ImGui::MarkIniSettingsDirty();
    requestFocus(EditorPanel::Scene);
}

void EditorDockManager::applyFocusRequest() {
    if (focusRequested_ == EditorPanel::Count) return;
    auto* window = ImGui::FindWindowByName(panelName(focusRequested_));
    if (!window) return; // Newly registered windows are available on their next frame.
    if (auto* node = window->DockNode) {
        node->SelectedTabId = window->TabId;
        if (node->TabBar) node->TabBar->NextSelectedTabId = window->TabId;
    }
    ImGui::SetWindowFocus(window->Name);
    focusRequested_ = EditorPanel::Count;
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
    // Preserve the existing root and user split geometry when adding panels.
    const ImGuiID dockId = ImHashStr("RestArtEngine.DockSpace.v1");
    if (resetRequested_ || !ImGui::DockBuilderGetNode(dockId)) {
        buildDefault(dockId, size.x, size.y);
        resetRequested_ = false;
    } else {
        migrateScenePanel();
    }
    applyFocusRequest();
    ImGui::DockSpace(dockId, {0, 0});
    ImGui::End();
}

void EditorDockManager::keepAlive() {
    const ImGuiID dockId = ImHashStr("RestArtEngine.DockSpace.v1");
    const auto* node = ImGui::DockBuilderGetNode(dockId);
    if (node && node->LastFrameAlive != ImGui::GetFrameCount())
        ImGui::DockSpace(dockId, {0, 0}, ImGuiDockNodeFlags_KeepAliveOnly);
}

void EditorDockManager::drawPanel(EditorPanel panel, const std::function<void()>& contents) {
    // Placement is owned by DockSpace and floating window settings. Never set
    // positions every frame: that would prevent undocking and native reflow.
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
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
        if (ImGui::MenuItem(label))
            requestFocus(static_cast<EditorPanel>(&panel - panels_.data()));
    }
    ImGui::EndPopup();
}

} // namespace rae::editor
