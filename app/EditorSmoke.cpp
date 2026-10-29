#include "EditorSmoke.h"
#include "DiagnosticsSmoke.h"
#include <RestArtEngine/Core/TraceProfiler.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui_internal.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <RestArtEngine/Assets/AssetRegistry.h>
#include <RestArtEngine/Assets/AssetFile.h>
#include <nlohmann/json.hpp>
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <array>
#include <vector>

namespace {
void require(bool value, const std::string& error) { if (!value) throw std::runtime_error(error); }
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
// Compare only the actual scene image: a slider value, activity text, or hover
// highlight changing elsewhere in the editor is not evidence of GPU feedback.
std::string scenePixels(const std::filesystem::path& path, const rae::renderer::SceneSettings& settings) {
    const auto bmp = contents(path);
    require(bmp.size() >= 54 && bmp[0] == 'B' && bmp[1] == 'M', "Invalid material-preview screenshot.");
    std::uint32_t offset = 0;
    std::int32_t width = 0, signedHeight = 0;
    std::memcpy(&offset, bmp.data() + 10, sizeof(offset));
    std::memcpy(&width, bmp.data() + 18, sizeof(width));
    std::memcpy(&signedHeight, bmp.data() + 22, sizeof(signedHeight));
    const auto height = std::abs(signedHeight);
    require(width > 0 && height > 0 && offset + static_cast<std::size_t>(width) * height * 4 <= bmp.size(),
        "Invalid material-preview screenshot dimensions.");
    const int left = std::clamp(settings.viewportX, 0, width);
    const int right = std::clamp(settings.viewportX + settings.viewportWidth, left, width);
    const int top = std::clamp(settings.viewportY, 0, height);
    const int bottom = std::clamp(settings.viewportY + settings.viewportHeight, top, height);
    require(right > left && bottom > top, "Material preview has no visible scene pixels.");
    std::string pixels;
    for (int y = top; y < bottom; ++y) {
        const auto row = signedHeight < 0 ? y : height - y - 1;
        pixels.append(bmp.data() + offset + (static_cast<std::size_t>(row) * width + left) * 4,
            static_cast<std::size_t>(right - left) * 4);
    }
    return pixels;
}
std::array<int, 3> centerPixel(const std::filesystem::path& path) {
    const auto bmp = contents(path);
    require(bmp.size() >= 54, "Missing two-sided rendering screenshot.");
    std::uint32_t offset = 0;
    std::int32_t width = 0, signedHeight = 0;
    std::memcpy(&offset, bmp.data() + 10, sizeof(offset));
    std::memcpy(&width, bmp.data() + 18, sizeof(width));
    std::memcpy(&signedHeight, bmp.data() + 22, sizeof(signedHeight));
    const int height = std::abs(signedHeight), y = height / 2;
    const int row = signedHeight < 0 ? y : height - y - 1;
    const auto pixel = offset + (static_cast<std::size_t>(row) * width + width / 2) * 4;
    require(width > 0 && height > 0 && pixel + 4 <= bmp.size(), "Invalid two-sided rendering screenshot.");
    return {static_cast<unsigned char>(bmp[pixel + 2]), static_cast<unsigned char>(bmp[pixel + 1]),
        static_cast<unsigned char>(bmp[pixel])};
}
struct ImGuiContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ImGuiContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ImGuiContextScope() { ImGui::SetCurrentContext(previous); }
};
// Inject after the Win32 backend gathers input, immediately before ImGui consumes it.
// This exercises normal widget behavior without moving the user's desktop cursor.
struct SmokeInput {
    ImGuiContext* context{};
    ImGuiID hook{};
    ImVec2 mouse{-100, -100};
    ImVec2 displaySize{};
    std::function<void(ImGuiIO&)> events;
    void attach(ImGuiContext* target) {
        context = target;
        context->IO.ConfigInputTrickleEventQueue = false;
        ImGuiContextHook callback;
        callback.Type = ImGuiContextHookType_NewFramePre;
        callback.UserData = this;
        callback.Callback = [](ImGuiContext* ctx, ImGuiContextHook* hook) {
            auto& input = *static_cast<SmokeInput*>(hook->UserData);
            ctx->IO.AddFocusEvent(true);
            ctx->IO.AddMousePosEvent(input.mouse.x, input.mouse.y);
            ImGuiViewportP* hovered = nullptr;
            // Multi-viewports use desktop coordinates. Override the real
            // desktop cursor's viewport together with its synthetic position.
            for (int index = ctx->Viewports.Size - 1; index >= 0; --index) {
                auto* viewport = ctx->Viewports[index];
                if (!(viewport->Flags & (ImGuiViewportFlags_NoInputs | ImGuiViewportFlags_IsMinimized)) &&
                    viewport->GetMainRect().Contains(input.mouse)) {
                    hovered = viewport;
                    break;
                }
            }
            if (ctx->IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
                ctx->IO.AddMouseViewportEvent(hovered ? hovered->ID : 0);
            if (input.displaySize.x > 0) ctx->IO.DisplaySize = input.displaySize;
            if (input.events) { auto pending = std::move(input.events); pending(ctx->IO); }
            // Real editor shortcuts also respect the foreground HWND. Activate
            // only this test's target window on a press, just as an OS click
            // would; merely hovering another viewport must not steal focus.
            if (hovered && hovered->PlatformHandle) {
                bool pressed = false;
                for (const auto& event : ctx->InputEventsQueue)
                    pressed |= event.Type == ImGuiInputEventType_MouseButton && event.MouseButton.Down;
                if (pressed) {
                    const auto window = static_cast<HWND>(hovered->PlatformHandle);
                    if (!IsWindowVisible(window)) ShowWindow(window, SW_SHOWNOACTIVATE);
                    SetForegroundWindow(window);
                }
            }
        };
        hook = ImGui::AddContextHook(context, &callback);
    }
    void detach() { if (hook) { ImGui::RemoveContextHook(context, hook); hook = 0; } }
    ~SmokeInput() { detach(); }
};
}

void runEditorSmoke(rae::renderer::SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
                    rae::editor::SceneEditor& editor, const rae::project::ProjectPaths& project,
                    const std::filesystem::path& outputDirectory) {
    using namespace rae;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    auto& world = editor.world();
    auto& assets = editor.assets();
    require(assets.findMaterial(assets::EngineDefaultMaterialId) &&
        assets.findMaterial(assets::EngineDefaultMaterialId)->origin == assets::AssetOrigin::Engine,
        "Editor initialization did not mount the engine default material.");
    const auto engineMaterialPath = project.engineRoot / "Content/Materials/DefaultMaterial.rasset";
    const auto engineMaterialBytes = contents(engineMaterialPath);
    const auto mountEngineMaterials = [&] {
        std::string mountError;
        require(assets.loadEngineMaterials(project.engineRoot / "Content/Materials", mountError), mountError);
    };
    world.createDefaultScene(); assets.createDefaults();
    mountEngineMaterials();
    // Command registry: the same ids back the buttons, the Edit menu and the
    // keyboard, and enablement tracks editor state.
    require(editor.commands().contains("actor.delete") && editor.commands().contains("edit.undo"),
        "Editor command registry is missing core commands.");
    require(editor.commands().enabled("actor.delete") == (editor.selectedActor() != 0),
        "Delete command enablement does not follow the Actor selection.");
    {
        // Asset registry + content moves. Everything happens in a temporary
        // content root so the project files are never touched.
        const auto root = std::filesystem::temp_directory_path() / "rae-content-registry-test";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "Materials");
        std::filesystem::create_directories(root / "Meshes");
        std::filesystem::create_directories(root / "Scenes");
        {
            std::ofstream material(root / "Materials" / "1.ramat", std::ios::binary);
            material << R"({"format":"RAEngine.Material","version":1,"material":{"id":7,"name":"RegistryMat","baseColor":[1.0,0.0,0.0],"roughness":0.5,"metallic":0.0,"grid":false,"twoSided":true}})";
        }
        {
            std::ofstream obj(root / "Meshes" / "tri.obj", std::ios::binary);
            obj << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        }
        scene::World testWorld;
        assets::AssetLibrary testAssets;
        testAssets.createDefaults();
        assets::AssetId importedId = 0;
        std::string testError;
        require(testAssets.importObj(root / "Meshes" / "tri.obj", importedId, testError), testError);
        testWorld.createActor("Tri", importedId, 1);
        require(testWorld.save(root / "Scenes" / "Test.rascene", testAssets, testError), testError);

        assets::AssetRegistry registry;
        registry.setContentRoot(root);
        require(registry.scan(testError), testError);
        require(registry.count(assets::AssetType::Material) == 1 &&
            registry.count(assets::AssetType::Scene) == 1 &&
            registry.count(assets::AssetType::Mesh) == 1,
            "The asset registry did not classify the test content.");
        const auto& folders = registry.folders();
        require(std::find(folders.begin(), folders.end(), "Materials") != folders.end() &&
            std::find(folders.begin(), folders.end(), "Scenes") != folders.end(),
            "The asset registry did not list content folders.");
        const auto* materialEntry = registry.find("Materials/1.ramat");
        require(materialEntry && materialEntry->id == 7 && materialEntry->name == "1",
            "The registry did not preserve the material GUID and display filename.");

        // Moving a scene re-bases its relative OBJ reference so the imported mesh
        // still resolves from the new folder.
        require(assets::moveContentEntry(root, "Scenes/Test.rascene", "Scenes/Sub", testError), testError);
        scene::World movedWorld;
        assets::AssetLibrary movedAssets = testAssets;
        require(movedWorld.load(root / "Scenes" / "Sub" / "Test.rascene", movedAssets, testError), testError);
        require(!movedWorld.actors.empty() && movedAssets.findMesh(movedWorld.actors.front().mesh.meshId) != nullptr,
            "A moved scene lost its imported mesh reference.");
        require(registry.scan(testError), testError);
        require(registry.count(assets::AssetType::Scene) == 1 && registry.folders().size() >= 4,
            "The registry did not refresh after the move.");
        std::filesystem::remove_all(root);
    }

    {
        // Scene v2 round-trip through disk: materials live in .rasset files, the
        // scene stores GUID references, and a scene loaded without its materials
        // must fail instead of silently rendering the wrong material.
        std::string v2Error;
        const auto v2Root = output / "SceneV2RoundTrip";
        std::filesystem::remove_all(v2Root);
        assets::AssetLibrary v2Assets;
        v2Assets.createDefaults();
        v2Assets.findMaterial(1)->roughness = 0.11f;
        require(v2Assets.saveMaterials(v2Root / "Materials", v2Error), v2Error);
        scene::World v2World;
        v2World.createDefaultScene(v2Assets);
        require(v2World.save(v2Root / "Scenes" / "V2.rascene", v2Assets, v2Error), v2Error);
        assets::AssetLibrary reloadedAssets;
        reloadedAssets.createDefaults();
        require(reloadedAssets.loadMaterials(v2Root / "Materials", v2Error), v2Error);
        scene::World reloadedWorld;
        require(reloadedWorld.load(v2Root / "Scenes" / "V2.rascene", reloadedAssets, v2Error), v2Error);
        require(reloadedWorld.actors.size() == v2World.actors.size() &&
            reloadedAssets.findMaterial(1) != nullptr &&
            reloadedAssets.findMaterial(1)->roughness == v2Assets.findMaterial(1)->roughness,
            "A scene v2 round-trip lost its actors or material values.");
        assets::AssetLibrary emptyAssets;
        scene::World rejectedWorld;
        require(!rejectedWorld.load(v2Root / "Scenes" / "V2.rascene", emptyAssets, v2Error) && !v2Error.empty(),
            "A scene v2 loaded without its materials instead of reporting the missing reference.");
        // The editor path: Content materials load first, then the scene resolves
        // its GUID references without any embedded snapshot.
        rae::project::ProjectPaths v2Project = project;
        v2Project.contentRoot = v2Root;
        v2Project.defaultScene = v2Root / "Scenes" / "V2.rascene";
        rae::editor::SceneEditor v2Editor;
        require(v2Editor.initialize(v2Project, renderer, v2Error), v2Error);
        assets::AssetQuery v2MaterialQuery;
        v2MaterialQuery.type = assets::AssetType::Material;
        v2MaterialQuery.recursive = true;
        require(v2Editor.world().actors.size() == v2World.actors.size() &&
            v2Editor.assetRegistry().query(v2MaterialQuery).size() == 2 &&
            v2Editor.assets().findMaterial(assets::EngineDefaultMaterialId),
            "The editor did not load a scene v2 with its GUID materials.");
        // Built-in primitives are generated as .rasset files the first time a
        // project opens; later runs reuse them and never duplicate the files.
        {
            const auto builtinDir = v2Root / "Meshes" / "Builtin";
            assets::AssetLibrary builtinAssets;
            builtinAssets.createDefaults();
            require(builtinAssets.ensureBuiltinMeshes(builtinDir, v2Error), v2Error);
            const auto countBuiltins = [&]() {
                std::size_t count = 0;
                for (const auto& entry : std::filesystem::directory_iterator(builtinDir))
                    if (entry.path().extension() == ".rasset") ++count;
                return count;
            };
            require(countBuiltins() == 3, "Built-in primitive assets were not generated.");
            require(builtinAssets.ensureBuiltinMeshes(builtinDir, v2Error), v2Error);
            require(countBuiltins() == 3, "Regenerating the built-in primitives duplicated files.");
            assets::AssetLibrary builtinReloaded;
            builtinReloaded.createDefaults();
            require(builtinReloaded.loadMeshes(v2Root / "Meshes", v2Error), v2Error);
            const auto* sphere = builtinReloaded.findMesh(1);
            require(sphere != nullptr && sphere->vertices.size() == builtinAssets.findMesh(1)->vertices.size() &&
                sphere->indices.size() == builtinAssets.findMesh(1)->indices.size(),
                "A built-in primitive did not round-trip through its generated asset.");

            // Thumbnails: generated on the CPU and stored in the container.
            const auto materialThumb = assets::renderMaterialThumbnail(*builtinAssets.findMaterial(1), 32);
            require(materialThumb.valid() && materialThumb.rgba.size() == 32u * 32u * 4u,
                "The material thumbnail was not rendered.");
            std::size_t opaquePixels = 0;
            for (std::size_t index = 3; index < materialThumb.rgba.size(); index += 4)
                if (materialThumb.rgba[index] != 0) ++opaquePixels;
            require(opaquePixels > 100, "The material thumbnail is empty.");
            const auto meshThumb = assets::renderMeshThumbnail(*builtinAssets.findMesh(3), 32);
            require(meshThumb.valid() && meshThumb.rgba.size() == 32u * 32u * 4u,
                "The mesh thumbnail was not rendered.");
            assets::AssetFile containerFile = assets::makeMeshAssetFile(*builtinAssets.findMesh(3));
            assets::attachThumbnail(containerFile, meshThumb);
            assets::Thumbnail extractedThumb;
            require(assets::extractThumbnail(containerFile, extractedThumb, v2Error) &&
                extractedThumb.rgba == meshThumb.rgba && extractedThumb.width == meshThumb.width,
                "The container did not round-trip its thumbnail.");
            // Legacy containers with the old .raasset extension stay readable.
            const auto legacyDir = v2Root / "LegacyContainers";
            std::filesystem::create_directories(legacyDir);
            std::filesystem::copy_file(builtinDir / "Cube.rasset", legacyDir / "Cube.raasset");
            assets::AssetLibrary legacyLibrary;
            legacyLibrary.createDefaults();
            require(legacyLibrary.loadMeshes(legacyDir, v2Error), v2Error);
            require(legacyLibrary.findMesh(3) != nullptr &&
                legacyLibrary.findMesh(3)->vertices.size() == builtinAssets.findMesh(3)->vertices.size(),
                "A legacy .raasset container was not readable after the rename.");
        }
        // Mesh assets: import an OBJ, persist it as a .rasset, then reload both
        // the geometry and a scene that only references the mesh GUID.
        {
            const auto objPath = v2Root / "Meshes" / "tri.obj";
            std::filesystem::create_directories(objPath.parent_path());
            std::ofstream obj(objPath, std::ios::binary);
            obj << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
            obj.close();
            assets::AssetLibrary meshAssets = v2Assets;
            assets::AssetId meshId = 0;
            require(meshAssets.importObj(objPath, meshId, v2Error), v2Error);
            auto* importedMesh = meshAssets.findMesh(meshId);
            importedMesh->id = assets::generateAssetId();
            importedMesh->name = "Tri";
            const auto newMeshId = importedMesh->id;
            require(meshAssets.saveMeshes(v2Root / "Meshes", v2Error), v2Error);
            assets::AssetLibrary meshReloaded = v2Assets;
            {
                assets::Thumbnail fromDisk;
                const auto meshPath = v2Root / "Meshes" / assets::AssetLibrary::meshAssetFileName(*meshAssets.findMesh(newMeshId));
                require(assets::readAssetThumbnail(meshPath, fromDisk, v2Error) && fromDisk.valid(),
                    "A saved mesh asset does not carry a readable thumbnail.");
            }
            require(meshReloaded.loadMeshes(v2Root / "Meshes", v2Error), v2Error);
            const auto* reloadedMesh = meshReloaded.findMesh(newMeshId);
            require(reloadedMesh && reloadedMesh->vertices.size() == 3 && reloadedMesh->indices.size() == 3,
                "A mesh asset did not round-trip its geometry.");
            scene::World meshScene;
            meshScene.createActor("Tri", newMeshId, v2Assets.findMaterial(1)->id);
            require(meshScene.save(v2Root / "Scenes" / "Mesh.rascene", meshReloaded, v2Error), v2Error);
            assets::AssetLibrary meshResolver = v2Assets;
            require(meshResolver.loadMeshes(v2Root / "Meshes", v2Error), v2Error);
            scene::World meshSceneReloaded;
            require(meshSceneReloaded.load(v2Root / "Scenes" / "Mesh.rascene", meshResolver, v2Error), v2Error);
            require(meshSceneReloaded.actors.size() == 1 &&
                meshResolver.findMesh(meshSceneReloaded.actors.front().mesh.meshId) != nullptr,
                "A scene v3 lost its mesh reference.");
            auto importedProject = v2Project;
            importedProject.defaultScene = v2Root / "Scenes" / "Mesh.rascene";
            rae::editor::SceneEditor importedEditor;
            require(importedEditor.initialize(importedProject, renderer, v2Error), v2Error);
            require(importedEditor.world().actors.size() == 1 &&
                importedEditor.world().actors.front().mesh.meshId == newMeshId,
                "Initial editor load did not resolve a scene v3 imported mesh from Content.");
            assets::AssetLibrary missingMesh = v2Assets;
            scene::World rejectedMeshScene;
            require(!rejectedMeshScene.load(v2Root / "Scenes" / "Mesh.rascene", missingMesh, v2Error) &&
                !v2Error.empty(), "A scene v3 loaded without its mesh instead of reporting the reference.");
        }

    }
    {
        // Theme preference round-trips through the per-user file the app reads
        // before creating the ImGui context (palette itself is covered by the
        // captured screenshots of both themes).
        using rae::imgui::EditorTheme;
        require(rae::imgui::kEditorThemeCount == 2, "Expected exactly two editor themes.");
        require(std::string(rae::imgui::themeName(EditorTheme::Black)) == "Black" &&
            std::string(rae::imgui::themeName(EditorTheme::DarkBlue)) == "Dark Blue",
            "Theme names changed unexpectedly.");
        const auto path = std::filesystem::temp_directory_path() / "rae-theme-preference-test.ini";
        rae::imgui::saveThemePreference(path, EditorTheme::DarkBlue);
        require(rae::imgui::loadThemePreference(path) == EditorTheme::DarkBlue,
            "Theme preference did not round-trip.");
        rae::imgui::saveThemePreference(path, EditorTheme::Black);
        require(rae::imgui::loadThemePreference(path) == EditorTheme::Black,
            "Theme preference did not round-trip back to Black.");
        // The UI menu edits the outline settings; they share the same file and
        // must survive a reload without disturbing the palette.
        auto preferences = rae::imgui::loadUiPreferences(path);
        preferences.theme = EditorTheme::DarkBlue;
        preferences.selectionOutline = false;
        preferences.outlineThickness = 4.5f;
        preferences.outlineThemeColor = false;
        preferences.outlineColor[0] = 0.1f;
        preferences.outlineColor[1] = 0.2f;
        preferences.outlineColor[2] = 0.3f;
        preferences.outlineFrontObjects = true;
        rae::imgui::saveUiPreferences(path, preferences);
        const auto reloaded = rae::imgui::loadUiPreferences(path);
        require(reloaded.theme == EditorTheme::DarkBlue && !reloaded.selectionOutline &&
            std::abs(reloaded.outlineThickness - 4.5f) < 0.001f && !reloaded.outlineThemeColor &&
            std::abs(reloaded.outlineColor[0] - 0.1f) < 0.001f &&
            std::abs(reloaded.outlineColor[2] - 0.3f) < 0.001f && reloaded.outlineFrontObjects,
            "Selection outline preferences did not round-trip.");
        std::filesystem::remove(path);
    }
    {
        // UndoStack merge (Godot MERGE_ENDS): same-key commits inside the window
        // collapse into one entry that keeps the oldest undo state.
        rae::editor::UndoStack merged;
        int value = 0;
        merged.push("Edit A", [&] { value = 0; }, [&] { value = 1; }, { "k", 1000 });
        merged.push("Edit B", [&] { value = 0; }, [&] { value = 2; }, { "k", 1000 });
        require(merged.size() == 1, "Merged commits created more than one history entry.");
        require(merged.undo() && value == 0, "Merged undo did not restore the oldest state.");
        require(merged.redo() && value == 2, "Merged redo did not apply the latest state.");
        require(merged.size() == 1, "Undo/redo changed the merged history size.");
    }
    renderer::SceneSettings settings;
    std::string error;
    ImGuiContext* context = nullptr;
    std::string renderedUiText;
    const auto capture = [&](const char* name, bool drawEditor = false, bool recordUiText = false,
                             bool useMaterialPreview = true) {
        struct TraceFrame {
            TraceFrame() { diagnostics::beginFrame(); }
            ~TraceFrame() { diagnostics::endFrame(); }
        } traceFrame;
        diagnostics::TraceScope trace("Editor smoke frame", "Validation");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            require(message.message != WM_QUIT, "Closing an editor tool window quit the main application.");
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        ui.beginFrame(1.0f / 60.0f);
        context = ImGui::GetCurrentContext();
        settings.viewportWidth = settings.viewportHeight = 0;
        settings.editorViewport = false;
        settings.viewportVisible = true;
        if (drawEditor) {
            bool vsync = false, screenshot = false;
            if (recordUiText) ImGui::LogToBuffer();
            editor.draw(settings, renderer, vsync, screenshot);
            // A viewport tab that was just floated can report a degenerate image
            // rectangle for one frame; retry so captures never record the cleared
            // surface and the pipeline comparison stays deterministic.
            for (int retry = 0; retry < 3 && settings.viewportVisible && settings.viewportWidth < 1; ++retry) {
                ui.endFrame();
                ui.beginFrame(1.0f / 60.0f);
                context = ImGui::GetCurrentContext();
                settings.viewportWidth = settings.viewportHeight = 0;
                settings.editorViewport = false;
                settings.viewportVisible = true;
                editor.draw(settings, renderer, vsync, screenshot);
            }
            if (recordUiText) {
                renderedUiText = context->LogBuffer.c_str();
                ImGui::LogFinish();
                if (std::string_view(name).starts_with("diagnostics_")) {
                    std::ofstream textOutput(output / (std::string(name) + ".txt"), std::ios::binary);
                    textOutput << renderedUiText;
                }
            }
    
        if (settings.viewportVisible) {
                const auto* viewport = ImGui::FindWindowByName("Scene Viewport##Interaction");
                require(viewport != nullptr, "Visible renderer viewport has no ImGui window.");
                const auto& rect = viewport->WorkRect;
                const auto origin = ImGui::GetMainViewport()->Pos;
                require(std::abs(settings.viewportX - (rect.Min.x - origin.x)) <= 1 &&
                    std::abs(settings.viewportY - (rect.Min.y - origin.y)) <= 1 &&
                    std::abs(settings.viewportWidth - rect.GetWidth()) <= 2 &&
                    std::abs(settings.viewportHeight - rect.GetHeight()) <= 2,
                    "Scene renderer rectangle differs from this frame's dock content rectangle.");
            }
        }
        require(renderer.render(settings, world, assets, error,
            useMaterialPreview ? editor.materialPreview() : nullptr), error);
        ui.endFrame();
        const auto screenshotPath = output / (std::string(name) + ".bmp");
        if (!renderer.saveScreenshot(screenshotPath.c_str(), error)) {
            int screenshotErrno = 0;
            unsigned long screenshotDosErrno = 0;
            _get_errno(&screenshotErrno);
            _get_doserrno(&screenshotDosErrno);
            const auto utf8 = screenshotPath.generic_u8string();
            require(false, error + " Output=" + std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()) +
                " errno=" + std::to_string(screenshotErrno) + " _doserrno=" + std::to_string(screenshotDosErrno));
        }
        require(renderer.present(false, error), error);
    };
    {
        // View the gizmo from the opposite side to inspect auto-flipped axes.
        const float originalYaw = settings.cameraYaw;
        const auto originalPreferences = imgui::uiPreferences();
        const auto gizmoPreferencesPath = output / "gizmo-preferences.ini";
        { std::ofstream legacy(gizmoPreferencesPath); legacy << "theme=black\n"; }
        require(imgui::loadUiPreferences(gizmoPreferencesPath).gizmoNegativeAxisHints,
            "UI preferences without the gizmo option should default to negative-axis hints enabled.");
        imgui::uiPreferences().gizmoNegativeAxisHints = false;
        settings.cameraYaw = -0.65f;
        capture("gizmo_solid_negative_axes_preroll", true);
        capture("gizmo_solid_negative_axes", true);
        imgui::uiPreferences().gizmoNegativeAxisHints = true;
        imgui::saveUiPreferences(gizmoPreferencesPath, imgui::uiPreferences());
        imgui::uiPreferences() = imgui::loadUiPreferences(gizmoPreferencesPath);
        require(imgui::uiPreferences().gizmoNegativeAxisHints, "Enabled negative-axis hints did not persist.");
        capture("gizmo_hatched_negative_axes", true);
        require(scenePixels(output / "gizmo_solid_negative_axes.bmp", settings) !=
            scenePixels(output / "gizmo_hatched_negative_axes.bmp", settings),
            "Enabling negative-axis hints did not change the displayed gizmo.");
        imgui::uiPreferences().gizmoNegativeAxisHints = false;
        imgui::saveUiPreferences(gizmoPreferencesPath, imgui::uiPreferences());
        require(!imgui::loadUiPreferences(gizmoPreferencesPath).gizmoNegativeAxisHints,
            "Disabled negative-axis hints did not persist.");
        capture("gizmo_solid_negative_axes_restored", true);
        imgui::uiPreferences() = originalPreferences;
        settings.cameraYaw = originalYaw;
        // Content browser with thumbnails: the first frame builds the layout, the
        // second one captures the panels.
        editor.selectContentFolder("Meshes/Builtin");
        capture("content_thumbnails_preroll", true);
        capture("content_thumbnails", true);
        editor.setContentGridView(true);
        capture("content_grid", true);
        editor.setContentGridView(true);
        editor.selectContentFolder("Scenes");
        capture("content_scenes_preroll", true);
        capture("content_scenes", true);
        editor.setContentGridView(false);
        {
            // Double-click equivalent: the asset details window previews the file.
            editor.showAssetDetails("Meshes/Builtin/Sphere.rasset");
            capture("asset_details_mesh_preroll", true);
            capture("asset_details_mesh", true);
            { ImGuiContextScope active(context); require(ImGui::FindWindowByName("Asset Details - Sphere###AssetDetails") != nullptr,
                "The asset details window did not open for a mesh asset."); }
            editor.showAssetDetails("Scenes/Default.rascene");
            capture("asset_details_scene_preroll", true);
            capture("asset_details_scene", true, true);
            { ImGuiContextScope active(context); require(ImGui::FindWindowByName("Asset Details - Default###AssetDetails") != nullptr,
                "The asset details window did not open for a scene asset."); }
            require(renderedUiText.find("Scenes/Default.rascene") != std::string::npos,
                "Asset Details did not switch from the mesh to the requested scene.");
        }
        editor.selectContentFolder("Meshes/Builtin");
        {
            const auto* sceneEntry = editor.assetRegistry().find("Scenes/Default.rascene");
            require(sceneEntry && sceneEntry->thumbnail.valid(),
                "A scene asset did not get a generated thumbnail.");
        }
        editor.selectContentFolder(std::string());
        const auto* cubeEntry = editor.assetRegistry().find("Meshes/Builtin/Cube.rasset");
        require(cubeEntry && cubeEntry->thumbnail.valid(),
            "The generated built-in mesh thumbnail is missing from the registry.");
        const auto selectedBeforeBrowsing = editor.selectedActor();
        editor.closeAssetDetails();
        editor.selectContentFolder("Meshes/Builtin");
        require(editor.navigateContentUp() && editor.contentFolder() == "Meshes",
            "Content Up did not select the parent directory.");
        require(editor.navigateContentBack() && editor.contentFolder() == "Meshes/Builtin",
            "Content Back did not restore the previous directory.");
        require(editor.navigateContentForward() && editor.contentFolder() == "Meshes",
            "Content Forward did not restore the next directory.");
        editor.selectContentFolder("");
        editor.setContentRecursive(true);
        editor.setContentTypeFilter(assets::AssetType::Mesh);
        editor.setContentSearch("sPhErE");
        const auto sphereResults = editor.visibleContentEntries();
        require(!sphereResults.empty() && std::all_of(sphereResults.begin(), sphereResults.end(), [](const auto* entry) {
            return entry->type == assets::AssetType::Mesh;
        }), "Recursive Content search did not combine case-insensitive search with the type filter.");
        editor.setContentGridView(true);
        capture("content_search_mesh", true);
        editor.setContentTypeFilter(assets::AssetType::Material);
        require(editor.visibleContentEntries().empty(), "Content type filter did not exclude mesh search results.");
        capture("content_empty_results", true);
        editor.setContentSearch("");
        editor.setContentTypeFilter(assets::AssetType::Unknown);
        editor.setContentRecursive(false);
        editor.setContentSort(assets::AssetSort::Name);
        capture("content_root_folders", true);
        editor.selectContentFolder("Meshes/Builtin");
        {
            ImGuiContextScope active(context);
            const auto originalTheme = imgui::currentTheme();
            for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
                imgui::setTheme(theme);
                editor.setContentGridView(true);
                capture(theme == imgui::EditorTheme::Black ? "content_browser_black" : "content_browser_blue", true);
                editor.setContentGridView(false);
                capture(theme == imgui::EditorTheme::Black ? "content_list_black" : "content_list_blue", true);
                // Mixed types in one view make semantic accents and the selected
                // row/card distinguishable in actual screenshots of both themes.
                editor.selectContentFolder("");
                editor.setContentRecursive(true);
                // Every asset path has an extension; this search suppresses
                // folder cards so all available file types fit the palette view.
                editor.setContentSearch(".");
                editor.setContentSort(assets::AssetSort::Type);
                editor.selectContentAsset("Scenes/Default.rascene");
                editor.setContentGridView(true);
                capture(theme == imgui::EditorTheme::Black ? "content_type_colors_black" : "content_type_colors_blue", true);
                editor.setContentGridView(false);
                capture(theme == imgui::EditorTheme::Black ? "content_type_colors_list_black" : "content_type_colors_list_blue", true);
                editor.setContentSearch("");
                editor.setContentRecursive(false);
                editor.setContentSort(assets::AssetSort::Name);
                editor.selectContentFolder("Meshes/Builtin");
            }
            imgui::setTheme(originalTheme);
        }
        require(editor.selectedActor() == selectedBeforeBrowsing,
            "Content navigation or filtering changed the selected Actor.");
        editor.setContentGridView(true);
        editor.selectContentFolder("");
        std::cout << "PASS: Content navigation history, recursive search, type filters and independent Actor selection.\n";
    }
        {
            assets::Thumbnail directThumb;
            std::string thumbError;
            require(assets::readAssetThumbnail(project.contentRoot / "Meshes" / "Builtin" / "Cube.rasset",
                directThumb, thumbError), thumbError);
        }

    const std::string originalSource = renderer.shaderSource();
    // ImGui text input filters CR. Keep API/disk checks byte-exact and compare
    // UI round trips with the text the widget accepts on CRLF checkouts.
    auto originalUiSource = originalSource;
    originalUiSource.erase(std::remove(originalUiSource.begin(), originalUiSource.end(), '\r'),
        originalUiSource.end());
    // Actual Apply uses the same API as the editor button.
    const auto revision = renderer.shaderRevision();
    const std::string changedSource = R"(
Surface EvaluateMaterial(float3 worldPosition, float3 normal, Surface material)
{
    material.albedo = float3(0.03, 0.72, 0.12);
    material.roughness = 0.55;
    material.metallic = 0;
    return material;
})";
    capture("shader_before");
    require(renderer.applyShaderSource(changedSource, error), error);
    require(renderer.shaderRevision() == revision + 1, "Apply did not increment shader revision.");
    capture("shader_applied");
    require(contents(output / "shader_before.bmp") != contents(output / "shader_applied.bmp"), "Apply did not change rendered pixels.");
    const auto appliedRevision = renderer.shaderRevision();
    require(!renderer.applyShaderSource("THIS IS NOT HLSL;\n", error), "Invalid HLSL unexpectedly compiled.");
    require(!error.empty(), "Failed compile did not provide diagnostics.");
    std::ofstream(output / "compile-error.txt") << error;
    require(renderer.shaderRevision() == appliedRevision && renderer.shaderSource() == changedSource,
        "Failed Apply changed the active shader.");
    capture("shader_failed_retained");
    require(contents(output / "shader_applied.bmp") == contents(output / "shader_failed_retained.bmp"),
        "Failed Apply changed pixels instead of retaining previous shader.");

    const std::string invalidBinding = R"(
Texture2D<float4> Unexpected : register(t7);
Surface EvaluateMaterial(float3 worldPosition, float3 normal, Surface material)
{
    material.albedo = Unexpected.Load(int3(0,0,0)).rgb;
    return material;
})";
    require(!renderer.applyShaderSource(invalidBinding, error), "Shader with unsupported resource binding was accepted.");
    require(renderer.shaderRevision() == appliedRevision, "Invalid resource contract changed revision.");
    std::ofstream(output / "contract-error.txt") << error;
    require(renderer.applyShaderSource(originalSource, error), error);

    // Test disk Save / Reload against a sandbox copy; never overwrite project shaders.
    const auto sandbox = output / std::filesystem::path(u8"ShaderSandbox_工程");
    std::filesystem::create_directories(sandbox);
    std::filesystem::copy(project.engineRoot / "Shaders", sandbox,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    renderer.setShaderRoot(sandbox);
    renderer.setMaterialShaderPath(sandbox / "Materials/Standard.hlsli");
    require(renderer.applyShaderSource(changedSource, error), error);
    require(renderer.saveShaderSource(error), error);
    require(contents(sandbox / "Materials/Standard.hlsli") == changedSource, "Saved shader content is incorrect.");
    { std::ofstream file(sandbox / "Materials/Standard.hlsli"); file << "INVALID SOURCE"; }
    require(!renderer.reloadShaders(error) && renderer.shaderSource() == changedSource, "Invalid reload did not roll back.");
    // Project-relative nested include and UTF-8 project paths use the same code path as the editor.
    { std::ofstream file(sandbox / "Materials/Tint.hlsli"); file << "float3 UserTint() { return float3(0.1, 0.7, 0.3); }\n"; }
    const std::string withInclude = "#include \"Tint.hlsli\"\nSurface EvaluateMaterial(float3 p, float3 n, Surface m) { m.albedo=UserTint(); return m; }";
    require(renderer.applyShaderSource(withInclude, error), error);
    require(renderer.saveShaderSource(error) && renderer.reloadShaders(error), error);
    renderer.setShaderRoot(project.engineRoot / "Shaders");
    renderer.setMaterialShaderPath(project.shaderRoot / "Materials/Standard.hlsli");
    require(renderer.reloadShaders(error), error);
    require(renderer.shaderSource() == originalSource, "Restoring the project shader failed.");

    {
        const auto savedWorld = world;
        const auto savedAssets = assets;
        const auto savedSettings = settings;
        assets.createDefaults();
        world.actors.clear();
        world.blackBackground = true;
        const auto planeId = world.createActor("Two-sided plane", 2, 1);
        auto* plane = world.findActor(planeId);
        for (auto& scale : plane->transform.scale) scale = .15f;
        auto* material = assets.findMaterial(1);
        material->baseColor[0] = .8f; material->baseColor[1] = .15f; material->baseColor[2] = .04f;
        material->roughness = .7f; material->metallic = 0; material->grid = false;
        settings.cameraYaw = 0;
        settings.cameraDistance = 6;
        std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.rayTracing = false;
        const auto nearPixel = [](const auto& first, const auto& second, int tolerance = 2) {
            for (int channel = 0; channel < 3; ++channel)
                if (std::abs(first[channel] - second[channel]) > tolerance) return false;
            return true;
        };
        // The same authored plane is viewed from either side. Mirroring X
        // preserves its +Y front, while mirroring Y changes that front to -Y.
        for (const auto path : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
            settings.path = path;
            const auto prefix = std::string("two_sided_") +
                (path == renderer::RenderPath::Forward ? "forward" : "deferred");
            for (int mirror = 0; mirror < 3; ++mirror) {
                plane->transform.scale[0] = mirror == 1 ? -.15f : .15f;
                plane->transform.scale[1] = mirror == 2 ? -.15f : .15f;
                for (int side = 0; side < 2; ++side) {
                    const bool front = side == 0;
                    const float normalY = mirror == 2 ? -1.f : 1.f;
                    settings.cameraPitch = (front ? normalY : -normalY) * .6f;
                    settings.debugView = 1;
                    const auto name = prefix + "_mirror" + std::to_string(mirror) + (front ? "_front" : "_back");
                    material->twoSided = false;
                    capture((name + "_single").c_str(), false, false, false);
                    const auto single = centerPixel(output / (name + "_single.bmp"));
                    material->twoSided = true;
                    capture((name + "_double").c_str(), false, false, false);
                    const auto two = centerPixel(output / (name + "_double.bmp"));
                    require(two[0] > 100, "A two-sided plane was invisible from its front or back.");
                    require(front ? nearPixel(single, two) : nearPixel(single, std::array<int, 3>{0, 0, 0}),
                        "Single-sided plane culling disagrees with its authored normal after mirroring.");
                    settings.debugView = 3;
                    capture((name + "_normals").c_str(), false, false, false);
                    const auto normal = centerPixel(output / (name + "_normals.bmp"));
                    const auto expected = std::array<int, 3>{128, settings.cameraPitch > 0 ? 255 : 0, 128};
                    require(nearPixel(normal, expected), "Two-sided World normals did not face the viewer.");
                }
            }
        }

        // A polished ground reflects the underside of a raised red plane.
        // The primary center ray misses that plane, so the center-pixel change
        // isolates secondary-ray face selection rather than raster visibility.
        plane->transform.position[1] = 2.f;
        plane->transform.position[2] = -3.f;
        std::fill(std::begin(plane->transform.scale), std::end(plane->transform.scale), .15f);
        const auto groundId = world.createActor("Two-sided reflection receiver", 2, 2);
        auto* ground = world.findActor(groundId);
        std::fill(std::begin(ground->transform.scale), std::end(ground->transform.scale), .4f);
        auto* groundMaterial = assets.findMaterial(2);
        groundMaterial->baseColor[0] = groundMaterial->baseColor[1] = groundMaterial->baseColor[2] = .8f;
        groundMaterial->roughness = .05f; groundMaterial->metallic = 1.f;
        groundMaterial->grid = false; groundMaterial->twoSided = false;
        auto trianglePlane = *assets.findMesh(2);
        trianglePlane.id = 4; trianglePlane.name = "Triangle two-sided plane";
        trianglePlane.primitive = assets::MeshPrimitive::Imported;
        assets.meshes.push_back(std::move(trianglePlane));
        // Actor and material containers may have moved while adding fixtures.
        plane = world.findActor(planeId); material = assets.findMaterial(1);
        settings.debugView = 0; settings.cameraPitch = .6f;
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        for (const auto path : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
            settings.path = path;
            for (const bool triangles : {false, true}) {
                const auto name = std::string("two_sided_reflection_") +
                    (path == renderer::RenderPath::Forward ? "forward_" : "deferred_") +
                    (triangles ? "triangles" : "analytic");
                plane->mesh.meshId = triangles ? 4 : 2;
                settings.rayTracing = false; material->twoSided = false;
                capture((name + "_no_rays").c_str(), false, false, false);
                const auto noRays = centerPixel(output / (name + "_no_rays.bmp"));
                settings.rayTracing = true;
                capture((name + "_single").c_str(), false, false, false);
                const auto single = centerPixel(output / (name + "_single.bmp"));
                material->twoSided = true;
                capture((name + "_double").c_str(), false, false, false);
                const auto two = centerPixel(output / (name + "_double.bmp"));
                require(!nearPixel(single, two, 4), "Two-sided back faces did not appear in ray reflections.");
                material->twoSided = false;
                plane->transform.scale[1] = -.15f;
                capture((name + "_reversed_front").c_str(), false, false, false);
                require(nearPixel(centerPixel(output / (name + "_reversed_front.bmp")), two),
                    "A two-sided reflected back face differs from the equivalent front-facing normal.");
                plane->transform.scale[1] = .15f;
                settings.rayTracing = false; material->twoSided = true;
                capture((name + "_no_rays_double").c_str(), false, false, false);
                require(nearPixel(centerPixel(output / (name + "_no_rays_double.bmp")), noRays),
                    "Reflection fixture's primary center ray intersects the occluder.");
            }
        }

        // Shadow rays travel toward the light: a +Y single-sided plane must
        // block downward incident light, and its -Y version must not. Closed
        // analytic primitives must still find their far facing-light surface.
        groundMaterial = assets.findMaterial(2);
        groundMaterial->metallic = 0; groundMaterial->roughness = .8f;
        plane->transform.position[0] = -.75f;
        plane->transform.position[1] = 1.5f;
        plane->transform.position[2] = -.5f;
        settings.path = renderer::RenderPath::Deferred;
        settings.rayTracing = false;
        for (int mode = 0; mode < 3; ++mode) {
            settings.shadowMethod = mode == 0 ? renderer::ShadowMethod::ShadowMap : renderer::ShadowMethod::AnalyticRay;
            plane->mesh.meshId = mode == 2 ? 4 : 2;
            std::fill(std::begin(plane->transform.scale), std::end(plane->transform.scale), .05f);
            const auto name = "two_sided_shadow_" + std::to_string(mode);
            material->twoSided = false;
            plane->visible = false;
            capture((name + "_clear").c_str(), false, false, false);
            const auto clear = centerPixel(output / (name + "_clear.bmp"));
            plane->visible = true;
            capture((name + "_front").c_str(), false, false, false);
            const auto front = centerPixel(output / (name + "_front.bmp"));
            require(front[0] + 15 < clear[0], "A single-sided plane facing the light did not cast a shadow.");
            plane->transform.scale[1] = -.05f;
            capture((name + "_back_single").c_str(), false, false, false);
            require(nearPixel(centerPixel(output / (name + "_back_single.bmp")), clear),
                "A back-facing single-sided plane cast an unwanted shadow.");
            material->twoSided = true;
            capture((name + "_back_double").c_str(), false, false, false);
            require(nearPixel(centerPixel(output / (name + "_back_double.bmp")), front),
                "A two-sided plane failed to cast its back-face shadow.");
        }
        settings.shadowMethod = renderer::ShadowMethod::AnalyticRay;
        for (const auto mesh : {assets::AssetId{1}, assets::AssetId{3}}) {
            plane->mesh.meshId = mesh;
            std::fill(std::begin(plane->transform.scale), std::end(plane->transform.scale), .4f);
            const auto name = "two_sided_closed_shadow_" + std::to_string(mesh);
            material->twoSided = false;
            capture((name + "_single").c_str(), false, false, false);
            const auto single = centerPixel(output / (name + "_single.bmp"));
            material->twoSided = true;
            capture((name + "_double").c_str(), false, false, false);
            require(nearPixel(centerPixel(output / (name + "_double.bmp")), single),
                "Single-sided analytic closed geometry missed the facing-light far intersection.");
            plane->visible = false;
            capture((name + "_clear").c_str(), false, false, false);
            require(single[0] + 15 < centerPixel(output / (name + "_clear.bmp"))[0],
                "Analytic sphere/cube shadow fixture did not cover the receiver.");
            plane->visible = true;
        }
        world = savedWorld; assets = savedAssets; settings = savedSettings;
        std::cout << "PASS: Per-material face culling, flipped back-face normals, mirrored transforms, "
            "Forward/Deferred, analytic/triangle ray back faces, and facing-light raster/analytic shadows.\n";
    }

    {
        require(!assets::MaterialAsset{}.twoSided, "New materials must default to single-sided rendering.");
        const auto fixtureRoot = output / "TwoSidedSerialization";
        const auto materialFolder = fixtureRoot / "Materials";
        const auto sceneFile = fixtureRoot / "scene.rascene";
        assets::AssetLibrary fixtureAssets;
        fixtureAssets.createDefaults();
        fixtureAssets.findMaterial(1)->twoSided = false;
        fixtureAssets.findMaterial(2)->twoSided = true;
        scene::World fixtureWorld;
        fixtureWorld.createDefaultScene();
        require(fixtureAssets.saveMaterials(materialFolder, error), error);
        require(fixtureWorld.save(sceneFile, fixtureAssets, error), error);
        assets::AssetLibrary loaded;
        loaded.createDefaults();
        require(loaded.loadMaterials(materialFolder, error), error);
        require(!loaded.findMaterial(1)->twoSided && loaded.findMaterial(2)->twoSided,
            "Material assets did not preserve independent Two-sided values.");
        {
            // .rasset container round-trip and header validation.
            const auto container = fixtureRoot / "RoundTrip.rasset";
            const auto* source = fixtureAssets.findMaterial(2);
            require(source != nullptr, "Container fixture material is missing.");
            require(assets::writeAssetFile(container, assets::makeMaterialAssetFile(*source), error), error);
            assets::AssetFile readBack;
            require(assets::readAssetFile(container, readBack, error), error);
            require(readBack.type == assets::AssetFileType::Material && readBack.id == source->id &&
                readBack.name == source->name, "The asset container lost its header fields.");
            assets::MaterialAsset containerMaterial;
            require(assets::readMaterialAssetFile(readBack, containerMaterial, error), error);
            require(containerMaterial.twoSided == source->twoSided &&
                containerMaterial.name == source->name, "The material payload did not round-trip.");
            const auto corrupt = fixtureRoot / "Corrupt.rasset";
            std::filesystem::copy_file(container, corrupt, std::filesystem::copy_options::overwrite_existing);
            {
                std::fstream stream(corrupt, std::ios::binary | std::ios::in | std::ios::out);
                stream.write("XXXX", 4);
            }
            assets::AssetFile rejected;
            require(!assets::readAssetFileHeader(corrupt, rejected, error) && !error.empty(),
                "A container with a corrupt magic was accepted.");
        }
        scene::World loadedWorld;
        {
            // The registry reads the container header: type, GUID and name come
            // from the manifest, not from the file extension.
            assets::AssetRegistry containerRegistry;
            containerRegistry.setContentRoot(materialFolder);
            require(containerRegistry.scan(error), error);
            require(containerRegistry.count(assets::AssetType::Material) == 2,
                "The registry did not classify .rasset materials.");
            const auto* sourceMaterial = fixtureAssets.findMaterial(1);
            const auto* containerEntry = containerRegistry.find(
                assets::AssetLibrary::materialAssetFileName(*sourceMaterial));
            require(containerEntry && containerEntry->id == sourceMaterial->id &&
                containerEntry->type == assets::AssetType::Material &&
                containerEntry->name == sourceMaterial->name,
                "The registry did not read the material GUID from the container.");
        }
        require(loadedWorld.load(sceneFile, loaded, error), error);
        require(!loaded.findMaterial(1)->twoSided && loaded.findMaterial(2)->twoSided,
            "Scene material snapshots did not preserve Two-sided values.");
        using Json = nlohmann::json;
        const auto writeFixture = [](const auto& path, const Json& value) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << value.dump(2) << '\n';
            require(file.good(), "Failed writing an isolated Two-sided fixture.");
        };
        // Legacy fixtures: remove the new containers so the .ramat path is exercised,
        // then write the old JSON shape without the Two-sided field.
        for (const auto& entry : std::filesystem::directory_iterator(materialFolder))
            if (entry.path().extension() == ".rasset") std::filesystem::remove(entry.path());
        for (const auto id : {1, 2}) {
            const auto* material = fixtureAssets.findMaterial(id);
            require(material != nullptr, "Legacy fixture material is missing.");
            const Json legacy{{"format", "RAEngine.Material"}, {"version", 1}, {"material", {
                {"id", material->id}, {"name", material->name},
                {"baseColor", {material->baseColor[0], material->baseColor[1], material->baseColor[2]}},
                {"roughness", material->roughness}, {"metallic", material->metallic}, {"grid", material->grid}}}};
            writeFixture(materialFolder / (std::to_string(id) + ".ramat"), legacy);
        }
        require(loaded.loadMaterials(materialFolder, error), error);
        require(loaded.findMaterial(1)->twoSided && loaded.findMaterial(2)->twoSided,
            "Legacy materials without Two-sided lost their previous double-sided visibility.");
        // Scene v2 stores references only; the legacy path is still exercised by
        // writing a v1 scene with embedded material payloads and no Two-sided field.
        auto legacyScene = Json::parse(contents(sceneFile));
        legacyScene["version"] = 1;
        legacyScene["materials"] = Json::array();
        for (const auto id : {1, 2}) {
            const auto* material = fixtureAssets.findMaterial(id);
            legacyScene["materials"].push_back({{"id", material->id}, {"name", material->name},
                {"baseColor", {material->baseColor[0], material->baseColor[1], material->baseColor[2]}},
                {"roughness", material->roughness}, {"metallic", material->metallic}, {"grid", material->grid}});
        }
        // Legacy scenes embedded mesh descriptors (v3 keeps references only).
        legacyScene["meshes"] = Json::array();
        for (const auto id : {1, 2, 3}) {
            const auto* mesh = fixtureAssets.findMesh(id);
            legacyScene["meshes"].push_back({{"id", mesh->id}, {"name", mesh->name},
                {"primitive", id == 1 ? "Sphere" : (id == 2 ? "Plane" : "Cube")}});
        }
        const auto legacySceneFile = fixtureRoot / "legacy.rascene";
        writeFixture(legacySceneFile, legacyScene);
        require(loadedWorld.load(legacySceneFile, loaded, error), error);
        require(loaded.findMaterial(1)->twoSided && loaded.findMaterial(2)->twoSided,
            "Legacy scene snapshots without Two-sided were not preserved as double-sided.");
        const auto retainedName = loaded.findMaterial(1)->name;
        const auto retainedActor = loadedWorld.actors.front().name;
        const auto badMaterialFile = materialFolder / "1.ramat";
        const auto legacyMaterial = Json::parse(contents(badMaterialFile));
        for (const auto& badType : std::array<Json, 3>{Json(1), Json("true"), Json(nullptr)}) {
            auto invalidMaterial = legacyMaterial;
            invalidMaterial["material"]["twoSided"] = badType;
            invalidMaterial["material"]["name"] = "Must not commit";
            writeFixture(badMaterialFile, invalidMaterial);
            require(!loaded.loadMaterials(materialFolder, error) && !error.empty() &&
                loaded.materials.size() == 2 && loaded.findMaterial(1)->twoSided &&
                loaded.findMaterial(1)->name == retainedName && loaded.findMaterial(2)->twoSided,
                "Invalid Two-sided material type was accepted or partially replaced the asset library.");
            auto invalidScene = legacyScene;
            invalidScene["materials"][0]["twoSided"] = badType;
            invalidScene["actors"][0]["name"] = "Must not commit";
            const auto badSceneFile = fixtureRoot / "invalid.rascene";
            writeFixture(badSceneFile, invalidScene);
            require(!loadedWorld.load(badSceneFile, loaded, error) && !error.empty() &&
                loadedWorld.actors.front().name == retainedActor && loaded.findMaterial(1)->name == retainedName &&
                loaded.findMaterial(1)->twoSided && loaded.findMaterial(2)->twoSided,
                "Invalid Two-sided scene type was accepted or partially replaced scene/assets.");
        }
        std::cout << "PASS: Two-sided asset/scene persistence, legacy missing-field compatibility, "
            "and transactional rejection of non-boolean values.\n";
    }

    {
        // GPU material preview: the offscreen sphere must be lit by the draft color
        // and change when the draft changes.
        assets::MaterialAsset previewMaterial = *assets.findMaterial(1);
        // A preview must use its own material ID, not assume the first library material.
        previewMaterial.id = 0x123456789abcdef;
        previewMaterial.baseColor[0] = 0.9f;
        previewMaterial.baseColor[1] = 0.1f;
        previewMaterial.baseColor[2] = 0.1f;
        previewMaterial.roughness = 0.4f;
        previewMaterial.metallic = 0.0f;
        require(renderer.renderMaterialPreview(previewMaterial, 64, error), error);
        require(renderer.materialPreviewTexture() != nullptr,
            "The GPU material preview did not expose a texture.");
        std::vector<std::uint8_t> redPixels;
        std::uint32_t previewWidth = 0, previewHeight = 0;
        require(renderer.readMaterialPreviewPixels(redPixels, previewWidth, previewHeight, error), error);
        require(previewWidth == 64 && previewHeight == 64 && redPixels.size() == 64u * 64u * 4u,
            "The GPU material preview did not produce a 64x64 image.");
        const auto centerPixel = [](const std::vector<std::uint8_t>& pixels) {
            const auto index = (static_cast<std::size_t>(32) * 64 + 32) * 4;
            return std::array<int, 3>{pixels[index], pixels[index + 1], pixels[index + 2]};
        };
        const auto red = centerPixel(redPixels);
        require(red[0] > 30 && red[0] > red[1],
            std::string("The GPU material preview is not lit by the draft base color. rgb=") +
                std::to_string(red[0]) + "," + std::to_string(red[1]) + "," + std::to_string(red[2]));
        const auto* materialTexture = renderer.materialPreviewTexture();
        auto detailsMaterial = previewMaterial;
        detailsMaterial.baseColor[0] = 0.1f;
        detailsMaterial.baseColor[1] = 0.9f;
        require(renderer.renderMaterialPreview(detailsMaterial, 96, error, renderer::PreviewSlot::AssetDetails), error);
        require(renderer.previewTexture(renderer::PreviewSlot::AssetDetails) != materialTexture &&
            renderer.materialPreviewTexture() == materialTexture,
            "Asset Details replaced the material editor preview texture.");
        std::vector<std::uint8_t> detailsPixels, preservedPixels;
        require(renderer.readPreviewPixels(renderer::PreviewSlot::AssetDetails, detailsPixels,
            previewWidth, previewHeight, error), error);
        const auto detailsCenter = (48u * 96u + 48u) * 4u;
        require(previewWidth == 96 && previewHeight == 96 &&
            detailsPixels[detailsCenter + 1] > detailsPixels[detailsCenter] + 20,
            "Asset Details did not retain its own green preview.");
        require(renderer.renderScenePreview(world, assets, 128, error), error);
        require(renderer.readMaterialPreviewPixels(preservedPixels, previewWidth, previewHeight, error), error);
        require(previewWidth == 64 && preservedPixels == redPixels &&
            renderer.materialPreviewTexture() == materialTexture,
            "Rendering another preview consumer changed the material editor image.");
        require(renderer.readPreviewPixels(renderer::PreviewSlot::AssetDetails, preservedPixels,
            previewWidth, previewHeight, error), error);
        require(preservedPixels == detailsPixels, "Scene thumbnail generation changed Asset Details pixels.");
        require(renderer.renderMaterialPreview(detailsMaterial, 128, error, renderer::PreviewSlot::AssetDetails), error);
        require(renderer.readMaterialPreviewPixels(preservedPixels, previewWidth, previewHeight, error), error);
        require(preservedPixels == redPixels && renderer.materialPreviewTexture() == materialTexture,
            "Resizing Asset Details invalidated the material editor preview.");
        previewMaterial.baseColor[0] = 0.1f;
        previewMaterial.baseColor[1] = 0.9f;
        previewMaterial.baseColor[2] = 0.1f;
        require(renderer.renderMaterialPreview(previewMaterial, 64, error), error);
        std::vector<std::uint8_t> greenPixels;
        require(renderer.readMaterialPreviewPixels(greenPixels, previewWidth, previewHeight, error), error);
        const auto green = centerPixel(greenPixels);
        require(green[1] > green[0] + 20 && green[1] > red[1] + 20,
            "Changing the draft base color did not change the GPU preview.");
    }

    {
        // Orbit camera: rotating the preview must change the rendered image.
        assets::MaterialAsset orbitMaterial = *assets.findMaterial(1);
        std::vector<std::uint8_t> orbitA, orbitB;
        std::uint32_t orbitWidth = 0, orbitHeight = 0;
        require(renderer.renderMaterialPreview(orbitMaterial, 64, 0.65f, 0.18f, 3.0f, error), error);
        require(renderer.readMaterialPreviewPixels(orbitA, orbitWidth, orbitHeight, error), error);
        require(renderer.renderMaterialPreview(orbitMaterial, 64, 1.85f, 0.18f, 3.0f, error), error);
        require(renderer.readMaterialPreviewPixels(orbitB, orbitWidth, orbitHeight, error), error);
        require(orbitA != orbitB, "Rotating the material preview camera did not change the image.");

        // Scene preview: a real GPU render of the loaded scene plus its sidecar.
        require(renderer.renderScenePreview(world, assets, 64, error), error);
        std::vector<std::uint8_t> scenePixels;
        std::uint32_t sceneWidth = 0, sceneHeight = 0;
        require(renderer.readPreviewPixels(renderer::PreviewSlot::SceneThumbnail, scenePixels, sceneWidth, sceneHeight, error), error);
        std::size_t brightPixels = 0;
        for (std::size_t index = 0; index + 3 < scenePixels.size(); index += 4)
            if (scenePixels[index] + scenePixels[index + 1] + scenePixels[index + 2] > 60) ++brightPixels;
        require(brightPixels > 100, "The GPU scene preview is empty.");
        const auto sceneThumbPath = std::filesystem::temp_directory_path() / "rae-scene-thumb.rthumb";
        assets::Thumbnail sceneThumbnail{sceneWidth, sceneHeight, std::move(scenePixels)};
        require(assets::writeSceneThumbnail(sceneThumbPath, sceneThumbnail, error), error);
        assets::Thumbnail reloadedThumbnail;
        require(assets::readSceneThumbnail(sceneThumbPath, reloadedThumbnail, error), error);
        require(reloadedThumbnail.width == sceneThumbnail.width &&
            reloadedThumbnail.rgba == sceneThumbnail.rgba,
            "The scene thumbnail sidecar did not round-trip.");
        std::filesystem::remove(sceneThumbPath);
    }

    auto* sphere = world.findActor(1);
    require(sphere != nullptr, "Default sphere missing.");
    sphere->transform.position[0] = -1.2f;
    sphere->transform.rotation[2] = 25.0f;
    sphere->transform.scale[0] = .65f;
    const auto second = world.duplicateActor(1);
    world.findActor(second)->transform.position[0] = 1.25f;
    const auto blueId = assets.createMaterial("Blue test material");
    auto* blue = assets.findMaterial(blueId);
    blue->baseColor[0] = .02f; blue->baseColor[1] = .15f; blue->baseColor[2] = .65f;
    world.findActor(second)->mesh.materialId = blueId;
    // Deferred lighting exposes the selectable direct-light shadow strategies.
    // Keep reflections disabled here so the captures isolate shadow behavior.
    settings.path = renderer::RenderPath::Deferred;
    settings.rayTracing = false;
    settings.shadowMethod = renderer::ShadowMethod::Disabled;
    capture("deferred_shadows_disabled");
    settings.shadowMethod = renderer::ShadowMethod::ShadowMap;
    capture("deferred_shadows_map");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_map.bmp"),
        "Deferred Shadow Map mode did not change the direct-light result.");
    settings.shadowMethod = renderer::ShadowMethod::ShadowMapPCSS;
    capture("deferred_shadows_pcss");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_pcss.bmp"),
        "Deferred PCSS shadow mode did not change the direct-light result.");
    settings.shadowMethod = renderer::ShadowMethod::ShadowMapCSM2;
    capture("deferred_shadows_csm2");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_csm2.bmp"),
        "Deferred two-cascade shadow mode did not change the direct-light result.");
    settings.shadowMethod = renderer::ShadowMethod::GTAO;
    capture("deferred_shadows_gtao");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_gtao.bmp"),
        "Deferred GTAO mode did not change the ambient-light result.");
    settings.shadowMethod = renderer::ShadowMethod::ContactShadow;
    capture("deferred_shadows_contact");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_contact.bmp"),
        "Deferred contact-shadow mode did not change the lighting result.");
    settings.shadowMethod = renderer::ShadowMethod::AnalyticRay;
    capture("deferred_shadows_analytic");
    require(contents(output / "deferred_shadows_disabled.bmp") != contents(output / "deferred_shadows_analytic.bmp"),
        "Deferred analytic shadow mode did not change the direct-light result.");

    // Reflection rays are a separate toggle; use the shadow map for the
    // forward/deferred parity capture below.
    settings.shadowMethod = renderer::ShadowMethod::ShadowMap;
    settings.rayTracing = true;
    require(renderer.rayTracingSupported(world, assets), "Transformed sphere/plane scene should support analytic rays.");
    settings.path = renderer::RenderPath::Forward; capture("actors_forward");
    settings.path = renderer::RenderPath::Deferred; capture("actors_deferred");
    const auto forwardPixels = contents(output / "actors_forward.bmp");
    const auto deferredPixels = contents(output / "actors_deferred.bmp");
    require(forwardPixels.size() == deferredPixels.size() && forwardPixels.size() > 54,
        "Forward/deferred captures have incompatible dimensions.");
    // Ignore BMP headers and alpha. Small rounding differences are expected from G-buffer storage.
    double pixelDifference = 0;
    for (std::size_t i = 54; i < forwardPixels.size(); ++i)
        if ((i - 54) % 4 != 3) pixelDifference += std::abs(
            static_cast<int>(static_cast<unsigned char>(forwardPixels[i])) -
            static_cast<int>(static_cast<unsigned char>(deferredPixels[i])));
    const double meanDifference = pixelDifference / ((forwardPixels.size() - 54) * .75);
    require(meanDifference < 2.0, "Forward/deferred actor image difference exceeds 2/255 per channel.");
    std::cout << "Forward/deferred actor mean RGB difference: " << meanDifference << "/255\n";
    const auto cube = world.createActor("Cube test", 3, 1);
    world.findActor(cube)->transform.position[2] = -3;
    require(renderer.rayTracingSupported(world, assets), "Sphere/plane/cube rays should remain available in the mixed scene.");
    require(renderer.rayTracingStatus(world, assets).find("sphere/plane/cube") != std::string::npos,
        "Mixed ray scene did not report cube support in analytic rays.");
    capture("mesh_cube");
    settings.rayTracing = false;
    capture("mesh_cube_no_rt");
    require(contents(output / "mesh_cube.bmp") != contents(output / "mesh_cube_no_rt.bmp"),
        "Mixed sphere/cube scene did not change when analytic ray effects were enabled.");
    // Imported meshes use the generic structured-triangle ray path. Keep this
    // tiny asset in the smoke output directory so the project Content folder
    // is never modified by validation.
    const auto importedPath = output / "RayTriangle.obj";
    {
        std::ofstream obj(importedPath, std::ios::binary | std::ios::trunc);
        obj << "v -0.8 0 -0.8\nv 0.8 0 -0.8\nv 0 1.2 0.8\nf 1 2 3\n";
    }
    assets::AssetId importedMesh = 0;
    require(assets.importObj(importedPath, importedMesh, error), error);
    const auto importedActor = world.createActor("Imported ray triangle", importedMesh, 1);
    if (auto* actor = world.findActor(importedActor)) actor->transform.position[0] = -2.4f;
    require(renderer.rayTracingSupported(world, assets), "Imported mesh should participate in analytic rays.");
    require(renderer.rayTracingStatus(world, assets).find("mesh triangle") != std::string::npos,
        "Ray status did not report generic mesh triangles.");
    settings.rayTracing = true;
    capture("mesh_imported_rt");
    settings.rayTracing = false;
    capture("mesh_imported_no_rt");
    require(contents(output / "mesh_imported_rt.bmp") != contents(output / "mesh_imported_no_rt.bmp"),
        "Imported mesh did not change when ray effects were enabled.");
    require(world.removeActor(importedActor), "Failed to remove imported ray-test actor.");
    settings.rayTracing = true;
    const auto scenePath = output / "roundtrip.rascene";
    require(world.save(scenePath, assets, error), error);
    require(world.load(scenePath, assets, error), error);
    capture("scene_reloaded");
    require(contents(output / "mesh_cube.bmp") == contents(output / "scene_reloaded.bmp"), "Saved scene did not render identically after loading.");

    // Restore default content for a useful editor screenshot.
    world.createDefaultScene(); assets.createDefaults();
    mountEngineMaterials();
    // Command registry: the same ids back the buttons, the Edit menu and the
    // keyboard, and enablement tracks editor state.
    require(editor.commands().contains("actor.delete") && editor.commands().contains("edit.undo"),
        "Editor command registry is missing core commands.");
    require(editor.commands().enabled("actor.delete") == (editor.selectedActor() != 0),
        "Delete command enablement does not follow the Actor selection.");
    {
        // Theme preference round-trips through the per-user file the app reads
        // before creating the ImGui context (palette itself is covered by the
        // captured screenshots of both themes).
        using rae::imgui::EditorTheme;
        require(rae::imgui::kEditorThemeCount == 2, "Expected exactly two editor themes.");
        require(std::string(rae::imgui::themeName(EditorTheme::Black)) == "Black" &&
            std::string(rae::imgui::themeName(EditorTheme::DarkBlue)) == "Dark Blue",
            "Theme names changed unexpectedly.");
        const auto path = std::filesystem::temp_directory_path() / "rae-theme-preference-test.ini";
        rae::imgui::saveThemePreference(path, EditorTheme::DarkBlue);
        require(rae::imgui::loadThemePreference(path) == EditorTheme::DarkBlue,
            "Theme preference did not round-trip.");
        rae::imgui::saveThemePreference(path, EditorTheme::Black);
        require(rae::imgui::loadThemePreference(path) == EditorTheme::Black,
            "Theme preference did not round-trip back to Black.");
        // The UI menu edits the outline settings; they share the same file and
        // must survive a reload without disturbing the palette.
        auto preferences = rae::imgui::loadUiPreferences(path);
        preferences.theme = EditorTheme::DarkBlue;
        preferences.selectionOutline = false;
        preferences.outlineThickness = 4.5f;
        preferences.outlineThemeColor = false;
        preferences.outlineColor[0] = 0.1f;
        preferences.outlineColor[1] = 0.2f;
        preferences.outlineColor[2] = 0.3f;
        preferences.outlineFrontObjects = true;
        rae::imgui::saveUiPreferences(path, preferences);
        const auto reloaded = rae::imgui::loadUiPreferences(path);
        require(reloaded.theme == EditorTheme::DarkBlue && !reloaded.selectionOutline &&
            std::abs(reloaded.outlineThickness - 4.5f) < 0.001f && !reloaded.outlineThemeColor &&
            std::abs(reloaded.outlineColor[0] - 0.1f) < 0.001f &&
            std::abs(reloaded.outlineColor[2] - 0.3f) < 0.001f && reloaded.outlineFrontObjects,
            "Selection outline preferences did not round-trip.");
        std::filesystem::remove(path);
    }
    {
        // UndoStack merge (Godot MERGE_ENDS): same-key commits inside the window
        // collapse into one entry that keeps the oldest undo state.
        rae::editor::UndoStack merged;
        int value = 0;
        merged.push("Edit A", [&] { value = 0; }, [&] { value = 1; }, { "k", 1000 });
        merged.push("Edit B", [&] { value = 0; }, [&] { value = 2; }, { "k", 1000 });
        require(merged.size() == 1, "Merged commits created more than one history entry.");
        require(merged.undo() && value == 0, "Merged undo did not restore the oldest state.");
        require(merged.redo() && value == 2, "Merged redo did not apply the latest state.");
        require(merged.size() == 1, "Undo/redo changed the merged history size.");
    }
    settings.rayTracing = false;
    capture("editor_warmup", true);
    capture("editor", true, true);
    require(renderedUiText.find("Direct-light shadows") != std::string::npos &&
        renderedUiText.find("Shadow method") != std::string::npos &&
        renderedUiText.find("Ray tracing: all mesh actors") != std::string::npos,
        "Editor viewport does not expose the selectable shadow method control.");
    SmokeInput input;
    input.attach(context);
    const auto findWindow = [&](const char* name) {
        for (auto* window : context->Windows)
            if (window->ID == ImHashStr(name)) return window;
        return static_cast<ImGuiWindow*>(nullptr);
    };
    const auto mainOrigin = [&] { return context->Viewports[0]->Pos; };
    const auto scenePoint = [&](float x, float y) {
        return ImVec2(mainOrigin().x + settings.viewportX + x,
            mainOrigin().y + settings.viewportY + y);
    };
    constexpr std::size_t panelCount = static_cast<std::size_t>(editor::EditorPanel::Count);
    const std::array<const char*, panelCount> panelNames = {"World Outliner", "Scene", "Details", "Viewport settings",
        "Content & Editors", "Scene Viewport##Interaction"};
    const ImGuiID dockspaceId = ImHashStr("RestArtEngine.DockSpace.v1");
    for (const auto* name : panelNames) {
        const auto* window = findWindow(name);
        require(window && window->DockId && window->DockNode,
            std::string("Default layout did not dock ") + name);
    }
    require(findWindow("Scene")->DockId == findWindow("Details")->DockId &&
        findWindow("Scene")->DockNode->SelectedTabId == findWindow("Scene")->TabId,
        "Scene and mesh Details are not a default tab group with Scene selected.");
    {
        // Exercise persisted user layouts in isolated, CPU-only ImGui contexts.
        // The engine context, OS windows and personal ini remain untouched.
        ImGuiContextScope restoreEngine(context);
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        const std::string currentLayout(data, size);
        const auto sectionRange = [](const std::string& ini, const char* name) {
            const auto marker = std::string("[Window][") + name + "]\n";
            const auto begin = ini.find(marker);
            require(begin != std::string::npos, std::string("Migration fixture has no window: ") + name);
            const auto next = ini.find("\n[", begin + marker.size());
            return std::pair(begin, next == std::string::npos ? ini.size() : next + 1);
        };
        const auto withoutScene = [&] {
            auto ini = currentLayout;
            const auto [begin, end] = sectionRange(ini, "Scene");
            ini.erase(begin, end - begin);
            return ini;
        };
        const auto floatingWindow = [&](std::string ini, const char* name, ImVec2ih position, ImVec2ih extent) {
            const auto [begin, end] = sectionRange(ini, name);
            auto section = ini.substr(begin, end - begin);
            for (const char* prefix : {"Pos=", "Size=", "ViewportId=", "ViewportPos=", "DockId="}) {
                const auto line = section.find(std::string("\n") + prefix);
                if (line == std::string::npos) continue;
                const auto next = section.find('\n', line + 1);
                section.erase(line + 1, next == std::string::npos ? std::string::npos : next - line);
            }
            section += "Pos=" + std::to_string(position.x) + "," + std::to_string(position.y) + "\n";
            section += "Size=" + std::to_string(extent.x) + "," + std::to_string(extent.y) + "\n\n";
            ini.replace(begin, end - begin, section);
            return ini;
        };
        constexpr std::size_t legacyPanelCount = 5;
        const std::array<const char*, legacyPanelCount> legacyPanels{"World Outliner", "Details", "Viewport settings",
            "Content & Editors", "Scene Viewport##Interaction"};
        const ImVec2 displaySize = context->IO.DisplaySize;
        const float toolbarHeight = findWindow("RestArtEngine Editor")->Size.y;
        for (int scenario = 0; scenario < 3; ++scenario) {
            std::string layout = scenario == 2 ? currentLayout : withoutScene();
            const char* fixture = scenario == 0 ? "layout-legacy-docked" :
                scenario == 1 ? "layout-legacy-floating-details" : "layout-custom-floating-scene";
            if (scenario == 1) layout = floatingWindow(std::move(layout), "Details", {120, 160}, {330, 420});
            if (scenario == 2) layout = floatingWindow(std::move(layout), "Scene", {500, 180}, {360, 400});
            std::ofstream(output / (std::string(fixture) + ".ini"), std::ios::binary) << layout;
            for (int reopen = 0; reopen < 2; ++reopen) {
                std::unique_ptr<ImGuiContext, decltype(&ImGui::DestroyContext)> isolated(
                    ImGui::CreateContext(), ImGui::DestroyContext);
                ImGui::SetCurrentContext(isolated.get());
                auto& io = ImGui::GetIO();
                io.IniFilename = nullptr;
                io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
                io.DisplaySize = displaySize;
                unsigned char* fontPixels = nullptr;
                int fontWidth = 0, fontHeight = 0;
                io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
                ImGui::LoadIniSettingsFromMemory(layout.data(), layout.size());
                struct PreservedWindow { ImGuiID id, dock; ImVec2ih pos, size; };
                std::array<PreservedWindow, legacyPanelCount> before{};
                for (std::size_t index = 0; index < legacyPanels.size(); ++index) {
                    const auto* state = ImGui::FindWindowSettingsByID(ImHashStr(legacyPanels[index]));
                    require(state != nullptr, "A legacy panel was lost while loading its layout.");
                    before[index] = {state->ID, state->DockId, state->Pos, state->Size};
                }
                editor::EditorDockManager manager;
                for (int frame = 0; frame < 3; ++frame) {
                    ImGui::NewFrame();
                    manager.beginFrame(toolbarHeight);
                    if (frame == 0) {
                        for (const auto& state : before) {
                            if (scenario == 1 && state.id == ImHashStr("Details") && reopen == 0) continue;
                            const auto* after = ImGui::FindWindowSettingsByID(state.id);
                            require(after && after->DockId == state.dock &&
                                after->Pos.x == state.pos.x && after->Pos.y == state.pos.y &&
                                after->Size.x == state.size.x && after->Size.y == state.size.y,
                                std::string("Layout migration reset an existing panel: ") + fixture);
                        }
                    }
                    for (unsigned panel = 0; panel < static_cast<unsigned>(editor::EditorPanel::Count); ++panel)
                        manager.drawPanel(static_cast<editor::EditorPanel>(panel), [] {
                            ImGui::TextUnformatted("Layout migration fixture");
                        });
                    ImGui::Render();
                }
                const auto* scene = ImGui::FindWindowByName("Scene");
                const auto* details = ImGui::FindWindowByName("Details");
                require(scene && details, "Layout migration did not create both lower-left pages.");
                if (scenario == 2) {
                    require(scene->DockId == 0 && std::abs(scene->Pos.x - 500) <= 1 &&
                        std::abs(scene->Pos.y - 180) <= 1 && std::abs(scene->Size.x - 360) <= 1 &&
                        std::abs(scene->Size.y - 400) <= 1,
                        "An existing custom floating Scene page was reset or merged into Details.");
                } else {
                    require(scene->DockId != 0 && scene->DockId == details->DockId,
                        "A legacy Details layout did not acquire Scene in the same tab group.");
                    if (scenario == 1) {
                        require(details->DockNode && std::abs(details->DockNode->Pos.x - 120) <= 1 &&
                            std::abs(details->DockNode->Pos.y - 160) <= 1 &&
                            std::abs(details->DockNode->Size.x - 330) <= 1 &&
                            std::abs(details->DockNode->Size.y - 420) <= 1,
                            "Migrating floating Details did not preserve its group position and size.");
                    }
                }
                for (const auto& state : before) {
                    if (scenario == 1 && state.id == ImHashStr("Details")) continue;
                    require(ImGui::FindWindowByID(state.id)->DockId == state.dock,
                        "Layout migration or reopen changed an unrelated panel's dock.");
                }
                unsigned sceneWindows = 0;
                for (const auto* window : isolated->Windows) sceneWindows += window->ID == ImHashStr("Scene");
                require(sceneWindows == 1, "Layout migration created duplicate Scene windows.");
                std::size_t migratedSize = 0;
                const char* migrated = ImGui::SaveIniSettingsToMemory(&migratedSize);
                layout.assign(migrated, migratedSize);
                ImGui::SetCurrentContext(context);
            }
        }
        std::cout << "PASS: Legacy docked/floating Details migration, unrelated panel bounds/docks preserved, "
            "custom floating Scene preserved, and idempotent fresh-context reopen.\n";
    }
    const auto dragDock = [&](ImVec2 start, ImVec2 end) {
        input.mouse = start;
        capture("ui_layout", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
        capture("ui_layout", true);
        for (int step = 1; step <= 5; ++step) {
            input.mouse = {start.x + (end.x - start.x) * step / 5.0f,
                start.y + (end.y - start.y) * step / 5.0f};
            capture("ui_layout", true);
        }
        // Docking previews need a held frame after reaching the target.
        capture("ui_layout", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
        capture("ui_layout", true);
        capture("ui_layout", true);
        capture("ui_layout", true);
    };
    const auto dockTabCenter = [](const ImGuiWindow* window) {
        if (window->DockNode && window->DockNode->TabBar) {
            const auto* bar = window->DockNode->TabBar;
            for (const auto& tab : bar->Tabs)
                if (tab.Window == window)
                    return ImVec2(bar->BarRect.Min.x + tab.Offset + tab.Width * .5f,
                        (bar->BarRect.Min.y + bar->BarRect.Max.y) * .5f);
        }
        return ImVec2(window->Pos.x + 70.0f, window->Pos.y + 10.0f);
    };
    // Exercise native docking with actual mouse events, not the former custom
    // edge-snap code: remove the bottom pane, then merge it with a left tab.
    const int initialHeight = settings.viewportHeight;
    dragDock(dockTabCenter(findWindow("Content & Editors")),
        {mainOrigin().x + context->IO.DisplaySize.x * .55f, findWindow("Editor workspace")->Pos.y + 52.0f});
    auto* content = findWindow("Content & Editors");
    require(content && (!content->DockNode || content->DockNode->IsFloatingNode()),
        "Dragging the Content dock tab did not detach it.");
    require(settings.viewportHeight > initialHeight + 80,
        "Undocking the bottom panel did not expand the scene viewport.");
    capture("dock_floating", true);
    {
        ImGuiContextScope active(context);
        auto* floatingHost = content->DockNode ? content->DockNode->HostWindow : content;
        require(floatingHost != nullptr, "Floating Content has no host window.");
        const auto originalSize = floatingHost->Size;
        ImGui::SetWindowSize(floatingHost, ImVec2(440, 360), ImGuiCond_Always);
        editor.selectContentFolder("Meshes/Builtin");
        editor.setContentGridView(true);
        capture("content_narrow_preroll", true);
        capture("content_narrow", true);
        ImGui::SetWindowSize(floatingHost, originalSize, ImGuiCond_Always);
        capture("content_size_restored", true);
    }

    const auto verifyLayoutRoundtrip = [&](const char* filename) {
        ImGuiContextScope active(context);
        std::size_t size = 0;
        const char* data = ImGui::SaveIniSettingsToMemory(&size);
        const std::string saved(data, size);
        require(saved.find("[Docking][Data]") != std::string::npos,
            "Saved layout does not contain native docking state.");
        std::ofstream(output / filename, std::ios::binary) << saved;
        struct WindowState { ImGuiID id, dock; short order; ImVec2ih pos, size; };
        std::array<WindowState, panelCount> expected{};
        for (std::size_t i = 0; i < panelNames.size(); ++i) {
            const auto* state = ImGui::FindWindowSettingsByID(findWindow(panelNames[i])->ID);
            require(state != nullptr, "Panel missing from saved layout.");
            expected[i] = {state->ID, state->DockId, state->DockOrder, state->Pos, state->Size};
        }
        struct NodeState { ImGuiID id, first, second, selected; ImGuiAxis axis; };
        std::vector<NodeState> expectedNodes;
        const std::function<void(const ImGuiDockNode*)> rememberNode = [&](const ImGuiDockNode* node) {
            if (!node) return;
            expectedNodes.push_back({node->ID, node->ChildNodes[0] ? node->ChildNodes[0]->ID : 0,
                node->ChildNodes[1] ? node->ChildNodes[1]->ID : 0, node->SelectedTabId, node->SplitAxis});
            rememberNode(node->ChildNodes[0]);
            rememberNode(node->ChildNodes[1]);
        };
        rememberNode(ImGui::DockBuilderGetNode(dockspaceId));
        // A fresh context has no live windows or layout history. This checks
        // the same serialized state a restarted editor consumes, while the
        // active renderer's context and .ini file remain untouched.
        ImGuiContext* restored = ImGui::CreateContext();
        ImGui::SetCurrentContext(restored);
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::LoadIniSettingsFromMemory(saved.data(), saved.size());
        bool matched = ImGui::DockBuilderGetNode(dockspaceId) != nullptr;
        for (const auto& before : expected) {
            const auto* after = ImGui::FindWindowSettingsByID(before.id);
            matched &= after && after->DockId == before.dock && (!before.dock || after->DockOrder == before.order) &&
                after->Pos.x == before.pos.x && after->Pos.y == before.pos.y &&
                after->Size.x == before.size.x && after->Size.y == before.size.y;
            if (before.dock) matched &= ImGui::DockBuilderGetNode(before.dock) != nullptr;
        }
        for (const auto& before : expectedNodes) {
            const auto* after = ImGui::DockBuilderGetNode(before.id);
            matched &= after && after->SplitAxis == before.axis && after->SelectedTabId == before.selected &&
                (after->ChildNodes[0] ? after->ChildNodes[0]->ID : 0) == before.first &&
                (after->ChildNodes[1] ? after->ChildNodes[1]->ID : 0) == before.second;
        }
        ImGui::DestroyContext(restored);
        ImGui::SetCurrentContext(context);
        require(matched, "Fresh-context layout restore lost docking, tab order or floating bounds.");
    };
    verifyLayoutRoundtrip("layout-floating.ini");
    const auto* details = findWindow("Details");
    dragDock(dockTabCenter(content), details->DockNode->Rect().GetCenter());
    content = findWindow("Content & Editors");
    details = findWindow("Details");
    require(content->DockId != 0 && content->DockId == details->DockId,
        "Dropping Content on the native center target did not merge the dock tabs.");
    verifyLayoutRoundtrip("layout-tabbed.ini");
    capture("dock_tabbed", true);

    // Split the central node through ImGui's native builder, exercising the
    // renderer's response to a different layout topology as well as dragging.
    const int widthBeforeSplit = settings.viewportWidth;
    {
        ImGuiContextScope active(context);
        const ImGuiID central = findWindow("Scene Viewport##Interaction")->DockId;
        const ImGuiID right = ImGui::DockBuilderSplitNode(central, ImGuiDir_Right, .3f, nullptr, nullptr);
        ImGui::DockBuilderDockWindow("Content & Editors", right);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    capture("dock_split", true);
    capture("dock_split", true);
    require(settings.viewportWidth < widthBeforeSplit - 80,
        "Splitting the scene dock did not resize the renderer viewport.");
    verifyLayoutRoundtrip("layout-split.ini");
    {
        ImGuiContextScope active(context);
        ImGui::DockBuilderDockWindow("Scene Viewport##Interaction", findWindow("Viewport settings")->DockId);
        ImGui::DockBuilderFinish(dockspaceId);
    }
    capture("dock_hidden", true);
    capture("dock_hidden", true);
    {
        auto* node = findWindow("Viewport settings")->DockNode;
        require(node && node->TabBar, "Merged viewport did not create a tab group.");
        node->TabBar->NextSelectedTabId = findWindow("Viewport settings")->TabId;
    }
    capture("dock_hidden", true);
    capture("dock_hidden", true);
    require(!settings.viewportVisible && settings.viewportWidth == 0 && settings.viewportHeight == 0,
        "An inactive Scene Viewport tab still requests scene rendering.");
    editor.resetLayout();
    capture("ui_layout", true);
    capture("ui_layout", true);
    require(std::abs(settings.viewportHeight - initialHeight) <= 2,
        "Reset layout did not restore the original scene viewport height.");
    dragDock(dockTabCenter(findWindow("Scene Viewport##Interaction")), {mainOrigin().x + 450, mainOrigin().y + 190});
    const auto* floatingViewport = findWindow("Scene Viewport##Interaction");
    require(settings.viewportVisible && floatingViewport &&
        (!floatingViewport->DockNode || floatingViewport->DockNode->IsFloatingNode()),
        "Scene Viewport did not remain renderable after detaching its dock tab.");
    capture("dock_viewport_floating", true);
    // The editor now composites an SRV instead of revealing the swapchain.
    // Verify both render paths still produce equivalent scene pixels through
    // that surface, including when the Scene Viewport is a floating window.
    // Warm-up frame: the viewport tab was just dragged out, so the floating
    // window needs one frame to report its image rectangle before the captures
    // used by the pipeline comparison.
    capture("editor_surface_warmup", true);
    settings.path = renderer::RenderPath::Forward;
    capture("editor_surface_forward", true);
    settings.path = renderer::RenderPath::Deferred;
    capture("editor_surface_deferred", true);
    const auto editorForward = contents(output / "editor_surface_forward.bmp");
    const auto editorDeferred = contents(output / "editor_surface_deferred.bmp");
    require(editorForward.size() == editorDeferred.size() && editorForward.size() > 54,
        "Editor scene surface captures have incompatible dimensions.");
    double editorDifference = 0;
    for (std::size_t i = 54; i < editorForward.size(); ++i)
        if ((i - 54) % 4 != 3) editorDifference += std::abs(
            static_cast<int>(static_cast<unsigned char>(editorForward[i])) -
            static_cast<int>(static_cast<unsigned char>(editorDeferred[i])));
    const double editorMeanDifference = editorDifference / ((editorForward.size() - 54) * .75);
    require(editorMeanDifference < 2.0, "Forward/deferred editor surface difference exceeds 2/255.");
    std::cout << "Forward/deferred editor surface mean RGB difference: " << editorMeanDifference << "/255\n";
    editor.resetLayout();
    capture("ui_layout", true);
    capture("ui_layout", true);
    const ImVec2 splitter{mainOrigin().x + settings.viewportX + settings.viewportWidth * .5f,
        findWindow("Content & Editors")->Pos.y - 1.0f};
    dragDock(splitter, {splitter.x, splitter.y - 70.0f});
    require(settings.viewportHeight < initialHeight - 40,
        "Dragging the native bottom splitter did not resize the scene viewport.");
    capture("dock_resized", true);
    editor.resetLayout();
    capture("ui_layout", true);
    capture("ui_layout", true);
    std::array<ImGuiID, panelCount> docksBeforeHiding{};
    for (std::size_t i = 0; i < panelNames.size(); ++i) docksBeforeHiding[i] = findWindow(panelNames[i])->DockId;
    for (int frame = 0; frame < 3; ++frame) capture("editor_hidden");
    capture("ui_layout", true);
    capture("ui_layout", true);
    for (std::size_t i = 0; i < panelNames.size(); ++i)
        require(findWindow(panelNames[i])->DockId == docksBeforeHiding[i],
            "Hiding and reopening the editor changed its dock layout.");
    const auto click = [&](ImVec2 position) {
        input.mouse = position;
        // Establish hover before pressing: tab items intentionally reject same-frame
        // mouse entry/press in some navigation states.
        capture("ui_interaction", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
        capture("ui_interaction", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
        capture("ui_interaction", true);
        capture("ui_interaction", true);
    };
    {
        // Regression for BeginMenu used as a full-width vertical item inside
        // the toolbar. Check native menu hit targets and popup placement, as
        // well as separation from the rendering row and dock tabs.
        ImGuiContextScope active(context);
        const auto originalTheme = imgui::currentTheme();
        const ImGuiStyle originalStyle = context->Style;
        const float originalFontScale = context->IO.FontGlobalScale;
        const ImVec2 originalSize = context->IO.DisplaySize;
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            for (const float scale : {1.0f, 1.5f}) {
                imgui::setTheme(theme);
                context->Style.ScaleAllSizes(scale);
                context->IO.FontGlobalScale = scale;
                input.displaySize = scale == 1.0f ? ImVec2(980, 700) : ImVec2(1440, 900);
                require(renderer.resize(static_cast<unsigned>(input.displaySize.x),
                    static_cast<unsigned>(input.displaySize.y), error), error);
                editor.resetLayout();
                capture("toolbar_warmup", true);
                capture("toolbar_warmup", true);
                const auto* toolbar = findWindow("RestArtEngine Editor");
                const auto* workspace = findWindow("Editor workspace");
                const auto* chrome = findWindow("##EngineWindowChrome");
                require(toolbar && workspace && toolbar->MenuBarHeight > 0,
                    "Toolbar menus have no horizontal menu bar.");
                require(chrome && toolbar->Pos.y >= chrome->Pos.y + chrome->Size.y &&
                    workspace->Pos.y + workspace->Size.y <= mainOrigin().y + context->IO.DisplaySize.y + 1,
                    "Custom title bar overlaps the toolbar or pushes docks outside the client surface.");
                require(toolbar->DC.MenuBarOffset.x < toolbar->Size.x,
                    "Toolbar menu labels or scene actions overflow the first row.");
                require(toolbar->WorkRect.Min.y >= toolbar->MenuBarRect().Max.y &&
                    toolbar->DC.CursorMaxPos.y <= toolbar->InnerRect.Max.y &&
                    workspace->Pos.y >= toolbar->Pos.y + toolbar->Size.y,
                    "Menu bar, rendering controls, or dock tabs overlap vertically.");
                const std::string suffix = std::string(theme == imgui::EditorTheme::Black ? "black" : "blue") +
                    (scale == 1.0f ? "_980" : "_1440_scaled");
                capture(("toolbar_" + suffix).c_str(), true);
                float menuX = toolbar->Pos.x + std::max(toolbar->WindowPadding.x, context->Style.ItemSpacing.x);
                const std::array<const char*, 3> labels{"Layout", "Edit", "UI"};
                const std::array<const char*, 3> contents{"Restore default layout", "Duplicate Actor", "Theme"};
                for (std::size_t menu = 0; menu < labels.size(); ++menu) {
                    const float labelWidth = ImGui::CalcTextSize(labels[menu]).x;
                    click({menuX + context->Style.ItemSpacing.x * .5f + labelWidth * .5f,
                        toolbar->MenuBarRect().GetCenter().y});
                    capture(("toolbar_" + suffix + "_" + labels[menu]).c_str(), true, true);
                    require(renderedUiText.find(contents[menu]) != std::string::npos,
                        std::string("Toolbar menu is not reachable: ") + labels[menu]);
                    require(!context->OpenPopupStack.empty() && context->OpenPopupStack[0].Window &&
                        context->OpenPopupStack[0].Window->Pos.y >= toolbar->MenuBarRect().Max.y - 1,
                        "Top-level menu opens beside/over its heading instead of below the menu bar.");
                    if (menu == 2) {
                        // The UI menu owns the editor-only viewport aids: theme and
                        // the selection outline settings. The latter open a small
                        // dedicated window instead of crowding the popup; verify the
                        // entry is reachable, the window opens, and it closes again.
                        auto* popup = context->OpenPopupStack[0].Window;
                        require(popup && popup->Size.y > 0, "UI menu popup has no size.");
                        // "Selection outline..." is the last entry; click its actual
                        // centre so disabled neighbours or extra separators cannot
                        // shift the hit target.
                        const float lastItemY = popup->Pos.y + popup->Size.y - context->Style.WindowPadding.y -
                            (ImGui::GetTextLineHeight() + context->Style.ItemSpacing.y) * 0.5f;
                        input.mouse = {popup->Pos.x + 40.0f, lastItemY};
                        capture("toolbar_ui_hover", true);
                        click(input.mouse);
                        capture("toolbar_outline_window", true, true);
                        const auto* outlineWindow = findWindow("Selection Outline");
                        require(outlineWindow != nullptr, "Selection outline settings window did not open.");
                        require(renderedUiText.find("Show outline") != std::string::npos &&
                            renderedUiText.find("Use theme accent") != std::string::npos &&
                            renderedUiText.find("Custom") != std::string::npos,
                            "Selection outline settings window has no controls.");
                        click({outlineWindow->Pos.x + outlineWindow->Size.x - outlineWindow->TitleBarHeight * 0.5f,
                            outlineWindow->Pos.y + outlineWindow->TitleBarHeight * 0.5f});
                        capture("toolbar_outline_closed", true);
                        // The close button flips p_open this frame; the window is
                        // removed on the following frame.
                        capture("toolbar_outline_closed", true);
                        // ImGui may keep the window object one extra frame; what
                        // matters is that it is no longer submitted/drawn.
                        const auto* closedWindow = findWindow("Selection Outline");
                        require(closedWindow == nullptr || !closedWindow->WasActive,
                            "Selection outline settings window did not close.");
                    }
                    input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, true); };
                    capture("toolbar_close", true);
                    input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Escape, false); };
                    capture("toolbar_close", true);
                    menuX += labelWidth + context->Style.ItemSpacing.x * 2;
                }
            }
        }
        imgui::setTheme(originalTheme);
        context->Style = originalStyle;
        context->IO.FontGlobalScale = originalFontScale;
        input.displaySize = {};
        input.mouse = {-100, -100};
        require(renderer.resize(static_cast<unsigned>(originalSize.x),
            static_cast<unsigned>(originalSize.y), error), error);
        editor.resetLayout();
        capture("ui_layout", true);
        capture("ui_layout", true);
        std::cout << "PASS: Layout/Edit/UI menus, toolbar/dock separation, both themes, "
            "980px and 1440px widths, and 150% UI scale.\n";
    }
    // Locate the submitted navigation rectangle, then use ordinary mouse input
    // for the action. This also reveals controls in narrow/scrolling panels.
    const auto itemRect = [&](ImGuiWindow* window, ImGuiID id) {
        require(window != nullptr, "Editor control window is missing.");
        ImGuiContextScope active(context);
        ImGui::FocusWindow(window);
        ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
        capture("material_locate", true);
        require(context->NavId == id && context->NavIdIsAlive,
            std::string("Editor control could not be located. window=") + window->Name +
            " expectedId=" + std::to_string(id) + " navId=" + std::to_string(context->NavId) +
            " navAlive=" + std::to_string(context->NavIdIsAlive) +
            " active=" + std::to_string(window->Active) + " wasActive=" + std::to_string(window->WasActive) +
            " scrollY=" + std::to_string(window->Scroll.y) + "/" + std::to_string(window->ScrollMax.y) +
            " viewport=" + std::to_string(window->ViewportId) +
            " navWindow=" + (context->NavWindow ? context->NavWindow->Name : "<none>"));
        for (int attempt = 0; attempt != 4; ++attempt) {
            const auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
            ImGui::ScrollToRectEx(window, rect, ImGuiScrollFlags_KeepVisibleEdgeY);
            capture("material_locate", true);
            const auto current = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
            if (context->NavId == id && window->InnerRect.Contains(current.GetCenter())) return current;
            ImGui::FocusWindow(window);
            ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
            capture("material_locate", true);
        }
        throw std::runtime_error(std::string("Editor control is still clipped after revealing it: ") + window->Name);
    };
    {
        const auto findChild = [&](const char* parent, const char* label) {
            for (auto* window : context->Windows)
                if (window->Active && std::strstr(window->Name, label))
                    for (auto* ancestor = window->ParentWindow; ancestor; ancestor = ancestor->ParentWindow)
                        if (ancestor->ID == ImHashStr(parent)) return window;
            throw std::runtime_error(std::string("Selection test cannot find child: ") + parent + "/" + label);
        };
        const auto clickMesh = [&](assets::AssetId id) {
            const auto* mesh = assets.findMesh(id);
            require(mesh != nullptr, "Mesh selection test fixture is missing.");
            const auto& entries = editor.assetRegistry().entries();
            const auto found = std::find_if(entries.begin(), entries.end(), [id](const auto& entry) {
                return entry.type == assets::AssetType::Mesh && entry.id == id;
            });
            require(found != entries.end(), "Mesh selection test asset is absent from Content.");
            editor.selectContentFolder(found->folder);
            editor.setContentSearch("");
            editor.setContentTypeFilter(assets::AssetType::Unknown);
            editor.setContentGridView(false);
            capture("content_click_preroll", true);
            auto* library = findChild("Content & Editors", "##content-assets");
            const auto assetId = ImHashStr(found->relativePath.c_str(), 0, library->ID);
            const auto rowId = ImHashStr("##asset-row", 0, assetId);
            click(itemRect(library, rowId).GetCenter());
        };
        const auto captureInspectorThemes = [&](const char* prefix) {
            ImGuiContextScope active(context);
            const auto originalTheme = imgui::currentTheme();
            for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
                imgui::setTheme(theme);
                const auto name = std::string(prefix) + (theme == imgui::EditorTheme::Black ? "_black" : "_blue");
                capture(name.c_str(), true);
            }
            imgui::setTheme(originalTheme);
            capture("inspector_theme_restored", true);
        };
        const auto clickActor = [&](scene::ActorId id) {
            auto* actors = findChild("World Outliner", "Actor list");
            const auto* actor = world.findActor(id);
            require(actor != nullptr, "Actor selection test fixture is missing.");
            const auto label = actor->name + "##" + std::to_string(id);
            click(itemRect(actors, actors->GetID(label.c_str())).GetCenter());
            capture("instance_details_selected", true, true);
            require(editor.selectedActor() == id &&
                findWindow("Details")->DockNode->SelectedTabId == findWindow("Details")->TabId,
                "Selecting an Actor did not reveal its instance Details.");
        };
    {
        // Outliner eye toggle: the row control hides the Actor and one undo
        // restores it, matching the Details visibility checkbox.
        auto* actors = findChild("World Outliner", "Actor list");
        auto* eyeActor = world.findActor(1);
        require(actors && eyeActor && eyeActor->visible, "Outliner eye test needs a visible Actor.");
        const std::string eyeLabel = "##visible" + std::to_string(eyeActor->id);
        click(itemRect(actors, actors->GetID(eyeLabel.c_str())).GetCenter());
        require(!world.findActor(1)->visible, "Outliner eye did not hide the Actor.");
        capture("outliner_eye_hidden", true, true);
        editor.undo();
        require(world.findActor(1)->visible, "Undo did not restore Actor visibility from the Outliner eye.");
    }
        clickActor(1);
        clickMesh(3);
        capture("instance_details_content_independent", true, true);
        require(editor.selectedActor() == 1 && findWindow("Scene")->DockId == findWindow("Details")->DockId,
            "Content browsing changed the scene Actor or inspector tab group.");
        const auto actorsBeforeContentDelete = world.actors.size();
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Delete, true); };
        capture("content_delete_key_down", true);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Delete, false); };
        capture("content_delete_key_up", true, true);
        require(world.actors.size() == actorsBeforeContentDelete && editor.selectedActor() == 1,
            "Delete while browsing Content deleted the scene Actor.");
        const auto* instanceMesh = assets.findMesh(world.findActor(1)->mesh.meshId);
        const auto vertices = std::string("Vertices: ") + std::to_string(instanceMesh->vertices.size());
        const auto triangles = std::string("Triangles: ") + std::to_string(instanceMesh->indices.size() / 3);
        const auto info = renderedUiText.find("Vertices:");
        require(renderedUiText.find("Actor #1") != std::string::npos &&
            renderedUiText.find(vertices) != std::string::npos && renderedUiText.find(triangles) != std::string::npos &&
            info != std::string::npos && renderedUiText.find("Vertices:", info + 1) == std::string::npos &&
            renderedUiText.find("Materials in scene") == std::string::npos,
            "Details followed Content geometry or still aggregated scene materials.");
        captureInspectorThemes("inspector_instance_details");

        const auto originalGroundMesh = world.findActor(2)->mesh.meshId;
        world.findActor(2)->mesh.meshId = world.findActor(1)->mesh.meshId;
        // ImGui's ButtonEx logs buttons as [ label ]. Match the actual material
        // buttons, not bare names: "Ground" is also an Outliner Actor name.
        const auto copperButton = "[ " + assets.findMaterial(1)->name + " ]";
        const auto groundButton = "[ " + assets.findMaterial(2)->name + " ]";
        // Only inspect the Details "Assigned material" section: the Outliner and
        // other panels may legitimately contain the same words.
        const auto assignedSection = [&]() {
            const auto assignedStart = renderedUiText.find("--- Assigned material");
            const auto meshStart = renderedUiText.find("--- Mesh");
            require(assignedStart != std::string::npos && meshStart != std::string::npos && assignedStart < meshStart,
                "Details did not render the Assigned material section.");
            return renderedUiText.substr(assignedStart, meshStart - assignedStart);
        };
        clickActor(2);
        const auto actor2Assigned = assignedSection();
        require(renderedUiText.find("Actor #2") != std::string::npos &&
            actor2Assigned.find(groundButton) != std::string::npos &&
            actor2Assigned.find(copperButton) == std::string::npos,
            "Shared geometry made Details show another instance's material. UI log:\n" + renderedUiText);
        clickActor(1);
        const auto actor1Assigned = assignedSection();
        require(renderedUiText.find("Actor #1") != std::string::npos &&
            actor1Assigned.find(copperButton) != std::string::npos &&
            actor1Assigned.find(groundButton) == std::string::npos,
            "Switching instances did not refresh the assigned material. UI log:\n" + renderedUiText);
        world.findActor(2)->mesh.meshId = originalGroundMesh;

        // Actor selection chooses Details once. Manually choosing Scene stays
        // stable, and Content asset browsing must not take that tab away.
        input.mouse = dockTabCenter(findWindow("Scene"));
        capture("scene_manual_hover", true);
        click(input.mouse);
        for (int frame = 0; frame < 3; ++frame) {
            capture("scene_manual_stable", true);
            require(findWindow("Scene")->DockNode->SelectedTabId == findWindow("Scene")->TabId,
                "A delayed Actor selection stole the manually selected Scene tab.");
        }
        clickMesh(2);
        require(editor.selectedActor() == 1 &&
            findWindow("Scene")->DockNode->SelectedTabId == findWindow("Scene")->TabId,
            "Content asset selection stole the Scene tab or changed the Actor selection.");
        // Actor instance fields live in Details; the Scene page now only holds
        // scene-level parameters.
        auto* detailsInspector = findWindow("Details");
        require(detailsInspector != nullptr, "The editor has no Details panel.");
        if (detailsInspector->DockNode && detailsInspector->DockNode->SelectedTabId != detailsInspector->TabId)
            click(dockTabCenter(detailsInspector));
        const auto actorName = itemRect(detailsInspector, detailsInspector->GetID("##actorName"));
        require(actorName.GetWidth() > 0 && actorName.GetHeight() > 0,
            "The Details page does not expose the selected Actor fields.");
        captureInspectorThemes("inspector_details");
        captureInspectorThemes("inspector_scene");
        std::cout << "PASS: Actor instance Details, shared-mesh material isolation, Content selection independence, "
            "and stable manual Scene selection.\n";
    }
    const auto openAssignedMaterial = [&] {
        auto* details = findWindow("Details");
        require(details != nullptr, "Material entry has no Details panel.");
        if (details->DockNode && details->DockNode->SelectedTabId != details->TabId)
            click(dockTabCenter(details));
        click(itemRect(details, details->GetID("Edit assigned material")).GetCenter());
        capture("material_opened", true);
        const auto* reopened = findWindow("Material Editor###MaterialWorkspace");
        require(reopened && reopened->Active && reopened->Viewport && reopened->Viewport->PlatformHandle,
            "The material entry click did not reopen a live platform window.");
    };
    auto* materialWindow = findWindow("Material Editor###MaterialWorkspace");
    require(!materialWindow || !materialWindow->Active,
        "The material workspace opened without an explicit material entry action.");
    for (int index = 0; index < context->TabBars.GetBufSize(); ++index) {
        auto* bar = context->TabBars.GetByIndex(index);
        if (!bar->Window || bar->Window->ID != ImHashStr("Content & Editors")) continue;
        for (auto& tab : bar->Tabs) {
            const auto* name = ImGui::TabBarGetTabName(bar, &tab);
            require(std::strcmp(name, "Material Editor") != 0 && std::strcmp(name, "Shader Editor") != 0,
                "Material/Shader editing is still embedded in the mesh browser.");
        }
    }
    require(context->IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable,
        "The editor smoke test did not exercise native platform windows.");
    const auto mainHwnd = static_cast<HWND>(context->Viewports[0]->PlatformHandle);
    require(mainHwnd && IsWindow(mainHwnd), "The main editor has no live native window.");
    RECT movedMain{};
    require(GetWindowRect(mainHwnd, &movedMain) != FALSE, "Cannot read the main editor window rectangle.");
    require(SetWindowPos(mainHwnd, nullptr, movedMain.left + 47, movedMain.top + 31, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Cannot move the main test window.");
    capture("main_window_moved", true);
    capture("main_window_moved", true);
    require(mainOrigin().x != 0 || mainOrigin().y != 0,
        "Viewport coordinate regression needs a nonzero desktop origin.");
    openAssignedMaterial();
    materialWindow = findWindow("Material Editor###MaterialWorkspace");
    require(materialWindow && materialWindow->Active && materialWindow->Viewport != context->Viewports[0],
        "Scene did not open a separate material platform viewport.");
    auto materialHwnd = static_cast<HWND>(materialWindow->Viewport->PlatformHandle);
    require(materialHwnd && materialHwnd != mainHwnd && IsWindow(materialHwnd),
        "The material workspace does not own an independent system window.");
    require(GetWindow(materialHwnd, GW_OWNER) == nullptr,
        "The material tool is owned by the main HWND and will hide when it is minimized.");
    require(GetWindowRect(mainHwnd, &movedMain) != FALSE, "Cannot read the moved main editor rectangle.");
    require(SetWindowPos(materialHwnd, nullptr, movedMain.right + 36, movedMain.top + 48, 760, 640,
        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Cannot move/resize the material system window.");
    capture("material_outside_main", true);
    capture("material_outside_main", true);
    RECT materialBounds{};
    require(GetWindowRect(materialHwnd, &materialBounds) && materialBounds.left > movedMain.right,
        "The material window was clamped back into the main editor window.");
    const auto platformWindowCount = [&] {
        int count = 0;
        for (const auto* viewport : context->Viewports)
            count += viewport != context->Viewports[0] && viewport->PlatformHandle &&
                IsWindow(static_cast<HWND>(viewport->PlatformHandle));
        return count;
    };
    const auto openedCount = platformWindowCount();
    openAssignedMaterial();
    require(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle == materialHwnd &&
        platformWindowCount() == openedCount,
        "Opening the same material created a duplicate native workspace.");
    ShowWindow(materialHwnd, SW_MINIMIZE);
    openAssignedMaterial();
    require(!IsIconic(materialHwnd), "Opening a material did not restore its minimized tool window.");

    const auto saveMaterialCapture = [&](const char* filename) {
        // Redraw this real platform viewport and read its swapchain before
        // Present: DISCARD swapchains do not guarantee post-Present contents.
        // No private backend structs or desktop occlusion are involved.
        ImGuiContextScope active(context);
        const auto* window = findWindow("Material Editor###MaterialWorkspace");
        require(window && window->Active && window->Viewport->DrawData,
            "Native material capture has no submitted material UI.");
        const ImGuiViewport* lastRendered = nullptr;
        for (int index = 1; index < context->PlatformIO.Viewports.Size; ++index) {
            const auto* viewport = context->PlatformIO.Viewports[index];
            if (!(viewport->Flags & ImGuiViewportFlags_IsMinimized)) lastRendered = viewport;
        }
        require(lastRendered == window->Viewport,
            "The material workspace was not the last platform target rendered for capture.");
        auto& platform = context->PlatformIO;
        require(platform.Renderer_RenderWindow && platform.Renderer_SwapBuffers,
            "Native material capture has no platform render/present callbacks.");
        platform.Renderer_RenderWindow(window->Viewport, nullptr);
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11RenderTargetView> target;
        renderer.context()->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        require(target != nullptr, "Native material capture has no DX11 render target.");
        ComPtr<ID3D11Resource> resource;
        target->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(resource.As(&texture)), "Native material target is not a 2D texture.");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        require(desc.Width == static_cast<UINT>(window->Viewport->Size.x) &&
            desc.Height == static_cast<UINT>(window->Viewport->Size.y) && desc.SampleDesc.Count == 1,
            "Native material target dimensions do not match its independent viewport.");
        require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
            "Native material capture has an unsupported pixel format.");
        const auto format = desc.Format;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())),
            "Could not allocate the native material capture readback texture.");
        std::vector<unsigned char> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        renderer.context()->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
            "Could not map the native material capture surface.");
        for (UINT row = 0; row < desc.Height; ++row) {
            const auto* source = static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch;
            auto* destination = pixels.data() + static_cast<std::size_t>(row) * desc.Width * 4;
            std::memcpy(destination, source, static_cast<std::size_t>(desc.Width) * 4);
            if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
                for (UINT column = 0; column < desc.Width; ++column)
                    std::swap(destination[column * 4], destination[column * 4 + 2]);
        }
        renderer.context()->Unmap(staging.Get(), 0);
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER bitmap{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(bitmap);
        require(pixels.size() <= std::numeric_limits<DWORD>::max() - file.bfOffBits,
            "Native material capture exceeds the BMP file size limit.");
        file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
        bitmap.biSize = sizeof(bitmap);
        bitmap.biWidth = static_cast<LONG>(desc.Width);
        bitmap.biHeight = -static_cast<LONG>(desc.Height);
        bitmap.biPlanes = 1;
        bitmap.biBitCount = 32;
        bitmap.biCompression = BI_RGB;
        bitmap.biSizeImage = static_cast<DWORD>(pixels.size());
        std::ofstream image(output / filename, std::ios::binary);
        image.write(reinterpret_cast<const char*>(&file), sizeof(file));
        image.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
        image.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        image.flush();
        require(static_cast<bool>(image), "Could not write the native material GPU UI capture.");
        platform.Renderer_SwapBuffers(window->Viewport, nullptr);
    };

    const auto tabBar = [&]() -> ImGuiTabBar* {
        for (int i = 0; i < context->TabBars.GetBufSize(); ++i) {
            auto* bar = context->TabBars.GetByIndex(i);
            if (bar->Window && bar->Window->ID == ImHashStr("Material Editor###MaterialWorkspace")) return bar;
        }
        throw std::runtime_error("Material workspace tab bar not found.");
    };
    const auto selectTab = [&](const char* name) {
        auto* bar = tabBar();
        // Locating a low material control can scroll the tab row out of view.
        // Reveal the real row before calculating its mouse target; clicking a
        // clipped BarRect would otherwise hit a different panel or control.
        if (bar->BarRect.Min.y < bar->Window->InnerRect.Min.y ||
            bar->BarRect.Max.y > bar->Window->InnerRect.Max.y) {
            ImGuiContextScope active(context);
            ImGui::ScrollToRectEx(bar->Window, bar->BarRect, ImGuiScrollFlags_KeepVisibleEdgeY);
            capture("ui_tab_reveal", true);
            bar = tabBar();
        }
        require(bar->BarRect.Min.y >= bar->Window->InnerRect.Min.y - 1 &&
            bar->BarRect.Max.y <= bar->Window->InnerRect.Max.y + 1,
            "Editor tab row is clipped after scrolling it into view.");
        for (auto& tab : bar->Tabs) {
            if (std::strcmp(ImGui::TabBarGetTabName(bar, &tab), name) != 0) continue;
            const auto id = tab.ID;
            click({bar->BarRect.Min.x + tab.Offset + tab.Width * .5f,
                (bar->BarRect.Min.y + bar->BarRect.Max.y) * .5f});
            require(tabBar()->SelectedTabId == id, std::string("Could not click ") + name);
            return;
        }
        throw std::runtime_error(std::string("Editor tab missing: ") + name);
    };
    selectTab("Material Editor");
    capture("material_editor", true);
    saveMaterialCapture("material_native.bmp");
    {
        ImGuiContextScope active(context);
        const auto originalTheme = imgui::currentTheme();
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            capture("material_theme", true);
            saveMaterialCapture(theme == imgui::EditorTheme::Black ? "material_native_black.bmp" : "material_native_blue.bmp");
        }
        imgui::setTheme(originalTheme);
        capture("material_editor", true);
    }
    {
        ImGuiContextScope active(context);
        const auto materialRect = [&](const char* label) {
            const auto* bar = tabBar();
            return itemRect(bar->Window, ImHashStr(label, 0, bar->SelectedTabId));
        };
        auto* detailsWindow = findWindow("Details");
        require(detailsWindow != nullptr, "Material-preview Details panel is missing.");
        if (detailsWindow->DockNode && detailsWindow->DockNode->SelectedTabId != detailsWindow->TabId)
            click(dockTabCenter(detailsWindow));
        click(itemRect(detailsWindow, detailsWindow->GetID("Edit assigned material")).GetCenter());
        selectTab("Material Editor");
        const auto* materialActor = world.findActor(editor.selectedActor());
        require(materialActor != nullptr && materialActor->mesh.materialId == 1 && assets.findMaterial(2),
            "Material preview requires the default copper sphere and ground.");
        const auto materialId = materialActor->mesh.materialId;
        const auto committed = *assets.findMaterial(materialId);
        const auto sceneOnDisk = contents(project.defaultScene);
        const auto materialPath = project.contentRoot / "Materials" / assets::AssetLibrary::materialAssetFileName(committed);
        const auto materialOnDisk = contents(materialPath);
        const auto shaderVersion = renderer.shaderRevision();
        // IO injection alone cannot validate native HWND dispatch, especially
        // when the backend's optional context property cannot be installed.
        // Deliver a real Win32 character to the focused material text field.
        click(materialRect("Name").GetCenter());
        require(context->IO.WantTextInput && context->ActiveId != 0,
            "The native material Name field did not acquire text input.");
        materialHwnd = static_cast<HWND>(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle);
        SendMessageW(materialHwnd, WM_CHAR, static_cast<WPARAM>('Q'), 0);
        capture("material_native_text_input", true);
        require(editor.materialPreview() && editor.materialPreview()->name != committed.name &&
            editor.materialPreview()->name.find('Q') != std::string::npos &&
            assets.findMaterial(materialId)->name == committed.name,
            "Native WM_CHAR did not edit only the material draft through its HWND procedure.");
        click(materialRect("Discard draft").GetCenter());
        require(!editor.materialPreview() && assets.findMaterial(materialId)->name == committed.name,
            "Discard did not restore the native-input material draft.");
        // A minimized main surface must not suspend an independent editor.
        // Exercise the same material-only UI frame used by the native loop.
        const auto toolFrame = [&] {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                require(message.message != WM_QUIT, "A material close request quit the application.");
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            ui.beginFrame(1.0f / 60.0f);
            editor.drawMaterialWindow(renderer);
            ui.endFrame(false);
        };
        const auto grid = materialRect("Grid").GetCenter();
        ShowWindow(mainHwnd, SW_MINIMIZE);
        require(IsIconic(mainHwnd), "The native main-window minimization fixture failed.");
        input.mouse = grid;
        toolFrame();
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
        toolFrame();
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
        toolFrame();
        require(editor.materialPreview() && editor.materialPreview()->grid != committed.grid &&
            assets.findMaterial(materialId)->grid == committed.grid && ui.hasVisiblePlatformWindows(),
            "The material tool could not edit its independent draft while the main window was minimized.");
        const auto retainedPreview = *editor.materialPreview();
        materialHwnd = static_cast<HWND>(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle);
        ShowWindow(materialHwnd, SW_MINIMIZE);
        SendMessageW(materialHwnd, WM_CLOSE, 0, 0);
        require(ui.hasPendingPlatformWindowRequests(),
            "Closing the minimized material window did not leave a pending native request.");
        toolFrame();
        toolFrame();
        require(!IsWindow(materialHwnd) && IsWindow(mainHwnd) && editor.materialPreview() &&
            editor.materialPreview()->grid == retainedPreview.grid,
            "Closing both-minimized tools lost a draft, left its HWND alive, or closed the main editor.");
        ShowWindow(mainHwnd, SW_RESTORE);
        capture("material_closed_draft_retained", true);
        // Native restore reactivates the main viewport and re-submits the dock
        // windows after material-only frames. Let platform focus/navigation
        // initialization settle before targeting a specific Scene item.
        capture("material_main_restored", true);
        capture("material_main_restored", true);
        openAssignedMaterial();
        selectTab("Material Editor");
        materialHwnd = static_cast<HWND>(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle);
        require(IsWindow(materialHwnd) && editor.materialPreview() &&
            editor.materialPreview()->grid == retainedPreview.grid,
            "Reopening the native material window did not preserve the hidden session draft.");
        click(materialRect("Discard draft").GetCenter());
        require(!editor.materialPreview(), "Native-window lifecycle test did not discard its temporary draft.");

        // Shared geometry must not merge instance material assignments.
        const auto originalGroundMesh = world.findActor(2)->mesh.meshId;
        world.findActor(2)->mesh.meshId = world.findActor(editor.selectedActor())->mesh.meshId;
        capture("instance_material_reference", true);
        auto* meshDetails = findWindow("Details");
        require(meshDetails && meshDetails->DockNode && meshDetails->DockNode->TabBar,
            "Instance information has no Details page.");
        click(dockTabCenter(meshDetails));
        const auto assignedMaterialLabel = committed.name + "###InstanceMaterial";
        click(itemRect(meshDetails, meshDetails->GetID(assignedMaterialLabel.c_str())).GetCenter());
        capture("instance_material_opened", true);
        require(std::strstr(findWindow("Material Editor###MaterialWorkspace")->Name, committed.name.c_str()),
            "Clicking an instance material opened another Actor's material.");
        world.findActor(2)->mesh.meshId = originalGroundMesh;
        click(dockTabCenter(findWindow("Scene")));
        openAssignedMaterial();
        selectTab("Material Editor");
        require(std::strstr(findWindow("Material Editor###MaterialWorkspace")->Name, committed.name.c_str()),
            "Scene did not preserve the Actor's assigned material after Details editing.");
        std::cout << "PASS: Explicit Scene and instance material entry, independent HWND, outside-main move/resize, "
            "repeat-open focus, minimized restore, material-only editing, both-minimized native close, "
            "and session draft preservation.\n";
        settings.path = renderer::RenderPath::Deferred;
        settings.rayTracing = false;
        capture("material_preview_before", true);
        const auto before = scenePixels(output / "material_preview_before.bmp", settings);
        const auto dragParameter = [&](const char* label, float first, float last, const char* prefix) {
            const auto rect = materialRect(label);
            const auto point = [&](float fraction) {
                return ImVec2(rect.Min.x + 4 + (rect.GetWidth() - 8) * fraction, rect.GetCenter().y);
            };
            input.mouse = point(first);
            capture("material_hover", true);
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("material_pressed", true);
        // Injected mouse state can be dropped if the frame between the move and
        // the press lands on a hover change; retry once so the drag always starts.
        if (context->ActiveId == 0) {
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("material_pressed_retry", true);
        }
            std::string previous;
            for (int step = 1; step <= 3; ++step) {
                input.mouse = point(first + (last - first) * step / 3.f);
                const auto name = std::string(prefix) + "_" + std::to_string(step);
                capture(name.c_str(), true);
                require(context->IO.MouseDown[0] && context->ActiveId != 0 && editor.materialPreview(),
                std::string("Material preview did not update while the slider was held. mouseDown=") + (context->IO.MouseDown[0] ? "1" : "0") + " activeId=" + std::to_string(static_cast<unsigned>(context->ActiveId)) + " preview=" + (editor.materialPreview() ? "1" : "0") + " step=" + std::to_string(step));
                const auto pixels = scenePixels(output / (name + ".bmp"), settings);
                require(!pixels.empty() && (step == 1 || pixels != previous),
                    "Dragging a material slider did not continuously change scene pixels.");
                previous = pixels;
                require(assets.findMaterial(materialId)->roughness == committed.roughness &&
                    assets.findMaterial(materialId)->metallic == committed.metallic,
                    "Live material preview changed committed asset parameters.");
                require(renderer.shaderRevision() == shaderVersion,
                    "A numeric material parameter change recompiled the shader.");
            }
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
            capture("material_released", true);
        };
        dragParameter("Roughness", .15f, .85f, "material_roughness_drag");
        require(scenePixels(output / "material_roughness_drag_3.bmp", settings) != before,
            "Unapplied material roughness did not change the viewport.");
        const auto previewScene = output / "MaterialPreviewSandbox/preview.rascene";
        std::filesystem::create_directories(previewScene.parent_path());
        require(world.save(previewScene, assets, error), error);
        scene::World restoredWorld;
        assets::AssetLibrary restoredAssets;
        // Scene v2 must reference materials instead of embedding payloads; the
        // committed library is the resolver for the reload.
        const auto previewJson = nlohmann::json::parse(contents(previewScene));
        require(previewJson.at("version").get<int>() >= 2 && previewJson.at("materials").front().size() == 1 &&
            previewJson.at("materials").front().contains("id"),
            "The saved scene still embeds material payloads.");
        restoredAssets = assets;
        require(restoredWorld.load(previewScene, restoredAssets, error), error);
        require(restoredAssets.findMaterial(materialId)->roughness == committed.roughness,
            "Saving a scene serialized the unapplied live material preview.");
        click(materialRect("Discard draft").GetCenter());
        require(editor.materialPreview() == nullptr, "Discard kept an active material preview.");
        capture("material_preview_discarded", true);
            // One more frame: the override clears during the click frame, so compare an
            // image that already rendered without the preview.
            capture("material_preview_discarded", true);
        require(scenePixels(output / "material_preview_discarded.bmp", settings) == before,
            "Discard did not restore the committed material's scene pixels.");

        dragParameter("Metallic", .1f, .9f, "material_metallic_drag");
        const auto desired = *editor.materialPreview();
        // An override and the same values committed in memory must render
        // identically, including reflected analytic and triangle materials.
        const auto groundBefore = *assets.findMaterial(2);
        assets.findMaterial(2)->roughness = .08f;
        assets.findMaterial(2)->metallic = 1.f;
        const auto spherePrimitive = assets.findMesh(1)->primitive;
        for (int mode = 0; mode < 5; ++mode) {
            settings.path = mode == 0 || mode == 2 ? renderer::RenderPath::Forward : renderer::RenderPath::Deferred;
            settings.rayTracing = mode >= 2;
            if (mode == 4) assets.findMesh(1)->primitive = assets::MeshPrimitive::Imported;
            const auto suffix = std::to_string(mode);
            capture(("material_override_" + suffix).c_str(), true);
            const auto overridden = scenePixels(output / ("material_override_" + suffix + ".bmp"), settings);
            *assets.findMaterial(materialId) = desired;
            capture(("material_equivalent_" + suffix).c_str(), true, false, false);
            require(scenePixels(output / ("material_equivalent_" + suffix + ".bmp"), settings) == overridden,
                "Material preview differs from equivalent committed raster/ray parameters.");
            *assets.findMaterial(materialId) = committed;
        }
        assets.findMesh(1)->primitive = spherePrimitive;
        *assets.findMaterial(2) = groundBefore;
        settings.path = renderer::RenderPath::Deferred;
        settings.rayTracing = false;
        capture("material_before_apply_settle", true);
        capture("material_before_apply", true);
        const auto unapplied = scenePixels(output / "material_before_apply.bmp", settings);
        const auto beforeApplySettings = settings;
        click(materialRect("Apply material").GetCenter());
        const auto& appliedMaterial = *assets.findMaterial(materialId);
        require(editor.materialPreview() == nullptr && appliedMaterial.id == desired.id &&
            appliedMaterial.name == desired.name && appliedMaterial.roughness == desired.roughness &&
            appliedMaterial.metallic == desired.metallic && appliedMaterial.grid == desired.grid &&
            appliedMaterial.twoSided == desired.twoSided &&
            std::memcmp(appliedMaterial.baseColor, desired.baseColor, sizeof(desired.baseColor)) == 0,
            "Apply did not commit the preview or clear its temporary override.");
        capture("material_after_apply_settle", true);
        capture("material_after_apply", true);
        require(settings.viewportX == beforeApplySettings.viewportX && settings.viewportY == beforeApplySettings.viewportY &&
            settings.viewportWidth == beforeApplySettings.viewportWidth && settings.viewportHeight == beforeApplySettings.viewportHeight &&
            settings.editorViewport == beforeApplySettings.editorViewport && settings.viewportVisible == beforeApplySettings.viewportVisible &&
            settings.path == beforeApplySettings.path && settings.shadowMethod == beforeApplySettings.shadowMethod &&
            settings.rayTracing == beforeApplySettings.rayTracing && settings.debugView == beforeApplySettings.debugView &&
            settings.roughness == beforeApplySettings.roughness && settings.metallic == beforeApplySettings.metallic &&
            std::memcmp(settings.baseColor, beforeApplySettings.baseColor, sizeof(settings.baseColor)) == 0 &&
            settings.lightIntensity == beforeApplySettings.lightIntensity && settings.exposure == beforeApplySettings.exposure &&
            settings.cameraYaw == beforeApplySettings.cameraYaw && settings.cameraPitch == beforeApplySettings.cameraPitch &&
            settings.cameraDistance == beforeApplySettings.cameraDistance &&
            std::memcmp(settings.cameraTarget, beforeApplySettings.cameraTarget, sizeof(settings.cameraTarget)) == 0,
            "Material Apply changed the viewport, camera or scene rendering settings.");
        const auto appliedPixels = scenePixels(output / "material_after_apply.bmp", settings);
        require(appliedPixels.size() == unapplied.size(), "Apply changed scene image dimensions.");
        unsigned maxDifference = 0;
        std::size_t changedChannels = 0, changedPixels = 0;
        for (std::size_t pixel = 0; pixel < unapplied.size(); pixel += 4) {
            bool changed = false;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto difference = static_cast<unsigned>(std::abs(
                    static_cast<int>(static_cast<unsigned char>(unapplied[pixel + channel])) -
                    static_cast<int>(static_cast<unsigned char>(appliedPixels[pixel + channel]))));
                maxDifference = std::max(maxDifference, difference);
                changedChannels += difference != 0;
                changed |= difference != 0;
            }
            changedPixels += changed;
        }
        // The native-window run exposed one ground-pixel green channel moving
        // by 1/255 despite exact material/camera/rect equality. Permit at most
        // four isolated channel quantization steps, never an image-wide drift.
        require(maxDifference <= 1 && changedChannels <= 4 && changedPixels <= 4,
            "Apply changed the already-previewed scene appearance: max delta=" + std::to_string(maxDifference) +
            ", changed channels=" + std::to_string(changedChannels) + ", changed pixels=" + std::to_string(changedPixels));
        std::cout << "Apply scene equivalence: exact material/camera/viewport state; " << changedPixels <<
            " changed pixel(s), " << changedChannels << " changed RGB channel(s), max delta " << maxDifference << "/255.\n";
        const auto savedFolder = previewScene.parent_path() / "Materials";
        require(assets.saveMaterials(savedFolder, error), error);
        require(restoredAssets.loadMaterials(savedFolder, error), error);
        require(restoredAssets.findMaterial(materialId)->metallic == desired.metallic,
            "Applied material parameters did not survive an isolated save/reload.");
        require(contents(project.defaultScene) == sceneOnDisk && contents(materialPath) == materialOnDisk,
            "Material preview/Apply changed the real project's saved files.");
        require(renderer.shaderRevision() == shaderVersion, "Material preview/Apply unexpectedly recompiled shaders.");
        *assets.findMaterial(materialId) = committed;
        // Refresh the editor draft from the restored asset using normal UI.
        dragParameter("Roughness", .2f, .75f, "material_restore_drag");
        click(materialRect("Discard draft").GetCenter());
        capture("material_editor", true);
        {
            const auto originalWorld = world;
            const auto originalSettings = settings;
            auto* actor = world.findActor(editor.selectedActor());
            require(actor != nullptr, "Two-sided UI fixture has no selected Actor.");
            for (auto& candidate : world.actors) candidate.visible = candidate.id == actor->id;
            actor->mesh.meshId = 2;
            actor->transform = {};
            std::fill(std::begin(actor->transform.scale), std::end(actor->transform.scale), .15f);
            settings.cameraYaw = 0; settings.cameraPitch = -.6f; settings.cameraDistance = 6;
            std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
            settings.path = renderer::RenderPath::Deferred;
            settings.debugView = 1;
            settings.rayTracing = false;
            settings.shadowMethod = renderer::ShadowMethod::Disabled;
            require(!assets.findMaterial(materialId)->twoSided,
                "Two-sided UI fixture expected the new default single-sided material.");
            capture("two_sided_ui_before", true);
            const auto single = scenePixels(output / "two_sided_ui_before.bmp", settings);
            click(materialRect("Two-sided").GetCenter());
            require(editor.materialPreview() && editor.materialPreview()->twoSided &&
                !assets.findMaterial(materialId)->twoSided && renderer.shaderRevision() == shaderVersion,
                "Two-sided checkbox did not preview independently without committing or recompiling.");
            capture("two_sided_ui_preview", true);
            const auto doubleSided = scenePixels(output / "two_sided_ui_preview.bmp", settings);
            require(single != doubleSided, "Two-sided checkbox did not immediately reveal the plane's back face.");
            click(materialRect("Discard draft").GetCenter());
            capture("two_sided_ui_discarded", true);
            require(!editor.materialPreview() &&
                scenePixels(output / "two_sided_ui_discarded.bmp", settings) == single,
                "Discard failed to restore single-sided visibility.");
            click(materialRect("Two-sided").GetCenter());
            click(materialRect("Apply material").GetCenter());
            capture("two_sided_ui_applied", true);
            require(!editor.materialPreview() && assets.findMaterial(materialId)->twoSided &&
                scenePixels(output / "two_sided_ui_applied.bmp", settings) == doubleSided,
                "Apply failed to preserve the already-previewed Two-sided appearance.");
            require(assets.saveMaterials(savedFolder, error), error);
            require(restoredAssets.loadMaterials(savedFolder, error), error);
            require(restoredAssets.findMaterial(materialId)->twoSided,
                "The UI-applied Two-sided value did not survive isolated material persistence.");
            const auto planeActorId = actor->id;
            const auto backFacePoint = scenePoint(settings.viewportWidth * .62f, settings.viewportHeight * .58f);
            click(materialRect("Two-sided").GetCenter());
            require(editor.materialPreview() && !editor.materialPreview()->twoSided,
                "Two-sided picking fixture did not preview single-sided visibility.");
            click(backFacePoint);
            require(editor.selectedActor() == 0, "The culled single-sided back face still blocked viewport picking.");
            click(materialRect("Two-sided").GetCenter());
            require(editor.materialPreview() && editor.materialPreview()->twoSided,
                "Two-sided picking fixture did not restore its double-sided preview.");
            click(backFacePoint);
            require(editor.selectedActor() == planeActorId,
                "A visible two-sided preview back face could not be selected.");
            require(contents(project.defaultScene) == sceneOnDisk && contents(materialPath) == materialOnDisk &&
                renderer.shaderRevision() == shaderVersion,
                "Two-sided editing changed real project files or recompiled shaders.");
            *assets.findMaterial(materialId) = committed;
            click(materialRect("Discard draft").GetCenter());
            world = originalWorld; settings = originalSettings;
            capture("material_editor", true);
            std::cout << "PASS: Two-sided checkbox real-time back-face preview, independent asset state, "
                "Discard/Apply, matching back-face picking, isolated persistence, and no shader recompilation.\n";
        }
        std::cout << "PASS: Material slider feedback while held, scene-only pixel changes, temporary asset overrides, "
            "Discard/Apply, isolated persistence, Forward/Deferred, analytic/triangle rays, and no shader recompilation.\n";
    }
    selectTab("Shader Editor");
    capture("shader_editor", true);
    saveMaterialCapture("shader_native.bmp");
    const auto buttonRow = [&]() {
        const auto* bar = tabBar();
        return itemRect(bar->Window, ImHashStr("Apply / Compile", 0, bar->SelectedTabId)).GetCenter();
    };
    const auto replaceDraft = [&](const std::string& source) {
        ImGuiWindow* textWindow = nullptr;
        for (auto* window : context->Windows)
            if (window->Active && std::strstr(window->Name, "##Shader source")) textWindow = window;
        require(textWindow != nullptr, "Shader source text box not found.");
        click({textWindow->Pos.x + 30, textWindow->Pos.y + 20});
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); };
        capture("ui_interaction", true);
        input.events = [&](ImGuiIO& io) {
            io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
            io.AddInputCharactersUTF8(source.c_str());
        };
        capture("ui_interaction", true);
    };
    const auto beforeUiApply = renderer.shaderRevision();
    replaceDraft(changedSource);
    materialHwnd = static_cast<HWND>(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle);
    SendMessageW(materialHwnd, WM_CLOSE, 0, 0);
    capture("shader_window_closed", true);
    capture("shader_window_closed", true);
    require(!IsWindow(materialHwnd) && renderer.shaderRevision() == beforeUiApply,
        "Closing the Shader tool committed a draft or left its native window alive.");
    openAssignedMaterial();
    selectTab("Shader Editor");
    click(buttonRow());
    require(renderer.shaderSource() == changedSource && renderer.shaderRevision() == beforeUiApply + 1,
        "Shader Editor text input / Apply button did not compile the edited source.");
    capture("shader_editor_applied", true);
    replaceDraft("THIS IS NOT HLSL;\n");
    click(buttonRow());
    require(renderer.shaderSource() == changedSource && renderer.shaderRevision() == beforeUiApply + 1,
        "Shader Editor failed compilation replaced the working shader.");
    capture("shader_editor_failed", true, true);
    require(renderedUiText.find("Compile failed") != std::string::npos &&
        renderedUiText.find("error X") != std::string::npos,
        "Shader Editor did not display failed compilation diagnostics.");
    replaceDraft(originalUiSource);
    click(buttonRow());
    require(renderer.shaderSource() == originalUiSource,
        "Shader Editor smoke test did not restore the original shader.");
    capture("shader_editor", true, true);
    require(renderedUiText.find("unapplied draft") == std::string::npos,
        "Shader Editor smoke test left an unapplied draft.");

    // Exercise the normal viewport input path: picking, a constrained gizmo drag,
    // Delete, and Delete while a text editor owns the keyboard.
    const auto projectPoint = [&](float x, float y, float z) {
        using namespace DirectX;
        const float distance = settings.cameraDistance;
        const float pitch = settings.cameraPitch;
        const auto target = XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 1.f);
        const auto eye = XMVectorAdd(target, XMVectorSet(
            distance * std::cos(pitch) * std::sin(settings.cameraYaw),
            distance * std::sin(pitch), distance * std::cos(pitch) * std::cos(settings.cameraYaw), 0.f));
        const auto view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
        const auto projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(50.f),
            static_cast<float>(settings.viewportWidth) / settings.viewportHeight, .05f, 100.f);
        const auto position = XMVector3Project(XMVectorSet(x, y, z, 1),
            mainOrigin().x + settings.viewportX, mainOrigin().y + settings.viewportY,
            static_cast<float>(settings.viewportWidth), static_cast<float>(settings.viewportHeight),
            0, 1, projection, view, XMMatrixIdentity());
        return ImVec2(XMVectorGetX(position), XMVectorGetY(position));
    };
    const auto pressKey = [&](ImGuiKey key) {
        input.events = [key](ImGuiIO& io) { io.AddKeyEvent(key, true); };
        capture("ui_interaction", true);
        input.events = [key](ImGuiIO& io) { io.AddKeyEvent(key, false); };
        capture("ui_interaction", true);
    };
    {
        // Drag actual Content widgets through ImGui's source/target delivery
        // path. No synthetic payload or direct actor-creation callback is used.
        const auto savedWorld = world;
        const auto savedSettings = settings;
        const auto savedFolder = editor.contentFolder();
        const auto savedAsset = editor.selectedContentAsset();
        const auto cameraUnchanged = [&](const renderer::SceneSettings& before) {
            require(settings.cameraYaw == before.cameraYaw && settings.cameraPitch == before.cameraPitch &&
                settings.cameraDistance == before.cameraDistance &&
                std::equal(std::begin(settings.cameraTarget), std::end(settings.cameraTarget),
                    std::begin(before.cameraTarget)), "A Content asset drag changed the scene camera.");
        };
        const auto assetPoint = [&](const std::string& path, bool grid) {
            const auto* entry = editor.assetRegistry().find(path);
            require(entry != nullptr, "Content drag fixture is absent from the asset registry: " + path);
            const auto folder = entry->folder;
            const auto name = entry->name;
            const auto type = entry->type;
            editor.selectContentFolder(folder);
            editor.setContentSearch(name);
            editor.setContentTypeFilter(type);
            editor.setContentRecursive(false);
            editor.setContentGridView(grid);
            editor.selectContentAsset(path);
            capture("content_drag_source_layout", true);
            capture("content_drag_source_layout", true);
            ImGuiWindow* browser = nullptr;
            for (auto* window : context->Windows)
                if (window->Active && std::strstr(window->Name, "##content-assets")) browser = window;
            require(browser != nullptr, "Content drag fixture has no asset child window.");
            const ImGuiID pathId = ImHashStr(path.c_str(), 0, browser->ID);
            return itemRect(browser, ImHashStr(grid ? "##asset-tile" : "##asset-row", 0, pathId)).GetCenter();
        };
        const auto beginAssetDrag = [&](const std::string& path, bool grid) {
            const auto start = assetPoint(path, grid);
            input.mouse = start;
            capture("content_drag_source_hover", true);
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("content_drag_source_press", true);
            input.mouse = {start.x + 18.0f, start.y};
            capture("content_drag_source_active", true);
            require(context->DragDropActive && context->DragDropPayload.IsDataType("RAE_ASSET") &&
                context->DragDropPayload.DataSize == static_cast<int>(path.size()) &&
                std::memcmp(context->DragDropPayload.Data, path.data(), path.size()) == 0,
                "Dragging a Content widget did not produce its real RAE_ASSET path payload.");
        };
        const auto holdAssetAt = [&](ImVec2 destination, const char* name) {
            input.mouse = destination;
            capture(name, true);
            // AcceptDragDropPayload records a candidate before the release
            // frame. Keep a second held frame so a drop tests actual delivery.
            capture(name, true);
        };
        const auto releaseAsset = [&](const char* name) {
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
            capture(name, true);
            capture(name, true, true);
        };
        const auto checkPlacedMesh = [&](const char* path, bool grid, assets::AssetId meshId,
                                         float x, float surfaceY, float z, const char* prefix) {
            const auto count = world.actors.size();
            const auto history = editor.undoStack().cursor();
            const auto camera = settings;
            beginAssetDrag(path, grid);
            const auto destination = projectPoint(x, surfaceY, z);
            require(findWindow("Scene Viewport##Interaction")->WorkRect.Contains(destination),
                "Mesh drop test point is outside the scene image.");
            holdAssetAt(destination, (std::string(prefix) + "_preview").c_str());
            require(world.actors.size() == count && editor.undoStack().cursor() == history,
                "Hovering a dragged Mesh created an Actor before mouse release.");
            // Camera input must remain unclaimed by the viewport while the
            // Content gesture owns the mouse, including wheel/Alt/right/middle.
            input.events = [](ImGuiIO& io) {
                io.AddKeyEvent(ImGuiMod_Alt, true);
                io.AddMouseButtonEvent(1, true); io.AddMouseButtonEvent(2, true);
                io.AddMouseWheelEvent(0, 1);
            };
            input.mouse = {destination.x + 9.0f, destination.y + 7.0f};
            capture("content_drag_camera_guard", true);
            cameraUnchanged(camera);
            input.events = [](ImGuiIO& io) {
                io.AddKeyEvent(ImGuiMod_Alt, false);
                io.AddMouseButtonEvent(1, false); io.AddMouseButtonEvent(2, false);
            };
            holdAssetAt(destination, (std::string(prefix) + "_preview").c_str());
            require(world.actors.size() == count && editor.undoStack().cursor() == history,
                "A held Content drag mutated the scene or its history.");
            releaseAsset((std::string(prefix) + "_placed").c_str());
            require(world.actors.size() == count + 1 && editor.undoStack().cursor() == history + 1 &&
                editor.undoStack().undoLabel() == "Place Mesh", "A Mesh drop did not create exactly one undoable Actor.");
            const auto* actor = world.findActor(editor.selectedActor());
            require(actor && actor->mesh.meshId == meshId && editor.selection().size() == 1 &&
                editor.selection().front() == actor->id && assets.findMaterial(actor->mesh.materialId),
                "The dropped Mesh did not become the sole selected Actor with valid asset references.");
            const auto placed = *actor;
            const auto* mesh = assets.findMesh(meshId);
            require(mesh && !mesh->vertices.empty(), "Placed Mesh has no geometry for the bounds assertion.");
            float bottom = std::numeric_limits<float>::max();
            for (const auto& vertex : mesh->vertices) bottom = std::min(bottom, vertex.position[1]);
            require(std::abs(placed.transform.position[0] - x) < .12f &&
                std::abs(placed.transform.position[2] - z) < .12f &&
                std::abs(placed.transform.position[1] + bottom - surfaceY) < .02f,
                "The dropped Mesh sank below its support or ignored the scene mouse position.");
            for (int axis = 0; axis < 3; ++axis)
                require(placed.transform.rotation[axis] == 0 && placed.transform.scale[axis] == 1,
                    "Dropping a Mesh changed its default rotation or scale.");
            require(findWindow("Details")->DockNode->SelectedTabId == findWindow("Details")->TabId &&
                renderedUiText.find("Actor #" + std::to_string(placed.id)) != std::string::npos,
                "A dropped Mesh did not reveal its Actor instance in Details.");
            cameraUnchanged(camera);
            require(editor.undo() && world.actors.size() == count && !world.findActor(placed.id) &&
                editor.undoStack().cursor() == history, "One Undo did not remove the dropped Actor.");
            require(editor.redo() && world.actors.size() == count + 1 && world.findActor(placed.id) &&
                world.findActor(placed.id)->mesh.meshId == meshId && editor.selectedActor() == placed.id &&
                std::memcmp(&world.findActor(placed.id)->transform, &placed.transform, sizeof(scene::Transform)) == 0,
                "Redo did not restore the same dropped Actor, GUID references and placement.");
            require(editor.undo(), "Mesh drop test could not restore the pre-drop scene.");
            capture("content_drag_undo_restored", true);
        };
        settings.cameraYaw = .4f; settings.cameraPitch = .65f; settings.cameraDistance = 8.0f;
        settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1.3f; settings.cameraTarget[2] = 0;
        world.actors.clear();
        const auto support = world.createActor("Mesh drop support", 2, 2);
        world.findActor(support)->transform.position[1] = 1.6f;
        capture("content_drag_surface_fixture", true);
        checkPlacedMesh("Meshes/Builtin/Cube.rasset", true, 3, .7f, 1.6f, .4f, "content_drop_grid_surface");
        world.actors.clear();
        capture("content_drag_ground_fixture", true);
        checkPlacedMesh("Meshes/Builtin/Sphere.rasset", false, 1, -.8f, 0, .65f, "content_drop_list_ground");
        world = savedWorld;
        settings = savedSettings;
        capture("content_drag_scene_restored", true);
        click(projectPoint(-.35f, 1.35f, 0));
        require(editor.selectedActor() == 1, "Canceled-drop fixtures need a selected Actor and its live gizmo.");
        const auto viewportToolbar = itemRect(findWindow("Scene Viewport##Interaction"),
            findWindow("Scene Viewport##Interaction")->GetID("Move [W]")).GetCenter();
        const auto sceneDestination = projectPoint(1.8f, 0, .4f);
        require(findWindow("Scene Viewport##Interaction")->WorkRect.Contains(sceneDestination) &&
            sceneDestination.y > viewportToolbar.y + 50.0f,
            "Canceled-drop fixture is outside the scene image or inside its toolbar.");
        assets::AssetQuery materialQuery;
        materialQuery.type = assets::AssetType::Material;
        materialQuery.recursive = true;
        const auto visibleMaterials = editor.assetRegistry().query(materialQuery);
        require(!visibleMaterials.empty(), "No visible Material exists for the unsupported drop test.");
        const std::string materialPath = visibleMaterials.front()->relativePath;
        for (int scenario = 0; scenario < 4; ++scenario) {
            const auto count = world.actors.size(), historySize = editor.undoStack().size(), history = editor.undoStack().cursor();
            const auto selected = editor.selectedActor();
            const auto camera = settings;
            beginAssetDrag(scenario == 3 ? materialPath : "Meshes/Builtin/Cube.rasset", scenario % 2 == 0);
            const auto destination = scenario == 1 ? ImVec2(mainOrigin().x - 60, mainOrigin().y - 60) :
                scenario == 2 ? viewportToolbar : sceneDestination;
            holdAssetAt(destination, "content_drop_rejected_hover");
            if (scenario == 0) {
                pressKey(ImGuiKey_Escape);
                require(!context->DragDropActive, "Escape did not cancel the Content payload.");
                input.mouse = {destination.x + 12, destination.y + 9};
                capture("content_drop_escape_still_held", true);
                require(!context->DragDropActive, "A canceled source restarted its payload before mouse release.");
            }
            releaseAsset(scenario == 0 ? "content_drop_escape" : scenario == 1 ? "content_drop_outside" :
                scenario == 2 ? "content_drop_toolbar" : "content_drop_material_rejected");
            require(world.actors.size() == count && editor.selectedActor() == selected &&
                editor.undoStack().size() == historySize && editor.undoStack().cursor() == history,
                "A canceled, outside, toolbar or unsupported Content drop changed the scene/history/selection.");
            for (const auto& original : savedWorld.actors) {
                const auto* actor = world.findActor(original.id);
                require(actor && std::memcmp(&actor->transform, &original.transform, sizeof(scene::Transform)) == 0,
                    "A rejected asset drag moved an existing Actor through its gizmo.");
            }
            cameraUnchanged(camera);
        }

        editor.setContentSearch(""); editor.setContentTypeFilter(assets::AssetType::Unknown);
        editor.setContentRecursive(false); editor.setContentGridView(false);
        editor.selectContentFolder(savedFolder); editor.selectContentAsset(savedAsset);
        input.mouse = {-100, -100};
        capture("content_drag_tests_restored", true);
        std::cout << "PASS: Grid/List Mesh delivery, surface/ground bounds, Details, undo/redo, "
            "cancellation and camera/gizmo input isolation.\n";
    }
    click(scenePoint(30, 100));
    require(editor.selectedActor() == 0, "Clicking empty viewport did not clear Actor selection.");
    capture("instance_details_empty", true, true);
    require(renderedUiText.find("Select an Actor in the viewport or Outliner.") != std::string::npos &&
        renderedUiText.find("Assigned material") == std::string::npos,
        "Empty selection retained stale instance Details.");
    click(projectPoint(-.35f, 1.35f, 0));
    require(editor.selectedActor() == 1, "Clicking the visible sphere did not select its Actor.");
    capture("instance_details_viewport", true, true);
    require(findWindow("Details")->DockNode->SelectedTabId == findWindow("Details")->TabId &&
        renderedUiText.find("Actor #1") != std::string::npos,
        "Viewport selection did not display the picked instance in Details.");
    const auto drag = [&](ImVec2 start, ImVec2 end, const char* screenshot) {
        input.mouse = start;
        capture("ui_interaction", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
        capture("ui_interaction", true);
        for (int step = 1; step <= 5; ++step) {
            const float amount = step / 5.f;
            input.mouse = {start.x + (end.x - start.x) * amount, start.y + (end.y - start.y) * amount};
            capture("ui_interaction", true);
        }
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
        capture(screenshot, true);
    };
    const auto defaultTransform = world.findActor(1)->transform;
    auto& rotated = world.findActor(1)->transform;
    rotated.rotation[0] = 17; rotated.rotation[1] = 29; rotated.rotation[2] = 11;
    rotated.scale[0] = 1.2f; rotated.scale[1] = .8f; rotated.scale[2] = 1.5f;
    const auto beforeRotation = rotated;
    pressKey(ImGuiKey_E);
    // The world Y rotation ring has a fixed screen size; aim at its visible,
    // camera-facing half, then move tangentially around it.
    const float ringRadius = .18f * settings.cameraDistance * std::tan(DirectX::XMConvertToRadians(25.f)) *
        (static_cast<float>(settings.viewportWidth) / settings.viewportHeight) * 1.2f;
    drag(projectPoint(ringRadius * std::cos(.35f), 1, ringRadius * std::sin(.35f)),
        projectPoint(ringRadius * std::cos(.7f), 1, ringRadius * std::sin(.7f)), "gizmo_rotated");
    const auto afterRotation = world.findActor(1)->transform;
    float rotationChange = 0;
    for (int axis = 0; axis < 3; ++axis) {
        rotationChange += std::abs(afterRotation.rotation[axis] - beforeRotation.rotation[axis]);
        require(std::isfinite(afterRotation.rotation[axis]) &&
            std::abs(afterRotation.position[axis] - beforeRotation.position[axis]) < .001f &&
            std::abs(afterRotation.scale[axis] - beforeRotation.scale[axis]) < .001f,
            "Rotation gizmo corrupted the Actor's nonuniform scale or position.");
    }
    require(rotationChange > 2.f && rotationChange < 100.f,
        "Rotation handle did not smoothly rotate the pre-rotated Actor.");
    require(editor.selectedActor() == 1, "Rotation gizmo lost Actor selection.");
    pressKey(ImGuiKey_R);
    const auto localXAxis = DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(1, 0, 0, 0),
        DirectX::XMMatrixRotationRollPitchYaw(DirectX::XMConvertToRadians(afterRotation.rotation[0]),
            DirectX::XMConvertToRadians(afterRotation.rotation[1]), DirectX::XMConvertToRadians(afterRotation.rotation[2])));
    const auto localAxisPoint = [&](float distance) {
        return projectPoint(distance * DirectX::XMVectorGetX(localXAxis),
            1 + distance * DirectX::XMVectorGetY(localXAxis), distance * DirectX::XMVectorGetZ(localXAxis));
    };
    drag(localAxisPoint(.55f), localAxisPoint(1.05f), "gizmo_scaled");
    const auto afterScale = world.findActor(1)->transform;
    require(std::abs(afterScale.scale[0] - afterRotation.scale[0]) > .05f,
        "Dragging the local X scale handle did not resize the rotated Actor.");
    for (int axis = 0; axis < 3; ++axis) {
        require(std::isfinite(afterScale.scale[axis]) && afterScale.scale[axis] > .001f &&
            std::abs(afterScale.position[axis] - afterRotation.position[axis]) < .001f &&
            std::abs(afterScale.rotation[axis] - afterRotation.rotation[axis]) < .001f,
            "Scale gizmo corrupted Actor rotation or position.");
        if (axis != 0) require(std::abs(afterScale.scale[axis] - afterRotation.scale[axis]) < .001f,
            "Local X scale handle changed another scale axis.");
    }
    world.findActor(1)->transform = defaultTransform;
    capture("ui_interaction", true);
    pressKey(ImGuiKey_W);
    const auto beforeDrag = world.findActor(1)->transform;
    const auto axisStart = projectPoint(.55f, 1, 0);
    const auto axisEnd = projectPoint(1.05f, 1, 0);
    input.mouse = axisStart;
    capture("gizmo_before", true);
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
    capture("ui_interaction", true);
    // Keep the button held over several frames, as with a physical drag.
    for (int step = 1; step <= 4; ++step) {
        const float amount = step / 4.f;
        input.mouse = {axisStart.x + (axisEnd.x - axisStart.x) * amount,
            axisStart.y + (axisEnd.y - axisStart.y) * amount};
        capture("ui_interaction", true);
    }
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
    capture("gizmo_moved", true);
    const auto afterDrag = world.findActor(1)->transform;
    require(std::abs(afterDrag.position[0] - beforeDrag.position[0]) > .1f,
        "Dragging the translation X handle did not move the selected Actor.");
    require(std::abs(afterDrag.position[1] - beforeDrag.position[1]) < .001f &&
        std::abs(afterDrag.position[2] - beforeDrag.position[2]) < .001f,
        "Dragging the X handle unexpectedly changed another translation axis.");
    require(editor.selectedActor() == 1, "Gizmo drag lost the selected Actor.");
    // Middle-mouse drag pans the orbit focus without touching yaw/pitch/distance.
    const float yawBeforePan = settings.cameraYaw;
    const float pitchBeforePan = settings.cameraPitch;
    const float distanceBeforePan = settings.cameraDistance;
    const float targetBeforePan[3] = {settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2]};
    const auto panStart = scenePoint(settings.viewportWidth * .5f, settings.viewportHeight * .5f);
    input.mouse = panStart;
    capture("ui_interaction", true);
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(2, true); };
    capture("ui_interaction", true);
    for (int step = 1; step <= 4; ++step) {
        input.mouse = {panStart.x + 18.f * step, panStart.y - 9.f * step};
        capture("ui_interaction", true);
    }
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(2, false); };
    capture("ui_interaction", true);
    const float panX = settings.cameraTarget[0] - targetBeforePan[0];
    const float panZ = settings.cameraTarget[2] - targetBeforePan[2];
    require(std::abs(panX) + std::abs(panZ) > 0.01f, "Middle-mouse drag did not pan the camera focus.");
    require(std::abs(settings.cameraYaw - yawBeforePan) < 1e-5f &&
            std::abs(settings.cameraPitch - pitchBeforePan) < 1e-5f &&
            std::abs(settings.cameraDistance - distanceBeforePan) < 1e-5f,
        "Panning changed yaw, pitch, or distance.");
    // Restore the framing so the remaining picking assertions are unaffected.
    settings.cameraTarget[0] = targetBeforePan[0];
    settings.cameraTarget[1] = targetBeforePan[1];
    settings.cameraTarget[2] = targetBeforePan[2];
    capture("ui_interaction", true);
    // Alt+left-drag must orbit exactly like right-drag.
    const float yawBeforeAlt = settings.cameraYaw, pitchBeforeAlt = settings.cameraPitch;
    const auto orbitStart = scenePoint(settings.viewportWidth * .5f, settings.viewportHeight * .5f);
    input.mouse = orbitStart;
    input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Alt, true); };
    capture("ui_interaction", true);
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
    capture("ui_interaction", true);
    for (int step = 1; step <= 3; ++step) {
        input.mouse = {orbitStart.x - 16.f * step, orbitStart.y};
        capture("ui_interaction", true);
    }
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); io.AddKeyEvent(ImGuiMod_Alt, false); };
    capture("ui_interaction", true);
    require(std::abs(settings.cameraYaw - yawBeforeAlt) > 1e-4f, "Alt+left-drag did not orbit the camera.");
    settings.cameraYaw = yawBeforeAlt;
    settings.cameraPitch = pitchBeforeAlt;
    // Orbit must also tilt below the horizon so the scene can be viewed from underneath.
    const float pitchBeforeTilt = settings.cameraPitch;
    input.mouse = orbitStart;
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); };
    capture("ui_interaction", true);
    for (int step = 1; step <= 8; ++step) {
        input.mouse = {orbitStart.x, orbitStart.y - 20.f * step};
        capture("ui_interaction", true);
    }
    input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); };
    capture("ui_interaction", true);
    require(settings.cameraPitch < -0.2f, "Orbit cannot tilt below the horizon.");
    require(settings.cameraPitch >= renderer::SceneSettings::minPitch - 1e-3f, "Orbit pitch left the valid range.");
    settings.cameraPitch = pitchBeforeTilt;
    capture("ui_interaction", true);
    // F frames the selected Actor by moving the orbit focus onto its mesh center.
    const auto* focused = world.findActor(1);
    require(focused != nullptr, "F test lost the selected sphere.");
    const float focusedPosition[3] = {focused->transform.position[0], focused->transform.position[1],
        focused->transform.position[2]};
    const float targetBeforeFocus[3] = {settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2]};
    const float distanceBeforeFocus = settings.cameraDistance;
    pressKey(ImGuiKey_F);
    require(std::abs(settings.cameraTarget[0] - focusedPosition[0]) < 0.05f &&
            std::abs(settings.cameraTarget[1] - focusedPosition[1]) < 0.05f &&
            std::abs(settings.cameraTarget[2] - focusedPosition[2]) < 0.05f,
        "F did not frame the selected Actor.");
    // Restore the view so the remaining picking assertions are unaffected.
    settings.cameraTarget[0] = targetBeforeFocus[0];
    settings.cameraTarget[1] = targetBeforeFocus[1];
    settings.cameraTarget[2] = targetBeforeFocus[2];
    settings.cameraDistance = distanceBeforeFocus;
    capture("ui_interaction", true);
    const auto meshCount = assets.meshes.size(), materialCount = assets.materials.size();
    const auto actorCount = world.actors.size();
    const auto groundBeforeDelete = *world.findActor(2);
    pressKey(ImGuiKey_Delete);
    require(world.actors.size() + 1 == actorCount && !world.findActor(1) && world.findActor(2),
        "Viewport Delete did not remove only the selected sphere Actor.");
    require(editor.selectedActor() == 0, "Delete left a stale Actor selection.");
    require(assets.meshes.size() == meshCount && assets.materials.size() == materialCount &&
        assets.findMesh(1) && assets.findMaterial(1), "Deleting an Actor removed a shared Mesh or Material asset.");
    require(world.findActor(2)->name == groundBeforeDelete.name &&
        std::memcmp(&world.findActor(2)->transform, &groundBeforeDelete.transform, sizeof(scene::Transform)) == 0,
        "Deleting the selected Actor changed the remaining ground Actor.");
    capture("actor_deleted", true, true);
    require(renderedUiText.find("Select an Actor in the viewport or Outliner.") != std::string::npos,
        "Deleting the selected Actor left stale Details information.");
    // Undo/redo (Godot-style transaction stack) through the real shortcut path:
    // Ctrl+Z restores the deleted Actor, Ctrl+Y removes it again, and a second
    // Ctrl+Z restores it so the follow-up steps start from a known state.
    const auto pressChord = [&](ImGuiKeyChord modifier, ImGuiKey key) {
        input.events = [modifier, key](ImGuiIO& io) { io.AddKeyEvent((ImGuiKey)modifier, true); io.AddKeyEvent(key, true); };
        capture("ui_interaction", true);
        input.events = [modifier, key](ImGuiIO& io) { io.AddKeyEvent(key, false); io.AddKeyEvent((ImGuiKey)modifier, false); };
        capture("ui_interaction", true);
    };
    pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
    require(world.findActor(1) && world.actors.size() == actorCount,
        "Ctrl+Z did not restore the deleted Actor.");
    require(editor.selectedActor() == 1, "Undo did not reselect the restored Actor.");
    capture("instance_details_undo", true, true);
    require(renderedUiText.find("Actor #1") != std::string::npos &&
        renderedUiText.find("Assigned material") != std::string::npos,
        "Undo did not restore the selected instance Details.");
    pressChord(ImGuiMod_Ctrl, ImGuiKey_Y);
    require(!world.findActor(1) && world.actors.size() + 1 == actorCount,
        "Ctrl+Y did not remove the Actor again.");
    pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
    require(world.findActor(1) && world.actors.size() == actorCount,
        "Second Ctrl+Z did not restore the Actor.");
    {
        // Multi-select: Ctrl+click extends the selection, Delete removes every
        // selected Actor, and one undo restores the whole batch.
        const auto ctrlClick = [&](ImVec2 position) {
            input.mouse = position;
            input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); };
            capture("ui_interaction", true);
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("ui_interaction", true);
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
            capture("ui_interaction", true);
            input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); };
            capture("ui_interaction", true);
        };
        click(projectPoint(-.35f, 1.35f, 0));
        require(editor.selection().size() == 1, "A plain click should select exactly one Actor.");
        ctrlClick(projectPoint(-2.4f, 0.0f, 1.2f));
        require(editor.selection().size() == 2, "Ctrl+click did not extend the Actor selection.");
        require(editor.commands().enabled("actor.delete"), "Delete command disabled with a live selection.");
        pressKey(ImGuiKey_Delete);
        require(world.actors.empty(), "Delete did not remove every selected Actor.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
        require(world.actors.size() == actorCount && world.findActor(1) && world.findActor(2),
            "Undo did not restore the whole multi-selection delete.");
    }
    world.createDefaultScene();
    capture("ui_interaction", true);
    click(projectPoint(-.35f, 1.35f, 0));
    require(editor.selectedActor() == 1, "Could not select the restored sphere Actor.");
    {
        // Alt belongs to gizmo duplication on a handle and to camera orbit
        // elsewhere. Exercise real input, including releasing Alt mid-drag.
        pressKey(ImGuiKey_W);
        const auto source = *world.findActor(1);
        const auto baseCount = world.actors.size();
        const auto baseCursor = editor.undoStack().cursor();
        const auto start = projectPoint(.55f, 1, 0);
        const auto end = projectPoint(1.25f, 1, 0);
        const float yaw = settings.cameraYaw, pitch = settings.cameraPitch;
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Alt, true); };
        capture("ui_interaction", true);
        click(start);
        require(world.actors.size() == baseCount && editor.undoStack().cursor() == baseCursor &&
            std::memcmp(&world.findActor(1)->transform, &source.transform, sizeof(scene::Transform)) == 0,
            "Alt-click without dragging created an Actor or edited the original.");
        input.mouse = start;
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
        capture("ui_interaction", true);
        for (int step = 1; step <= 6; ++step) {
            input.mouse = {start.x + (end.x - start.x) * step / 6.f,
                start.y + (end.y - start.y) * step / 6.f};
            if (step == 3) input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Alt, false); };
            capture("ui_interaction", true);
            require(world.actors.size() <= baseCount + 1, "Alt-drag duplicated more than once per gesture.");
        }
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
        capture("alt_drag_single", true);
        const auto copyId = editor.selectedActor();
        require(world.actors.size() == baseCount + 1 && copyId != source.id && world.findActor(copyId),
            "Alt-drag did not create and select a copy.");
        const auto copy = *world.findActor(copyId);
        require(std::memcmp(&world.findActor(1)->transform, &source.transform, sizeof(scene::Transform)) == 0 &&
            copy.mesh.meshId == source.mesh.meshId && copy.mesh.materialId == source.mesh.materialId &&
            copy.visible == source.visible && copy.name == source.name + " Copy" &&
            std::abs(copy.transform.position[0] - source.transform.position[0]) > .1f &&
            std::abs(copy.transform.position[0] - source.transform.position[0]) < 1.0f,
            "Alt-drag changed the original, lost Actor data, or added a fixed duplicate offset.");
        require(editor.undoStack().cursor() == baseCursor + 1 &&
            settings.cameraYaw == yaw && settings.cameraPitch == pitch,
            "Alt-drag split the transaction or orbited the camera.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
        require(world.actors.size() == baseCount && !world.findActor(copyId) &&
            editor.selection() == std::vector<scene::ActorId>{source.id},
            "Undo did not remove the drag-copy and restore the source selection.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Y);
        require(world.findActor(copyId) && editor.selectedActor() == copyId &&
            std::memcmp(&world.findActor(copyId)->transform, &copy.transform, sizeof(scene::Transform)) == 0,
            "Redo did not restore the same drag-copy ID and final transform.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);

        // Select the ground first, then add the sphere as the group pivot.
        click(projectPoint(-2.4f, 0, 1.2f));
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); };
        capture("ui_interaction", true);
        click(projectPoint(-.35f, 1.35f, 0));
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, false); };
        capture("ui_interaction", true);
        const auto sourceSelection = editor.selection();
        const auto sourcePrimary = editor.selectedActor();
        require(sourceSelection.size() == 2 && sourcePrimary == 1, "Alt-drag group setup failed.");
        capture("instance_details_primary", true, true);
        require(renderedUiText.find("2 Actors selected; showing primary instance.") != std::string::npos &&
            renderedUiText.find("Actor #1") != std::string::npos,
            "Multi-selection Details did not identify its primary instance.");
        const auto ground = *world.findActor(2);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Alt, true); };
        capture("ui_interaction", true);
        drag(start, end, "alt_drag_group");
        // Holding Alt after mouse-up must not spawn another batch.
        capture("alt_drag_group", true);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Alt, false); };
        capture("ui_interaction", true);
        const auto groupSelection = editor.selection();
        const auto groupPrimary = editor.selectedActor();
        require(world.actors.size() == baseCount + 2 && groupSelection.size() == 2 &&
            groupPrimary != sourcePrimary && editor.undoStack().cursor() == baseCursor + 1,
            "Alt-drag did not duplicate the group in one transaction.");
        const float groupDelta = world.findActor(groupPrimary)->transform.position[0] - source.transform.position[0];
        require(std::abs(groupDelta) > .1f &&
            std::memcmp(&world.findActor(1)->transform, &source.transform, sizeof(scene::Transform)) == 0 &&
            std::memcmp(&world.findActor(2)->transform, &ground.transform, sizeof(scene::Transform)) == 0,
            "Alt-drag moved the original group or left the copies stationary.");
        for (const auto id : groupSelection) {
            require(id != 1 && id != 2, "Alt-drag kept an original in the copy selection.");
            const auto* actor = world.findActor(id);
            const auto& original = actor->mesh.meshId == source.mesh.meshId ? source : ground;
            require(std::abs(actor->transform.position[0] - original.transform.position[0] - groupDelta) < .001f,
                "Alt-drag copies did not receive the same translation delta.");
        }
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
        require(world.actors.size() == baseCount && editor.selection() == sourceSelection &&
            editor.selectedActor() == sourcePrimary, "Group drag-copy undo lost the original selection.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Y);
        require(world.actors.size() == baseCount + 2 && editor.selection() == groupSelection &&
            editor.selectedActor() == groupPrimary, "Group drag-copy redo lost copy identities or selection.");
        pressChord(ImGuiMod_Ctrl, ImGuiKey_Z);
        click(projectPoint(-.35f, 1.35f, 0));
        std::cout << "PASS: Alt-gizmo single/group duplication, no-op click, one copy per gesture, "
            "early Alt release, original preservation, and atomic undo/redo.\n";
    }
    selectTab("Shader Editor");
    replaceDraft(originalUiSource);
    require(context->IO.WantTextInput && context->ActiveId != 0,
        "Shader source text field did not acquire keyboard input.");
    pressKey(ImGuiKey_Home);
    pressKey(ImGuiKey_Delete);
    require(world.actors.size() == actorCount && world.findActor(1) && editor.selectedActor() == 1,
        "Delete inside Shader Editor incorrectly deleted the selected Actor.");
    replaceDraft(originalUiSource);
    click(buttonRow());
    require(renderer.shaderSource() == originalUiSource,
        "Keyboard isolation smoke test did not restore the project shader draft.");
    // Return to a selected sphere with handles for the final preview, without saving.
    selectTab("Material Editor");
    click(projectPoint(-.35f, 1.35f, 0));
    capture("editor", true);
    {
        // Engine Content is a shared, immutable source; only the new project
        // copy may enter the existing draft/apply/save workflow.
        ImGuiContextScope active(context);
        editor.selectContentFolder("engine://Materials");
        editor.setContentGridView(true);
        capture("engine_content_preroll", true);
        capture("engine_content", true, true);
        const auto entries = editor.visibleContentEntries();
        require(entries.size() == 1 && entries.front()->id == assets::EngineDefaultMaterialId &&
            entries.front()->readOnly(), "Engine Content did not expose the read-only default material.");
        require(editor.navigateContentUp() && editor.contentFolder() == "engine://" &&
            !editor.navigateContentUp(), "Engine Content navigation escaped its mount root.");
        editor.selectContentFolder("engine://Materials");
        editor.showAssetDetails("engine://Materials/DefaultMaterial.rasset");
        capture("engine_asset_details_preroll", true);
        capture("engine_asset_details", true, true);
        require(renderedUiText.find("Engine (read-only)") != std::string::npos,
            "Engine asset details did not explain its source and protection.");
        editor.closeAssetDetails();

        require(editor.commands().enabled("actor.addSphere"), "Default material does not permit creating a sphere.");
        bool created = false;
        for (const auto& command : editor.commands().commands()) if (command.info.id == "actor.addSphere") {
            command.action(); created = true; break;
        }
        const auto* actor = world.findActor(editor.selectedActor());
        require(created && actor && actor->mesh.materialId == assets::EngineDefaultMaterialId,
            "New actors must use the shared engine default, regardless of browser material selection.");
        capture("engine_default_actor", true);
        auto* detailsWindow = findWindow("Details");
        if (detailsWindow->DockNode && detailsWindow->DockNode->SelectedTabId != detailsWindow->TabId)
            click(dockTabCenter(detailsWindow));
        click(itemRect(detailsWindow, detailsWindow->GetID("Edit assigned material")).GetCenter());
        selectTab("Material Editor");
        const auto materialRect = [&](const char* label) {
            const auto* bar = tabBar();
            return itemRect(bar->Window, ImHashStr(label, 0, bar->SelectedTabId));
        };
        capture("engine_material_readonly", true, true);
        saveMaterialCapture("engine_material_readonly_native.bmp");
        require(renderedUiText.find("Live preview") != std::string::npos && renderer.materialPreviewTexture(),
            "A clean read-only engine material must retain its preview sphere.");
        require(renderedUiText.find("Engine - read-only") != std::string::npos,
            "Engine material editor lacks its read-only explanation.");
        click(materialRect("Grid").GetCenter());
        click(materialRect("Apply material").GetCenter());
        require(!editor.materialPreview() && !assets.findMaterial(assets::EngineDefaultMaterialId)->grid,
            "Disabled engine material controls changed the shared material or activated a draft.");
        const auto count = assets.materials.size();
        click(materialRect("Copy to Project").GetCenter());
        require(assets.materials.size() == count + 1 && editor.contentFolder() == "Materials",
            "Copy to Project did not publish and select an independent project material.");
        const auto* copyEntry = editor.assetRegistry().find(editor.selectedContentAsset());
        require(copyEntry && !copyEntry->readOnly() && copyEntry->id != assets::EngineDefaultMaterialId,
            "Material copy kept the engine identity or read-only origin.");
        const auto copiedId = copyEntry->id;
        const auto copyPath = copyEntry->path;
        click(materialRect("Grid").GetCenter());
        require(editor.materialPreview() && editor.materialPreview()->id == copiedId && editor.materialPreview()->grid,
            "Project copy could not use live material editing.");
        click(materialRect("Apply material").GetCenter());
        click(materialRect("Save material assets").GetCenter());
        assets::AssetFile savedCopy;
        assets::MaterialAsset savedMaterial;
        require(assets::readAssetFile(copyPath, savedCopy, error) &&
            assets::readMaterialAssetFile(savedCopy, savedMaterial, error) && savedMaterial.grid,
            "Edited project copy did not persist its own parameters.");
        require(contents(engineMaterialPath) == engineMaterialBytes &&
            !assets.findMaterial(assets::EngineDefaultMaterialId)->grid,
            "Editing/saving a project copy modified the shared engine default.");
        capture("engine_material_copy", true);
        saveMaterialCapture("engine_material_copy_native.bmp");
        std::cout << "PASS: Engine Content navigation, read-only material UI, default Actor assignment, "
            "Copy to Project, editable independent parameters, save and unchanged engine file.\n";
    }
    runDiagnosticsSmoke(renderer, output / "diagnostics",
        contents(project.engineRoot / "Shaders/Materials/Standard.hlsli"));
    {
        // Use the same real widgets and input routing as the user. Keep capture
        // texels independent from the scene being edited throughout this block.
        ImGuiContextScope active(context);
        const auto originalTheme = imgui::currentTheme();
        const auto captureFrame = renderer.frameCapture().frameId;
        const auto diagnosticActorCount = world.actors.size();
        const auto selected = editor.selectedActor();
        const auto undoCount = editor.undoStack().size();
        const char* diagnosticName = "Diagnostics##RestArtEngine.Diagnostics";
        editor.showDiagnostics(editor::DiagnosticsTab::Profiler);
        capture("diagnostics_profiler_preroll", true);
        auto* diagnosticWindow = findWindow(diagnosticName);
        require(diagnosticWindow && editor.diagnosticsOpen(), "Diagnostics Profiler did not open.");
        ImGui::SetWindowPos(diagnosticName, {mainOrigin().x + 40, mainOrigin().y + 100}, ImGuiCond_Always);
        ImGui::SetWindowSize(diagnosticName, {760, 660}, ImGuiCond_Always);
        capture("diagnostics_profiler_layout", true);
        const auto diagnosticControl = [&](const char* label) {
            auto* window = findWindow(diagnosticName);
            const auto* bar = context->TabBars.GetByKey(ImHashStr("Diagnostics tabs", 0, window->ID));
            require(bar && bar->SelectedTabId, "Diagnostics has no selected tab.");
            return itemRect(window, ImHashStr(label, 0, bar->SelectedTabId)).GetCenter();
        };
        click(diagnosticControl("Record CPU trace"));
        capture("diagnostics_recording", true, true);
        require(diagnostics::profiler().isRecording() && renderedUiText.find("Recording") != std::string::npos,
            "Record CPU trace did not begin a visible recording.");
        capture("diagnostics_recording_samples", true);
        click(diagnosticControl("Stop CPU trace"));
        capture("diagnostics_profiler_stopped", true, true);
        require(!diagnostics::profiler().isRecording() && diagnostics::profiler().snapshot().eventCount > 0 &&
            renderedUiText.find("CPU scopes: frame") != std::string::npos,
            "Stopping a recording lost its inspectable CPU events.");
        require(diagnostics::profiler().save(output / "diagnostics/ui-trace.json", error), error);
        const auto recordedTrace = nlohmann::json::parse(contents(output / "diagnostics/ui-trace.json"));
        require(!recordedTrace.at("traceEvents").empty(), "GUI trace export is empty.");

        // A diagnostics window retaining keyboard focus must not send scene
        // shortcuts to the live viewport merely because the mouse is over it.
        ImGui::FocusWindow(diagnosticWindow);
        input.mouse = scenePoint(static_cast<float>(settings.viewportWidth - 12),
            static_cast<float>(settings.viewportHeight) * .4f);
        capture("diagnostics_hover_live", true);
        const auto cameraBefore = settings;
        pressKey(ImGuiKey_F);
        pressKey(ImGuiKey_Delete);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_D, true); };
        capture("diagnostics_shortcut_down", true);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_D, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); };
        capture("diagnostics_shortcut_up", true);
        require(world.actors.size() == diagnosticActorCount && editor.selectedActor() == selected &&
            editor.undoStack().size() == undoCount && settings.cameraDistance == cameraBefore.cameraDistance &&
            std::equal(std::begin(settings.cameraTarget), std::end(settings.cameraTarget),
                std::begin(cameraBefore.cameraTarget)), "Diagnostics focus allowed scene keyboard edits.");

        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            const std::string suffix = theme == imgui::EditorTheme::Black ? "black" : "blue";
            editor.showDiagnostics(editor::DiagnosticsTab::Profiler);
            ImGui::SetWindowSize(diagnosticName, {760, 660}, ImGuiCond_Always);
            ImGui::SetScrollY(diagnosticWindow, 0);
            // ImGui applies a requested tab selection on the following frame.
            capture(("diagnostics_profiler_" + suffix + "_preroll").c_str(), true);
            capture(("diagnostics_profiler_" + suffix).c_str(), true, true);
            require(renderedUiText.find("CPU frame") != std::string::npos &&
                !renderer.diagnosticsFrame().passes.empty(),
                "Profiler has no renderer pass statistics. Recorded UI bytes=" +
                    std::to_string(renderedUiText.size()) + "; actual passes=" +
                    std::to_string(renderer.diagnosticsFrame().passes.size()));
            editor.showDiagnostics(editor::DiagnosticsTab::FrameInspector);
            capture(("diagnostics_frame_" + suffix + "_preroll").c_str(), true);
            capture(("diagnostics_frame_" + suffix).c_str(), true, true);
            require(renderedUiText.find("Captured frame") != std::string::npos &&
                renderedUiText.find("Metadata export contains no images") != std::string::npos,
                "Frame Inspector lacks captured-frame identity or export scope.");
            click(diagnosticControl("Read pixel"));
            capture(("diagnostics_pixel_" + suffix).c_str(), true, true);
            require(renderedUiText.find("RGBA =") != std::string::npos &&
                renderer.frameCapture().frameId == captureFrame,
                "Captured pixel control failed or consumed a new live frame.");
            ImGui::SetWindowSize(diagnosticName, {300, 640}, ImGuiCond_Always);
            ImGui::SetScrollY(diagnosticWindow, 0);
            capture(("diagnostics_narrow_" + suffix).c_str(), true);
            // Navigation scrolls the real control into view before clicking.
            click(diagnosticControl("Read pixel"));
            capture(("diagnostics_narrow_pixel_" + suffix).c_str(), true, true);
            require(renderedUiText.find("RGBA =") != std::string::npos,
                "Narrow Frame Inspector made pixel readback unreachable.");
        }
        require(world.actors.size() == diagnosticActorCount && editor.selectedActor() == selected &&
            editor.undoStack().size() == undoCount, "Captured-image inspection changed scene selection or undo.");
        imgui::setTheme(originalTheme);
        editor.closeDiagnostics();
        capture("diagnostics_closed", true);
        require(!editor.diagnosticsOpen() && !renderer.diagnosticsEnabled(),
            "Closing stopped diagnostics left continuous collection enabled.");
        renderer.clearFrameCapture();
        std::cout << "PASS: Diagnostics Record/Stop, retained events, trace export, frozen Frame Inspector pixel reads, "
            "both themes, 300px window, keyboard ownership and unchanged scene/undo.\n";
    }
    const auto finalMaterialHwnd = static_cast<HWND>(findWindow("Material Editor###MaterialWorkspace")->Viewport->PlatformHandle);
    input.detach();
    ui.shutdown();
    require(!finalMaterialHwnd || !IsWindow(finalMaterialHwnd),
        "UI shutdown left the material platform window alive after releasing its DX11 backend.");
    require(renderer.validationErrorCount() == 0, "D3D11 validation reported an editor error.");
    require(editor.commands().contains("view.toggleOutline") &&
        editor.commands().contains("actor.toggleVisibility"),
        "Editor is missing the outline (G) or visibility (H) shortcut.");
    std::cout << "PASS: Actor transforms, mesh/material assignment, shader Apply, failed compile rollback, "
        "binding validation, shader Save/Reload, scene roundtrip, Shader Editor mouse/text Apply, "
        "viewport picking, translation/rotation/local-scale gizmo drags, camera orbit/pan/focus, "
        "Actor deletion, text-input Delete isolation, native dock drag/merge/split, "
        "viewport reflow and fresh-context layout persistence.\n";
    std::cout << "D3D11 debug layer: " << (renderer.validationEnabled() ? "enabled" : "unavailable")
        << "; validation errors: " << renderer.validationErrorCount() << '\n';
}

