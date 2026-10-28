#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/Assets/AssetFile.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

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
bool readOnlyMaterial(const assets::MaterialAsset& material) {
    return material.origin == assets::AssetOrigin::Engine || assets::isEngineAssetId(material.id);
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
    if (!assets_.loadEngineMaterials(project_.engineRoot / "Content/Materials", error)) return false;
    // Materials are the single source of truth for scene v2 references: load the
    // content folder first so the scene can resolve its GUIDs.
    {
        std::string materialError;
        const auto materialFolder = project_.contentRoot / "Materials";
        if (std::filesystem::is_directory(materialFolder) && !assets_.loadMaterials(materialFolder, materialError)) {
            error = materialError;
            return false;
        }
    }
    {
        std::string meshError;
        const auto meshFolder = project_.contentRoot / "Meshes";
        const auto builtinFolder = meshFolder / "Builtin";
        std::filesystem::create_directories(builtinFolder);
        // Built-in primitives become real assets so the Content browser lists
        // them alongside imported meshes; existing files win. They live in their
        // own generated folder so user mesh assets stay easy to tell apart.
        if (!assets_.ensureBuiltinMeshes(builtinFolder, meshError) || !assets_.loadMeshes(meshFolder, meshError)) {
            error = meshError;
            return false;
        }
    }
    // Scene v3 refers to mesh GUIDs. Resolve the complete Content library first.
    world_.createDefaultScene(assets_);
    scenePath_ = project_.defaultScene;
    if (std::filesystem::exists(scenePath_) && !world_.load(scenePath_, assets_, error)) return false;
    refreshContent();
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
            selectedActor_ = world_.createActor("Sphere", 1, assets::EngineDefaultMaterialId);
            world_.findActor(selectedActor_)->transform.position[1] = 1;
            selection_.assign(1, selectedActor_);
            pushActorCreated(*world_.findActor(selectedActor_), "Create Sphere");
            sceneDirty_ = true;
        },
        [this] { return assets_.findMesh(1) != nullptr && assets_.findMaterial(assets::EngineDefaultMaterialId) != nullptr; });
    commands_.add("actor.addCube", "actor", "+ Cube", "", 0,
        [this] {
            selectedActor_ = world_.createActor("Cube", 3, assets::EngineDefaultMaterialId);
            world_.findActor(selectedActor_)->transform.position[1] = 1;
            selection_.assign(1, selectedActor_);
            pushActorCreated(*world_.findActor(selectedActor_), "Create Cube");
            sceneDirty_ = true;
        },
        [this] { return assets_.findMesh(3) != nullptr && assets_.findMaterial(assets::EngineDefaultMaterialId) != nullptr; });
    commands_.add("scene.save", "scene", "Save Scene", "Ctrl+S", ImGuiMod_Ctrl | ImGuiKey_S,
        [this] { saveScene(); });
    commands_.add("actor.addFromMesh", "actor", "Add Actor from mesh", "", 0,
        [this] {
            scene::Transform transform;
            transform.position[1] = 1;
            createMeshActor(selectedMesh_, transform, "Create Actor from mesh");
        },
        [this] { return assets_.findMesh(selectedMesh_) && assets_.findMaterial(assets::EngineDefaultMaterialId); });
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
    const auto* material = assets_.findMaterial(selectedMaterial_);
    if (!materialDraftDirty_ || materialDraft_.id != selectedMaterial_ ||
        !material || readOnlyMaterial(*material) || !validMaterialParameters(materialDraft_)) return nullptr;
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
    if (readOnlyMaterial(*target)) { status_ = "Engine materials are read-only. Copy to Project to edit."; return false; }
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

bool SceneEditor::saveScene(renderer::SceneRenderer* renderer) {
    std::string error;
    // Persist materials before the scene so its GUID references always resolve.
    if (!assets_.saveMaterials(project_.contentRoot / "Materials", error)) {
        status_ = "Material save failed: " + error; return false;
    }
    if (!assets_.saveMeshes(project_.contentRoot / "Meshes", error)) {
        status_ = "Mesh save failed: " + error; return false;
    }
    if (!world_.save(scenePath_, assets_, error)) {
        status_ = "Scene save failed: " + error; return false;
    }
    sceneDirty_ = false;
    // Produce the GPU scene thumbnail next to the scene so the Content browser can
    // show a real render; the CPU layout stays as a fallback when it is missing.
    if (renderer) {
        std::string previewError;
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0, height = 0;
        if (renderer->renderScenePreview(world_, assets_, 128, previewError) &&
            renderer->readPreviewPixels(renderer::PreviewSlot::SceneThumbnail, pixels, width, height, previewError) && width > 0 && height > 0) {
            assets::Thumbnail thumbnail;
            thumbnail.width = width;
            thumbnail.height = height;
            thumbnail.rgba = std::move(pixels);
            auto sidecarPath = scenePath_;
            sidecarPath += ".rthumb";
            (void)assets::writeSceneThumbnail(sidecarPath, thumbnail, previewError);
        }
    }
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
    if (!saveScene(&renderer)) return false;
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

bool SceneEditor::createMeshActor(assets::AssetId meshId, const scene::Transform& transform, const std::string& label) {
    const auto* mesh = assets_.findMesh(meshId);
    if (!mesh || !assets_.findMaterial(assets::EngineDefaultMaterialId)) return false;
    selectedActor_ = world_.createActor(mesh->name, meshId, assets::EngineDefaultMaterialId);
    auto& actor = *world_.findActor(selectedActor_);
    actor.transform = transform;
    selection_.assign(1, selectedActor_);
    pushActorCreated(actor, label);
    sceneDirty_ = true;
    status_ = "Placed '" + actor.name + "'. Use Move [W] handles to reposition.";
    return true;
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
    auto& preferences = imgui::uiPreferences();
    if (ImGui::MenuItem("Gizmo negative-axis hints", nullptr, &preferences.gizmoNegativeAxisHints))
        imgui::saveUiPreferences(imgui::defaultUiPreferencesPath(), preferences);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Show stripes when a move or scale axis flips to its negative direction.");
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
    if (assets::isEngineContentPath(folder) && slash < assets::EngineContentPrefix.size())
        return std::string(assets::EngineContentPrefix);
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

// Draws a decoded thumbnail as a small block grid. Avoids a texture cache while
// keeping the asset rows readable at 16-24px.
void drawThumbnail(ImDrawList* list, ImVec2 topLeft, float size, const assets::Thumbnail& thumbnail) {
    if (!thumbnail.valid()) return;
    const int steps = std::max(8, static_cast<int>(size));
    for (int y = 0; y < steps; ++y) {
        const int sy = y * static_cast<int>(thumbnail.height) / steps;
        for (int x = 0; x < steps; ++x) {
            const int sx = x * static_cast<int>(thumbnail.width) / steps;
            const auto* pixel = thumbnail.rgba.data() +
                (static_cast<std::size_t>(sy) * thumbnail.width + static_cast<std::size_t>(sx)) * 4;
            if (pixel[3] == 0) continue;
            const ImVec2 a{topLeft.x + size * x / steps, topLeft.y + size * y / steps};
            const ImVec2 b{topLeft.x + size * (x + 1) / steps, topLeft.y + size * (y + 1) / steps};
            list->AddRectFilled(a, b, IM_COL32(pixel[0], pixel[1], pixel[2], 255));
        }
    }
}

} // namespace

void SceneEditor::drawOutliner() {
    ImGui::BeginDisabled(!assets_.findMesh(1) || !assets_.findMaterial(assets::EngineDefaultMaterialId));
    commandButton("actor.addSphere");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!assets_.findMesh(3) || !assets_.findMaterial(assets::EngineDefaultMaterialId));
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
    if (assets::isEngineContentPath(contentFolder_)) { status_ = "Choose Project Content to import assets."; return; }
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"Wavefront OBJ (*.obj)\0*.obj\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    try {
        // Validate into an isolated copy, then persist a self-contained .rasset.
        auto proposed = assets_;
        assets::AssetId imported = 0;
        std::string error;
        if (!proposed.importObj(path, imported, error)) { status_ = error; return; }
        auto* mesh = proposed.findMesh(imported);
        mesh->id = assets::generateAssetId();
        const auto utf8Path = std::filesystem::path(path).generic_u8string();
        mesh->sourcePath.assign(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
        imported = mesh->id;
        const auto folder = project_.contentRoot / "Meshes";
        std::filesystem::create_directories(folder);
        if (!proposed.saveMeshes(folder, error)) { status_ = error; return; }
        assets_ = std::move(proposed);
        selectedMesh_ = imported;
        sceneDirty_ = true;
        refreshContent();
        status_ = "OBJ imported as a .rasset in Content/Meshes. Add an Actor from this mesh, then save.";
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
    stagedAssets.createDefaults();
    if (!stagedAssets.loadEngineMaterials(project_.engineRoot / "Content/Materials", error)) return false;
    {
        std::string assetError;
        const auto materialFolder = project_.contentRoot / "Materials";
        const auto meshFolder = project_.contentRoot / "Meshes";
        if ((std::filesystem::is_directory(materialFolder) && !stagedAssets.loadMaterials(materialFolder, assetError)) ||
            (std::filesystem::is_directory(meshFolder) && !stagedAssets.loadMeshes(meshFolder, assetError))) {
            error = assetError;
            return false;
        }
    }
    if (!stagedWorld.load(path, stagedAssets, error)) return false;
    world_ = std::move(stagedWorld);
    assets_ = std::move(stagedAssets);
    scenePath_ = path;
    sceneDirty_ = false;
    materialsDirty_ = false;
    materialDraftDirty_ = false;
    pendingMaterial_ = 0;
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

void SceneEditor::selectContentFolder(std::string folder) {
    if (folder == ".") folder.clear();
    if (std::find(registry_.folders().begin(), registry_.folders().end(), folder) == registry_.folders().end()) return;
    if (assets::isEngineContentPath(folder)) contentEngineVisible_ = true;
    if (folder == contentFolder_) return;
    contentFolder_ = std::move(folder);
    contentRevealFolder_ = true;
    selectedAsset_.clear();
    contentHistory_.resize(contentHistoryIndex_ + 1);
    contentHistory_.push_back(contentFolder_);
    contentHistoryIndex_ = contentHistory_.size() - 1;
}

std::vector<const assets::AssetEntry*> SceneEditor::visibleContentEntries() const {
    auto query = contentQuery_;
    query.folder = contentFolder_;
    return registry_.query(query);
}

void SceneEditor::selectContentAsset(std::string path) {
    if (registry_.find(path)) selectedAsset_ = std::move(path);
}

bool SceneEditor::copyMaterialToProject(assets::AssetId sourceId, std::string& error) {
    error.clear();
    if (materialDraftDirty_) {
        error = "Apply or discard the current material draft before copying another asset.";
        return false;
    }
    const auto* source = assets_.findMaterial(sourceId);
    if (!source) { error = "The source material is not loaded."; return false; }
    std::filesystem::path staging;
    bool ownsStaging = false;
    try {
        auto proposed = assets_;
        const auto id = proposed.cloneMaterial(sourceId, source->name + " Copy");
        auto* copy = proposed.findMaterial(id);
        const auto folder = project_.contentRoot / "Materials";
        const auto resolvedRoot = std::filesystem::weakly_canonical(project_.contentRoot);
        const auto resolvedFolder = std::filesystem::weakly_canonical(folder);
        const auto relative = resolvedFolder.lexically_relative(resolvedRoot);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            error = "Project Materials must remain inside Project Content.";
            return false;
        }
        const auto resolvedEngine = std::filesystem::weakly_canonical(project_.engineRoot / "Content");
        const auto fromEngine = resolvedFolder.lexically_relative(resolvedEngine);
        if (resolvedFolder == resolvedEngine || (!fromEngine.empty() && !fromEngine.is_absolute() && *fromEngine.begin() != "..")) {
            error = "Cannot copy into Engine Content. Choose a separate project.";
            return false;
        }
        std::filesystem::create_directories(folder);
        const auto baseName = assets::sanitizeAssetName(copy->name);
        std::string filename;
        for (std::size_t suffix = 1;; ++suffix) {
            copy->name = baseName + (suffix == 1 ? std::string() : " " + std::to_string(suffix));
            filename = copy->name + ".rasset";
            const bool nameInUse = std::any_of(assets_.materials.begin(), assets_.materials.end(), [&](const auto& material) {
                return material.name == copy->name;
            });
            if (!nameInUse && !std::filesystem::exists(folder / utf8Path(filename)) &&
                !std::filesystem::exists(folder / utf8Path(copy->name + ".ramat")) &&
                !std::filesystem::exists(folder / utf8Path(copy->name + ".raasset"))) break;
        }
        const auto destination = folder / utf8Path(filename);
        staging = folder / (".copy-" + assets::assetIdText(id));
        if (!std::filesystem::create_directory(staging)) {
            error = "Cannot reserve a temporary folder for the material copy.";
            return false;
        }
        ownsStaging = true;
        const auto temporary = staging / "material.rasset";
        if (!assets::writeAssetFile(temporary, assets::makeMaterialAssetFile(*copy), error)) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(staging, ignored);
            return false;
        }
        // Publish only the new file, without replacing an existing destination.
        // Other project materials and unapplied drafts are never saved here.
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            error = "Cannot publish the material copy: " + std::system_category().message(GetLastError());
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            std::filesystem::remove(staging, ignored);
            return false;
        }
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        staging.clear();
        assets_ = std::move(proposed);
        refreshContent();
        selectContentFolder("Materials");
        selectedAsset_ = "Materials/" + filename;
        selectMaterial(id);
        status_ = "Copied material to Project Content. Assign the copy to an Actor to use your edits.";
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        if (ownsStaging && !staging.empty()) {
            std::error_code ignored;
            std::filesystem::remove(staging / "material.rasset", ignored);
            std::filesystem::remove(staging, ignored);
        }
        return false;
    }
}

bool SceneEditor::navigateContentBack() {
    if (contentHistoryIndex_ == 0) return false;
    contentFolder_ = contentHistory_[--contentHistoryIndex_];
    if (assets::isEngineContentPath(contentFolder_)) contentEngineVisible_ = true;
    contentRevealFolder_ = true;
    selectedAsset_.clear();
    return true;
}

bool SceneEditor::navigateContentForward() {
    if (contentHistoryIndex_ + 1 >= contentHistory_.size()) return false;
    contentFolder_ = contentHistory_[++contentHistoryIndex_];
    if (assets::isEngineContentPath(contentFolder_)) contentEngineVisible_ = true;
    contentRevealFolder_ = true;
    selectedAsset_.clear();
    return true;
}

bool SceneEditor::navigateContentUp() {
    if (contentFolder_.empty() || contentFolder_ == assets::EngineContentPrefix) return false;
    selectContentFolder(parentFolder(contentFolder_));
    return true;
}

void SceneEditor::refreshContent() {
    registry_.setContentRoot(project_.contentRoot);
    registry_.setEngineContentRoot(project_.engineRoot / "Content");
    registry_.scan(contentError_);
    if (!selectedAsset_.empty() && !registry_.find(selectedAsset_)) selectedAsset_.clear();
    if (std::find(registry_.folders().begin(), registry_.folders().end(), contentFolder_) == registry_.folders().end())
        selectContentFolder(std::string());
}

void SceneEditor::activateAsset(const assets::AssetEntry& entry) {
    switch (entry.type) {
    case assets::AssetType::Scene:
        if (entry.readOnly()) status_ = "Engine scenes are read-only; open a project scene to edit Actors.";
        else requestSceneOpen(entry.path);
        break;
    case assets::AssetType::Material:
        if (assets_.findMaterial(entry.id)) selectMaterial(entry.id);
        else status_ = "Material '" + entry.name + "' belongs to a scene that is not open.";
        break;
    case assets::AssetType::Mesh:
        if (assets_.findMesh(entry.id)) {
            selectedMesh_ = entry.id;
            status_ = "Mesh selected: use Add Actor from mesh.";
        } else {
            status_ = "This mesh is not loaded from Content/Meshes.";
        }
        break;
    default:
        status_ = entry.name + " is consumed by scenes and shader parameters.";
        break;
    }
}

void SceneEditor::showAssetDetails(std::string relativePath) {
    if (relativePath.empty()) { closeAssetDetails(); return; }
    assetDetailsError_.clear();
    assetDetailsPath_ = std::move(relativePath);
    assetDetailsOpen_ = true;
    assetDetailsFocusRequested_ = true;
    assetDetailsAge_ = 1.0f;
}

void SceneEditor::openAssetDetails(const assets::AssetEntry& entry) {
    assetDetailsError_.clear();
    assetDetailsPath_ = entry.relativePath;
    assetDetailsOpen_ = true;
    assetDetailsFocusRequested_ = true;
    assetDetailsAge_ = 1.0f;
}

void SceneEditor::drawAssetDetails(renderer::SceneRenderer& renderer) {
    assetDetailsFocused_ = false;
    if (!assetDetailsOpen_) return;
    const auto* entry = registry_.find(assetDetailsPath_);
    if (!entry) {
        assetDetailsOpen_ = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(560.0f, 330.0f), ImGuiCond_FirstUseEver);
    if (assetDetailsFocusRequested_) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + (viewport->WorkSize.x - 560.0f) * 0.5f,
            viewport->WorkPos.y + (viewport->WorkSize.y - 330.0f) * 0.5f), ImGuiCond_Always);
        ImGui::SetNextWindowFocus();
        assetDetailsFocusRequested_ = false;
    }
    const auto title = std::string("Asset Details - ") + entry->name + "###AssetDetails";
    if (!ImGui::Begin(title.c_str(), &assetDetailsOpen_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    assetDetailsFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    const ImVec2 previewOrigin = ImGui::GetCursorScreenPos();
    const float previewSize = 190.0f;
    ImGui::Dummy(ImVec2(previewSize, previewSize));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(previewOrigin, ImVec2(previewOrigin.x + previewSize, previewOrigin.y + previewSize),
        ImGui::GetColorU32(ImGuiCol_Border));

    // Preview: the material ball and mesh use the GPU preview target; scenes and
    // unknown files fall back to the stored thumbnail.
    bool drewPreview = false;
    if (entry->type == assets::AssetType::Material) {
        const auto* material = assets_.findMaterial(entry->id);
        if (material) {
            assetDetailsAge_ += ImGui::GetIO().DeltaTime;
            if (assetDetailsAge_ > 0.3f) {
                (void)renderer.renderMaterialPreview(*material, static_cast<std::uint32_t>(previewSize),
                    assetDetailsError_, renderer::PreviewSlot::AssetDetails);
                assetDetailsAge_ = 0.0f;
            }
            if (auto* texture = renderer.previewTexture(renderer::PreviewSlot::AssetDetails)) {
                draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
                    previewOrigin, ImVec2(previewOrigin.x + previewSize, previewOrigin.y + previewSize));
                drewPreview = true;
            }
        }
    } else if (entry->type == assets::AssetType::Mesh) {
        const auto* mesh = assets_.findMesh(entry->id);
        if (mesh) {
            scene::World previewWorld;
            previewWorld.createActor(mesh->name, mesh->id, assets::EngineDefaultMaterialId);
            assetDetailsAge_ += ImGui::GetIO().DeltaTime;
            if (assetDetailsAge_ > 0.3f) {
                (void)renderer.renderScenePreview(previewWorld, assets_,
                    static_cast<std::uint32_t>(previewSize), assetDetailsError_, renderer::PreviewSlot::AssetDetails);
                assetDetailsAge_ = 0.0f;
            }
            if (auto* texture = renderer.previewTexture(renderer::PreviewSlot::AssetDetails)) {
                draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
                    previewOrigin, ImVec2(previewOrigin.x + previewSize, previewOrigin.y + previewSize));
                drewPreview = true;
            }
        }
    }
    if (!drewPreview) {
        if (entry->thumbnail.valid()) {
            drawThumbnail(draw, ImVec2(previewOrigin.x + 15.0f, previewOrigin.y + 15.0f),
                previewSize - 30.0f, entry->thumbnail);
        } else {
            drawAssetIcon(draw, ImVec2(previewOrigin.x + previewSize * 0.5f, previewOrigin.y + previewSize * 0.5f),
                previewSize * 0.3f, entry->type, ImGui::GetColorU32(imgui::assetTypeColor(entry->type)));
        }
    }

    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("Name: %s", entry->name.c_str());
    ImGui::TextColored(imgui::assetTypeColor(entry->type), "Type: %s", assets::assetTypeName(entry->type));
    ImGui::Text("GUID: %s", entry->id ? assets::assetIdText(entry->id).c_str() : "-");
    ImGui::TextDisabled("Source: %s", entry->readOnly() ? "Engine Content (read-only)" : "Project Content");
    ImGui::TextWrapped("Path: %s", entry->relativePath.c_str());
    char sizeBuffer[32]{};
    ImGui::Text("Size: %s", sizeLabel(entry->sizeBytes, sizeBuffer, sizeof(sizeBuffer)));

    if (entry->type == assets::AssetType::Mesh) {
        if (const auto* mesh = assets_.findMesh(entry->id)) {
            ImGui::TextDisabled("Geometry");
            ImGui::Text("%zu vertices / %zu triangles", mesh->vertices.size(), mesh->indices.size() / 3);
        }
    } else if (entry->type == assets::AssetType::Material) {
        if (const auto* material = assets_.findMaterial(entry->id)) {
            ImGui::TextDisabled("Parameters");
            ImGui::ColorButton("##base-color", ImVec4(material->baseColor[0], material->baseColor[1],
                material->baseColor[2], 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(26.0f, 16.0f));
            ImGui::SameLine();
            ImGui::Text("base color, roughness %.2f, metallic %.2f", material->roughness, material->metallic);
        }
    } else if (entry->type == assets::AssetType::Scene) {
        ImGui::TextDisabled("Scene");
        ImGui::TextWrapped("Open the scene to edit its Actors. The preview uses the saved thumbnail.");
    }
    if (!assetDetailsError_.empty()) ImGui::TextDisabled("%s", assetDetailsError_.c_str());
    ImGui::Spacing();
    const auto copyId = entry->readOnly() && entry->type == assets::AssetType::Material ? entry->id : 0;
    if (ImGui::Button("Open", ImVec2(90.0f, 0.0f))) {
        const auto selected = *entry;
        activateAsset(selected);
        assetDetailsOpen_ = false;
    }
    if (copyId) {
        continueButtonRow("Copy to Project");
        ImGui::BeginDisabled(materialDraftDirty_ || !assets_.findMaterial(copyId));
        if (ImGui::Button("Copy to Project")) pendingContentCopy_ = copyId;
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) assetDetailsOpen_ = false;
    ImGui::EndGroup();
    ImGui::End();
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
    contentFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool engineFolder = assets::isEngineContentPath(contentFolder_);
    if (ImGui::Button("Add...")) ImGui::OpenPopup("##content-add");
    if (ImGui::BeginPopup("##content-add")) {
        if (ImGui::MenuItem("New folder...", nullptr, false, !engineFolder)) { newFolderBuffer_.clear(); newFolderOpen_ = true; }
        if (ImGui::MenuItem("Import OBJ...", nullptr, false, !engineFolder)) importMesh();
        ImGui::Separator();
        ImGui::TextDisabled("Scene mesh library");
        for (const auto& mesh : assets_.meshes) {
            const auto label = mesh.name + "##mesh-" + std::to_string(mesh.id);
            if (ImGui::MenuItem(label.c_str(), nullptr, selectedMesh_ == mesh.id)) selectedMesh_ = mesh.id;
        }
        ImGui::Separator();
        commandButton("actor.addFromMesh");
        ImGui::EndPopup();
    }
    continueButtonRow("Import...");
    ImGui::BeginDisabled(engineFolder);
    if (ImGui::Button("Import...")) importMesh();
    ImGui::EndDisabled();
    continueButtonRow("Refresh");
    if (ImGui::Button("Refresh")) refreshContent();
    continueButtonRow("View...");
    if (ImGui::Button("View...")) ImGui::OpenPopup("##content-view");
    if (ImGui::BeginPopup("##content-view")) {
        ImGui::Checkbox("Show Sources", &contentSourcesVisible_);
        if (ImGui::Checkbox("Show Engine Content", &contentEngineVisible_) && !contentEngineVisible_ && engineFolder)
            selectContentFolder(std::string());
        ImGui::Separator();
        if (ImGui::RadioButton("Tiles", contentGridView_)) contentGridView_ = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("List", !contentGridView_)) contentGridView_ = false;
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Thumbnail size", &contentThumbnailSize_, 48.0f, 128.0f, "%.0f");
        const char* sorts[] = {"Name", "Type", "Size"};
        int sort = static_cast<int>(contentQuery_.sort);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Sort by", &sort, sorts, 3)) contentQuery_.sort = static_cast<assets::AssetSort>(sort);
        ImGui::Checkbox("Descending", &contentQuery_.descending);
        ImGui::Checkbox("Show superseded legacy files", &contentQuery_.showLegacy);
        ImGui::EndPopup();
    }
    continueButtonRow("<");
    ImGui::BeginDisabled(contentHistoryIndex_ == 0);
    if (ImGui::SmallButton("<##content-back")) navigateContentBack();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(contentHistoryIndex_ + 1 >= contentHistory_.size());
    if (ImGui::SmallButton(">##content-forward")) navigateContentForward();
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::BeginDisabled(contentFolder_.empty() || contentFolder_ == assets::EngineContentPrefix);
    if (ImGui::SmallButton("Up")) navigateContentUp();
    ImGui::EndDisabled();
    // The source switch remains available when the Sources pane is collapsed.
    const bool currentEngine = assets::isEngineContentPath(contentFolder_);
    const auto rootLabel = currentEngine ? "Engine Content" : "Project Content";
    continueButtonRow(rootLabel);
    if (ImGui::SmallButton(rootLabel)) selectContentFolder(currentEngine ? std::string(assets::EngineContentPrefix) : std::string());
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::SmallButton("v##content-source")) ImGui::OpenPopup("##content-source-menu");
    if (ImGui::BeginPopup("##content-source-menu")) {
        if (ImGui::MenuItem("Project Content", nullptr, !currentEngine)) selectContentFolder(std::string());
        if (contentEngineVisible_ && ImGui::MenuItem("Engine Content", nullptr, currentEngine)) selectContentFolder(std::string(assets::EngineContentPrefix));
        ImGui::EndPopup();
    }
    {
        // Navigation mutates the live path; breadcrumb views borrow this copy.
        const auto path = contentFolder_;
        std::string accumulated = assets::isEngineContentPath(path) ? std::string(assets::EngineContentPrefix) : std::string();
        std::string_view rest = path;
        if (!accumulated.empty()) rest.remove_prefix(assets::EngineContentPrefix.size());
        while (!rest.empty()) {
            const auto slash = rest.find('/');
            const auto piece = rest.substr(0, slash);
            accumulated += (accumulated.empty() || accumulated == assets::EngineContentPrefix ? "" : "/") + std::string(piece);
            const auto label = std::string(piece) + "##crumb-" + accumulated;
            continueButtonRow(std::string(piece).c_str());
            if (ImGui::SmallButton(label.c_str())) selectContentFolder(accumulated);
            if (slash == std::string_view::npos) break;
            rest.remove_prefix(slash + 1);
        }
    }
    const float filterWidth = 110.0f;
    const float recursiveWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Include subfolders").x + 16.0f;
    const float searchAvailable = ImGui::GetContentRegionAvail().x;
    const bool inlineFilters = searchAvailable > filterWidth + recursiveWidth + 160.0f;
    ImGui::SetNextItemWidth(std::max(80.0f, searchAvailable - (inlineFilters ? filterWidth + recursiveWidth + 16.0f : 0.0f)));
    textInput("##content-search", contentQuery_.search);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search names and paths. All words must match.");
    if (contentQuery_.search.empty() && !ImGui::IsItemActive()) {
        const auto min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x,
            min.y + ImGui::GetStyle().FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), "Search assets...");
    }
    const char* types[] = {"All types", "Scenes", "Materials", "Meshes", "Shaders"};
    int type = static_cast<int>(contentQuery_.type);
    if (inlineFilters) ImGui::SameLine();
    ImGui::SetNextItemWidth(filterWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, type == 0 ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
        : imgui::assetTypeColor(contentQuery_.type));
    const bool typeMenuOpen = ImGui::BeginCombo("##content-type", types[type]);
    ImGui::PopStyleColor();
    if (typeMenuOpen) {
        for (int index = 0; index < 5; ++index) {
            const auto assetType = static_cast<assets::AssetType>(index);
            ImGui::PushStyleColor(ImGuiCol_Text, index == 0 ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                : imgui::assetTypeColor(assetType));
            if (ImGui::Selectable(types[index], type == index)) contentQuery_.type = assetType;
            ImGui::PopStyleColor();
            if (type == index) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    continueButtonRow("Include subfolders");
    ImGui::Checkbox("Include subfolders", &contentQuery_.recursive);
    if (!contentError_.empty()) ImGui::TextWrapped("Content: %s", contentError_.c_str());

    const float browserHeight = std::max(48.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    // Sources collapse on narrow floating panels; breadcrumbs remain available.
    if (contentSourcesVisible_ && availableWidth >= 500.0f) {
        contentSourcesWidth_ = std::clamp(contentSourcesWidth_, 100.0f, std::max(100.0f, availableWidth - 160.0f));
        ImGui::BeginChild("##content-folders", ImVec2(contentSourcesWidth_, browserHeight), ImGuiChildFlags_Borders);
        ImGui::TextDisabled("Sources");
        drawFolderTreeNode(std::string());
        if (contentEngineVisible_) drawFolderTreeNode(std::string(assets::EngineContentPrefix));
        contentRevealFolder_ = false;
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::InvisibleButton("##content-splitter", ImVec2(5.0f, browserHeight));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) contentSourcesWidth_ += ImGui::GetIO().MouseDelta.x;
        ImGui::SameLine(0.0f, 2.0f);
    }
    ImGui::BeginChild("##content-assets", ImVec2(0.0f, browserHeight), ImGuiChildFlags_Borders);
    if (contentGridView_) drawAssetGrid(); else drawAssetRows();
    ImGui::EndChild();
    const auto count = visibleContentEntries().size();
    const auto folders = contentQuery_.search.empty() && contentQuery_.type == assets::AssetType::Unknown
        ? registry_.childFolders(contentFolder_).size() : 0;
    ImGui::TextDisabled("%zu assets | %zu folders", count, folders);
    if (!selectedAsset_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| Selected: %s", leafName(selectedAsset_).c_str());
    }
    processContentActions();
    drawContentDialogs();
}

void SceneEditor::drawContentFolderTarget(const std::string& folder) {
    if (assets::isEngineContentPath(folder)) return;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("RAE_ASSET")) {
            pendingContentMove_.assign(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize));
            pendingContentMoveFolder_ = folder;
        }
        ImGui::EndDragDropTarget();
    }
}

void SceneEditor::drawFolderTreeNode(const std::string& folder) {
    const auto children = registry_.childFolders(folder);
    const bool root = folder.empty() || folder == assets::EngineContentPrefix;
    const auto label = root ? std::string(folder.empty() ? "Project Content" : "Engine Content") : leafName(folder);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (contentFolder_ == folder) flags |= ImGuiTreeNodeFlags_Selected;
    if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (root) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const auto id = label + "###folder-" + folder;
    if (contentRevealFolder_ && (root || contentFolder_ == folder || contentFolder_.starts_with(folder + "/")))
        ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", label.c_str());
    if (contentRevealFolder_ && contentFolder_ == folder) ImGui::SetScrollHereY(.5f);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectContentFolder(folder);
    drawContentFolderTarget(folder);
    if (open && !(flags & ImGuiTreeNodeFlags_NoTreePushOnOpen)) {
        for (const auto& child : children) drawFolderTreeNode(child);
        ImGui::TreePop();
    }
}

std::string SceneEditor::contentDeleteReason(const assets::AssetEntry& entry) const {
    if (entry.readOnly()) return "Engine Content is read-only. Copy to Project to edit.";
    if (entry.path.lexically_normal() == scenePath_.lexically_normal()) return "The open scene cannot be deleted.";
    if (entry.type == assets::AssetType::Mesh) {
        if (const auto* mesh = assets_.findMesh(entry.id); mesh && mesh->primitive != assets::MeshPrimitive::Imported)
            return "Built-in meshes are required by the editor.";
    }
    for (const auto& actor : world_.actors) {
        if ((entry.type == assets::AssetType::Mesh && actor.mesh.meshId == entry.id) ||
            (entry.type == assets::AssetType::Material && actor.mesh.materialId == entry.id))
            return "This asset is used by an Actor in the open scene.";
    }
    if (entry.type == assets::AssetType::Material && entry.id == selectedMaterial_ && materialWindowOpen_)
        return "Close the material editor before deleting this asset.";
    if ((entry.type == assets::AssetType::Material && assets_.findMaterial(entry.id)) ||
        (entry.type == assets::AssetType::Mesh && assets_.findMesh(entry.id)))
        return "Loaded assets cannot be deleted while this project is open.";
    return {};
}

void SceneEditor::drawAssetItemActions(const assets::AssetEntry& entry) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\n%s\nDouble-click: Details%s", entry.name.c_str(), entry.relativePath.c_str(),
        entry.readOnly() ? "Engine Content (read-only)" : "Project Content",
        entry.type == assets::AssetType::Mesh ? "\nDrag: place in Viewport" : entry.readOnly() ? "" : "\nDrag: move to a folder");
    if (!assetDragCancelled_ && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("RAE_ASSET", entry.relativePath.data(), entry.relativePath.size());
        ImGui::TextUnformatted(entry.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##asset-menu")) {
        selectedAsset_ = entry.relativePath;
        if (ImGui::MenuItem("Open")) pendingContentOpen_ = entry.relativePath;
        if (ImGui::MenuItem("Details...")) openAssetDetails(entry);
        if (entry.type == assets::AssetType::Mesh && ImGui::MenuItem("Add Actor from mesh", nullptr, false,
            assets_.findMesh(entry.id) && assets_.findMaterial(assets::EngineDefaultMaterialId))) {
            selectedMesh_ = entry.id;
            commands_.invoke("actor.addFromMesh");
        }
        if (entry.readOnly() && entry.type == assets::AssetType::Material &&
            ImGui::MenuItem("Copy to Project", nullptr, false, !materialDraftDirty_ && assets_.findMaterial(entry.id)))
            pendingContentCopy_ = entry.id;
        const auto* mesh = entry.type == assets::AssetType::Mesh ? assets_.findMesh(entry.id) : nullptr;
        const bool builtin = mesh && mesh->primitive != assets::MeshPrimitive::Imported;
        if (ImGui::MenuItem("Rename file...", nullptr, false, !builtin && !entry.readOnly())) {
            renameTarget_ = entry.relativePath;
            renameBuffer_ = pathText(entry.path.stem());
            renameOpen_ = true;
        }
        const auto reason = contentDeleteReason(entry);
        if (ImGui::MenuItem("Delete...", nullptr, false, reason.empty())) {
            deleteTarget_ = entry.relativePath;
            deleteOpen_ = true;
        }
        if (!reason.empty()) ImGui::TextDisabled("%s", reason.c_str());
        ImGui::EndPopup();
    }
}

void SceneEditor::processContentActions() {
    if (!pendingContentMove_.empty()) {
        const auto source = std::exchange(pendingContentMove_, {});
        const auto target = std::exchange(pendingContentMoveFolder_, {});
        const auto* entry = registry_.find(source);
        const auto* mesh = entry && entry->type == assets::AssetType::Mesh ? assets_.findMesh(entry->id) : nullptr;
        const std::string root = entry && entry->type == assets::AssetType::Material ? "Materials"
            : entry && entry->type == assets::AssetType::Mesh ? "Meshes"
            : entry && entry->type == assets::AssetType::Scene ? "Scenes" : "";
        std::string error;
        if (!entry || entry->readOnly() || assets::isEngineContentPath(target)) {
            contentError_ = "Engine Content is read-only. Copy to Project to edit.";
        } else if (!root.empty() && target != root && !target.starts_with(root + "/")) {
            contentError_ = "This asset must remain inside Content/" + root + ".";
        } else if (mesh && mesh->primitive != assets::MeshPrimitive::Imported) {
            contentError_ = "Built-in meshes cannot be moved.";
        } else if (assets::moveContentEntry(project_.contentRoot, source, target, error)) {
            const auto destination = target.empty() ? leafName(source) : target + "/" + leafName(source);
            if ((project_.contentRoot / utf8Path(source)).lexically_normal() == scenePath_.lexically_normal())
                scenePath_ = project_.contentRoot / utf8Path(destination);
            if (assetDetailsPath_ == source) assetDetailsPath_ = destination;
            refreshContent();
            selectContentFolder(target);
            selectedAsset_ = destination;
            status_ = "Moved '" + source + "'.";
        } else contentError_ = error;
    }
    if (!pendingContentOpen_.empty()) {
        const auto path = std::exchange(pendingContentOpen_, {});
        // Copy before activation: opening scenes replaces the registry.
        if (const auto* found = registry_.find(path)) {
            const auto entry = *found;
            activateAsset(entry);
        }
    }
    if (pendingContentCopy_) {
        const auto source = std::exchange(pendingContentCopy_, 0);
        if (!copyMaterialToProject(source, contentError_)) status_ = contentError_;
    }
}

void SceneEditor::drawAssetRows() {
    const auto entries = visibleContentEntries();
    const auto folders = contentQuery_.search.empty() && contentQuery_.type == assets::AssetType::Unknown
        ? registry_.childFolders(contentFolder_) : std::vector<std::string>{};
    if (entries.empty() && folders.empty()) {
        ImGui::TextWrapped(contentQuery_.search.empty() && contentQuery_.type == assets::AssetType::Unknown
            ? "This folder is empty. Import an OBJ or create a folder using Add."
            : "No matching assets. Clear the search, choose All types, or include subfolders.");
        return;
    }
    const float rowHeight = ImGui::GetTextLineHeight() + 12.0f;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(folders.size() + entries.size()), rowHeight + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const bool folder = static_cast<std::size_t>(i) < folders.size();
        const auto* entry = folder ? nullptr : entries[i - folders.size()];
        const auto& path = folder ? folders[i] : entry->relativePath;
        const auto name = folder ? leafName(path) : entry->name;
        ImGui::PushID(path.c_str());
        if (ImGui::Selectable("##asset-row", !folder && selectedAsset_ == path, ImGuiSelectableFlags_AllowDoubleClick,
            ImVec2(0.0f, rowHeight))) {
            if (folder) { if (ImGui::IsMouseDoubleClicked(0)) selectContentFolder(path); }
            else { selectedAsset_ = path; if (ImGui::IsMouseDoubleClicked(0)) openAssetDetails(*entry); }
        }
        const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float icon = rowHeight - 8.0f;
        if (folder) {
            draw->AddRectFilled(ImVec2(min.x + 4, min.y + 8), ImVec2(min.x + 4 + icon, min.y + 6 + icon), IM_COL32(110, 151, 190, 255), 2.0f);
            draw->AddRectFilled(ImVec2(min.x + 4, min.y + 4), ImVec2(min.x + 10, min.y + 9), IM_COL32(110, 151, 190, 255));
            drawContentFolderTarget(path);
        } else {
            const auto typeColor = ImGui::GetColorU32(imgui::assetTypeColor(entry->type));
            draw->AddRectFilled(ImVec2(min.x, min.y + 3), ImVec2(min.x + 3, max.y - 3), typeColor);
            if (entry->thumbnail.valid()) drawThumbnail(draw, ImVec2(min.x + 4, min.y + 4), icon, entry->thumbnail);
            else drawAssetIcon(draw, ImVec2(min.x + 4 + icon * .5f, min.y + rowHeight * .5f), icon * .7f, entry->type, typeColor);
            drawAssetItemActions(*entry);
        }
        char buffer[32]{};
        const auto meta = folder ? std::string("Folder") : std::string(assets::assetTypeName(entry->type)) + "  " + sizeLabel(entry->sizeBytes, buffer, sizeof(buffer));
        const float metaWidth = ImGui::CalcTextSize(meta.c_str()).x;
        const bool showMeta = max.x - min.x > 280.0f;
        const float textY = min.y + 6.0f;
        draw->PushClipRect(ImVec2(min.x + rowHeight, min.y), ImVec2(showMeta ? max.x - metaWidth - 16 : max.x - 4, max.y), true);
        draw->AddText(ImVec2(min.x + rowHeight, textY), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
        draw->PopClipRect();
        if (showMeta) draw->AddText(ImVec2(max.x - metaWidth - 4, textY), folder ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
            : ImGui::GetColorU32(imgui::assetTypeColor(entry->type)), meta.c_str());
        ImGui::PopID();
    }
}

void SceneEditor::drawAssetGrid() {
    const auto entries = visibleContentEntries();
    const auto folders = contentQuery_.search.empty() && contentQuery_.type == assets::AssetType::Unknown
        ? registry_.childFolders(contentFolder_) : std::vector<std::string>{};
    const auto count = folders.size() + entries.size();
    if (count == 0) { drawAssetRows(); return; }
    const float labelsHeight = ImGui::GetTextLineHeight() * 3.0f + 16.0f;
    // Fit one complete card in a short dock: names and type must remain visible.
    const float thumbnailSize = std::min(contentThumbnailSize_,
        std::max(24.0f, ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y * 2.0f - labelsHeight - 4.0f));
    const float width = std::max(88.0f, thumbnailSize + 32.0f);
    const float height = thumbnailSize + labelsHeight;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (width + spacing)));
    const float originX = ImGui::GetCursorScreenPos().x;
    const int rows = (static_cast<int>(count) + columns - 1) / columns;
    ImGuiListClipper clipper;
    clipper.Begin(rows, height + ImGui::GetStyle().ItemSpacing.y);
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const float y = ImGui::GetCursorScreenPos().y;
        for (int col = 0; col < columns; ++col) {
            const auto i = static_cast<std::size_t>(row * columns + col);
            if (i >= count) break;
            const bool folder = i < folders.size();
            const auto* entry = folder ? nullptr : entries[i - folders.size()];
            const auto& path = folder ? folders[i] : entry->relativePath;
            const auto name = folder ? leafName(path) : entry->name;
            const ImVec2 position(originX + col * (width + spacing), y);
            ImGui::SetCursorScreenPos(position);
            ImGui::PushID(path.c_str());
            ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
                ImGui::GetColorU32(ImGuiCol_FrameBg, 0.55f), 3.0f);
            if (ImGui::Selectable("##asset-tile", !folder && selectedAsset_ == path, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(width, height))) {
                if (folder) { if (ImGui::IsMouseDoubleClicked(0)) selectContentFolder(path); }
                else { selectedAsset_ = path; if (ImGui::IsMouseDoubleClicked(0)) openAssetDetails(*entry); }
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const ImVec2 thumb(position.x + (width - thumbnailSize) * 0.5f, position.y + 6.0f);
            if (folder) {
                draw->AddRectFilled(ImVec2(thumb.x + 6, thumb.y + thumbnailSize * .3f), ImVec2(thumb.x + thumbnailSize - 6, thumb.y + thumbnailSize - 8), IM_COL32(110, 151, 190, 255), 4.0f);
                draw->AddRectFilled(ImVec2(thumb.x + 6, thumb.y + thumbnailSize * .2f), ImVec2(thumb.x + thumbnailSize * .45f, thumb.y + thumbnailSize * .4f), IM_COL32(110, 151, 190, 255), 3.0f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDouble-click to open folder", path.c_str());
                drawContentFolderTarget(path);
            } else {
                if (entry->thumbnail.valid()) drawThumbnail(draw, thumb, thumbnailSize, entry->thumbnail);
                else drawAssetIcon(draw, ImVec2(thumb.x + thumbnailSize * .5f, thumb.y + thumbnailSize * .5f), thumbnailSize * .5f, entry->type, ImGui::GetColorU32(imgui::assetTypeColor(entry->type)));
                drawAssetItemActions(*entry);
                draw->AddRectFilled(ImVec2(position.x, position.y + height - 3), ImVec2(position.x + width, position.y + height),
                    ImGui::GetColorU32(imgui::assetTypeColor(entry->type)));
            }
            const float textY = position.y + thumbnailSize + 10.0f;
            draw->PushClipRect(ImVec2(position.x + 5, textY), ImVec2(position.x + width - 5, textY + ImGui::GetTextLineHeight() * 2), true);
            draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(position.x + 5, textY), ImGui::GetColorU32(ImGuiCol_Text), name.c_str(), nullptr, width - 10.0f);
            draw->PopClipRect();
            draw->AddText(ImVec2(position.x + 5, position.y + height - ImGui::GetTextLineHeight() - 4),
                folder ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(imgui::assetTypeColor(entry->type)),
                folder ? "Folder" : assets::assetTypeName(entry->type));
            ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(originX, y + height + ImGui::GetStyle().ItemSpacing.y));
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
            const bool valid = !newFolderBuffer_.empty() && newFolderBuffer_ != "." && newFolderBuffer_ != ".." &&
                newFolderBuffer_.back() != '.' && newFolderBuffer_.back() != ' ' &&
                newFolderBuffer_.find_first_of(std::string_view("/\\:*?\"<>|")) == std::string::npos;
            if (assets::isEngineContentPath(contentFolder_)) {
                status_ = "Engine Content is read-only.";
            } else if (!valid) {
                status_ = "Folder names cannot be empty or contain path characters.";
            } else {
                std::error_code code;
                const auto target = project_.contentRoot / utf8Path(contentFolder_) / utf8Path(newFolderBuffer_);
                std::filesystem::create_directories(target, code);
                if (code) {
                    status_ = "Cannot create the folder: " + code.message();
                } else {
                    refreshContent();
                    selectContentFolder(contentFolder_.empty() ? newFolderBuffer_ : contentFolder_ + "/" + newFolderBuffer_);
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
        ImGui::TextUnformatted("Rename the asset file; its GUID remains unchanged.");
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
            const bool valid = !renameBuffer_.empty() && renameBuffer_ != "." && renameBuffer_ != ".." &&
                renameBuffer_.back() != '.' && renameBuffer_.back() != ' ' &&
                renameBuffer_.find_first_of(std::string_view("/\\:*?\"<>|")) == std::string::npos;
            const auto destination = source.parent_path() / utf8Path(renameBuffer_ + extension);
            std::error_code code;
            const auto* entry = registry_.find(renameTarget_);
            if (!entry || entry->readOnly() || assets::isEngineContentPath(renameTarget_)) {
                status_ = "Engine Content is read-only.";
            } else if (!valid) {
                status_ = "Asset names cannot be empty or contain path characters.";
            } else if (std::filesystem::exists(destination, code)) {
                status_ = "An asset with that name already exists.";
            } else {
                std::filesystem::rename(source, destination, code);
                if (code) {
                    status_ = "Cannot rename the asset: " + code.message();
                } else {
                    auto folder = pathText(source.parent_path().lexically_relative(project_.contentRoot));
                    if (folder == ".") folder.clear();
                    const auto renamed = folder.empty() ? renameBuffer_ + extension : folder + "/" + renameBuffer_ + extension;
                    if (source.lexically_normal() == scenePath_.lexically_normal()) scenePath_ = destination;
                    if (assetDetailsPath_ == renameTarget_) assetDetailsPath_ = renamed;
                    // A saved scene thumbnail follows the filename. Failure is reported without hiding it.
                    auto oldThumbnail = source; oldThumbnail += ".rthumb";
                    auto newThumbnail = destination; newThumbnail += ".rthumb";
                    std::error_code thumbnailError;
                    if (std::filesystem::exists(oldThumbnail, thumbnailError))
                        std::filesystem::rename(oldThumbnail, newThumbnail, thumbnailError);
                    refreshContent();
                    selectedAsset_ = renamed;
                    if (thumbnailError) contentError_ = "Asset renamed, but thumbnail rename failed: " + thumbnailError.message();
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
        const auto* target = registry_.find(deleteTarget_);
        const auto reason = target ? contentDeleteReason(*target) : std::string("Asset no longer exists.");
        if (!reason.empty()) ImGui::TextWrapped("%s", reason.c_str());
        ImGui::TextUnformatted("References in other saved scenes are not checked.");
        ImGui::BeginDisabled(!reason.empty());
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
        ImGui::EndDisabled();
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
            const std::string label = candidate.name + (readOnlyMaterial(candidate) ? " (Engine)" : "") + "##" + std::to_string(candidate.id);
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
        ImGui::TextDisabled("%s", readOnlyMaterial(*material) ? "Engine Content (read-only)" : "Project Content");
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
            const auto label = material.name + (readOnlyMaterial(material) ? " (Engine)" : "") + "##" + std::to_string(material.id);
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
    if (readOnlyMaterial(materialDraft_)) {
        continueButtonRow("Copy to Project");
        if (ImGui::Button("Copy to Project")) {
            std::string error;
            if (!copyMaterialToProject(selectedMaterial_, error)) status_ = error;
        }
    }
    continueButtonRow("Apply material");
    ImGui::BeginDisabled(readOnlyMaterial(materialDraft_));
    if (ImGui::Button("Apply material")) applyMaterial();
    ImGui::EndDisabled();
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
    const bool readOnly = readOnlyMaterial(materialDraft_);
    if (readOnly) ImGui::TextWrapped("Engine Content - read-only. Copy to Project to customize this material.");
    else ImGui::TextWrapped("Project Content. Parameters preview live. Apply confirms the draft; Save writes assets.");
    if (materialDraftDirty_) ImGui::TextColored({1,.7f,.25f,1}, "Live preview: draft not applied or saved.");
    else ImGui::TextDisabled("No unapplied material changes.");
    ImGui::BeginDisabled(readOnly);
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
    ImGui::EndDisabled();
    ImGui::TextDisabled("Project shared shader: %s", pathText(project_.shaderRoot / "Materials/Standard.hlsli").c_str());
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
    ImGui::TextWrapped("Project shared shader. Changes affect every material rendered in this project.");
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
        // Live GPU preview sphere with orbit interaction.
        // The tool preview is always visible, including clean/read-only assets.
        // materialPreview() remains reserved for unapplied scene overrides.
        const auto* previewMaterial = materialPreview();
        if (!previewMaterial) previewMaterial = assets_.findMaterial(selectedMaterial_);
        if (const auto* draft = previewMaterial) {
            const float previewSize = 160.0f;
            ImGui::InvisibleButton("##material-preview", ImVec2(previewSize, previewSize),
                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            bool cameraChanged = false;
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 delta = ImGui::GetIO().MouseDelta;
                if (delta.x != 0.0f || delta.y != 0.0f) {
                    materialPreviewYaw_ -= delta.x * 0.01f;
                    materialPreviewPitch_ = std::clamp(materialPreviewPitch_ + delta.y * 0.01f, -1.4f, 1.4f);
                    cameraChanged = true;
                }
            }
            if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
                materialPreviewDistance_ = std::clamp(
                    materialPreviewDistance_ * (1.0f - ImGui::GetIO().MouseWheel * 0.1f), 1.6f, 8.0f);
                cameraChanged = true;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                materialPreviewYaw_ = 0.65f;
                materialPreviewPitch_ = 0.18f;
                materialPreviewDistance_ = 3.0f;
                cameraChanged = true;
            }
            if (hovered) ImGui::SetTooltip("Drag to rotate, wheel to zoom, double-click to reset.");
            materialPreviewAge_ += ImGui::GetIO().DeltaTime;
            const bool parametersChanged = !materialPreviewParamsValid_ ||
                draft->baseColor[0] != materialPreviewParams_.baseColor[0] ||
                draft->baseColor[1] != materialPreviewParams_.baseColor[1] ||
                draft->baseColor[2] != materialPreviewParams_.baseColor[2] ||
                draft->roughness != materialPreviewParams_.roughness ||
                draft->metallic != materialPreviewParams_.metallic ||
                draft->grid != materialPreviewParams_.grid ||
                draft->twoSided != materialPreviewParams_.twoSided;
            if (cameraChanged || parametersChanged || materialPreviewAge_ > 0.15f) {
                if (!renderer.renderMaterialPreview(*draft, 160, materialPreviewYaw_, materialPreviewPitch_,
                        materialPreviewDistance_, materialPreviewError_))
                    status_ = materialPreviewError_;
                else {
                    materialPreviewParams_ = *draft;
                    materialPreviewParamsValid_ = true;
                }
                materialPreviewAge_ = 0.0f;
            }
            if (auto* texture = renderer.materialPreviewTexture()) {
                const ImVec2 min = ImGui::GetItemRectMin();
                ImGui::GetWindowDrawList()->AddImage(
                    static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)),
                    min, ImVec2(min.x + previewSize, min.y + previewSize));
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextDisabled("Live preview");
            ImGui::TextWrapped("Drag to rotate, wheel to zoom, double-click to reset.");
            ImGui::EndGroup();
        }
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
    // Details may request a copy while Content is hidden. Consume before any
    // panel borrows registry entries, never during an asset traversal.
    if (pendingContentCopy_) {
        const auto source = std::exchange(pendingContentCopy_, 0);
        if (!copyMaterialToProject(source, contentError_)) status_ = contentError_;
    }
    actorPanelFocused_ = false;
    contentFocused_ = false;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) assetDragCancelled_ = false;
    if (const auto* payload = ImGui::GetDragDropPayload(); payload && payload->IsDataType("RAE_ASSET") &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        // ImGui clears ActiveId on Escape, but can retain the payload until
        // release. Cancel both scene placement and file moves for this gesture.
        assetDragCancelled_ = true;
        ImGui::ClearDragDrop();
    }
    // Editor-only selection outline: the renderer reads this borrowed list for
    // this frame only. Colour follows the active theme accent so Black and Dark
    // Blue keep a consistent selection language.
    const auto& preferences = imgui::uiPreferences();
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
        [this] { duplicateForDrag(); }, registry_,
        [this](assets::AssetId mesh, const scene::Transform& transform) {
            createMeshActor(mesh, transform, "Place Mesh");
        }, materialPreview());
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
        drawAssets();
    });
    drawMaterialWindow(renderer);
    drawAssetDetails(renderer);
    // Evaluate after the tool window has submitted this frame's focus state.
    // Its text edits and shortcuts must never mutate the selected scene Actor.
    if (!contentFocused_ && !garbageCollectionFocused_ && !outlineSettingsFocused_ && !materialWindowFocused_ && !assetDetailsFocused_ && !ImGui::GetIO().WantTextInput && !viewport_.isUsing() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        commands_.processShortcuts();
    if (!contentFocused_ && !garbageCollectionFocused_ && !outlineSettingsFocused_ && !materialWindowFocused_ && !assetDetailsFocused_ && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && !viewport_.isUsing() &&
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
    // Placement and the other controls may replace/reallocate selection_. Bind
    // this borrowed span only after all of this frame's selection edits.
    const bool outlineVisible = preferences.selectionOutline && !selection_.empty();
    settings.outlineActors = outlineVisible ? selection_.data() : nullptr;
    settings.outlineActorCount = outlineVisible ? static_cast<std::uint32_t>(selection_.size()) : 0;
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

