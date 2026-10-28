#pragma once

#include <RestArtEngine/Scene/World.h>
#include <RestArtEngine/Core/GarbageCollector.h>
#include <RestArtEngine/Project/Project.h>
#include <RestArtEngine/Editor/ViewportInteraction.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <filesystem>
#include <string>
#include <RestArtEngine/Editor/EditorDockManager.h>
#include <RestArtEngine/Editor/UndoStack.h>
#include <RestArtEngine/Editor/EditorCommands.h>
#include <RestArtEngine/Editor/DiagnosticsPanel.h>
#include <RestArtEngine/Assets/AssetRegistry.h>

namespace rae::editor {

// UI orchestration is isolated from the runtime scene/assets and renderer.
class SceneEditor {
public:
    bool initialize(const project::ProjectPaths&, renderer::SceneRenderer&, std::string& error);
    // Borrowed service; the caller keeps it alive until detached or editor destruction.
    void setGarbageCollector(rae::GarbageCollector* gc, const std::filesystem::path& settingsPath);
    void draw(renderer::SceneSettings&, renderer::SceneRenderer&, bool& vsync, bool& capture);
    // Also submitted while the scene window is minimized or its UI is hidden.
    // The tool keeps the same material/shader drafts when its OS window closes.
    void drawMaterialWindow(renderer::SceneRenderer&);
    void resetLayout() { dockManager_.resetLayout(); }
    void requestClose() { closeRequested_ = true; }
    bool drawCloseDialog(renderer::SceneRenderer&); // true only after explicit exit or a clean close
    scene::World& world() { return world_; }
    assets::AssetLibrary& assets() { return assets_; }
    // Current parameter draft for rendering only; Apply and Save keep their
    // existing commit/persistence boundaries. Valid until the next editor edit.
    [[nodiscard]] const assets::MaterialAsset* materialPreview() const;
    // Content tree cache; the editor rescans it after every file operation.
    [[nodiscard]] const assets::AssetRegistry& assetRegistry() const noexcept { return registry_; }
    // Scene currently bound to the editor. Starts at the project default scene and
    // changes when the Content browser opens another one.
    [[nodiscard]] const std::filesystem::path& scenePath() const noexcept { return scenePath_; }
    // Selects a Content key (project-relative, "" = Project root; "engine://" = Engine root). Used by
    // the browser breadcrumb and by tests that need a deterministic view.
    void selectContentFolder(std::string folder);
    // Opens the asset details window for a content-relative path (also used by
    // tests to verify the inspector without double-clicking).
    void showAssetDetails(std::string relativePath);
    void closeAssetDetails() { assetDetailsOpen_ = false; assetDetailsFocused_ = false; }
    void showDiagnostics(DiagnosticsTab tab = DiagnosticsTab::Profiler) { diagnosticsPanel_.show(tab); }
    void closeDiagnostics() { diagnosticsPanel_.close(); }
    [[nodiscard]] bool diagnosticsOpen() const noexcept { return diagnosticsPanel_.isOpen(); }
    // Switches the Content browser between the list and the thumbnail grid.
    void setContentGridView(bool grid) { contentGridView_ = grid; }
    void setContentSearch(std::string search) { contentQuery_.search = std::move(search); }
    void setContentTypeFilter(assets::AssetType type) { contentQuery_.type = type; }
    void setContentRecursive(bool recursive) { contentQuery_.recursive = recursive; }
    void setContentSort(assets::AssetSort sort, bool descending = false) { contentQuery_.sort = sort; contentQuery_.descending = descending; }
    [[nodiscard]] const std::string& contentFolder() const noexcept { return contentFolder_; }
    [[nodiscard]] std::vector<const assets::AssetEntry*> visibleContentEntries() const;
    bool navigateContentBack();
    bool navigateContentForward();
    bool navigateContentUp();
    void selectContentAsset(std::string path);
    // Save a new Project material without applying/saving unrelated drafts or assets.
    bool copyMaterialToProject(assets::AssetId source, std::string& error);
    [[nodiscard]] const std::string& selectedContentAsset() const noexcept { return selectedAsset_; }
    bool openScene(const std::filesystem::path& path, std::string& error);
    bool createScene(const std::filesystem::path& path, std::string& error);
    scene::ActorId selectedActor() const { return selectedActor_; }
    // Full selection; the primary Actor (gizmo pivot / Scene and Details target) is selectedActor().
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
    void drawScene();
    void drawSceneLighting(renderer::SceneSettings& settings);
    void setSceneBackground(bool black);
    // Toggles one Actor's visibility as a single undoable edit.
    void setActorVisible(scene::ActorId id, bool visible);
    void drawInstanceDetails();
    void selectActor(scene::ActorId id, bool additive);
    void deleteSelectedActor();
    void duplicateSelectedActor();
    void duplicateForDrag();
    bool createMeshActor(assets::AssetId mesh, const scene::Transform& transform, const std::string& label);
    void registerCommands();
    void drawEditMenu();
    // Flips visibility for every selected Actor as one undoable edit (H).
    void toggleSelectedVisibility();
    void drawUiMenu();
    void drawDebugMenu(renderer::SceneRenderer& renderer);
    void drawGarbageCollection();
    void drawOutlineSettings();
    bool commandButton(std::string_view id);
    // Records the mutation that just happened; both directions are captured as
    // copies so replaying never reads the live selection.
    void pushActorCreated(scene::Actor actor, const std::string& label);
    // Scene-panel transform edit: one merged entry per finished drag.
    void pushDetailsTransform(scene::ActorId id, const scene::Transform& before,
                             const scene::Transform& after, const char* field);
    // Tracks gizmo drags: remembers the last settled transform, then commits one
    // undo entry when the drag ends (UE-style scope around the interaction).
    void trackTransformEdit();
    void eraseActor(scene::ActorId id);
    void insertActor(std::size_t index, const scene::Actor& actor);
    void drawAssets();
    void drawFolderTreeNode(const std::string& folder);
    void drawAssetRows();
    void drawAssetGrid();
    void drawAssetItemActions(const assets::AssetEntry&);
    void drawContentFolderTarget(const std::string&);
    void processContentActions();
    [[nodiscard]] std::string contentDeleteReason(const assets::AssetEntry&) const;
    void drawContentDialogs();
    // Double-click: opens the asset details window. "Open" inside it (or the
    // context menu) runs the type-specific action.
    void openAssetDetails(const assets::AssetEntry& entry);
    void activateAsset(const assets::AssetEntry& entry);
    void drawAssetDetails(renderer::SceneRenderer& renderer);
    void requestSceneOpen(const std::filesystem::path& path);
    void refreshContent();
    bool saveSceneAs(const std::filesystem::path& path, std::string& error);
    void drawMaterial();
    void drawShader(renderer::SceneRenderer&);
    void selectMaterial(assets::AssetId);
    void importMesh();
    bool applyMaterial();
    void discardMaterial();
    bool saveScene(renderer::SceneRenderer* renderer = nullptr);
    bool saveAll(renderer::SceneRenderer&);
    bool dirty(const renderer::SceneRenderer&) const;

    project::ProjectPaths project_;
    assets::AssetRegistry registry_;
    std::filesystem::path scenePath_;
    std::string contentFolder_;    // source-qualified registry folder key
    std::string selectedAsset_;    // source-qualified registry entry key
    std::string renameBuffer_, newFolderBuffer_;
    std::string renameTarget_, deleteTarget_;
    bool renameOpen_ = false, newFolderOpen_ = false, deleteOpen_ = false;
    bool sceneSwitchPrompt_ = false;
    std::filesystem::path pendingScenePath_;
    bool contentGridView_ = true;
    assets::AssetQuery contentQuery_;
    std::vector<std::string> contentHistory_{std::string()};
    std::size_t contentHistoryIndex_ = 0;
    bool contentSourcesVisible_ = true, contentFocused_ = false, contentRevealFolder_ = true;
    bool contentEngineVisible_ = true;
    float contentSourcesWidth_ = 170.0f, contentThumbnailSize_ = 72.0f;
    std::string pendingContentOpen_, pendingContentMove_, pendingContentMoveFolder_;
    assets::AssetId pendingContentCopy_ = 0;
    bool assetDragCancelled_ = false;
    // Offscreen material preview: throttled re-render of the draft parameters.
    float materialPreviewAge_ = 1.0f;
    assets::MaterialAsset materialPreviewParams_{};
    bool materialPreviewParamsValid_ = false;
    std::string materialPreviewError_;
    bool assetDetailsOpen_ = false, assetDetailsFocusRequested_ = false, assetDetailsFocused_ = false;
    std::string assetDetailsPath_;
    float assetDetailsAge_ = 1.0f;
    std::string assetDetailsError_;
    float materialPreviewYaw_ = 0.65f, materialPreviewPitch_ = 0.18f, materialPreviewDistance_ = 3.0f;
    std::string contentError_;
    scene::World world_;
    ViewportInteraction viewport_;
    bool actorPanelFocused_ = false;
    assets::AssetLibrary assets_;
    scene::ActorId selectedActor_ = 0;
    scene::ActorId sceneInspectorActor_ = 0;
    assets::AssetId selectedMesh_ = 1, selectedMaterial_ = 1, pendingMaterial_ = 0;
    assets::MaterialAsset materialDraft_;
    std::string shaderDraft_, shaderSaved_, diagnostics_, status_;
    bool sceneDirty_ = false, materialsDirty_ = false, materialDraftDirty_ = false;
    bool closeRequested_ = false, shaderReloadRequested_ = false, sceneReloadRequested_ = false;
    bool openMaterialTab_ = false, openShaderTab_ = false;
    bool materialWindowOpen_ = false, materialWindowFocusRequested_ = false;
    bool materialWindowFocused_ = false;
    rae::GarbageCollector* garbageCollector_ = nullptr;
    std::filesystem::path garbageCollectionSettingsPath_;
    rae::GarbageCollectionSettings garbageCollectionDraft_;
    bool garbageCollectionOpen_ = false, garbageCollectionDraftDirty_ = false;
    // Editor-only selection outline settings window, opened from the UI menu.
    bool outlineSettingsOpen_ = false, outlineSettingsFocused_ = false;
    bool garbageCollectionFocused_ = false;
    bool garbageCollectionRequested_ = false;
    std::uint64_t garbageCollectionRequestCount_ = 0;
    std::string garbageCollectionMessage_;
    EditorDockManager dockManager_;
    EditorCommandRegistry commands_;
    DiagnosticsPanel diagnosticsPanel_;
    UndoStack undo_;
    std::vector<scene::ActorId> selection_;
    struct ActorSnapshot { scene::ActorId id = 0; scene::Transform transform; };
    std::vector<ActorSnapshot> settledSelection_, dragStartTransforms_;
    std::vector<scene::ActorId> dragCopies_, dragSourceSelection_;
    scene::ActorId dragSourcePrimary_ = 0;
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
