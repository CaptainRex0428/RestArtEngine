#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rae::editor {
namespace {
int resizeText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto& text = *static_cast<std::string*>(data->UserData);
        text.resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text.data();
    }
    return 0;
}
bool textInput(const char* label, std::string& text) {
    return ImGui::InputText(label, text.data(), text.capacity() + 1,
        ImGuiInputTextFlags_CallbackResize, resizeText, &text);
}
const char* meshType(assets::MeshPrimitive type) {
    switch (type) {
    case assets::MeshPrimitive::Sphere: return "Sphere";
    case assets::MeshPrimitive::Plane: return "Plane";
    case assets::MeshPrimitive::Cube: return "Cube";
    default: return "Imported OBJ";
    }
}

namespace {

std::filesystem::path pickSceneFile(bool save, const std::filesystem::path& folder, const wchar_t* title) {
    wchar_t path[32768]{};
    if (!folder.empty()) {
        const auto text = folder.wstring();
        wcsncpy_s(path, text.c_str(), std::size(path) - 2);
    }
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"RestArt scene (*.rascene)\0*.rascene\0All files (*.*)\0*.*\0\0";
    dialog.lpstrTitle = title;
    dialog.lpstrDefExt = L"rascene";
    dialog.Flags = OFN_NOCHANGEDIR |
        (save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    const BOOL accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    return accepted ? std::filesystem::path(path) : std::filesystem::path();
}

} // namespace
void message(const std::string& text) {
    if (!text.empty()) ImGui::TextWrapped("%s", text.c_str());
}
bool validMaterialParameters(const assets::MaterialAsset& material) {
    const auto unitValue = [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; };
    return unitValue(material.roughness) && unitValue(material.metallic) &&
        std::all_of(std::begin(material.baseColor), std::end(material.baseColor), unitValue);
}
void continueButtonRow(const char* nextLabel) {
    const auto& style = ImGui::GetStyle();
    const float nextWidth = ImGui::CalcTextSize(nextLabel).x + 2 * style.FramePadding.x;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextWidth <= right)
        ImGui::SameLine();
}
std::string pathText(const std::filesystem::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

}

project::ProjectPaths resolveProject(const std::filesystem::path& explicitProject,
                                     const std::filesystem::path& engineRootHint) {
    project::ProjectPaths result;
    std::string error;
    // A moved project keeps a stale (or absent) engine link. Try the recorded
    // link first, then rebind to the running engine or a registered one so the
    // project opens on another machine without going through the manager.
    const auto open = [&engineRootHint, &result](const std::filesystem::path& path, std::string& openError) {
        if (project::loadProject(path, result, openError)) return true;
        std::string recoveryError;
        if (project::recoverProject(path, engineRootHint, result, recoveryError)) return true;
        return false;
    };
    if (!explicitProject.empty()) {
        if (!open(explicitProject, error)) throw std::runtime_error(error);
        return result;
    }
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    for (auto start : {std::filesystem::current_path(), std::filesystem::path(executable).parent_path()}) {
        for (auto path = start; !path.empty(); path = path.parent_path()) {
            const auto candidate = path / "Projects/Sandbox/Sandbox.raproject";
            if (std::filesystem::is_regular_file(candidate)) {
                if (!open(candidate, error)) throw std::runtime_error(error);
                return result;
            }
            if (path.parent_path() == path) break;
        }
    }
    throw std::runtime_error("Cannot locate a project. Use --project <folder or .raproject file>.");
}
bool SceneEditor::initialize(const project::ProjectPaths& project, renderer::SceneRenderer& renderer, std::string& error) {
    project_ = project;
    assets_.createDefaults();
    // Materials are the single source of truth for scene v2 references: load the
    // content folder first so the scene can resolve its GUIDs.
    {
        std::string materialError;
        const auto materialFolder = project_.contentRoot / "Materials";
        if (std::filesystem::is_directory(materialFolder)) (void)assets_.loadMaterials(materialFolder, materialError);
    }
    world_.createDefaultScene(assets_);
    scenePath_ = project_.defaultScene;
    if (std::filesystem::exists(scenePath_) && !world_.load(scenePath_, assets_, error)) return false;
    registry_.setContentRoot(project_.contentRoot);
    registry_.scan(contentError_);
    selectedActor_ = world_.actors.empty() ? 0 : world_.actors.front().id;
    sceneInspectorActor_ = selectedActor_;
    selection_.assign(selectedActor_ ? 1 : 0, selectedActor_);
    selectedMaterial_ = assets_.materials.empty() ? 0 : assets_.materials.front().id;
    selectedMesh_ = assets_.meshes.empty() ? 0 : assets_.meshes.front().id;
    if (auto* actor = world_.findActor(selectedActor_)) selectedMaterial_ = actor->mesh.materialId;
    if (const auto* material = assets_.findMaterial(selectedMaterial_)) materialDraft_ = *material;
    shaderDraft_ = shaderSaved_ = renderer.shaderSource();
    shaderDraft_.reserve(std::max<std::size_t>(65536, shaderDraft_.size() * 2));
    status_ = "Project loaded. Actor edits and material parameters preview live; Shader edits require Apply.";
    registerCommands();
    return true;
}

void SceneEditor::registerCommands() {
    // Single place that defines every editor action: the Edit menu, the panel
    // buttons and the keyboard all resolve through these ids.
    commands_.add("edit.undo", "edit", "Undo", "Ctrl+Z", ImGuiMod_Ctrl | ImGuiKey_Z,
        [this] { undo(); }, [this] { return undo_.canUndo(); });
    commands_.add("edit.redo", "edit", "Redo", "Ctrl+Y", ImGuiMod_Ctrl | ImGuiKey_Y,
        [this] { redo(); }, [this] { return undo_.canRedo(); });
    commands_.add("edit.redoAlt", "hidden", "Redo (Shift)", "Ctrl+Shift+Z",
        ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, [this] { redo(); },
        [this] { return undo_.canRedo(); });
    commands_.add("actor.duplicate", "edit", "Duplicate Actor", "Ctrl+D", ImGuiMod_Ctrl | ImGuiKey_D,
        [this] { duplicateSelectedActor(); },
        [this] { return selectedActor_ != 0 && !viewport_.isUsing(); });
    commands_.add("actor.delete", "edit", "Delete Actor", "Delete", ImGuiKey_Delete,
        [this] { deleteSelectedActor(); },
        [this] { return selectedActor_ != 0 && !viewport_.isUsing(); });
    commands_.add("actor.addSphere", "actor", "+ Sphere", "", 0,
        [this] {
            selectedActor_ = world_.createActor("Sphere", 1, selectedMaterial_);
            world_.findActor(selectedActor_)->transform.position[1] = 1;
            selection_.assign(1, selectedActor_);
            pushActorCreated(*world_.findActor(selectedActor_), "Create Sphere");
            sceneDirty_ = true;
        },
        [this] { return assets_.findMesh(1) != nullptr && assets_.findMaterial(selectedMaterial_) != nullptr; });
    commands_.add("actor.addCube", "actor", "+ Cube", "", 0,
        [this] {
            selectedActor_ = world_.createActor("Cube", 3, selectedMaterial_);
            world_.findActor(selectedActor_)->transform.position[1] = 1;
            selection_.assign(1, selectedActor_);
            pushActorCreated(*world_.findActor(selectedActor_), "Create Cube");
            sceneDirty_ = true;
        },
        [this] { return assets_.findMesh(3) != nullptr && assets_.findMaterial(selectedMaterial_) != nullptr; });
    commands_.add("scene.save", "scene", "Save Scene", "Ctrl+S", ImGuiMod_Ctrl | ImGuiKey_S,
        [this] { saveScene(); });
    commands_.add("view.toggleOutline", "edit", "Toggle Selection Outline", "G", ImGuiKey_G,
        [this] {
            auto& preferences = imgui::uiPreferences();
            preferences.selectionOutline = !preferences.selectionOutline;
            imgui::saveUiPreferences(imgui::defaultUiPreferencesPath(), preferences);
            status_ = preferences.selectionOutline ? "Selection outline shown." : "Selection outline hidden.";
        });
    commands_.add("actor.toggleVisibility", "edit", "Toggle Visibility", "H", ImGuiKey_H,
        [this] { toggleSelectedVisibility(); },
        [this] { return !selection_.empty() && !viewport_.isUsing(); });
    commands_.add("layout.restore", "layout", "Restore Default Layout", "", 0,
        [this] { dockManager_.resetLayout(); });
}

bool SceneEditor::commandButton(std::string_view id) {
    const auto* info = commands_.info(id);
    if (!info) return false;
    const bool usable = commands_.enabled(id);
    ImGui::BeginDisabled(!usable);
    const bool pressed = ImGui::Button(info->label.c_str());
    ImGui::EndDisabled();
    if (pressed) commands_.invoke(id);
    return pressed;
}

void SceneEditor::duplicateSelectedActor() {
    if (selection_.empty()) return;
    std::vector<scene::ActorId> created;
    for (const auto id : selection_) {
        const auto copy = world_.duplicateActor(id);
        if (!copy) continue;
        world_.findActor(copy)->transform.position[0] += 2.5f;
        pushActorCreated(*world_.findActor(copy), "Duplicate Actor");
        created.push_back(copy);
    }
    if (created.empty()) return;
    selection_ = created;
    selectedActor_ = created.back();
    sceneDirty_ = true;
}

void SceneEditor::duplicateForDrag() {
    dragSourceSelection_ = selection_;
    dragSourcePrimary_ = selectedActor_;
    dragCopies_.clear();
    for (const auto id : dragSourceSelection_) {
        const auto copy = world_.duplicateActor(id);
        if (!copy) continue;
        dragCopies_.push_back(copy);
        if (id == dragSourcePrimary_) selectedActor_ = copy;
    }
    selection_ = dragCopies_;
    sceneDirty_ = true;
}

void SceneEditor::selectMaterial(assets::AssetId id) {
    if (!assets_.findMaterial(id)) return;
    materialWindowOpen_ = true;
    materialWindowFocusRequested_ = true;
    if (id == selectedMaterial_) { openMaterialTab_ = true; return; }
    if (materialDraftDirty_) { pendingMaterial_ = id; openMaterialTab_ = true; return; }
    if (const auto* material = assets_.findMaterial(id)) {
        selectedMaterial_ = id;
        materialDraft_ = *material;
        openMaterialTab_ = true;
    }
}

const assets::MaterialAsset* SceneEditor::materialPreview() const {
    if (!materialDraftDirty_ || materialDraft_.id != selectedMaterial_ ||
        !assets_.findMaterial(selectedMaterial_) || !validMaterialParameters(materialDraft_)) return nullptr;
    return &materialDraft_;
}

void SceneEditor::discardMaterial() {
    if (const auto* material = assets_.findMaterial(selectedMaterial_)) materialDraft_ = *material;
    materialDraftDirty_ = false;
    status_ = "Material draft discarded. Preview restored to the applied material.";
}

bool SceneEditor::applyMaterial() {
    auto* target = assets_.findMaterial(selectedMaterial_);
    if (!target) { status_ = "Material no longer exists."; return false; }
    if (materialDraft_.name.empty() || !validMaterialParameters(materialDraft_)) {
        status_ = "Material needs a name and finite Base color/Roughness/Metallic values between zero and one.";
        return false;
    }
    *target = materialDraft_;
    materialDraftDirty_ = false;
    materialsDirty_ = sceneDirty_ = true;
    status_ = "Material applied to every Actor referencing this asset. Save to persist.";
    return true;
}

bool SceneEditor::saveScene() {
    std::string error;
    // Persist materials before the scene so its GUID references always resolve.
    if (!assets_.saveMaterials(project_.contentRoot / "Materials", error)) {
        status_ = "Material save failed: " + error; return false;
    }
    if (!world_.save(scenePath_, assets_, error)) {
        status_ = "Scene save failed: " + error; return false;
    }
    sceneDirty_ = false;
    status_ = "Saved " + pathText(scenePath_) + " (applied materials only).";
    return true;
}

bool SceneEditor::saveAll(renderer::SceneRenderer& renderer) {
    if (materialDraftDirty_ && !applyMaterial()) return false;
    if (shaderDraft_ != renderer.shaderSource() && !renderer.applyShaderSource(shaderDraft_, diagnostics_)) {
        status_ = "Shader compile failed. Nothing was closed; fix the error or discard your draft."; return false;
    }
    std::string error;
    if (!renderer.saveShaderSource(error)) { status_ = error; return false; }
    shaderSaved_ = renderer.shaderSource();
    if (!assets_.saveMaterials(project_.contentRoot / "Materials", error)) { status_ = error; return false; }
    materialsDirty_ = false;
    if (!saveScene()) return false;
    status_ = "Saved scene, material assets and applied shader source.";
    return true;
}

bool SceneEditor::dirty(const renderer::SceneRenderer& renderer) const {
    return sceneDirty_ || materialsDirty_ || materialDraftDirty_ ||
        shaderDraft_ != renderer.shaderSource() || renderer.shaderSource() != shaderSaved_;
}

bool SceneEditor::isSelected(scene::ActorId id) const {
    return std::find(selection_.begin(), selection_.end(), id) != selection_.end();
}

void SceneEditor::selectActor(scene::ActorId id, bool additive) {
    dockManager_.requestFocus(EditorPanel::Details);
    if (!id) {
        selection_.clear();
        selectedActor_ = 0;
        return;
    }
    if (!additive) {
        selection_.assign(1, id);
        selectedActor_ = id;
        return;
    }
    // Ctrl/Shift click toggles membership; the primary falls back to the last
    // remaining Actor so both inspectors refer to the same selected instance.
    const auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        selection_.erase(it);
        if (selectedActor_ == id) selectedActor_ = selection_.empty() ? 0 : selection_.back();
    } else {
        selection_.push_back(id);
        selectedActor_ = id;
    }
}

void SceneEditor::eraseActor(scene::ActorId id) {
    auto& actors = world_.actors;
    actors.erase(std::remove_if(actors.begin(), actors.end(),
        [id](const scene::Actor& actor) { return actor.id == id; }), actors.end());
    selection_.erase(std::remove(selection_.begin(), selection_.end(), id), selection_.end());
    if (selectedActor_ == id) selectedActor_ = selection_.empty() ? 0 : selection_.back();
}

void SceneEditor::insertActor(std::size_t index, const scene::Actor& actor) {
    auto& actors = world_.actors;
    if (index > actors.size()) index = actors.size();
    actors.insert(actors.begin() + static_cast<std::ptrdiff_t>(index), actor);
}

void SceneEditor::pushActorCreated(scene::Actor actor, const std::string& label) {
    dockManager_.requestFocus(EditorPanel::Details);
    // Undo removes the new Actor, redo puts the same copy back. Both directions
    // capture the identity by value so a later selection change cannot leak in.
    const scene::ActorId id = actor.id;
    undo_.push(label,
        [this, id] { eraseActor(id); sceneDirty_ = true; },
        [this, actor] { world_.actors.push_back(actor); selectedActor_ = actor.id; selection_.assign(1, actor.id); sceneDirty_ = true; });
}


void SceneEditor::pushDetailsTransform(scene::ActorId id, const scene::Transform& before,
                                       const scene::Transform& after, const char* field) {
    if (!id || std::memcmp(&before, &after, sizeof(scene::Transform)) == 0) return;
    undo_.push(std::string("Edit ") + field,
        [this, id, before] { if (auto* target = world_.findActor(id)) { target->transform = before; sceneDirty_ = true; } },
        [this, id, after] { if (auto* target = world_.findActor(id)) { target->transform = after; sceneDirty_ = true; } },
        UndoStack::Merge{"details." + std::to_string(id) + "." + field, 700});
}

void SceneEditor::trackTransformEdit() {
    if (viewport_.isUsing()) {
        if (!dragging_) {
            dragging_ = true;
            dragStartTransforms_ = settledSelection_;
            // Fall back to live values when the selection changed since the last
            // settled frame.
            if (dragStartTransforms_.empty())
                for (const auto id : selection_)
                    if (const auto* actor = world_.findActor(id))
                        dragStartTransforms_.push_back({id, actor->transform});
        }
        return;
    }
    if (dragging_) {
        dragging_ = false;
        if (!dragCopies_.empty()) {
            // Creation and the final placement form one transaction. Capture
            // full Actor values so redo retains IDs, references and transforms.
            std::vector<scene::Actor> copies;
            for (const auto id : dragCopies_)
                if (const auto* actor = world_.findActor(id)) copies.push_back(*actor);
            const auto sourceSelection = dragSourceSelection_;
            const auto sourcePrimary = dragSourcePrimary_;
            const auto copySelection = dragCopies_;
            const auto copyPrimary = selectedActor_;
            undo_.push("Duplicate and transform Actors",
                [this, copySelection, sourceSelection, sourcePrimary] {
                    for (const auto id : copySelection) eraseActor(id);
                    selection_ = sourceSelection;
                    selectedActor_ = sourcePrimary;
                    sceneDirty_ = true;
                },
                [this, copies, copySelection, copyPrimary] {
                    for (const auto& actor : copies) world_.actors.push_back(actor);
                    selection_ = copySelection;
                    selectedActor_ = copyPrimary;
                    sceneDirty_ = true;
                });
            dragCopies_.clear();
            dragSourceSelection_.clear();
            dragSourcePrimary_ = 0;
            dragStartTransforms_.clear();
            settledSelection_.clear();
            settledTransformActor_ = 0;
            return;
        }
        std::vector<ActorSnapshot> before, after;
        for (const auto& snapshot : dragStartTransforms_) {
            const auto* actor = world_.findActor(snapshot.id);
            if (!actor || std::memcmp(&actor->transform, &snapshot.transform, sizeof(scene::Transform)) == 0) continue;
            before.push_back(snapshot);
            after.push_back({snapshot.id, actor->transform});
        }
        dragStartTransforms_.clear();
        settledSelection_.clear();
        settledTransformActor_ = 0;
        if (!before.empty()) {
            const std::string label = before.size() > 1
                ? "Transform " + std::to_string(before.size()) + " Actors"
                : "Transform " + (world_.findActor(before.front().id) ? world_.findActor(before.front().id)->name
                                                                     : std::string("Actor"));
            undo_.push(label,
                [this, before] { for (const auto& s : before) if (auto* target = world_.findActor(s.id)) target->transform = s.transform; sceneDirty_ = true; },
                [this, after] { for (const auto& s : after) if (auto* target = world_.findActor(s.id)) target->transform = s.transform; sceneDirty_ = true; });
        }
        return;
    }
    // Nothing is being dragged: remember the visible transforms of the current
    // selection as the next drag's "before" snapshot.
    settledSelection_.clear();
    for (const auto id : selection_)
        if (const auto* actor = world_.findActor(id))
            settledSelection_.push_back({id, actor->transform});
    // Nothing is being dragged: remember the visible transform as the next
    // drag's "before" snapshot.
    const auto* actor = world_.findActor(selectedActor_);
    if (actor) {
        settledTransform_ = actor->transform;
        settledTransformActor_ = actor->id;
    } else {
        settledTransformActor_ = 0;
    }
}

bool SceneEditor::undo() {
    if (!undo_.undo()) return false;
    sceneDirty_ = true;
    status_ = "Undo: " + undo_.redoLabel();
    return true;
}

bool SceneEditor::redo() {
    if (!undo_.redo()) return false;
    sceneDirty_ = true;
    status_ = "Redo: " + undo_.undoLabel();
    return true;
}

void SceneEditor::setGarbageCollector(rae::GarbageCollector* gc, const std::filesystem::path& settingsPath) {
    garbageCollector_ = gc;
    garbageCollectionSettingsPath_ = settingsPath;
    garbageCollectionDraft_ = gc ? gc->settings() : rae::GarbageCollectionSettings{};
    garbageCollectionDraftDirty_ = false;
    garbageCollectionRequested_ = false;
    garbageCollectionMessage_.clear();
    if (!gc) garbageCollectionOpen_ = false;
}

void SceneEditor::drawUiMenu() {
    if (!ImGui::BeginMenu("UI")) return;
    if (ImGui::BeginMenu("Theme")) {
        for (int index = 0; index < imgui::kEditorThemeCount; ++index) {
            const auto theme = static_cast<imgui::EditorTheme>(index);
            if (ImGui::MenuItem(imgui::themeName(theme), nullptr, imgui::currentTheme() == theme)) {
                imgui::setTheme(theme);
                auto& preferences = imgui::uiPreferences();
                preferences.theme = theme;
                imgui::saveUiPreferences(imgui::defaultUiPreferencesPath(), preferences);
                status_ = std::string("Theme: ") + imgui::themeName(theme);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Material editor...", nullptr, materialWindowOpen_, !assets_.materials.empty())) {
        selectMaterial(assets_.findMaterial(selectedMaterial_) ? selectedMaterial_ : assets_.materials.front().id);
    }
    if (ImGui::MenuItem("Garbage collection...", nullptr, garbageCollectionOpen_, garbageCollector_ != nullptr)) {
        if (!garbageCollectionDraftDirty_) garbageCollectionDraft_ = garbageCollector_->settings();
        garbageCollectionOpen_ = true;
        ImGui::SetWindowFocus("Garbage collection");
    }
    // Editor-only viewport aids get their own small window, matching the other
    // settings entries in this menu instead of crowding the popup itself.
    if (ImGui::MenuItem("Selection outline...", nullptr, outlineSettingsOpen_)) {
        outlineSettingsOpen_ = true;
        ImGui::SetWindowFocus("Selection Outline");
    }
    ImGui::EndMenu();
}

void SceneEditor::drawOutlineSettings() {
    // Clear the focus flag before the early-out: a window closed this frame must
    // not keep suppressing scene shortcuts on the next ones.
    outlineSettingsFocused_ = false;
    if (!outlineSettingsOpen_) return;
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    // Small centered dialog: auto-resize keeps the controls visible instead of
    // pushing the colour presets below a scrollbar.
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints({360, 0}, {FLT_MAX, FLT_MAX});
    const bool visible = ImGui::Begin("Selection Outline", &outlineSettingsOpen_,
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
    outlineSettingsFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) { ImGui::End(); return; }
    ImGui::TextWrapped("Editor-only highlight for the selected Actors. "
        "Not saved into the scene and not drawn in standalone runs.");
    ImGui::SeparatorText("Style");
    auto& preferences = imgui::uiPreferences();
    bool changed = false;
    changed |= ImGui::Checkbox("Show outline", &preferences.selectionOutline);
    ImGui::BeginDisabled(!preferences.selectionOutline);
    ImGui::SetNextItemWidth(180);
    changed |= ImGui::SliderFloat("Thickness", &preferences.outlineThickness, 1.0f, 6.0f, "%.1f px");
    changed |= ImGui::Checkbox("Use theme accent", &preferences.outlineThemeColor);
    changed |= ImGui::Checkbox("Outline front objects", &preferences.outlineFrontObjects);

    ImGui::SeparatorText("Colour");
    ImGui::BeginDisabled(preferences.outlineThemeColor);
    struct Preset { const char* name; float color[3]; };
    static const Preset presets[] = {
        {"Orange", {0.992f, 0.471f, 0.016f}},
        {"White", {0.95f, 0.95f, 0.95f}},
        {"Cyan", {0.25f, 0.80f, 1.0f}},
    };
    bool presetSelected = false;
    for (const auto& preset : presets) {
        const std::string swatchId = std::string("##") + preset.name;
        ImGui::ColorButton(swatchId.c_str(), ImVec4(preset.color[0], preset.color[1], preset.color[2], 1.0f),
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(18, 18));
        ImGui::SameLine();
        const bool selected = std::abs(preferences.outlineColor[0] - preset.color[0]) < 0.02f &&
            std::abs(preferences.outlineColor[1] - preset.color[1]) < 0.02f &&
            std::abs(preferences.outlineColor[2] - preset.color[2]) < 0.02f;
        presetSelected = presetSelected || selected;
        if (ImGui::RadioButton(preset.name, selected)) {
            for (int channel = 0; channel < 3; ++channel)
                preferences.outlineColor[channel] = preset.color[channel];
            preferences.outlineThemeColor = false;
            changed = true;
        }
    }
    // Custom colour: selecting the mode keeps the current swatch, and any picked
    // colour switches the outline off the theme accent.
    const std::string customSwatch = "##custom-swatch";
    ImGui::ColorButton(customSwatch.c_str(),
        ImVec4(preferences.outlineColor[0], preferences.outlineColor[1], preferences.outlineColor[2], 1.0f),
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoAlpha, ImVec2(18, 18));
    ImGui::SameLine();
    if (ImGui::RadioButton("Custom", !presetSelected)) {
        preferences.outlineThemeColor = false;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::ColorEdit3("##custom-color", preferences.outlineColor, ImGuiColorEditFlags_NoInputs)) {
        preferences.outlineThemeColor = false;
        changed = true;
    }
    ImGui::EndDisabled();
    if (preferences.outlineThemeColor) ImGui::TextDisabled("Using the active theme accent.");
    ImGui::EndDisabled();
    if (changed) {
        imgui::saveUiPreferences(imgui::defaultUiPreferencesPath(), preferences);
        status_ = "Selection outline updated.";
    }
    ImGui::End();
}

void SceneEditor::drawGarbageCollection() {
    garbageCollectionFocused_ = false;
    if (!garbageCollectionOpen_ || !garbageCollector_) return;
    const auto& stats = garbageCollector_->stats();
    if (garbageCollectionRequested_ && stats.collections != garbageCollectionRequestCount_) {
        garbageCollectionRequested_ = false;
        garbageCollectionMessage_ = "Collection finished. See the latest result below.";
    }
    ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
    ImGui::SetNextWindowSize({480, 620}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({280, 220}, {FLT_MAX, FLT_MAX});
    const bool visible = ImGui::Begin("Garbage collection", &garbageCollectionOpen_, ImGuiWindowFlags_NoDocking);
    garbageCollectionFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!visible) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Reclaim unused memory and expired temporary captures. Permanent screenshots are never deleted.");
    ImGui::SeparatorText("Settings");
    ImGui::TextWrapped("Changes take effect only with Apply & save. Closing this window keeps the draft until you exit the editor.");
    garbageCollectionDraftDirty_ |= ImGui::Checkbox("Automatic collection", &garbageCollectionDraft_.enabled);
    const auto secondsInput = [this](const char* label, const char* id, double& value) {
        ImGui::TextWrapped("%s", label);
        ImGui::SetNextItemWidth(-1);
        garbageCollectionDraftDirty_ |= ImGui::InputDouble(id, &value, 0, 0, "%.3f");
    };
    secondsInput("Check interval (seconds, 1 - 86400)", "##gcInterval", garbageCollectionDraft_.intervalSeconds);
    secondsInput("Memory idle time (seconds, 0 - 604800)", "##gcIdle", garbageCollectionDraft_.memoryIdleSeconds);
    secondsInput("Temporary capture retention (seconds, 1 - 31536000)", "##gcCaptureAge",
        garbageCollectionDraft_.temporaryCaptureMaxAgeSeconds);
    ImGui::TextWrapped("Memory must be unused for the idle time before it is eligible. Collection uses the currently applied settings.");
    if (garbageCollectionDraftDirty_) ImGui::TextWrapped("Draft changes are not applied or saved.");
    ImGui::BeginDisabled(garbageCollectionSettingsPath_.empty());
    if (ImGui::Button("Apply & save")) {
        const auto previous = garbageCollector_->settings();
        std::string error;
        // Core validates finite values and ranges. No collection runs inside
        // this UI transaction: the application services requests at frame end.
        if (!garbageCollector_->setSettings(garbageCollectionDraft_, error)) {
            garbageCollectionMessage_ = "Settings not applied: " + error;
        } else if (!garbageCollector_->saveSettings(garbageCollectionSettingsPath_, error)) {
            std::string rollbackError;
            if (garbageCollector_->setSettings(previous, rollbackError))
                garbageCollectionMessage_ = "Save failed; previous running settings restored. Draft kept. " + error;
            else
                garbageCollectionMessage_ = "Save failed; draft settings remain active but are not saved. " + error +
                    " Restore failed: " + rollbackError;
        } else {
            garbageCollectionDraftDirty_ = false;
            garbageCollectionMessage_ = "Settings applied and saved.";
        }
    }
    ImGui::EndDisabled();
    continueButtonRow("Discard draft");
    ImGui::BeginDisabled(!garbageCollectionDraftDirty_);
    if (ImGui::Button("Discard draft")) {
        garbageCollectionDraft_ = garbageCollector_->settings();
        garbageCollectionDraftDirty_ = false;
        garbageCollectionMessage_ = "Draft discarded. Showing currently applied settings.";
    }
    ImGui::EndDisabled();
    if (garbageCollectionSettingsPath_.empty()) ImGui::TextWrapped("Saving is unavailable: no settings path was supplied.");
    ImGui::SeparatorText("Collection");
    ImGui::TextWrapped("Automatic collection is currently %s. Manual collection is available even when it is off.",
        garbageCollector_->settings().enabled ? "on" : "off");
    ImGui::BeginDisabled(garbageCollectionRequested_);
    if (ImGui::Button("Collect now")) {
        garbageCollectionRequestCount_ = stats.collections;
        garbageCollectionRequested_ = true;
        garbageCollector_->requestCollection();
        garbageCollectionMessage_ = "Collection queued for the end of this frame using applied settings.";
    }
    ImGui::EndDisabled();
    message(garbageCollectionMessage_);
    ImGui::SeparatorText("Latest result");
    if (!stats.collections) {
        ImGui::TextWrapped("No collection has run in this session.");
    } else {
        ImGui::TextWrapped("Collections this session: %llu", static_cast<unsigned long long>(stats.collections));
        ImGui::TextWrapped("Last duration: %.3f ms", stats.lastDurationMs);
        ImGui::TextWrapped("CPU memory released: %llu bytes", static_cast<unsigned long long>(stats.memory.cpuBytesReleased));
        ImGui::TextWrapped("GPU memory released (estimate): %llu bytes",
            static_cast<unsigned long long>(stats.memory.gpuBytesReleasedEstimate));
        ImGui::TextWrapped("Objects released: %llu", static_cast<unsigned long long>(stats.memory.objectsReleased));
        ImGui::TextWrapped("Temporary captures removed: %llu (%llu bytes)",
            static_cast<unsigned long long>(stats.temporaryFilesRemoved),
            static_cast<unsigned long long>(stats.temporaryBytesRemoved));
    }
    if (!stats.lastError.empty()) ImGui::TextWrapped("Last collection error: %s", stats.lastError.c_str());
    ImGui::End();
}

void SceneEditor::drawEditMenu() {
    if (!ImGui::BeginMenu("Edit")) return;
    for (const auto& command : commands_.commands()) {
        if (command.info.group != "edit") continue;
        const bool usable = !command.enabled || command.enabled();
        // Undo/redo show the pending action name, like Godot's history entries.
        std::string label = command.info.label;
        if (command.info.id == "edit.undo" && undo_.canUndo()) label += " " + undo_.undoLabel();
        if (command.info.id == "edit.redo" && undo_.canRedo()) label += " " + undo_.redoLabel();
        if (ImGui::MenuItem(label.c_str(), command.info.shortcut.c_str(), false, usable))
            commands_.invoke(command.info.id);
    }
    ImGui::EndMenu();
}

void SceneEditor::toggleSelectedVisibility() {
    if (selection_.empty() || viewport_.isUsing()) return;
    std::vector<std::pair<scene::ActorId, bool>> before;
    bool anyVisible = false;
    for (const auto id : selection_) {
        if (const auto* actor = world_.findActor(id)) {
            before.emplace_back(id, actor->visible);
            anyVisible = anyVisible || actor->visible;
        }
    }
    if (before.empty()) return;
    // Match Gizmo/multi-select semantics: one gesture, one undo entry, and the
    // whole selection converges on the same visibility so it stays coherent.
    const bool target = !anyVisible;
    const auto apply = [this, before, target] {
        for (const auto& [id, wasVisible] : before) {
            if (auto* actor = world_.findActor(id)) actor->visible = target;
        }
        sceneDirty_ = true;
    };
    apply();
    undo_.push(target ? "Hide Actors" : "Show Actors",
        [this, before] {
            for (const auto& [id, wasVisible] : before) {
                if (auto* actor = world_.findActor(id)) actor->visible = wasVisible;
            }
            sceneDirty_ = true;
        },
        apply);
    status_ = target ? "Actors visible." : "Actors hidden.";
}

void SceneEditor::deleteSelectedActor() {
    if (selection_.empty() || viewport_.isUsing()) return;
    std::vector<std::pair<scene::Actor, std::size_t>> removed;
    for (const auto id : selection_) {
        const auto* actor = world_.findActor(id);
        if (!actor) continue;
        std::size_t index = 0;
        while (index < world_.actors.size() && world_.actors[index].id != id) ++index;
        removed.emplace_back(*actor, index);
    }
    if (removed.empty()) return;
    // Remove from the highest index down so the captured positions stay valid.
    std::sort(removed.begin(), removed.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& entry : removed) world_.removeActor(entry.first.id);
    undo_.push(removed.size() > 1 ? "Delete " + std::to_string(removed.size()) + " Actors"
                                  : "Delete " + removed.front().first.name,
        [this, removed] {
            // Re-insert in ascending index order to restore the original order.
            for (auto it = removed.rbegin(); it != removed.rend(); ++it) insertActor(it->second, it->first);
            selectedActor_ = removed.front().first.id;
            selection_.assign(1, selectedActor_);
            sceneDirty_ = true;
        },
        [this, removed] {
            for (const auto& entry : removed) eraseActor(entry.first.id);
            sceneDirty_ = true;
        });
    selection_.clear();
    selectedActor_ = 0;
    sceneDirty_ = true;
    status_ = "Deleted " + std::to_string(removed.size()) + " Actor(s). Save Scene to persist.";
}

namespace {

// Small hand-drawn eye used by the Outliner rows. A glyph would depend on the
// loaded font, so the lens and pupil are drawn with the window draw list.
void drawEyeIcon(ImDrawList* list, ImVec2 center, float size, bool visible, bool highlighted) {
    // Line-art eye: almond outline plus a hollow pupil. Cubic beziers are used
    // because the quadratic form only reaches half of its control offset, which
    // flattens the lens enough that the pupil ring no longer fits inside it.
    // Keep the eye legible on the dark theme: the disabled text colour alone is
    // too faint for a 1px-stroke shape at 20px.
    ImVec4 tint = ImGui::GetStyleColorVec4(highlighted ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    if (!highlighted) tint.w = std::max(tint.w, visible ? 0.85f : 0.55f);
    const ImU32 color = ImGui::GetColorU32(tint);
    const float stroke = std::max(1.4f, size * 0.09f);
    const float halfWidth = size * 0.44f;
    const float bend = size * 0.32f;
    const ImVec2 left{center.x - halfWidth, center.y};
    const ImVec2 right{center.x + halfWidth, center.y};
    list->PathClear();
    list->PathLineTo(left);
    list->PathBezierCubicCurveTo(ImVec2(left.x + halfWidth * 0.38f, center.y - bend),
        ImVec2(right.x - halfWidth * 0.38f, center.y - bend), right, 14);
    list->PathBezierCubicCurveTo(ImVec2(right.x - halfWidth * 0.38f, center.y + bend),
        ImVec2(left.x + halfWidth * 0.38f, center.y + bend), left, 14);
    list->PathStroke(color, ImDrawFlags_Closed, stroke);
    list->AddCircleFilled(center, size * 0.145f, color, 16);
    if (!visible) {
        list->AddLine(ImVec2(center.x - size * 0.37f, center.y + size * 0.30f),
            ImVec2(center.x + size * 0.37f, center.y - size * 0.30f), color, stroke);
    }
}

std::filesystem::path utf8Path(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()),
        reinterpret_cast<const char8_t*>(text.data()) + text.size()));
}

std::string parentFolder(std::string_view folder) {
    const auto slash = folder.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(folder.substr(0, slash));
}

std::string leafName(std::string_view folder) {
    const auto slash = folder.rfind('/');
    return slash == std::string_view::npos ? std::string(folder) : std::string(folder.substr(slash + 1));
}

const char* sizeLabel(std::uint64_t bytes, char* buffer, std::size_t size) {
    if (bytes >= 1024ull * 1024ull) std::snprintf(buffer, size, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024ull) std::snprintf(buffer, size, "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(buffer, size, "%llu B", static_cast<unsigned long long>(bytes));
    return buffer;
}

// Content browser icons are drawn, not sampled, so they stay crisp at any DPI.
void drawAssetIcon(ImDrawList* list, ImVec2 center, float size, assets::AssetType type, ImU32 color) {
    const float thickness = std::max(1.2f, size * 0.09f);
    const float half = size * 0.42f;
    switch (type) {
    case assets::AssetType::Scene: {
        const ImVec2 a{center.x - half, center.y - half};
        const ImVec2 b{center.x + half * 0.55f, center.y + half * 0.15f};
        list->AddRect(a, b, color, 1.5f, 0, thickness);
        list->AddRect(ImVec2(a.x + half * 0.35f, a.y + half * 0.35f),
            ImVec2(b.x + half * 0.35f, b.y + half * 0.35f), color, 1.5f, 0, thickness);
        break;
    }
    case assets::AssetType::Material:
        list->AddCircle(center, half * 0.85f, color, 20, thickness);
        list->AddCircleFilled(ImVec2(center.x - half * 0.25f, center.y - half * 0.25f), half * 0.16f, color, 10);
        break;
    case assets::AssetType::Mesh:
        list->PathClear();
        list->PathLineTo(ImVec2(center.x, center.y - half));
        list->PathLineTo(ImVec2(center.x + half, center.y + half * 0.75f));
        list->PathLineTo(ImVec2(center.x - half, center.y + half * 0.75f));
        list->PathStroke(color, ImDrawFlags_Closed, thickness);
        break;
    case assets::AssetType::Shader:
        list->AddLine(ImVec2(center.x - half * 0.2f, center.y - half), ImVec2(center.x - half, center.y), color, thickness);
        list->AddLine(ImVec2(center.x - half, center.y), ImVec2(center.x - half * 0.2f, center.y + half), color, thickness);
        list->AddLine(ImVec2(center.x + half * 0.2f, center.y - half), ImVec2(center.x + half, center.y), color, thickness);
        list->AddLine(ImVec2(center.x + half, center.y), ImVec2(center.x + half * 0.2f, center.y + half), color, thickness);
        break;
    default:
        list->AddRect(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half),
            color, 2.0f, 0, thickness);
        break;
    }
}

} // namespace

void SceneEditor::drawOutliner() {
    ImGui::BeginDisabled(!assets_.findMesh(1) || !assets_.findMaterial(selectedMaterial_));
    commandButton("actor.addSphere");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!assets_.findMesh(3) || !assets_.findMaterial(selectedMaterial_));
    commandButton("actor.addCube");
    ImGui::EndDisabled();
    commandButton("actor.duplicate");
    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedActor_ || viewport_.isUsing());
    commandButton("actor.delete");
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginChild("Actor list", {0, 0});
    bool deleteRequested = false;
    // One icon column per window: 20px rows keep the eye legible and give the
    // label a stable left edge.
    const float eyeSize = std::max(18.0f, ImGui::GetTextLineHeight() + 4.0f);
    const float eyeColumn = eyeSize + 14.0f;
    for (auto& actor : world_.actors) {
        const ImVec2 rowStart = ImGui::GetCursorScreenPos();
        const float startX = ImGui::GetCursorPosX();
        const auto label = actor.name + "##" + std::to_string(actor.id);
        // The Selectable defines the row: it owns the label, the selection and the
        // context menu. The eye is placed afterwards inside the same row rect so
        // the two share one vertical centre regardless of padding changes.
        ImGui::SetCursorPosX(startX + eyeColumn + 8.0f);
        if (!actor.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str(), isSelected(actor.id)))
            selectActor(actor.id, ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift);
        if (!actor.visible) ImGui::PopStyleColor();
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        const float rowHeight = std::max(1.0f, rowMax.y - rowMin.y);
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x - eyeColumn - 8.0f, rowMin.y));
        const std::string eyeId = "##visible" + std::to_string(actor.id);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
        const bool eyeClicked = ImGui::Button(eyeId.c_str(), ImVec2(eyeColumn, rowHeight));
        ImGui::PopStyleColor(3);
        const ImVec2 eyeMin = ImGui::GetItemRectMin();
        const ImVec2 eyeMax = ImGui::GetItemRectMax();
        const ImVec2 eyeCenter{(eyeMin.x + eyeMax.x) * 0.5f, (eyeMin.y + eyeMax.y) * 0.5f};
        const bool eyeHovered = ImGui::IsItemHovered();
        if (eyeHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(actor.visible ? "Hide actor" : "Show actor");
        }
        drawEyeIcon(ImGui::GetWindowDrawList(), eyeCenter, rowHeight * 0.86f, actor.visible,
            eyeHovered || ImGui::IsItemActive());
        if (eyeClicked) setActorVisible(actor.id, !actor.visible);
        if (ImGui::BeginPopupContextItem(label.c_str())) {
            selectedActor_ = actor.id;
            if (ImGui::MenuItem("Delete Actor", "Delete")) deleteRequested = true;
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(rowStart.x, rowMax.y + ImGui::GetStyle().ItemSpacing.y));
    }
    ImGui::EndChild();
    if (deleteRequested) deleteSelectedActor();
    actorPanelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
}

void SceneEditor::setActorVisible(scene::ActorId id, bool visible) {
    auto* actor = world_.findActor(id);
    if (!actor || actor->visible == visible) return;
    const bool previous = actor->visible;
    actor->visible = visible;
    sceneDirty_ = true;
    undo_.push(visible ? "Show Actor" : "Hide Actor",
        [this, id, previous] { if (auto* target = world_.findActor(id)) { target->visible = previous; sceneDirty_ = true; } },
        [this, id, visible] { if (auto* target = world_.findActor(id)) { target->visible = visible; sceneDirty_ = true; } });
    status_ = visible ? "Actor visible." : "Actor hidden.";
}

void SceneEditor::drawSceneLighting(renderer::SceneSettings& settings) {
    ImGui::SeparatorText("Lighting");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##Light", &settings.lightIntensity, .1f, 12.0f, "Key light %.1f");
    ImGui::TextDisabled("Temporary scene-level key light; it moves into a direct-light Actor component later.");
    ImGui::SeparatorText("Direct-light shadows");
    ImGui::TextDisabled("Ray tracing: all mesh actors");
    ImGui::Text("Shadow method");
    int shadowMethod = static_cast<int>(settings.shadowMethod);
    shadowMethod = std::clamp(shadowMethod, 0, 8);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##shadowMethod", &shadowMethod,
        "Shadow map (2x2 PCF)\0Analytic rays (all mesh actors)\0Disabled\0Shadow map (hard)\0Shadow map (3x3 PCF)\0Shadow map (PCSS soft)\0Shadow map (2-cascade CSM)\0GTAO (deferred ambient occlusion)\0Contact shadow (deferred screen space)\0")) {
        settings.shadowMethod = static_cast<renderer::ShadowMethod>(shadowMethod);
    }
    if (settings.shadowMethod == renderer::ShadowMethod::ShadowMap)
        ImGui::TextWrapped("Raster shadow map with a 2x2 PCF filter for stable deferred and forward shadows.");
    else if (settings.shadowMethod == renderer::ShadowMethod::ShadowMapHard)
        ImGui::TextWrapped("Raster shadow map with one comparison sample for the sharpest edge.");
    else if (settings.shadowMethod == renderer::ShadowMethod::ShadowMapPCF9)
        ImGui::TextWrapped("Raster shadow map with a 3x3 PCF filter for softer edges.");
    else if (settings.shadowMethod == renderer::ShadowMethod::ShadowMapPCSS)
        ImGui::TextWrapped("Raster shadow map with blocker search and percentage-closer soft shadows.");
    else if (settings.shadowMethod == renderer::ShadowMethod::ShadowMapCSM2)
        ImGui::TextWrapped("Two cascaded directional shadow maps: a tight near cascade plus a wider far cascade.");
    else if (settings.shadowMethod == renderer::ShadowMethod::GTAO)
        ImGui::TextWrapped("GTAO adds screen-space ambient occlusion to deferred lighting and keeps the raster shadow map for direct light.");
    else if (settings.shadowMethod == renderer::ShadowMethod::ContactShadow)
        ImGui::TextWrapped("Contact shadows ray-march the deferred position buffer near the receiver; forward rendering falls back to the raster shadow map.");
    else if (settings.shadowMethod == renderer::ShadowMethod::AnalyticRay)
        ImGui::TextWrapped("Analytic rays: contact shadows for every visible mesh actor; built-in primitives use exact intersections and other meshes use triangle data.");
    else
        ImGui::TextWrapped("Direct-light shadows are disabled; ambient and reflection lighting remain active.");
}
void SceneEditor::drawScene() {
    // Scene-level parameters only. Actor instance properties live in Details.
    ImGui::TextDisabled("Project");
    ImGui::TextWrapped("%s", project_.name.c_str());
    ImGui::TextDisabled("Content");
    ImGui::TextWrapped("%s", pathText(project_.contentRoot).c_str());
    ImGui::SeparatorText("Overview");
    ImGui::Text("Actors: %zu", world_.actors.size());
    ImGui::Text("Meshes: %zu / Materials: %zu", assets_.meshes.size(), assets_.materials.size());
    if (sceneDirty_) ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.25f, 1.0f), "Unsaved changes");
    else ImGui::TextDisabled("Saved");
    ImGui::SeparatorText("Environment");
    int background = world_.blackBackground ? 1 : 0;
    if (ImGui::RadioButton("Sky", background == 0)) setSceneBackground(false);
    ImGui::SameLine();
    if (ImGui::RadioButton("Black", background == 1)) setSceneBackground(true);
    ImGui::TextDisabled("Background applies to the whole scene and is saved in the scene file.");
    ImGui::SeparatorText("Scene asset");
    ImGui::TextDisabled("Current scene");
    ImGui::TextWrapped("%s", pathText(scenePath_.filename()).c_str());
    if (ImGui::Button("Open...")) {
        const auto picked = pickSceneFile(false, project_.contentRoot / "Scenes", L"Open scene");
        if (!picked.empty()) requestSceneOpen(picked);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        const auto picked = pickSceneFile(true, project_.contentRoot / "Scenes", L"Save scene as");
        if (!picked.empty()) {
            std::string error;
            if (!saveSceneAs(picked, error)) status_ = error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) {
        const auto folder = project_.contentRoot / "Scenes";
        std::error_code code;
        std::filesystem::create_directories(folder, code);
        auto path = folder / "Scene.rascene";
        for (int suffix = 1; std::filesystem::exists(path, code) && suffix < 1000; ++suffix)
            path = folder / ("Scene_" + std::to_string(suffix) + ".rascene");
        std::string error;
        if (!createScene(path, error)) status_ = error;
    }
}


void SceneEditor::setSceneBackground(bool black) {
    if (world_.blackBackground == black) return;
    const bool previous = world_.blackBackground;
    world_.blackBackground = black;
    sceneDirty_ = true;
    undo_.push(black ? "Scene Background: Black" : "Scene Background: Sky",
        [this, previous] { world_.blackBackground = previous; sceneDirty_ = true; },
        [this, black] { world_.blackBackground = black; sceneDirty_ = true; });
    status_ = black ? "Scene background: black" : "Scene background: sky";
}

void SceneEditor::importMesh() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    try {
        // Validate before copying. The editor owns the imported copy, not the external source.
        auto proposed = assets_;
        assets::AssetId id{};
        std::string error;
        if (!proposed.importObj(path, id, error)) { status_ = error; return; }
        const auto folder = project_.contentRoot / "Meshes";
        std::filesystem::create_directories(folder);
        auto destination = folder / std::filesystem::path(path).filename();
        const bool same = std::filesystem::exists(destination) && std::filesystem::equivalent(path, destination);
        if (!same) {
            int suffix = 1;
            while (std::filesystem::exists(destination))
                destination = folder / (std::filesystem::path(path).stem().wstring() + L"_" + std::to_wstring(suffix++) + L".obj");
            std::filesystem::copy_file(path, destination);
        }
        const auto utf8Path = destination.generic_u8string();
        proposed.findMesh(id)->sourcePath.assign(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
        assets_ = std::move(proposed); selectedMesh_ = id; sceneDirty_ = true;
        status_ = "OBJ imported. Add an Actor from this mesh to inspect the instance, then save the scene.";
    } catch (const std::exception& error) { status_ = error.what(); }
}


bool SceneEditor::openScene(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        error = "Scene not found: " + pathText(path);
        return false;
    }
    // Parse into isolated objects: a failed load must not disturb the open scene.
    scene::World stagedWorld;
    assets::AssetLibrary stagedAssets;
    if (!stagedWorld.load(path, stagedAssets, error)) return false;
    world_ = std::move(stagedWorld);
    assets_ = std::move(stagedAssets);
    scenePath_ = path;
    sceneDirty_ = false;
    materialsDirty_ = false;
    materialDraftDirty_ = false;
    undo_.clear();
    selectedActor_ = world_.actors.empty() ? 0 : world_.actors.front().id;
    sceneInspectorActor_ = selectedActor_;
    selection_.assign(selectedActor_ ? 1 : 0, selectedActor_);
    selectedMesh_ = assets_.meshes.empty() ? 0 : assets_.meshes.front().id;
    selectedMaterial_ = assets_.materials.empty() ? 0 : assets_.materials.front().id;
    if (auto* actor = world_.findActor(selectedActor_)) {
        selectedMesh_ = actor->mesh.meshId;
        selectedMaterial_ = actor->mesh.materialId;
    }
    if (const auto* material = assets_.findMaterial(selectedMaterial_)) materialDraft_ = *material;
    refreshContent();
    status_ = "Opened " + pathText(path) + ".";
    return true;
}

bool SceneEditor::createScene(const std::filesystem::path& path, std::string& error) {
    error.clear();
    scene::World fresh;
    fresh.createDefaultScene(assets_);
    assets::AssetLibrary library = assets_;
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) { error = "Cannot create the scene folder: " + code.message(); return false; }
    if (!fresh.save(path, library, error)) return false;
    return openScene(path, error);
}

bool SceneEditor::saveSceneAs(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) { error = "Cannot create the scene folder: " + code.message(); return false; }
    if (!world_.save(path, assets_, error)) return false;
    scenePath_ = path;
    sceneDirty_ = false;
    refreshContent();
    status_ = "Saved " + pathText(path) + ".";
    return true;
}

void SceneEditor::refreshContent() {
    registry_.setContentRoot(project_.contentRoot);
    registry_.scan(contentError_);
}

void SceneEditor::openAsset(const assets::AssetEntry& entry) {
    switch (entry.type) {
    case assets::AssetType::Scene:
        requestSceneOpen(entry.path);
        break;
    case assets::AssetType::Material:
        if (assets_.findMaterial(entry.id)) selectMaterial(entry.id);
        else status_ = "Material '" + entry.name + "' belongs to a scene that is not open.";
        break;
    case assets::AssetType::Mesh:
        status_ = "Imported meshes are loaded by the scene that references them.";
        break;
    default:
        status_ = entry.name + " is consumed by scenes and shader parameters.";
        break;
    }
}

void SceneEditor::requestSceneOpen(const std::filesystem::path& path) {
    if (sceneDirty_ || materialsDirty_ || materialDraftDirty_) {
        pendingScenePath_ = path;
        sceneSwitchPrompt_ = true;
        return;
    }
    std::string error;
    if (!openScene(path, error)) status_ = error;
}

void SceneEditor::drawAssets() {
    if (registry_.contentRoot() != project_.contentRoot) refreshContent();
    if (ImGui::Button("Import OBJ...")) importMesh();
    ImGui::SameLine();
    if (ImGui::Button("New folder")) { newFolderBuffer_.clear(); newFolderOpen_ = true; }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshContent();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu assets", registry_.entries().size());
    if (!contentError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.35f, 1.0f), "%s", contentError_.c_str());

    ImGui::Separator();
    if (ImGui::SmallButton("Content")) { contentFolder_.clear(); selectedAsset_.clear(); }
    {
        std::string accumulated;
        std::string_view rest = contentFolder_;
        while (!rest.empty()) {
            const auto slash = rest.find('/');
            const auto piece = rest.substr(0, slash);
            accumulated = accumulated.empty() ? std::string(piece) : accumulated + "/" + std::string(piece);
            ImGui::SameLine(0.0f, 4.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 4.0f);
            const auto label = std::string(piece) + "##crumb-" + accumulated;
            if (ImGui::SmallButton(label.c_str())) { contentFolder_ = accumulated; selectedAsset_.clear(); }
            if (slash == std::string_view::npos) break;
            rest.remove_prefix(slash + 1);
        }
    }

    const float browserHeight = std::max(90.0f, ImGui::GetContentRegionAvail().y - 132.0f);
    ImGui::BeginChild("##content-folders", ImVec2(190.0f, browserHeight), ImGuiChildFlags_Borders);
    drawFolderTreeNode(std::string());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content-assets", ImVec2(0.0f, browserHeight), ImGuiChildFlags_Borders);
    drawAssetRows();
    ImGui::EndChild();
    // In-memory meshes are scene data, not files yet: keep them visible so
    // "Add Actor from mesh" keeps working until the asset format lands.
    if (ImGui::CollapsingHeader("Scene mesh library", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginChild("Mesh library", ImVec2(0.0f, 96.0f), ImGuiChildFlags_Borders);
        for (const auto& mesh : assets_.meshes) {
            const auto label = mesh.name + "##" + std::to_string(mesh.id);
            if (ImGui::Selectable(label.c_str(), selectedMesh_ == mesh.id)) selectedMesh_ = mesh.id;
        }
        ImGui::EndChild();
    }
    drawContentDialogs();
}

void SceneEditor::drawFolderTreeNode(const std::string& folder) {
    std::vector<std::string> children;
    for (const auto& candidate : registry_.folders())
        if (candidate != folder && parentFolder(candidate) == folder) children.push_back(candidate);

    const bool root = folder.empty();
    const auto label = root ? std::string("Content") : leafName(folder);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (contentFolder_ == folder) flags |= ImGuiTreeNodeFlags_Selected;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (root) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const auto id = root ? std::string("Content###content-root") : label + "###folder-" + folder;
    const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        contentFolder_ = folder;
        selectedAsset_.clear();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAE_ASSET")) {
            const std::string source(static_cast<const char*>(payload->Data),
                static_cast<std::size_t>(payload->DataSize));
            std::string error;
            if (assets::moveContentEntry(project_.contentRoot, source, folder, error)) {
                refreshContent();
                contentFolder_ = folder;
                selectedAsset_.clear();
                status_ = "Moved '" + source + "'.";
            } else {
                status_ = error;
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        for (const auto& child : children) drawFolderTreeNode(child);
        ImGui::TreePop();
    }
}

void SceneEditor::drawAssetRows() {
    const auto entries = registry_.inFolder(contentFolder_);
    if (entries.empty()) {
        ImGui::TextDisabled("No assets in this folder. Use Import OBJ or New folder.");
        return;
    }
    for (const auto* entry : entries) {
        ImGui::PushID(entry->relativePath.c_str());
        const float rowHeight = ImGui::GetTextLineHeight() + 10.0f;
        const bool selected = selectedAsset_ == entry->relativePath;
        if (ImGui::Selectable("##asset-row", selected, ImGuiSelectableFlags_AllowDoubleClick,
            ImVec2(0.0f, rowHeight))) {
            selectedAsset_ = entry->relativePath;
            if (ImGui::IsMouseDoubleClicked(0)) openAsset(*entry);
        }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("RAE_ASSET", entry->relativePath.data(), entry->relativePath.size());
            ImGui::TextUnformatted(entry->name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginPopupContextItem("##asset-menu")) {
            if (ImGui::MenuItem("Open")) openAsset(*entry);
            if (ImGui::MenuItem("Rename")) {
                renameTarget_ = entry->relativePath;
                renameBuffer_ = entry->name;
                renameOpen_ = true;
            }
            if (ImGui::MenuItem("Delete")) {
                deleteTarget_ = entry->relativePath;
                deleteOpen_ = true;
            }
            ImGui::EndPopup();
        }
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        drawAssetIcon(draw, ImVec2(rowMin.x + 14.0f, rowMin.y + rowHeight * 0.5f), rowHeight * 0.55f,
            entry->type, color);
        const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 30.0f, textY));
        ImGui::TextUnformatted(entry->name.c_str());
        char buffer[32]{};
        const auto meta = std::string(assets::assetTypeName(entry->type)) + "   " +
            sizeLabel(entry->sizeBytes, buffer, sizeof(buffer));
        const float metaWidth = ImGui::CalcTextSize(meta.c_str()).x;
        ImGui::SetCursorScreenPos(ImVec2(rowMax.x - metaWidth - 6.0f, textY));
        ImGui::TextDisabled("%s", meta.c_str());
        ImGui::SetCursorScreenPos(ImVec2(rowMin.x, rowMax.y));
        ImGui::PopID();
    }
}

void SceneEditor::drawContentDialogs() {
    if (newFolderOpen_) { ImGui::OpenPopup("New folder"); newFolderOpen_ = false; }
    if (ImGui::BeginPopupModal("New folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Create a folder inside the current one.");
        ImGui::SetNextItemWidth(240.0f);
        textInput("##new-folder-name", newFolderBuffer_);
        const bool create = ImGui::Button("Create", ImVec2(90.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            newFolderOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        if (create) {
            const bool valid = !newFolderBuffer_.empty() &&
                newFolderBuffer_.find_first_of(std::string_view("/\\:*?\"<>|")) == std::string::npos;
            if (!valid) {
                status_ = "Folder names cannot be empty or contain path characters.";
            } else {
                std::error_code code;
                const auto target = project_.contentRoot / utf8Path(contentFolder_) / utf8Path(newFolderBuffer_);
                std::filesystem::create_directories(target, code);
                if (code) {
                    status_ = "Cannot create the folder: " + code.message();
                } else {
                    refreshContent();
                    contentFolder_ = contentFolder_.empty() ? newFolderBuffer_
                        : contentFolder_ + "/" + newFolderBuffer_;
                    status_ = "Created folder '" + newFolderBuffer_ + "'.";
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    if (renameOpen_) {
        ImGui::OpenPopup("Rename asset");
        renameOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(240.0f);
        textInput("##rename-asset", renameBuffer_);
        const bool rename = ImGui::Button("Rename", ImVec2(90.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            renameTarget_.clear();
            ImGui::CloseCurrentPopup();
        }
        if (rename && !renameTarget_.empty()) {
            const auto source = project_.contentRoot / utf8Path(renameTarget_);
            std::string extension = pathText(source.extension());
            const bool valid = !renameBuffer_.empty() &&
                renameBuffer_.find_first_of(std::string_view("/\\:*?\"<>|")) == std::string::npos;
            const auto destination = source.parent_path() / utf8Path(renameBuffer_ + extension);
            std::error_code code;
            if (!valid) {
                status_ = "Asset names cannot be empty or contain path characters.";
            } else if (std::filesystem::exists(destination, code)) {
                status_ = "An asset with that name already exists.";
            } else {
                std::filesystem::rename(source, destination, code);
                if (code) {
                    status_ = "Cannot rename the asset: " + code.message();
                } else {
                    const auto folder = pathText(source.parent_path().lexically_relative(project_.contentRoot));
                    refreshContent();
                    selectedAsset_ = folder.empty() ? renameBuffer_ + extension : folder + "/" + renameBuffer_ + extension;
                    status_ = "Renamed to '" + renameBuffer_ + extension + "'.";
                    renameTarget_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }

    if (deleteOpen_) {
        ImGui::OpenPopup("Delete asset");
        deleteOpen_ = false;
    }
    if (ImGui::BeginPopupModal("Delete asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete '%s' from disk?", deleteTarget_.c_str());
        if (ImGui::Button("Delete", ImVec2(90.0f, 0.0f))) {
            std::error_code code;
            std::filesystem::remove(project_.contentRoot / utf8Path(deleteTarget_), code);
            if (code) {
                status_ = "Cannot delete the asset: " + code.message();
            } else {
                status_ = "Deleted '" + deleteTarget_ + "'.";
                if (selectedAsset_ == deleteTarget_) selectedAsset_.clear();
                deleteTarget_.clear();
                refreshContent();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            deleteTarget_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (sceneSwitchPrompt_) {
        ImGui::OpenPopup("Open another scene?");
        sceneSwitchPrompt_ = false;
    }
    if (ImGui::BeginPopupModal("Open another scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("'%s' has unsaved changes.", pathText(scenePath_.filename()).c_str());
        ImGui::TextUnformatted("Open the selected scene and discard them?");
        if (ImGui::Button("Discard & open", ImVec2(130.0f, 0.0f))) {
            std::string error;
            if (!openScene(pendingScenePath_, error)) status_ = error;
            pendingScenePath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            pendingScenePath_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


void SceneEditor::drawInstanceDetails() {
    // Details owns everything about the selected Actor instance: identity,
    // visibility, transform and its mesh/material assignment. Scene-wide
    // parameters moved to the Scene panel.
    auto* actor = world_.findActor(selectedActor_);
    if (!actor) {
        ImGui::TextWrapped("Select an Actor in the viewport or Outliner.");
        return;
    }
    ImGui::TextDisabled("Actor #%llu", static_cast<unsigned long long>(actor->id));
    if (selection_.size() > 1)
        ImGui::TextWrapped("%zu Actors selected; showing primary instance.", selection_.size());
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= textInput("##actorName", actor->name);
    bool actorVisible = actor->visible;
    if (ImGui::Checkbox("Visible", &actorVisible)) setActorVisible(actor->id, actorVisible);
    ImGui::SeparatorText("Transform");
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= ImGui::DragFloat3("##position", actor->transform.position, .03f, -1000, 1000, "%.2f");
    if (ImGui::IsItemActivated() && settledTransformActor_ == actor->id) detailsBefore_ = settledTransform_;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushDetailsTransform(actor->id, detailsBefore_, actor->transform, "Position");
    ImGui::TextDisabled("Position X / Y / Z");
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= ImGui::DragFloat3("##rotation", actor->transform.rotation, .5f, -360, 360, "%.1f");
    if (ImGui::IsItemActivated() && settledTransformActor_ == actor->id) detailsBefore_ = settledTransform_;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushDetailsTransform(actor->id, detailsBefore_, actor->transform, "Rotation");
    ImGui::TextDisabled("Rotation X / Y / Z (degrees)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat3("##scale", actor->transform.scale, .02f, .01f, 100, "%.2f")) {
        for (auto& value : actor->transform.scale) value = std::clamp(value, .01f, 100.0f);
        sceneDirty_ = true;
    }
    if (ImGui::IsItemActivated() && settledTransformActor_ == actor->id) detailsBefore_ = settledTransform_;
    if (ImGui::IsItemDeactivatedAfterEdit()) pushDetailsTransform(actor->id, detailsBefore_, actor->transform, "Scale");
    ImGui::TextDisabled("Scale X / Y / Z");
    ImGui::SeparatorText("Static Mesh Component");
    const auto* mesh = assets_.findMesh(actor->mesh.meshId);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Mesh", mesh ? mesh->name.c_str() : "<missing>")) {
        for (const auto& candidate : assets_.meshes) {
            const std::string label = candidate.name + "##" + std::to_string(candidate.id);
            if (ImGui::Selectable(label.c_str(), actor->mesh.meshId == candidate.id)) {
                actor->mesh.meshId = candidate.id; sceneDirty_ = true;
            }
        }
        ImGui::EndCombo();
    }
    const auto* assigned = assets_.findMaterial(actor->mesh.materialId);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Material", assigned ? assigned->name.c_str() : "<missing>")) {
        for (const auto& candidate : assets_.materials) {
            const std::string label = candidate.name + "##" + std::to_string(candidate.id);
            if (ImGui::Selectable(label.c_str(), actor->mesh.materialId == candidate.id)) {
                actor->mesh.materialId = candidate.id; sceneDirty_ = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(!assigned);
    if (ImGui::Button("Edit assigned material")) selectMaterial(actor->mesh.materialId);
    ImGui::EndDisabled();
    // The assigned-material button is the instance's own entry point into the
    // material workspace; it must follow this Actor, not the shared mesh.
    ImGui::SeparatorText("Assigned material");
    if (const auto* material = assets_.findMaterial(actor->mesh.materialId)) {
        const auto label = material->name + "###InstanceMaterial";
        const float naturalWidth = ImGui::CalcTextSize(material->name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        const float buttonWidth = std::min(naturalWidth, std::max(1.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::Button(label.c_str(), {buttonWidth, 0})) selectMaterial(actor->mesh.materialId);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", material->name.c_str());
    } else {
        ImGui::TextWrapped("Missing material #%llu", static_cast<unsigned long long>(actor->mesh.materialId));
    }
    ImGui::SeparatorText("Mesh");
    if (mesh) {
        ImGui::TextWrapped("Type: %s", meshType(mesh->primitive));
        ImGui::TextWrapped("Vertices: %zu  Triangles: %zu", mesh->vertices.size(), mesh->indices.size() / 3);
        message(mesh->sourcePath.empty() ? "Procedural built-in mesh" : mesh->sourcePath);
    } else {
        ImGui::TextWrapped("Missing mesh #%llu", static_cast<unsigned long long>(actor->mesh.meshId));
    }
}

void SceneEditor::drawMaterial() {
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 105.0f, 40.0f, 240.0f));
    if (ImGui::BeginCombo("Asset", materialDraft_.name.c_str())) {
        for (const auto& material : assets_.materials) {
            const auto label = material.name + "##" + std::to_string(material.id);
            if (ImGui::Selectable(label.c_str(), selectedMaterial_ == material.id)) selectMaterial(material.id);
        }
        ImGui::EndCombo();
    }
    continueButtonRow("New material");
    ImGui::BeginDisabled(materialDraftDirty_);
    if (ImGui::Button("New material")) {
        const auto id = assets_.createMaterial("New Material");
        selectMaterial(id); materialsDirty_ = sceneDirty_ = true;
    }
    ImGui::EndDisabled();
    continueButtonRow("Apply material");
    if (ImGui::Button("Apply material")) applyMaterial();
    continueButtonRow("Discard draft");
    ImGui::BeginDisabled(!materialDraftDirty_);
    if (ImGui::Button("Discard draft")) discardMaterial();
    ImGui::EndDisabled();
    continueButtonRow("Save material assets");
    ImGui::BeginDisabled(materialDraftDirty_);
    if (ImGui::Button("Save material assets")) {
        std::string error;
        if (assets_.saveMaterials(project_.contentRoot / "Materials", error)) {
            materialsDirty_ = false;
            if (saveScene()) status_ = "Saved " + pathText(project_.contentRoot / "Materials") + " and current scene material references.";
        } else status_ = error;
    }
    ImGui::EndDisabled();
    continueButtonRow("Reload material assets");
    ImGui::BeginDisabled(materialDraftDirty_);
    if (ImGui::Button("Reload material assets")) {
        auto proposed = assets_;
        std::string error;
        if (proposed.loadMaterials(project_.contentRoot / "Materials", error)) {
            bool valid = true;
            for (const auto& actor : world_.actors) valid &= proposed.findMaterial(actor.mesh.materialId) != nullptr;
            if (valid) {
                assets_ = std::move(proposed);
                selectedMaterial_ = assets_.materials.front().id;
                materialDraft_ = *assets_.findMaterial(selectedMaterial_);
                materialsDirty_ = false; sceneDirty_ = true;
                status_ = "Reloaded material assets. Save Scene to persist this selection.";
            } else status_ = "Reload rejected: a material referenced by an Actor would be missing.";
        } else status_ = error;
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Parameters preview live. Apply confirms the draft; Save writes assets.");
    if (materialDraftDirty_) ImGui::TextColored({1,.7f,.25f,1}, "Live preview: draft not applied or saved.");
    else ImGui::TextDisabled("No unapplied material changes.");
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 105.0f, 40.0f, 240.0f));
    materialDraftDirty_ |= textInput("Name", materialDraft_.name);
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 105.0f, 40.0f, 240.0f));
    materialDraftDirty_ |= ImGui::ColorEdit3("Base color", materialDraft_.baseColor);
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 105.0f, 40.0f, 240.0f));
    materialDraftDirty_ |= ImGui::SliderFloat("Roughness", &materialDraft_.roughness, .05f, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x >= 600.0f) ImGui::SameLine();
    ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 105.0f, 40.0f, 180.0f));
    materialDraftDirty_ |= ImGui::SliderFloat("Metallic", &materialDraft_.metallic, 0, 1, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    materialDraftDirty_ |= ImGui::Checkbox("Grid", &materialDraft_.grid);
    const auto& style = ImGui::GetStyle();
    const float twoSidedWidth = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Two-sided").x;
    const float contentRight = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + twoSidedWidth <= contentRight) ImGui::SameLine();
    materialDraftDirty_ |= ImGui::Checkbox("Two-sided", &materialDraft_.twoSided);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render both faces and flip back-face normals. Changes preview immediately.");
    ImGui::TextDisabled("Shared shader: %s", pathText(project_.shaderRoot / "Materials/Standard.hlsli").c_str());
    if (pendingMaterial_) {
        ImGui::OpenPopup("Unapplied material draft");
        if (ImGui::BeginPopupModal("Unapplied material draft", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Apply or discard this draft before switching material assets.");
            bool change = false;
            if (ImGui::Button("Apply & switch")) change = applyMaterial();
            ImGui::SameLine();
            if (ImGui::Button("Discard & switch")) { discardMaterial(); change = true; }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { pendingMaterial_ = 0; ImGui::CloseCurrentPopup(); }
            if (change) {
                const auto id = pendingMaterial_; pendingMaterial_ = 0; selectMaterial(id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void SceneEditor::drawShader(renderer::SceneRenderer& renderer) {
    const bool unapplied = shaderDraft_ != renderer.shaderSource();
    ImGui::TextDisabled("%s  |  revision %llu%s", pathText(renderer.materialShaderPath()).c_str(),
        static_cast<unsigned long long>(renderer.shaderRevision()), unapplied ? "  |  unapplied draft" : "");
    if (ImGui::Button("Apply / Compile")) {
        const auto started = std::chrono::steady_clock::now();
        if (renderer.applyShaderSource(shaderDraft_, diagnostics_)) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            status_ = "Shader applied in " + std::to_string(static_cast<int>(ms)) + " ms. Save to persist.";
            if (diagnostics_.empty()) diagnostics_ = "Compile succeeded. All forward/deferred stages replaced together.";
        } else status_ = "Compile failed. Previous working shader remains active.";
    }
    continueButtonRow("Save shader");
    ImGui::BeginDisabled(unapplied);
    if (ImGui::Button("Save shader")) {
        std::string error;
        if (renderer.saveShaderSource(error)) { shaderSaved_ = renderer.shaderSource(); status_ = "Saved " + pathText(renderer.materialShaderPath()); }
        else status_ = error;
    }
    ImGui::EndDisabled();
    continueButtonRow("Reload from disk");
    if (ImGui::Button("Reload from disk")) shaderReloadRequested_ = true;
    continueButtonRow("Revert draft");
    if (ImGui::Button("Revert draft")) shaderDraft_ = renderer.shaderSource();

    if (shaderReloadRequested_) {
        ImGui::OpenPopup("Reload shader source?");
        if (ImGui::BeginPopupModal("Reload shader source?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace the editor draft and active shader with the saved source?");
            if (ImGui::Button("Reload & compile")) {
                if (renderer.reloadShaders(diagnostics_)) {
                    shaderDraft_ = shaderSaved_ = renderer.shaderSource();
                    status_ = "Shader reloaded from disk.";
                } else status_ = "Reload failed. Current compiled shader retained.";
                shaderReloadRequested_ = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { shaderReloadRequested_ = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    const float available = ImGui::GetContentRegionAvail().y;
    ImGui::InputTextMultiline("##Shader source", shaderDraft_.data(), shaderDraft_.capacity() + 1,
        {-1, std::max(80.0f, available - 68.0f)},
        ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput, resizeText, &shaderDraft_);
    ImGui::BeginChild("Compiler output", {0, 0}, ImGuiChildFlags_Borders);
    ImGui::TextUnformatted(diagnostics_.empty() ? "Edit EvaluateMaterial() and click Apply. Failed compilation never replaces the active shader." : diagnostics_.c_str());
    ImGui::EndChild();
}

void SceneEditor::drawMaterialWindow(renderer::SceneRenderer& renderer) {
    dockManager_.keepAlive();
    materialWindowFocused_ = false;
    if (!materialWindowOpen_) return;

    const auto* mainViewport = ImGui::GetMainViewport();
    const bool nativeWindow = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
    if (nativeWindow) {
        ImGuiWindowClass windowClass;
        windowClass.ParentViewportId = 0; // A separate taskbar window survives minimizing the scene window.
        windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        windowClass.ViewportFlagsOverrideClear = ImGuiViewportFlags_NoDecoration | ImGuiViewportFlags_NoTaskBarIcon;
        ImGui::SetNextWindowClass(&windowClass);
    }

    ImVec2 workPos = mainViewport->WorkPos, workSize = mainViewport->WorkSize;
    const auto center = mainViewport->GetCenter();
    for (const auto& monitor : ImGui::GetPlatformIO().Monitors) {
        if (center.x >= monitor.MainPos.x && center.x < monitor.MainPos.x + monitor.MainSize.x &&
            center.y >= monitor.MainPos.y && center.y < monitor.MainPos.y + monitor.MainSize.y) {
            workPos = monitor.WorkPos;
            workSize = monitor.WorkSize;
            break;
        }
    }
    const ImVec2 initialSize{std::min(880.0f, std::max(280.0f, workSize.x * .85f)),
                             std::min(650.0f, std::max(220.0f, workSize.y * .85f))};
    ImGui::SetNextWindowSize(initialSize, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos({workPos.x + (workSize.x - initialSize.x) * .5f,
                             workPos.y + (workSize.y - initialSize.y) * .5f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({280, 220}, {FLT_MAX, FLT_MAX});
    if (materialWindowFocusRequested_) ImGui::SetNextWindowFocus();
    const auto title = std::string("Material Editor - ") + materialDraft_.name + "###MaterialWorkspace";
    const auto flags = ImGuiWindowFlags_NoDocking | (nativeWindow ? ImGuiWindowFlags_NoTitleBar : 0);
    const bool visible = ImGui::Begin(title.c_str(), &materialWindowOpen_, flags);
    materialWindowFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const auto* toolViewport = ImGui::GetWindowViewport();
    auto* nativeHandle = static_cast<HWND>(toolViewport->PlatformHandleRaw ? toolViewport->PlatformHandleRaw : toolViewport->PlatformHandle);
    if (nativeWindow && toolViewport->ID != mainViewport->ID && nativeHandle)
        materialWindowFocused_ |= GetForegroundWindow() == nativeHandle;
    if (materialWindowFocusRequested_) {
        if (nativeWindow && toolViewport->ID != mainViewport->ID && nativeHandle && IsWindow(nativeHandle)) {
            if (IsIconic(nativeHandle)) ShowWindow(nativeHandle, SW_RESTORE);
            SetForegroundWindow(nativeHandle);
        }
        materialWindowFocusRequested_ = false;
    }
    if (visible && materialWindowOpen_) {
        ImGui::TextWrapped("Closing this window keeps your drafts and live preview. Reopen it to continue; use Save to persist changes.");
        if (materialsDirty_) ImGui::TextDisabled("Material assets have unsaved changes.");
        message(status_);
        if (ImGui::BeginTabBar("Editors", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
            if (ImGui::BeginTabItem("Material Editor", nullptr, openMaterialTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                openMaterialTab_ = false;
                drawMaterial();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Shader Editor", nullptr, openShaderTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
                openShaderTab_ = false;
                drawShader(renderer);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void SceneEditor::draw(renderer::SceneSettings& settings, renderer::SceneRenderer& renderer, bool& vsync, bool& capture) {
    actorPanelFocused_ = false;
    // Editor-only selection outline: the renderer reads this borrowed list for
    // this frame only. Colour follows the active theme accent so Black and Dark
    // Blue keep a consistent selection language.
    const auto& preferences = imgui::uiPreferences();
    const bool outlineVisible = preferences.selectionOutline && !selection_.empty();
    settings.outlineActors = outlineVisible ? selection_.data() : nullptr;
    settings.outlineActorCount = outlineVisible ? static_cast<std::uint32_t>(selection_.size()) : 0;
    const ImVec4 accent = preferences.outlineThemeColor
        ? ImGui::GetStyleColorVec4(ImGuiCol_CheckMark)
        : ImVec4(preferences.outlineColor[0], preferences.outlineColor[1], preferences.outlineColor[2], 1.0f);
    settings.outlineColor[0] = accent.x;
    settings.outlineColor[1] = accent.y;
    settings.outlineColor[2] = accent.z;
    settings.outlineColor[3] = 1.0f;
    settings.outlineThickness = preferences.outlineThickness;
    settings.outlineFrontObjects = preferences.outlineFrontObjects;
    const auto* mainViewport = ImGui::GetMainViewport();
    // BeginMenu needs a native horizontal menu bar. SameLine in a normal
    // window leaves menus in vertical mode, consuming the remaining width.
    // Reserve the menu decoration plus one padded row of rendering controls.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowHeight = ImGui::GetFrameHeight();
    const float toolbarHeight = rowHeight * 2.0f + style.WindowPadding.y * 2.0f + style.WindowBorderSize * 2.0f;
    dockManager_.beginFrame(toolbarHeight);
    ImGui::SetNextWindowViewport(mainViewport->ID);
    ImGui::SetNextWindowPos(mainViewport->WorkPos);
    ImGui::SetNextWindowSize({mainViewport->WorkSize.x, toolbarHeight});
    ImGui::Begin("RestArtEngine Editor", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar);
    if (ImGui::BeginMenuBar()) {
        dockManager_.drawLayoutMenu();
        drawEditMenu();
        drawUiMenu();
        commandButton("scene.save");
        if (ImGui::Button("Load Scene")) sceneReloadRequested_ = true;
        if (ImGui::Button("Save All")) saveAll(renderer);
        ImGui::TextDisabled("%s", sceneDirty_ ? "Scene *" : "Scene saved");
        ImGui::EndMenuBar();
    }
    // EndMenuBar restores the content cursor below the menu decoration.
    ImGui::SetNextItemWidth(110);
    int path = settings.path == renderer::RenderPath::Deferred ? 1 : 0;
    if (ImGui::Combo("##pipeline", &path, "Forward\0Deferred\0"))
        settings.path = path ? renderer::RenderPath::Deferred : renderer::RenderPath::Forward;
    ImGui::SameLine(); ImGui::SetNextItemWidth(135);
    ImGui::Combo("##view", &settings.debugView, "Lit\0Unlit\0Albedo\0World normals\0World position\0");
    ImGui::SameLine(); ImGui::Checkbox("Ray tracing", &settings.rayTracing);
    ImGui::SameLine(); capture |= ImGui::Button("Capture");
    ImGui::SameLine();
    ImGui::TextDisabled("Project: %s | DX11 / %s | %.0f FPS | F1 UI, F2 pipeline, F3 rays, F11 fullscreen",
        project_.name.c_str(), renderer.adapterName().c_str(), ImGui::GetIO().Framerate);
    ImGui::End();

    drawGarbageCollection();
    drawOutlineSettings();

    sceneDirty_ |= viewport_.draw(settings, world_, assets_, selection_, selectedActor_, renderer,
        [this] { duplicateForDrag(); }, materialPreview());
    if (selectedActor_ != sceneInspectorActor_ || viewport_.pickedThisFrame())
        dockManager_.requestFocus(EditorPanel::Details);
    sceneInspectorActor_ = selectedActor_;
    trackTransformEdit();

    dockManager_.drawPanel(EditorPanel::Outliner, [&] { drawOutliner(); });
    dockManager_.drawPanel(EditorPanel::Scene, [&] {
        drawScene();
        drawSceneLighting(settings);
        actorPanelFocused_ |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    });
    dockManager_.drawPanel(EditorPanel::Details, [&] {
        // Actor and scene status live with the inspector so the effect of an
        // edit is visible next to the properties that produced it.
        ImGui::SeparatorText("Activity");
        message(status_);
        drawInstanceDetails();
        actorPanelFocused_ |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    });
    dockManager_.drawPanel(EditorPanel::Settings, [&] {
        ImGui::SeparatorText("View");
        ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##Exposure", &settings.exposure, .25f, 3, "Exposure %.2f");
        ImGui::TextDisabled("Final exposure applied after lighting and post processing.");
        ImGui::Checkbox("VSync", &vsync);
        if (ImGui::Button("Reset camera")) {
            settings.cameraYaw=.65f; settings.cameraPitch=.3f; settings.cameraDistance=6;
            settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1; settings.cameraTarget[2] = 0;
        }
    });

    dockManager_.drawPanel(EditorPanel::Content, [&] {
        if (ImGui::BeginTabBar("Editors", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs)) {
            if (ImGui::BeginTabItem("Meshes")) { drawAssets(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    });
    drawMaterialWindow(renderer);
    // Evaluate after the tool window has submitted this frame's focus state.
    // Its text edits and shortcuts must never mutate the selected scene Actor.
    if (!garbageCollectionFocused_ && !outlineSettingsFocused_ && !materialWindowFocused_ && !ImGui::GetIO().WantTextInput && !viewport_.isUsing() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        commands_.processShortcuts();
    if (!garbageCollectionFocused_ && !outlineSettingsFocused_ && !materialWindowFocused_ && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && !viewport_.isUsing() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        (actorPanelFocused_ || viewport_.isHovered() || viewport_.isFocused()) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        deleteSelectedActor();
    if (sceneReloadRequested_) {
        ImGui::OpenPopup("Load saved scene?");
        if (ImGui::BeginPopupModal("Load saved scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace current Actors and materials with the saved scene?\nUnapplied material edits will be discarded.");
            if (ImGui::Button("Load & replace")) {
                std::string error;
                if (world_.load(scenePath_, assets_, error)) {
                    // History entries reference Actors from the replaced scene.
                    undo_.clear();
                    selectedActor_ = world_.actors.empty() ? 0 : world_.actors.front().id;
                    sceneInspectorActor_ = selectedActor_;
    selection_.assign(selectedActor_ ? 1 : 0, selectedActor_);
                    materialDraftDirty_ = materialsDirty_ = sceneDirty_ = false;
                    pendingMaterial_ = 0;
                    materialDraft_ = {};
                    selectedMesh_ = assets_.meshes.empty() ? 0 : assets_.meshes.front().id;
                    // Refresh session data without a user-open/focus request.
                    // Loading a scene preserves the tool's visibility and tab.
                    selectedMaterial_ = assets_.materials.empty() ? 0 : assets_.materials.front().id;
                    if (const auto* material = assets_.findMaterial(selectedMaterial_)) materialDraft_ = *material;
                    status_ = "Scene loaded.";
                } else status_ = error;
                sceneReloadRequested_ = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { sceneReloadRequested_ = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
    // Outliner/commands can change the selection after the viewport was drawn.
    // Queue their activation once, then retain the final selection as baseline.
    if (selectedActor_ != sceneInspectorActor_)
        dockManager_.requestFocus(EditorPanel::Details);
    sceneInspectorActor_ = selectedActor_;
}

bool SceneEditor::drawCloseDialog(renderer::SceneRenderer& renderer) {
    if (!closeRequested_) return false;
    if (!dirty(renderer)) return true;
    bool exit = false;
    ImGui::OpenPopup("Unsaved editor changes");
    if (ImGui::BeginPopupModal("Unsaved editor changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("There are unsaved scene, material or shader changes.");
        if (ImGui::Button("Save All & Exit")) {
            exit = saveAll(renderer);
            if (exit) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard & Exit")) { exit = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { closeRequested_ = false; ImGui::CloseCurrentPopup(); }
        message(status_);
        ImGui::EndPopup();
    }
    return exit;
}

} // namespace rae::editor

