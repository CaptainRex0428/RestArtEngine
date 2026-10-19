#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>
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
constexpr ImGuiWindowFlags panelFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
void beginPanel(const char* name, ImVec2 position, ImVec2 size, ImGuiWindowFlags extra = 0) {
    ImGui::SetNextWindowPos(position);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin(name, nullptr, panelFlags | extra);
}
void message(const std::string& text) {
    if (!text.empty()) ImGui::TextWrapped("%s", text.c_str());
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
    world_.createDefaultScene();
    const auto scenePath = project_.defaultScene;
    if (std::filesystem::exists(scenePath) && !world_.load(scenePath, assets_, error)) return false;
    selectedActor_ = world_.actors.empty() ? 0 : world_.actors.front().id;
    selectedMaterial_ = assets_.materials.empty() ? 0 : assets_.materials.front().id;
    selectedMesh_ = assets_.meshes.empty() ? 0 : assets_.meshes.front().id;
    if (auto* actor = world_.findActor(selectedActor_)) selectedMaterial_ = actor->mesh.materialId;
    if (const auto* material = assets_.findMaterial(selectedMaterial_)) materialDraft_ = *material;
    shaderDraft_ = shaderSaved_ = renderer.shaderSource();
    shaderDraft_.reserve(std::max<std::size_t>(65536, shaderDraft_.size() * 2));
    status_ = "Project loaded. Actor edits are live; Material and Shader edits require Apply.";
    return true;
}

void SceneEditor::selectMaterial(assets::AssetId id) {
    if (id == selectedMaterial_) { openMaterialTab_ = true; return; }
    if (materialDraftDirty_) { pendingMaterial_ = id; openMaterialTab_ = true; return; }
    if (const auto* material = assets_.findMaterial(id)) {
        selectedMaterial_ = id;
        materialDraft_ = *material;
        openMaterialTab_ = true;
    }
}

bool SceneEditor::applyMaterial() {
    auto* target = assets_.findMaterial(selectedMaterial_);
    if (!target) { status_ = "Material no longer exists."; return false; }
    if (materialDraft_.name.empty() || !std::isfinite(materialDraft_.roughness) || !std::isfinite(materialDraft_.metallic) ||
        materialDraft_.roughness < 0 || materialDraft_.roughness > 1 || materialDraft_.metallic < 0 || materialDraft_.metallic > 1) {
        status_ = "Material needs a name and Roughness/Metallic values between zero and one."; return false;
    }
    for (const auto channel : materialDraft_.baseColor) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) {
            status_ = "Material base color values must be finite and between zero and one."; return false;
        }
    }
    *target = materialDraft_;
    materialDraftDirty_ = false;
    materialsDirty_ = sceneDirty_ = true;
    status_ = "Material applied to every Actor referencing this asset. Save to persist.";
    return true;
}

bool SceneEditor::saveScene() {
    std::string error;
    if (!world_.save(project_.defaultScene, assets_, error)) {
        status_ = "Scene save failed: " + error; return false;
    }
    sceneDirty_ = false;
    status_ = "Saved " + pathText(project_.defaultScene) + " (applied materials only).";
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

void SceneEditor::deleteSelectedActor() {
    if (!selectedActor_ || viewport_.isUsing()) return;
    const auto* actor = world_.findActor(selectedActor_);
    const std::string name = actor ? actor->name : "Actor";
    if (world_.removeActor(selectedActor_)) {
        selectedActor_ = 0;
        sceneDirty_ = true;
        status_ = "Deleted " + name + ". Save Scene to persist.";
    }
}

void SceneEditor::drawOutliner() {
    ImGui::BeginDisabled(!assets_.findMesh(1) || !assets_.findMaterial(selectedMaterial_));
    if (ImGui::Button("+ Sphere")) {
        selectedActor_ = world_.createActor("Sphere", 1, selectedMaterial_);
        world_.findActor(selectedActor_)->transform.position[1] = 1;
        sceneDirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!assets_.findMesh(3) || !assets_.findMaterial(selectedMaterial_));
    if (ImGui::Button("+ Cube")) {
        selectedActor_ = world_.createActor("Cube", 3, selectedMaterial_);
        world_.findActor(selectedActor_)->transform.position[1] = 1;
        sceneDirty_ = true;
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Duplicate") && selectedActor_) {
        const auto id = world_.duplicateActor(selectedActor_);
        if (id) { selectedActor_ = id; world_.findActor(id)->transform.position[0] += 2.5f; sceneDirty_ = true; }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedActor_ || viewport_.isUsing());
    if (ImGui::Button("Delete")) deleteSelectedActor();
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginChild("Actor list", {0, 0});
    bool deleteRequested = false;
    for (const auto& actor : world_.actors) {
        const auto label = actor.name + (actor.visible ? "" : " (hidden)") + "##" + std::to_string(actor.id);
        if (ImGui::Selectable(label.c_str(), selectedActor_ == actor.id)) selectedActor_ = actor.id;
        if (ImGui::BeginPopupContextItem()) {
            selectedActor_ = actor.id;
            if (ImGui::MenuItem("Delete Actor", "Delete")) deleteRequested = true;
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    if (deleteRequested) deleteSelectedActor();
    actorPanelFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
}

void SceneEditor::drawDetails() {
    auto* actor = world_.findActor(selectedActor_);
    if (!actor) { ImGui::TextDisabled("Select an Actor in the Outliner."); return; }
    ImGui::TextDisabled("Actor #%llu", static_cast<unsigned long long>(actor->id));
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= textInput("##actorName", actor->name);
    sceneDirty_ |= ImGui::Checkbox("Visible", &actor->visible);
    ImGui::SeparatorText("Transform");
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= ImGui::DragFloat3("##position", actor->transform.position, .03f, -1000, 1000, "%.2f");
    ImGui::TextDisabled("Position X / Y / Z");
    ImGui::SetNextItemWidth(-1);
    sceneDirty_ |= ImGui::DragFloat3("##rotation", actor->transform.rotation, .5f, -360, 360, "%.1f");
    ImGui::TextDisabled("Rotation X / Y / Z (degrees)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat3("##scale", actor->transform.scale, .02f, .01f, 100, "%.2f")) {
        for (auto& value : actor->transform.scale) value = std::clamp(value, .01f, 100.0f);
        sceneDirty_ = true;
    }
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
    const auto* material = assets_.findMaterial(actor->mesh.materialId);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##Material", material ? material->name.c_str() : "<missing>")) {
        for (const auto& candidate : assets_.materials) {
            const std::string label = candidate.name + "##" + std::to_string(candidate.id);
            if (ImGui::Selectable(label.c_str(), actor->mesh.materialId == candidate.id)) {
                actor->mesh.materialId = candidate.id; sceneDirty_ = true;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Edit assigned material")) selectMaterial(actor->mesh.materialId);
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
        status_ = "OBJ imported. Select it below and add an Actor, then save the scene.";
    } catch (const std::exception& error) { status_ = error.what(); }
}

void SceneEditor::drawAssets() {
    if (ImGui::Button("Import OBJ...")) importMesh();
    ImGui::SameLine();
    ImGui::BeginDisabled(!assets_.findMesh(selectedMesh_) || !assets_.findMaterial(selectedMaterial_));
    if (ImGui::Button("Add Actor from mesh")) {
        if (const auto* mesh = assets_.findMesh(selectedMesh_)) {
            selectedActor_ = world_.createActor(mesh->name, mesh->id, selectedMaterial_);
            sceneDirty_ = true;
        }
    }
    ImGui::EndDisabled();
    ImGui::BeginChild("Mesh library", {220, 0}, ImGuiChildFlags_Borders);
    for (const auto& mesh : assets_.meshes) {
        const auto label = mesh.name + "##" + std::to_string(mesh.id);
        if (ImGui::Selectable(label.c_str(), selectedMesh_ == mesh.id)) selectedMesh_ = mesh.id;
    }
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginGroup();
    if (const auto* mesh = assets_.findMesh(selectedMesh_)) {
        ImGui::TextUnformatted(mesh->name.c_str());
        ImGui::Text("Type: %s", meshType(mesh->primitive));
        ImGui::Text("Vertices: %zu  Triangles: %zu", mesh->vertices.size(), mesh->indices.size() / 3);
        message(mesh->sourcePath.empty() ? "Procedural built-in mesh" : mesh->sourcePath);
        ImGui::TextWrapped("OBJ imports static geometry. Materials are assigned separately in Details.");
    }
    ImGui::EndGroup();
}

void SceneEditor::drawMaterial() {
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo("Asset", materialDraft_.name.c_str())) {
        for (const auto& material : assets_.materials) {
            const auto label = material.name + "##" + std::to_string(material.id);
            if (ImGui::Selectable(label.c_str(), selectedMaterial_ == material.id)) selectMaterial(material.id);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(materialDraftDirty_);
    if (ImGui::Button("New material")) {
        const auto id = assets_.createMaterial("New Material");
        selectMaterial(id); materialsDirty_ = sceneDirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Apply material")) applyMaterial();
    ImGui::SameLine();
    ImGui::BeginDisabled(materialDraftDirty_);
    if (ImGui::Button("Save material assets")) {
        std::string error;
        if (assets_.saveMaterials(project_.contentRoot / "Materials", error)) {
            materialsDirty_ = false;
            if (saveScene()) status_ = "Saved " + pathText(project_.contentRoot / "Materials") + " and current scene material references.";
        } else status_ = error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
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
    if (materialDraftDirty_) ImGui::TextColored({1,.7f,.25f,1}, "Draft changed: Apply to update Actors; Save writes assets.");
    ImGui::SetNextItemWidth(240);
    materialDraftDirty_ |= textInput("Name", materialDraft_.name);
    ImGui::SetNextItemWidth(240);
    materialDraftDirty_ |= ImGui::ColorEdit3("Base color", materialDraft_.baseColor);
    ImGui::SetNextItemWidth(240);
    materialDraftDirty_ |= ImGui::SliderFloat("Roughness", &materialDraft_.roughness, .05f, 1);
    ImGui::SameLine(); ImGui::SetNextItemWidth(180);
    materialDraftDirty_ |= ImGui::SliderFloat("Metallic", &materialDraft_.metallic, 0, 1);
    materialDraftDirty_ |= ImGui::Checkbox("Grid", &materialDraft_.grid);
    ImGui::TextDisabled("Shared shader: %s", pathText(project_.shaderRoot / "Materials/Standard.hlsli").c_str());
    if (pendingMaterial_) {
        ImGui::OpenPopup("Unapplied material draft");
        if (ImGui::BeginPopupModal("Unapplied material draft", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Apply or discard this draft before switching material assets.");
            bool change = false;
            if (ImGui::Button("Apply & switch")) change = applyMaterial();
            ImGui::SameLine();
            if (ImGui::Button("Discard & switch")) { materialDraftDirty_ = false; change = true; }
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
    ImGui::SameLine();
    ImGui::BeginDisabled(unapplied);
    if (ImGui::Button("Save shader")) {
        std::string error;
        if (renderer.saveShaderSource(error)) { shaderSaved_ = renderer.shaderSource(); status_ = "Saved " + pathText(renderer.materialShaderPath()); }
        else status_ = error;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from disk")) shaderReloadRequested_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Revert draft")) shaderDraft_ = renderer.shaderSource();
    ImGui::SameLine(); ImGui::Checkbox("Expand editor", &expandedEditor_);
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

void SceneEditor::draw(renderer::SceneSettings& settings, renderer::SceneRenderer& renderer, bool& vsync, bool& capture) {
    const auto display = ImGui::GetIO().DisplaySize;
    const float side = display.x < 1200 ? 230.0f : 280.0f;
    const float bottom = expandedEditor_ ? display.y * .65f : std::clamp(display.y * .34f, 220.0f, 360.0f);
    const float middle = display.y - 100 - bottom;
    settings.viewportX = static_cast<int>(side);
    settings.viewportY = 90;
    settings.viewportWidth = static_cast<int>(display.x - 2 * side);
    settings.viewportHeight = static_cast<int>(middle);
    beginPanel("RestArtEngine Editor", {0, 0}, {display.x, 90}, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::Button("Save Scene")) saveScene();
    ImGui::SameLine();
    if (ImGui::Button("Load Scene")) sceneReloadRequested_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Save All")) saveAll(renderer);
    ImGui::SameLine(); ImGui::TextDisabled("%s", sceneDirty_ ? "Scene *" : "Scene saved");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    int path = settings.path == renderer::RenderPath::Deferred ? 1 : 0;
    if (ImGui::Combo("##pipeline", &path, "Forward\0Deferred\0"))
        settings.path = path ? renderer::RenderPath::Deferred : renderer::RenderPath::Forward;
    ImGui::SameLine(); ImGui::SetNextItemWidth(135);
    ImGui::Combo("##view", &settings.debugView, "Lit\0Unlit\0Albedo\0World normals\0World position\0");
    ImGui::SameLine(); ImGui::Checkbox("Ray tracing", &settings.rayTracing);
    ImGui::SameLine(); capture |= ImGui::Button("Capture");
    ImGui::TextDisabled("Project: %s | DX11 / %s | %.1f FPS | F1 UI, F2 pipeline, F3 rays, RMB orbit, wheel zoom",
        project_.name.c_str(), renderer.adapterName().c_str(), ImGui::GetIO().Framerate);
    ImGui::End();
    beginPanel("World Outliner", {0, 90}, {side, middle * .42f});
    drawOutliner(); ImGui::End();
    beginPanel("Details", {0, 90 + middle * .42f}, {side, middle * .58f});
    drawDetails();
    actorPanelFocused_ |= ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    beginPanel("Viewport settings", {display.x - side, 90}, {side, middle});
    ImGui::TextDisabled("Scene");
    ImGui::Text("Actors: %zu", world_.actors.size());
    ImGui::Text("Meshes: %zu / Materials: %zu", assets_.meshes.size(), assets_.materials.size());
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##Light", &settings.lightIntensity, .1f, 12, "Key light %.1f");
    ImGui::SetNextItemWidth(-1); ImGui::SliderFloat("##Exposure", &settings.exposure, .25f, 3, "Exposure %.2f");
    ImGui::TextDisabled("Direct-light shadows");
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
    ImGui::Checkbox("VSync", &vsync);
    if (ImGui::Button("Reset camera")) {
        settings.cameraYaw=.65f; settings.cameraPitch=.3f; settings.cameraDistance=6;
        settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1; settings.cameraTarget[2] = 0;
    }
    message(renderer.rayTracingStatus(world_, assets_));
    ImGui::SeparatorText("Activity");
    message(status_);
    ImGui::End();

    beginPanel("Content & Editors", {0, display.y - bottom}, {display.x, bottom});
    if (ImGui::BeginTabBar("Editors")) {
        if (ImGui::BeginTabItem("Meshes")) { drawAssets(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Material Editor", nullptr, openMaterialTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            openMaterialTab_ = false; drawMaterial(); ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shader Editor", nullptr, openShaderTab_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            openShaderTab_ = false; drawShader(renderer); ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    sceneDirty_ |= viewport_.draw(settings, world_, assets_, selectedActor_);
    if (!ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() && !viewport_.isUsing() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        (actorPanelFocused_ || viewport_.isHovered() || viewport_.isFocused()) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        deleteSelectedActor();
    if (sceneReloadRequested_) {
        ImGui::OpenPopup("Load saved scene?");
        if (ImGui::BeginPopupModal("Load saved scene?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace current Actors and materials with the saved scene?\nUnapplied material edits will be discarded.");
            if (ImGui::Button("Load & replace")) {
                std::string error;
                if (world_.load(project_.defaultScene, assets_, error)) {
                    selectedActor_ = world_.actors.empty() ? 0 : world_.actors.front().id;
                    materialDraftDirty_ = materialsDirty_ = sceneDirty_ = false;
                    pendingMaterial_ = 0;
                    materialDraft_ = {};
                    selectedMesh_ = assets_.meshes.empty() ? 0 : assets_.meshes.front().id;
                    selectedMaterial_ = 0;
                    selectMaterial(assets_.materials.empty() ? 0 : assets_.materials.front().id);
                    status_ = "Scene loaded.";
                } else status_ = error;
                sceneReloadRequested_ = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { sceneReloadRequested_ = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
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

