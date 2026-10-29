#include "ImportSmoke.h"
#include <RestArtEngine/Assets/AssetFile.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

void nativeDrop(HWND hwnd, ImVec2 point, const std::vector<std::filesystem::path>& paths) {
    require(hwnd && IsWindow(hwnd), "Import test target has no native window.");
    std::wstring names;
    for (const auto& path : paths) { names += path.wstring(); names.push_back(L'\0'); }
    names.push_back(L'\0');
    const auto allocation = GlobalAlloc(GHND, sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
    require(allocation != nullptr, "Cannot allocate native file-drop test payload.");
    auto* data = static_cast<DROPFILES*>(GlobalLock(allocation));
    require(data != nullptr, "Cannot lock native file-drop test payload.");
    data->pFiles = sizeof(DROPFILES);
    data->pt = {static_cast<LONG>(point.x), static_cast<LONG>(point.y)};
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        ScreenToClient(hwnd, &data->pt);
    data->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(data) + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    GlobalUnlock(allocation);
    // Ownership transfers to the adapter, which must call DragFinish exactly once.
    SendMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(allocation), 0);
}

ImGuiWindow* contentArea() {
    for (auto* window : ImGui::GetCurrentContext()->Windows)
        if (window->Active && std::strstr(window->Name, "##content-assets")) return window;
    throw std::runtime_error("Native import test cannot find the visible Content asset area.");
}
} // namespace

void runImportSmoke(rae::editor::SceneEditor& editor, rae::imgui::ImGuiLayer& ui, rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& output, const std::function<void(const char*)>& frame,
    const std::function<void(bool)>& setImportOption) {
    namespace fs = std::filesystem;
    using namespace rae;
    require(ui.isInitialized(), "Native import test requires an initialized UI adapter.");
    const auto sources = output / "import-sources";
    fs::create_directories(sources);
    auto& preferences = imgui::uiPreferences();
    struct RestoreImportPreference {
        bool original;
        ~RestoreImportPreference() { rae::imgui::uiPreferences().importSkipDegenerateTriangles = original; }
    } restoreImportPreference{preferences.importSkipDegenerateTriangles};
    const auto preferencesPath = sources / "import-options.ui.ini";
    auto savedPreferences = preferences;
    savedPreferences.importSkipDegenerateTriangles = false;
    imgui::saveUiPreferences(preferencesPath, savedPreferences);
    const auto strictPreferences = imgui::loadUiPreferences(preferencesPath);
    require(!strictPreferences.importSkipDegenerateTriangles && strictPreferences.theme == savedPreferences.theme &&
        strictPreferences.gizmoNegativeAxisHints == savedPreferences.gizmoNegativeAxisHints &&
        strictPreferences.selectionOutline == savedPreferences.selectionOutline,
        "Saving import options did not preserve strict mode and existing UI preferences.");
    savedPreferences.importSkipDegenerateTriangles = true;
    imgui::saveUiPreferences(preferencesPath, savedPreferences);
    require(imgui::loadUiPreferences(preferencesPath).importSkipDegenerateTriangles,
        "Saving import options did not preserve skip mode.");
    const auto obj = sources / L"导入测试.OBJ";
    const auto fbx = sources / "Cube.FBX";
    const auto bad = sources / "Unsupported.png";
    { std::ofstream stream(obj); stream << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
    { std::ofstream stream(bad); stream << "unsupported texture fixture"; }
    // Self-authored FBX cube in left-handed Y-up meters, with generated normals.
    // An extra zero-area triangle must be reported and skipped, preserving the cube.
    { std::ofstream stream(fbx); stream << R"fbx(; FBX 7.4.0 project file
FBXHeaderExtension: {
 FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: {
 Version: 1000
 Properties70: {
  P: "UpAxis", "int", "Integer", "", 1
  P: "UpAxisSign", "int", "Integer", "", 1
  P: "FrontAxis", "int", "Integer", "", 2
  P: "FrontAxisSign", "int", "Integer", "", -1
  P: "CoordAxis", "int", "Integer", "", 0
  P: "CoordAxisSign", "int", "Integer", "", 1
  P: "UnitScaleFactor", "double", "Number", "", 100
 }
}
Objects: {
 Geometry: 100, "Geometry::Cube", "Mesh" {
  Vertices: *27 {
   a: -1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1, -1,-1,1, 1,-1,1, 1,1,1, -1,1,1, 0,-1,-1
  }
  PolygonVertexIndex: *27 {
   a: 0,3,2,-2, 4,5,6,-8, 0,1,5,-5, 3,7,6,-3, 0,4,7,-4, 1,2,6,-6, 0,8,-2
  }
 }
 Model: 200, "Model::Cube", "Mesh" {
  Version: 232
 }
}
Connections: {
 C: "OO", 100, 200
 C: "OO", 200, 0
}
)fbx"; }
    const auto content = editor.assetRegistry().contentRoot();
    const auto actorCount = editor.world().actors.size();
    const auto selectedActor = editor.selectedActor();
    const auto historyCount = editor.undoStack().size();
    const auto meshCount = editor.assets().meshes.size();
    const auto originalTheme = imgui::currentTheme();
    const auto originalFolder = editor.contentFolder();
    editor.closeAssetDetails();
    editor.closeImportResults();
    // Use an existing non-Meshes folder from the initialized Content registry.
    editor.selectContentFolder("Scenes");
    require(editor.contentFolder() == "Scenes", "The test project's Scenes folder is absent from Content.");
    editor.setContentSearch("");
    editor.setContentTypeFilter(assets::AssetType::Unknown);
    frame("import_preroll");
    auto* panel = ImGui::FindWindowByName("Content & Editors");
    require(panel != nullptr, "Native import test cannot find Content.");
    const auto origin = ImGui::GetMainViewport()->Pos;
    ImGui::SetWindowDock(panel, 0, ImGuiCond_Always);
    ImGui::SetWindowPos(panel, {origin.x + 20, origin.y + 90}, ImGuiCond_Always);
    ImGui::SetWindowSize(panel, {760, 550}, ImGuiCond_Always);
    ImGui::FocusWindow(panel);
    frame("import_content_ready");
    frame("import_content_stable");
    auto drop = [&](const std::vector<fs::path>& paths) {
        const auto* area = contentArea();
        const auto point = area->InnerRect.GetCenter();
        const auto hwnd = static_cast<HWND>(area->Viewport->PlatformHandle);
        require((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0,
            "The editor has not enabled native external file drops.");
        nativeDrop(hwnd, point, paths);
    };
    setImportOption(false);
    require(!preferences.importSkipDegenerateTriangles, "The import options checkbox did not enable strict mode.");
    const auto selectionBeforeStrictImport = editor.selectedContentAsset();
    drop({fbx});
    frame("import_strict_rejected");
    require(editor.lastImportReport().importedCount() == 0 && editor.lastImportReport().rejectedCount() == 1 &&
        editor.lastImportReport().warningCount() == 0 && editor.assets().meshes.size() == meshCount &&
        editor.selectedContentAsset() == selectionBeforeStrictImport,
        "Disabling skip-degenerate-triangles did not reject the whole FBX without changing assets or selection.");
    editor.closeImportResults();
    ImGui::FocusWindow(panel);
    frame("import_strict_closed");
    setImportOption(true);
    require(preferences.importSkipDegenerateTriangles, "The import options checkbox did not enable skip mode.");
    drop({obj, bad, fbx});
    frame("import_mixed_delivery");
    require(editor.lastImportReport().importedCount() == 2 && editor.lastImportReport().rejectedCount() == 1,
        "Native Content drop did not preserve its valid source and report the unsupported source.");
    require(editor.assets().meshes.size() == meshCount + 2,
        "A mixed external drop added an incorrect number of assets.");
    const auto id = editor.lastImportReport().items[0].id;
    const auto fbxId = editor.lastImportReport().items[2].id;
    const auto firstPath = editor.lastImportReport().items[0].assetPath;
    require(!editor.lastImportReport().items[2].warnings.empty() && editor.lastImportReport().warningCount() > 0,
        "The native FBX import did not report its skipped degenerate triangle.");
    require(editor.assets().findMesh(fbxId) && editor.assets().findMesh(fbxId)->indices.size() == 36,
        "Skipping the degenerate FBX triangle changed valid cube geometry.");
    require(editor.selectedContentAsset() ==
        editor.lastImportReport().items[2].assetPath.lexically_relative(content).generic_string(),
        "A successful FBX import with warnings was not selected in Content.");
    require(firstPath.parent_path() == content / "Scenes" && fs::is_regular_file(firstPath),
        "External Content import did not persist in the selected project directory.");
    frame("import_mixed_results_ready");
    auto* results = ImGui::FindWindowByName("Import results");
    require(results && results->Active, "External sources with warnings or rejections have no inspectable result.");
    // Show the complete mixed batch, including the warning on its last source.
    ImGui::SetWindowPos(results, {origin.x + 40, origin.y + 100}, ImGuiCond_Always);
    ImGui::SetWindowSize(results, {800, 590}, ImGuiCond_Always);
    for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
        imgui::setTheme(theme);
        frame(theme == imgui::EditorTheme::Black ? "import_results_black" : "import_results_blue");
        results = ImGui::FindWindowByName("Import results");
        require(results && results->Active, "Rejected external source has no inspectable import result.");
    }
    editor.closeImportResults();
    ImGui::FocusWindow(panel);
    frame("import_results_closed");
    drop({obj});
    frame("import_duplicate_delivery");
    require(editor.lastImportReport().importedCount() == 1 && editor.lastImportReport().items[0].id != id &&
        editor.lastImportReport().items[0].assetPath != firstPath && fs::is_regular_file(firstPath),
        "A repeated native import overwrote an existing asset or reused its identity.");
    editor.closeImportResults();
    ImGui::FocusWindow(panel);
    frame("import_warning_ready");
    drop({fbx});
    frame("import_warning_delivery");
    require(editor.lastImportReport().importedCount() == 1 && editor.lastImportReport().rejectedCount() == 0 &&
        editor.lastImportReport().warningCount() > 0,
        "Geometry cleanup warnings were treated as a rejected import.");
    frame("import_warning_results");
    results = ImGui::FindWindowByName("Import results");
    require(results && results->Active, "A successful import with warnings did not open its result window.");
    editor.closeImportResults();
    ImGui::FocusWindow(panel);
    frame("import_warning_closed");
    // A native drop received by an independent tool window must not import.
    auto* material = ImGui::FindWindowByName("Material Editor###MaterialWorkspace");
    require(material && material->Viewport->PlatformHandle, "Import regression requires the existing material tool window.");
    const auto materialPosition = material->Pos;
    const auto obscuredContentPoint = contentArea()->InnerRect.GetCenter();
    ImGui::SetWindowPos(material, {obscuredContentPoint.x - 100, obscuredContentPoint.y - 100}, ImGuiCond_Always);
    frame("import_overlapping_tool");
    require(material->Viewport != contentArea()->Viewport,
        "The independent tool lost its native viewport during the overlap check.");
    nativeDrop(static_cast<HWND>(material->Viewport->PlatformHandle), obscuredContentPoint, {obj});
    frame("import_tool_rejected");
    require(editor.lastImportReport().importedCount() == 0 && editor.lastImportReport().rejectedCount() == 1,
        "Dropping into another native tool window imported into hidden Content.");
    ImGui::SetWindowPos(material, materialPosition, ImGuiCond_Always);
    editor.closeImportResults();
    editor.selectContentFolder("engine://");
    ImGui::FocusWindow(panel);
    frame("import_engine_ready");
    drop({obj});
    frame("import_engine_rejected");
    require(editor.lastImportReport().importedCount() == 0 && editor.lastImportReport().rejectedCount() == 1,
        "An external drop wrote into the read-only Engine mount.");
    editor.closeImportResults();
    editor.selectContentFolder("Scenes");
    ImGui::FocusWindow(panel);
    frame("import_outside_ready");
    nativeDrop(static_cast<HWND>(panel->Viewport->PlatformHandle), {origin.x + 2, origin.y + 2}, {obj});
    frame("import_outside_rejected");
    require(editor.lastImportReport().importedCount() == 0 && editor.lastImportReport().rejectedCount() == 1,
        "A native drop outside the Content asset region was imported.");
    require(editor.world().actors.size() == actorCount && editor.selectedActor() == selectedActor &&
        editor.undoStack().size() == historyCount, "Importing changed scene Actors, selection, or undo history.");
    fs::remove(obj);
    assets::AssetLibrary reloaded;
    reloaded.createDefaults();
    std::string error;
    require(reloaded.loadMeshes(content, error) && reloaded.findMesh(id),
        "Imported geometry could not reload after deleting the external source.");
    require(reloaded.saveMeshes(content, error), "Saving meshes did not preserve current-folder imports.");
    assets::AssetRegistry registry;
    registry.setContentRoot(content);
    require(registry.scan(error) && std::count_if(registry.entries().begin(), registry.entries().end(),
        [id](const auto& entry) { return entry.id == id; }) == 1,
        "Saving meshes duplicated a current-folder imported asset.");
    require(reloaded.findMesh(fbxId) && reloaded.findMesh(fbxId)->indices.size() == 36,
        "Native FBX cube import did not persist its triangulated geometry.");
    scene::World preview;
    preview.createActor("Imported FBX cube", fbxId, reloaded.materials.front().id);
    renderer::SceneSettings previewSettings;
    previewSettings.cameraDistance = 6;
    previewSettings.cameraTarget[1] = 0;
    require(renderer.render(previewSettings, preview, reloaded, error), "Imported FBX geometry failed GPU rendering.");
    require(renderer.saveScreenshot((output / "import_fbx_render.bmp").c_str(), error),
        "Cannot capture the imported FBX render.");
    editor.closeImportResults();
    editor.selectContentFolder(originalFolder);
    imgui::setTheme(originalTheme);
    std::cout << "PASS: native Content external multi-file import, persisted strict/skip options, Unicode, cleanup warnings and unsupported results in both themes, "
        "duplicate names, read-only Engine/outside/tool rejection, unchanged Actors/undo, source-independent reload and save.\n";
}
