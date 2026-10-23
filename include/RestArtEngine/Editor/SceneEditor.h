#pragma once

#include <RestArtEngine/Scene/World.h>
#include <RestArtEngine/Project/Project.h>
#include <RestArtEngine/Editor/ViewportInteraction.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <filesystem>
#include <string>
#include <RestArtEngine/Editor/EditorDockManager.h>
#include <RestArtEngine/Editor/UndoStack.h>
#include <RestArtEngine/Editor/EditorCommands.h>

namespace rae::editor {

// UI orchestration is isolated from the runtime scene/assets and renderer.
class SceneEditor {
public:
    bool initialize(const project::ProjectPaths&, renderer::SceneRenderer&, std::string& error);
    void draw(renderer::SceneSettings&, renderer::SceneRenderer&, bool& vsync, bool& capture);
    void resetLayout() { dockManager_.resetLayout(); }
    void requestClose() { closeRequested_ = true; }
    bool drawCloseDialog(renderer::SceneRenderer&); // true only after explicit exit or a clean close
    scene::World& world() { return world_; }
    assets::AssetLibrary& assets() { return assets_; }
    scene::ActorId selectedActor() const { return selectedActor_; }
    // Full selection; the primary Actor (gizmo pivot / Details target) is selectedActor().
    [[nodiscard]] const std::vector<scene::ActorId>& selection() const noexcept { return selection_; }
    [[nodiscard]] bool isSelected(scene::ActorId id) const;
    // Editor transactions (Ctrl+Z / Ctrl+Y). Exposed so the self-test and any
    // future command palette drive exactly the same path as the Edit menu.
    bool undo();
    bool redo();
    [[nodiscard]] const UndoStack& undoStack() const noexcept { return undo_; }
    // Editor command registry (shortcuts + Edit menu). Exposed for tests so the
    // same ids the UI uses can be invoked and their enablement inspected.
    [[nodiscard]] const EditorCommandRegistry& commands() const noexcept { return commands_; }
private:
    void drawOutliner();
    void drawDetails();
    void selectActor(scene::ActorId id, bool additive);
    void deleteSelectedActor();
    void duplicateSelectedActor();
    void registerCommands();
    void drawEditMenu();
    bool commandButton(std::string_view id);
    // Records the mutation that just happened; both directions are captured as
    // copies so replaying never reads the live selection.
    void pushActorCreated(scene::Actor actor, const std::string& label);
    // Details-panel transform edit: one merged entry per finished drag.
    void pushDetailsTransform(scene::ActorId id, const scene::Transform& before,
                             const scene::Transform& after, const char* field);
    // Tracks gizmo drags: remembers the last settled transform, then commits one
    // undo entry when the drag ends (UE-style scope around the interaction).
    void trackTransformEdit();
    void eraseActor(scene::ActorId id);
    void insertActor(std::size_t index, const scene::Actor& actor);
    void drawAssets();
    void drawMaterial();
    void drawShader(renderer::SceneRenderer&);
    void selectMaterial(assets::AssetId);
    void importMesh();
    bool applyMaterial();
    bool saveScene();
    bool saveAll(renderer::SceneRenderer&);
    bool dirty(const renderer::SceneRenderer&) const;

    project::ProjectPaths project_;
    scene::World world_;
    ViewportInteraction viewport_;
    bool actorPanelFocused_ = false;
    assets::AssetLibrary assets_;
    scene::ActorId selectedActor_ = 0;
    assets::AssetId selectedMesh_ = 1, selectedMaterial_ = 1, pendingMaterial_ = 0;
    assets::MaterialAsset materialDraft_;
    std::string shaderDraft_, shaderSaved_, diagnostics_, status_;
    bool sceneDirty_ = false, materialsDirty_ = false, materialDraftDirty_ = false;
    bool closeRequested_ = false, shaderReloadRequested_ = false, sceneReloadRequested_ = false;
    bool openMaterialTab_ = false, openShaderTab_ = false;
    EditorDockManager dockManager_;
    EditorCommandRegistry commands_;
    UndoStack undo_;
    std::vector<scene::ActorId> selection_;
    struct ActorSnapshot { scene::ActorId id = 0; scene::Transform transform; };
    std::vector<ActorSnapshot> settledSelection_, dragStartTransforms_;
    scene::Transform settledTransform_{};
    scene::ActorId settledTransformActor_ = 0;
    scene::Transform detailsBefore_{};
    bool dragging_ = false;
};

// Resolve from --project, working directory, then executable ancestors.
// `engineRootHint` is the running engine; it is used to recover a project whose
// cached engine link no longer resolves (moved project, new machine).
project::ProjectPaths resolveProject(const std::filesystem::path& explicitProject = {},
                                     const std::filesystem::path& engineRootHint = {});

} // namespace rae::editor
