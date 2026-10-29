// Included inside rae::editor; the editor owns all view interaction state.
namespace {
void copyViewportCamera(renderer::SceneSettings& target, const renderer::SceneSettings& source) {
    target.cameraYaw = source.cameraYaw; target.cameraPitch = source.cameraPitch; target.cameraDistance = source.cameraDistance;
    std::copy_n(source.cameraTarget, 3, target.cameraTarget);
    target.cameraLens = source.cameraLens; target.cameraPose = source.cameraPose; target.exposure = source.exposure;
}
}

void SceneEditor::resetViewportBindings() {
    viewport_.stopCameraNavigation(world_);
    if (cameraSettings_) viewport_.returnToFreeCamera(*cameraSettings_);
    for (int index = 0; index < 4; ++index) {
        otherViewports_[index].stopCameraNavigation(world_);
        otherViewports_[index].returnToFreeCamera(viewportSettings_[index]);
    }
    pendingActiveViewport_ = -1;
}

void SceneEditor::switchActiveViewport(renderer::SceneSettings& settings, int index) {
    if (index == activeViewport_ || index < 0 || index > 3 || viewport_.isUsing() || viewport_.isCameraNavigating()) return;
    copyViewportCamera(viewportSettings_[activeViewport_], settings);
    std::swap(viewport_, otherViewports_[activeViewport_]);
    activeViewport_ = index;
    std::swap(viewport_, otherViewports_[activeViewport_]);
    copyViewportCamera(settings, viewportSettings_[activeViewport_]);
    settings.viewId = static_cast<std::uint64_t>(activeViewport_ + 1);
}

bool SceneEditor::drawViewports(renderer::SceneSettings& settings, renderer::SceneRenderer& renderer, bool mainSurfaceVisible) {
    if (!viewportsInitialized_) {
        viewportsInitialized_ = true;
        for (auto& view : viewportSettings_) copyViewportCamera(view, settings);
        const float rotations[3][2] = {{-90, 0}, {0, 0}, {0, 90}};
        for (int index = 1; index < 4; ++index) {
            auto& destination = viewportSettings_[index];
            auto camera = renderer::resolveCamera(settings);
            camera.pose.rotation[0] = rotations[index - 1][0]; camera.pose.rotation[1] = rotations[index - 1][1]; camera.pose.rotation[2] = 0;
            camera.lens.projection = camera::Projection::Orthographic;
            camera.lens.aspectRatio = 0;
            const auto basis = camera::evaluate(camera, 1);
            for (int axis = 0; axis < 3; ++axis) camera.pose.position[axis] = settings.cameraTarget[axis] - basis.forward[axis] * settings.cameraDistance;
            destination.cameraPose = camera.pose; destination.cameraLens = camera.lens;
        }
    }
    if (pendingQuadView_ >= 0 && !viewport_.isUsing() && !viewport_.isCameraNavigating()) {
        quadView_ = pendingQuadView_ != 0; pendingQuadView_ = -1; viewportMaximized_ = false;
    }
    if (pendingMaximized_ >= 0 && !viewport_.isUsing() && !viewport_.isCameraNavigating()) {
        viewportMaximized_ = pendingMaximized_ != 0; pendingMaximized_ = -1;
    }
    if (pendingActiveViewport_ >= 0 && !viewport_.isUsing() && !viewport_.isCameraNavigating()) {
        switchActiveViewport(settings, pendingActiveViewport_); pendingActiveViewport_ = -1;
    }
    settings.viewId = static_cast<std::uint64_t>(activeViewport_ + 1);
    const bool externalKeyboard = diagnosticsPanel_.isFocused() || projectSettingsFocused_ || importResultsFocused_ ||
        consoleInputActive_ || outputLogFocused_ || cameraPreviewFocused_;
    const auto drawOne = [&](ViewportInteraction& interaction, renderer::SceneSettings& view, const ViewportInteraction::Surface* surface) {
        interaction.onCameraEdited = [this](scene::ActorId id, const scene::Transform& before, const camera::Lens& beforeLens,
            const scene::Transform& after, const camera::Lens& afterLens) {
            pushCameraEdit(id, before, beforeLens, after, afterLens, "Pilot camera");
        };
        return interaction.draw(view, world_, assets_, selection_, selectedActor_, renderer,
            [this] { duplicateForDrag(); }, registry_,
            [this](assets::AssetId mesh, const scene::Transform& transform) { createMeshActor(mesh, transform, "Place Mesh"); },
            [this](scene::ActorId actor, assets::AssetId material, std::size_t slot) { assignActorMaterial(actor, material, slot); },
            materialPreview(), externalKeyboard || (surface && !surface->active), mainSurfaceVisible, surface);
    };
    if (!quadView_) {
        const bool changed = drawOne(viewport_, settings, nullptr);
        copyViewportCamera(viewportSettings_[activeViewport_], settings);
        return changed;
    }
    ImGuizmo::BeginFrame();
    ImGui::SetNextWindowSize({640, 480}, ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    const bool visible = ImGui::Begin(EditorDockManager::panelName(EditorPanel::Viewport), nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    settings.editorViewport = true; settings.viewportVisible = false;
    if (!visible) { viewport_.stopCameraNavigation(world_); ImGui::End(); return false; }
    ImGui::BeginDisabled(viewport_.isUsing() || viewport_.isCameraNavigating());
    if (ImGui::SmallButton("Single view")) setQuadView(false);
    ImGui::SameLine();
    if (ImGui::SmallButton(viewportMaximized_ ? "Restore four views" : "Maximize active view")) setViewportMaximized(!viewportMaximized_);
    ImGui::SetNextItemWidth(120);
    int nextActive = activeViewport_;
    if (ImGui::Combo("##ActiveView", &nextActive, "View 1\0View 2\0View 3\0View 4\0")) setActiveViewport(nextActive);
    ImGui::EndDisabled();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const auto available = ImGui::GetContentRegionAvail();
    constexpr float gap = 3;
    const bool showFour = !viewportMaximized_ && available.x >= 440 && available.y >= 360;
    const ImVec2 cellSize = showFour ? ImVec2((available.x - gap) * .5f, (available.y - gap) * .5f) : available;
    if (cellSize.x < 1 || cellSize.y < 1) { ImGui::End(); return false; }
    std::string error;
    if (!renderer.ensureSceneTargetSize(std::max(256u, static_cast<unsigned>(std::ceil(cellSize.x))),
        std::max(256u, static_cast<unsigned>(std::ceil(cellSize.y))), error)) {
        ImGui::TextWrapped("Viewport unavailable: %s", error.c_str()); ImGui::End(); return false;
    }
    std::array<ImVec2, 4> cellOrigins;
    for (int index = 0; index < 4; ++index)
        cellOrigins[index] = showFour ? ImVec2(origin.x + (index % 2) * (cellSize.x + gap), origin.y + (index / 2) * (cellSize.y + gap)) : origin;
    const auto* payload = ImGui::GetDragDropPayload();
    const bool assetDrag = payload && payload->IsDataType("RAE_ASSET");
    if (showFour && !viewport_.isUsing() && !viewport_.isCameraNavigating() &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        for (int index = 0; index < 4; ++index) {
            const auto pos = cellOrigins[index];
            if (ImGui::IsMouseHoveringRect(pos, {pos.x + cellSize.x, pos.y + cellSize.y}) &&
                (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2) || ImGui::GetIO().MouseWheel != 0 || assetDrag))
                switchActiveViewport(settings, index);
        }
    }
    const char* names[] = {"Perspective##Viewport0", "Top##Viewport1", "Front##Viewport2", "Right##Viewport3"};
    bool changed = false;
    // Inactive images finish first; the active view owns the final submission.
    for (int pass = 0; pass < 4; ++pass) {
        const int index = pass == 3 ? activeViewport_ : (activeViewport_ + pass + 1) % 4;
        if (!showFour && index != activeViewport_) continue;
        auto view = settings;
        if (index != activeViewport_) copyViewportCamera(view, viewportSettings_[index]);
        view.viewId = static_cast<std::uint64_t>(index + 1);
        view.outlineActors = selection_.empty() ? nullptr : selection_.data();
        view.outlineActorCount = static_cast<std::uint32_t>(selection_.size());
        ViewportInteraction::Surface surface{names[index], cellSize.x, cellSize.y, index, index == activeViewport_,
            static_cast<renderer::PreviewSlot>(static_cast<int>(renderer::PreviewSlot::Viewport0) + index)};
        ImGui::SetCursorScreenPos(cellOrigins[index]);
        auto& interaction = index == activeViewport_ ? viewport_ : otherViewports_[index];
        changed |= drawOne(interaction, view, &surface);
        copyViewportCamera(viewportSettings_[index], view);
        if (index == activeViewport_) settings = view;
        const auto pos = cellOrigins[index];
        ImGui::GetWindowDrawList()->AddRect(pos, {pos.x + cellSize.x, pos.y + cellSize.y},
            ImGui::GetColorU32(index == activeViewport_ ? ImGuiCol_PlotHistogram : ImGuiCol_Border), 0, 0, 1.0f);
    }
    ImGui::End();
    return changed;
}
