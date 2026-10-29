#include "EditorSmoke.h"
#include "DiagnosticsSmoke.h"
#include "ImportSmoke.h"
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
// Material checks compare a readback of the scene target, before ImGui overlays.
// The asynchronous statistics HUD must not affect material equivalence tests.
std::string scenePixels(const std::filesystem::path& path, const rae::renderer::SceneSettings& settings) {
    if (!path.stem().string().starts_with("gizmo_")) {
        auto rawPath = path;
        rawPath.replace_extension(".scene.rgba");
        require(std::filesystem::exists(rawPath), "Missing raw scene readback: " + rawPath.string());
        return contents(rawPath);
    }
    // This fixture frames the selected Actor at the centre. Compare its Gizmo
    // in the central 60% of the composed view, excluding the statistics HUD.
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
    const int left = std::clamp(settings.viewportX + settings.viewportWidth / 5, 0, width);
    const int right = std::clamp(settings.viewportX + settings.viewportWidth * 4 / 5, left, width);
    const int top = std::clamp(settings.viewportY + settings.viewportHeight / 5, 0, height);
    const int bottom = std::clamp(settings.viewportY + settings.viewportHeight * 4 / 5, top, height);
    require(right > left && bottom > top, "Material preview has no visible scene pixels.");
    std::string pixels;
    for (int y = top; y < bottom; ++y) {
        const auto row = signedHeight < 0 ? y : height - y - 1;
        pixels.append(bmp.data() + offset + (static_cast<std::size_t>(row) * width + left) * 4,
            static_cast<std::size_t>(right - left) * 4);
    }
    return pixels;
}

void saveScenePixels(rae::renderer::SceneRenderer& renderer, const rae::renderer::SceneSettings& settings,
                     const std::filesystem::path& path) {
    using Microsoft::WRL::ComPtr;
    auto* handle = renderer.sceneTexture();
    require(handle && settings.viewportWidth > 0 && settings.viewportHeight > 0, "No scene target for raw readback.");
    auto* view = static_cast<ID3D11ShaderResourceView*>(handle);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source, staging;
    require(SUCCEEDED(resource.As(&source)), "Raw scene target is not a texture.");
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    require(description.Format == DXGI_FORMAT_R8G8B8A8_UNORM && settings.viewportX >= 0 && settings.viewportY >= 0 &&
        settings.viewportX + settings.viewportWidth <= static_cast<int>(description.Width) &&
        settings.viewportY + settings.viewportHeight <= static_cast<int>(description.Height), "Invalid raw scene rectangle/format.");
    description.Width = static_cast<UINT>(settings.viewportWidth);
    description.Height = static_cast<UINT>(settings.viewportHeight);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&description, nullptr, &staging)), "Cannot create raw scene staging target.");
    const D3D11_BOX rectangle{static_cast<UINT>(settings.viewportX), static_cast<UINT>(settings.viewportY), 0,
        static_cast<UINT>(settings.viewportX + settings.viewportWidth), static_cast<UINT>(settings.viewportY + settings.viewportHeight), 1};
    renderer.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source.Get(), 0, &rectangle);
    std::string pixels(static_cast<std::size_t>(description.Width) * description.Height * 4, '\0');
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Cannot read raw scene target.");
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * description.Width * 4,
            static_cast<const char*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(description.Width) * 4);
    renderer.context()->Unmap(staging.Get(), 0);
    auto rawPath = path;
    rawPath.replace_extension(".scene.rgba");
    std::ofstream stream(rawPath, std::ios::binary | std::ios::trunc);
    stream.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    stream.close();
    require(stream.good(), "Could not write raw scene pixels.");
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
    bool usePlatformZOrder = false;
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
            if (input.usePlatformZOrder)
                hovered = ImGui::FindHoveredViewportFromPlatformWindowStack(input.mouse);
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
        require(assets.loadEngineMeshes(project.engineRoot / "Content/Meshes", mountError), mountError);
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
        require(!std::filesystem::exists(v2Root / "Meshes" / "Builtin") &&
            !v2Editor.assetRegistry().find("Meshes/Builtin/Sphere.rasset") &&
            v2Editor.assetRegistry().find("engine://Meshes/Sphere.rasset") &&
            v2Editor.assetRegistry().find("engine://Meshes/Plane.rasset"),
            "Editor startup must mount Engine meshes without creating project Builtin files.");
        assets::AssetQuery v2MaterialQuery;
        v2MaterialQuery.type = assets::AssetType::Material;
        v2MaterialQuery.recursive = true;
        require(v2Editor.world().actors.size() == v2World.actors.size() &&
            v2Editor.assetRegistry().query(v2MaterialQuery).size() == 2 &&
            v2Editor.assets().findMaterial(assets::EngineDefaultMaterialId),
            "The editor did not load a scene v2 with its GUID materials.");
        {
            auto freshProject = v2Project;
            freshProject.defaultScene = v2Root / "Scenes" / "Fresh.rascene";
            rae::editor::SceneEditor freshEditor;
            require(freshEditor.initialize(freshProject, renderer, v2Error), v2Error);
            require(freshEditor.world().findActor(1) && freshEditor.world().findActor(2) &&
                freshEditor.world().findActor(1)->mesh.meshId == assets::EngineSphereMeshId &&
                freshEditor.world().findActor(2)->mesh.meshId == assets::EnginePlaneMeshId &&
                !std::filesystem::exists(v2Root / "Meshes" / "Builtin"),
                "A fresh editor scene must reference Engine sphere/plane without writing project primitives.");
        }
        // Legacy primitive files remain readable when explicitly supplied by an
        // older project; editor startup must no longer generate them.
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
    bool smokeMainSurfaceVisible = true;
    const auto capture = [&](const char* name, bool drawEditor = false, bool recordUiText = false,
                             bool useMaterialPreview = true, bool useOutline = true) {
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
            editor.draw(settings, renderer, vsync, screenshot, smokeMainSurfaceVisible);
            // A viewport tab that was just floated can report a degenerate image
            // rectangle for one frame; retry so captures never record the cleared
            // surface and the pipeline comparison stays deterministic.
            for (int retry = 0; retry < 3 && settings.viewportVisible && settings.viewportWidth < 1; ++retry) {
                ui.endFrame(smokeMainSurfaceVisible);
                if (!smokeMainSurfaceVisible) renderer.releaseRetiredSceneTargets();
                ui.beginFrame(1.0f / 60.0f);
                context = ImGui::GetCurrentContext();
                settings.viewportWidth = settings.viewportHeight = 0;
                settings.editorViewport = false;
                settings.viewportVisible = true;
                editor.draw(settings, renderer, vsync, screenshot, smokeMainSurfaceVisible);
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
                const bool external = viewport->Viewport != ImGui::GetMainViewport();
                if (!editor.quadView() && settings.cameraLens.aspectRatio == 0) require(std::abs(settings.viewportX - (external ? 0.0f : rect.Min.x - origin.x)) <= 1 &&
                    std::abs(settings.viewportY - (external ? 0.0f : rect.Min.y - origin.y)) <= 1 &&
                    std::abs(settings.viewportWidth - rect.GetWidth()) <= 2 &&
                    std::abs(settings.viewportHeight - rect.GetHeight()) <= 2,
                    "Scene renderer rectangle differs from this frame's dock content rectangle.");
                const auto& image = editor.cameraImageRect();
                require(image[0] >= rect.Min.x - 1 && image[1] >= rect.Min.y - 1 &&
                    image[2] <= rect.Max.x + 1 && image[3] <= rect.Max.y + 1 &&
                    std::abs(image[2] - image[0] - settings.viewportWidth) <= 1 &&
                    std::abs(image[3] - image[1] - settings.viewportHeight) <= 1,
                    "Camera image/interaction rectangle escapes the viewport or disagrees with the rendered image.");
                if (!editor.quadView() && external && settings.cameraLens.aspectRatio == 0)
                    require(renderer.sceneTextureWidth() == std::max(256u, static_cast<unsigned>(settings.viewportWidth)) &&
                        renderer.sceneTextureHeight() == std::max(256u, static_cast<unsigned>(settings.viewportHeight)),
                        "External scene target does not match its complete floating image rectangle.");
            }
        }
        auto renderSettings = settings;
        if (!useOutline) { renderSettings.outlineActors = nullptr; renderSettings.outlineActorCount = 0; }
        if (smokeMainSurfaceVisible || settings.viewportVisible)
            require(renderer.render(renderSettings, world, assets, error,
                useMaterialPreview ? editor.materialPreview() : nullptr), error);
        const auto screenshotPath = output / (std::string(name) + ".bmp");
        const std::string_view captureName(name);
        if (drawEditor && (captureName.starts_with("material_") || captureName.starts_with("two_sided_ui_") ||
            captureName.starts_with("cull_front_ui_") || captureName.starts_with("depth_never_ui_")))
            saveScenePixels(renderer, renderSettings, screenshotPath);
        if (drawEditor && settings.viewportVisible && captureName.starts_with("floating_"))
            saveScenePixels(renderer, renderSettings, screenshotPath);
        ui.endFrame(smokeMainSurfaceVisible);
        // A minimized main swap chain is neither rendered nor presented. Native
        // window evidence is captured from each platform target below.
        if (!smokeMainSurfaceVisible) {
            renderer.releaseRetiredSceneTargets();
            return;
        }
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
        editor.selectContentFolder("engine://Meshes");
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
            editor.showAssetDetails("engine://Meshes/Sphere.rasset");
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
        editor.selectContentFolder("engine://Meshes");
        {
            const auto* sceneEntry = editor.assetRegistry().find("Scenes/Default.rascene");
            require(sceneEntry && sceneEntry->thumbnail.valid(),
                "A scene asset did not get a generated thumbnail.");
        }
        editor.selectContentFolder(std::string());
        const auto* cubeEntry = editor.assetRegistry().find("engine://Meshes/Cube.rasset");
        require(cubeEntry && cubeEntry->thumbnail.valid(),
            "The generated built-in mesh thumbnail is missing from the registry.");
        const auto selectedBeforeBrowsing = editor.selectedActor();
        editor.closeAssetDetails();
        editor.selectContentFolder("engine://Meshes");
        require(editor.navigateContentUp() && editor.contentFolder() == "engine://",
            "Content Up did not select the parent directory.");
        require(editor.navigateContentBack() && editor.contentFolder() == "engine://Meshes",
            "Content Back did not restore the previous directory.");
        require(editor.navigateContentForward() && editor.contentFolder() == "engine://",
            "Content Forward did not restore the next directory.");
        editor.selectContentFolder("engine://");
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
        editor.selectContentFolder("engine://Meshes");
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
                editor.selectContentFolder("engine://Meshes");
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
            require(assets::readAssetThumbnail(project.engineRoot / "Content" / "Meshes" / "Cube.rasset",
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
        editor.selectContentFolder("engine://Meshes");
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
            editor.setContentSearch(mesh->name);
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
        clickMesh(assets::EngineCubeMeshId);
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
        clickMesh(assets::EnginePlaneMeshId);
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

    const auto savePlatformCapture = [&](const ImGuiWindow* window, const char* filename) {
        // Redraw this real platform viewport and read its swapchain before
        // Present: DISCARD swapchains do not guarantee post-Present contents.
        // No private backend structs or desktop occlusion are involved.
        ImGuiContextScope active(context);
        require(window && window->Active && window->Viewport != context->Viewports[0] && window->Viewport->DrawData,
            "Native platform capture has no submitted window UI.");
        auto& platform = context->PlatformIO;
        require(platform.Renderer_RenderWindow && platform.Renderer_SwapBuffers,
            "Native platform capture has no platform render/present callbacks.");
        platform.Renderer_RenderWindow(window->Viewport, nullptr);
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11RenderTargetView> target;
        renderer.context()->OMGetRenderTargets(1, target.GetAddressOf(), nullptr);
        require(target != nullptr, "Native platform capture has no DX11 render target.");
        ComPtr<ID3D11Resource> resource;
        target->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        require(SUCCEEDED(resource.As(&texture)), "Native platform target is not a 2D texture.");
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        require(desc.Width == static_cast<UINT>(window->Viewport->Size.x) &&
            desc.Height == static_cast<UINT>(window->Viewport->Size.y) && desc.SampleDesc.Count == 1,
            "Native platform target dimensions do not match its independent viewport.");
        require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM,
            "Native platform capture has an unsupported pixel format.");
        const auto format = desc.Format;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())),
            "Could not allocate the native platform capture readback texture.");
        std::vector<unsigned char> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        renderer.context()->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
            "Could not map the native platform capture surface.");
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
            "Native platform capture exceeds the BMP file size limit.");
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
        require(static_cast<bool>(image), "Could not write the native platform GPU UI capture.");
        platform.Renderer_SwapBuffers(window->Viewport, nullptr);
    };

    const auto saveMaterialCapture = [&](const char* filename) {
        const auto* window = findWindow("Material Editor###MaterialWorkspace");
        require(window && window->Active, "Material workspace is not submitted for capture.");
        const ImGuiViewport* lastRendered = nullptr;
        for (int index = 1; index < context->PlatformIO.Viewports.Size; ++index) {
            const auto* viewport = context->PlatformIO.Viewports[index];
            if (!(viewport->Flags & ImGuiViewportFlags_IsMinimized)) lastRendered = viewport;
        }
        require(lastRendered == window->Viewport,
            "The material workspace was not the last platform target rendered for capture.");
        savePlatformCapture(window, filename);
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
        const auto selectMaterialOption = [&](const char* control, const char* option) {
            click(materialRect(control).GetCenter());
            require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
                std::string("Material combo did not open: ") + control);
            auto* popup = context->OpenPopupStack.back().Window;
            click(itemRect(popup, ImHashStr(option, 0, popup->ID)).GetCenter());
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
            settings.hardwareRayTracing = false;
            settings.shadowMethod = renderer::ShadowMethod::Disabled;
            require(!assets.findMaterial(materialId)->twoSided,
                "Two-sided UI fixture expected the new default single-sided material.");
            capture("two_sided_ui_before", true);
            const auto single = scenePixels(output / "two_sided_ui_before.bmp", settings);
            selectMaterialOption("Cull", "Off (two-sided)");
            require(editor.materialPreview() && editor.materialPreview()->twoSided &&
                !assets.findMaterial(materialId)->twoSided && renderer.shaderRevision() == shaderVersion,
                "Cull Off did not preview independently without committing or recompiling.");
            capture("two_sided_ui_preview", true);
            const auto doubleSided = scenePixels(output / "two_sided_ui_preview.bmp", settings);
            require(single != doubleSided, "Cull Off did not immediately reveal the plane's back face.");
            click(materialRect("Discard draft").GetCenter());
            capture("two_sided_ui_discarded", true);
            require(!editor.materialPreview() &&
                scenePixels(output / "two_sided_ui_discarded.bmp", settings) == single,
                "Discard failed to restore single-sided visibility.");
            selectMaterialOption("Cull", "Off (two-sided)");
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
            selectMaterialOption("Cull", "Back");
            require(editor.materialPreview() && !editor.materialPreview()->twoSided,
                "Two-sided picking fixture did not preview single-sided visibility.");
            click(backFacePoint);
            require(editor.selectedActor() == 0, "The culled single-sided back face still blocked viewport picking.");
            selectMaterialOption("Cull", "Off (two-sided)");
            require(editor.materialPreview() && editor.materialPreview()->twoSided,
                "Two-sided picking fixture did not restore its double-sided preview.");
            click(backFacePoint);
            require(editor.selectedActor() == planeActorId,
                "A visible two-sided preview back face could not be selected.");
            selectMaterialOption("Cull", "Front");
            require(editor.materialPreview() &&
                editor.materialPreview()->effectiveCullMode() == assets::MaterialCullMode::Front &&
                assets.findMaterial(materialId)->effectiveCullMode() == assets::MaterialCullMode::Off,
                "Cull Front did not create an independent live draft.");
            capture("cull_front_ui_preview", true);
            require(scenePixels(output / "cull_front_ui_preview.bmp", settings) != single,
                "Cull Front did not retain the plane's back face.");
            click(materialRect("Discard draft").GetCenter());
            selectMaterialOption("ZTest", "Never");
            require(editor.materialPreview() && editor.materialPreview()->depthTest == assets::MaterialDepthTest::Never &&
                assets.findMaterial(materialId)->depthTest == assets::MaterialDepthTest::Less,
                "ZTest did not preview independently from the applied material.");
            capture("depth_never_ui_preview", true);
            require(scenePixels(output / "depth_never_ui_preview.bmp", settings) != doubleSided,
                "ZTest Never did not change the rendered plane during preview.");
            click(materialRect("Discard draft").GetCenter());
            require(!editor.materialPreview() && assets.findMaterial(materialId)->depthTest == assets::MaterialDepthTest::Less,
                "Discard did not restore the applied depth test.");
            selectMaterialOption("Cull", "Front");
            selectMaterialOption("ZTest", "Always");
            click(materialRect("ZWrite").GetCenter());
            require(editor.materialPreview() && !editor.materialPreview()->depthWrite &&
                editor.materialPreview()->depthTest == assets::MaterialDepthTest::Always &&
                assets.findMaterial(materialId)->depthWrite && renderer.shaderRevision() == shaderVersion,
                "ZWrite/ZTest draft committed or recompiled before Apply.");
            click(materialRect("Apply material").GetCenter());
            require(!editor.materialPreview() &&
                assets.findMaterial(materialId)->effectiveCullMode() == assets::MaterialCullMode::Front &&
                assets.findMaterial(materialId)->depthTest == assets::MaterialDepthTest::Always &&
                !assets.findMaterial(materialId)->depthWrite, "Apply did not commit the complete render state.");
            require(assets.saveMaterials(savedFolder, error), error);
            require(restoredAssets.loadMaterials(savedFolder, error), error);
            require(restoredAssets.findMaterial(materialId)->effectiveCullMode() == assets::MaterialCullMode::Front &&
                restoredAssets.findMaterial(materialId)->depthTest == assets::MaterialDepthTest::Always &&
                !restoredAssets.findMaterial(materialId)->depthWrite,
                "UI-applied Cull/ZTest/ZWrite did not survive isolated persistence.");
            require(contents(project.defaultScene) == sceneOnDisk && contents(materialPath) == materialOnDisk &&
                renderer.shaderRevision() == shaderVersion,
                "Two-sided editing changed real project files or recompiled shaders.");
            *assets.findMaterial(materialId) = committed;
            click(materialRect("Discard draft").GetCenter());
            world = originalWorld; settings = originalSettings;
            capture("material_editor", true);
            std::cout << "PASS: Cull Off/Front/Back and ZTest/ZWrite live controls, independent drafts, "
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
        const auto camera = rae::camera::evaluate(renderer::resolveCamera(settings),
            static_cast<float>(settings.viewportWidth) / settings.viewportHeight);
        XMFLOAT4X4 viewValues{}, projectionValues{};
        std::memcpy(&viewValues, camera.view.data(), sizeof(viewValues));
        std::memcpy(&projectionValues, camera.projection.data(), sizeof(projectionValues));
        const auto view = XMLoadFloat4x4(&viewValues), projection = XMLoadFloat4x4(&projectionValues);
        const auto& image = editor.cameraImageRect();
        const auto position = XMVector3Project(XMVectorSet(x, y, z, 1),
            image[0], image[1],
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
            // The smoke clock advances by fixed 1/60 s frames, regardless of
            // screenshot I/O time. Repeating a drag from the same tile can
            // otherwise become a double-click and correctly open Asset Details
            // instead of starting the intended independent drag gesture.
            const double nextSingleClick = context->IO.MouseClickedTime[0] + context->IO.MouseDoubleClickTime;
            for (int settle = 0; context->Time <= nextSingleClick && settle < 60; ++settle)
                capture("content_drag_single_click_settle", true);
            require(context->Time > nextSingleClick && !context->IO.MouseDown[0],
                "Content drag fixture did not settle between independent click gestures.");
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("content_drag_source_press", true);
            require(!context->IO.MouseDoubleClicked[0], "Content drag fixture accidentally generated a double-click.");
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
        checkPlacedMesh("engine://Meshes/Cube.rasset", true, assets::EngineCubeMeshId, .7f, 1.6f, .4f, "content_drop_grid_surface");
        world.actors.clear();
        capture("content_drag_ground_fixture", true);
        checkPlacedMesh("engine://Meshes/Sphere.rasset", false, assets::EngineSphereMeshId, -.8f, 0, .65f, "content_drop_list_ground");
        const std::pair<const char*, assets::AssetId> engineMeshes[] = {
            {"Cube", assets::EngineCubeMeshId}, {"Sphere", assets::EngineSphereMeshId},
            {"Cylinder", assets::EngineCylinderMeshId}, {"Cone", assets::EngineConeMeshId},
            {"Torus", assets::EngineTorusMeshId}, {"Plane", assets::EnginePlaneMeshId}};
        for (const auto& [name, id] : engineMeshes) {
            const auto path = std::string("engine://Meshes/") + name + ".rasset";
            const auto prefix = std::string("engine_mesh_drop_") + name;
            require(assets.findMesh(id) && assets.findMesh(id)->origin == assets::AssetOrigin::Engine,
                "Engine mesh is not mounted in the editor.");
            checkPlacedMesh(path.c_str(), true, id, -.8f, 0, .65f, prefix.c_str());
        }
        std::cout << "PASS: six Engine Content StaticMeshes, real tile drag placement and undo/redo.\n";
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
        const auto unsupportedEntry = std::find_if(editor.assetRegistry().entries().begin(),
            editor.assetRegistry().entries().end(), [](const auto& entry) { return entry.type == assets::AssetType::Scene; });
        require(unsupportedEntry != editor.assetRegistry().entries().end(), "No Scene exists for the unsupported drop test.");
        const std::string unsupportedPath = unsupportedEntry->relativePath;
        for (int scenario = 0; scenario < 4; ++scenario) {
            const auto count = world.actors.size(), historySize = editor.undoStack().size(), history = editor.undoStack().cursor();
            const auto selected = editor.selectedActor();
            const auto camera = settings;
            beginAssetDrag(scenario == 3 ? unsupportedPath : "engine://Meshes/Cube.rasset", scenario % 2 == 0);
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
                scenario == 2 ? "content_drop_toolbar" : "content_drop_scene_rejected");
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

        {
            // Materials use the same real source widgets, but assign only the
            // hit instance. A separate fixture keeps empty-space and occlusion
            // assertions independent from the default scene's ground plane.
            const auto savedPreferences = imgui::uiPreferences();
            imgui::uiPreferences().selectionOutline = false;
            settings.cameraYaw = 0; settings.cameraPitch = .2f; settings.cameraDistance = 9;
            settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1; settings.cameraTarget[2] = 0;
            world.actors.clear();
            const auto peer = world.createActor("Material drop selected peer", 1, 2);
            world.findActor(peer)->transform.position[0] = -1.3f;
            world.findActor(peer)->transform.position[1] = 1;
            const auto target = world.createActor("Material drop target", 1, 1);
            world.findActor(target)->transform.position[0] = 1.3f;
            world.findActor(target)->transform.position[1] = 1;
            // Behind the target along the exact center camera ray, so the
            // frontmost visible triangle must win the material assignment.
            const auto occluded = world.createActor("Material drop occluded peer", 1, 2);
            world.findActor(occluded)->transform.position[0] = 1.3f * 1.3f;
            world.findActor(occluded)->transform.position[1] = 1 - .3f * 9 * std::sin(.2f);
            world.findActor(occluded)->transform.position[2] = -.3f * 9 * std::cos(.2f);
            for (auto& actor : world.actors)
                for (float& scale : actor.transform.scale) scale = .65f;
            capture("material_drop_fixture", true);
            click(projectPoint(-1.3f, 1, 0));
            require(editor.selectedActor() == peer, "Material drop fixture could not select the unaffected peer.");
            const auto selected = editor.selectedActor();
            const auto selection = editor.selection();
            const auto actorCount = world.actors.size();
            const auto camera = settings;
            const auto projectEntry = std::find_if(editor.assetRegistry().entries().begin(),
                editor.assetRegistry().entries().end(), [&](const auto& entry) {
                    return entry.type == assets::AssetType::Material && entry.origin == assets::AssetOrigin::Project &&
                        entry.path.extension() == ".rasset" && assets.findMaterial(entry.id) != nullptr;
                });
            require(projectEntry != editor.assetRegistry().entries().end(), "No loaded project material exists for drag assignment.");
            const auto projectMaterial = projectEntry->id;
            const auto projectMaterialPath = projectEntry->relativePath;
            const std::string engineDragPath = "engine://Materials/DefaultMaterial.rasset";
            const auto checkOutline = [&](scene::ActorId hovered) {
                require(settings.outlineActorCount == (hovered ? 1u : 0u) &&
                    (!hovered || (settings.outlineActors && settings.outlineActors[0] == hovered)),
                    "Material hover outline did not contain exactly the current target.");
                require(!imgui::uiPreferences().selectionOutline && editor.selectedActor() == selected &&
                    editor.selection() == selection, "Material hover changed selection or its outline preference.");
            };
            const auto checkInstances = [&](assets::AssetId targetMaterial) {
                require(world.actors.size() == actorCount && world.findActor(target)->mesh.materialId == targetMaterial &&
                    world.findActor(peer)->mesh.materialId == 2 && world.findActor(occluded)->mesh.materialId == 2 &&
                    editor.selectedActor() == selected && editor.selection() == selection,
                    "Material drop changed another shared-mesh instance, Actor count or selection.");
                cameraUnchanged(camera);
            };
            const auto checkMaterialDrop = [&](const std::string& path, bool grid, assets::AssetId material,
                                                const char* prefix) {
                const auto beforeMaterial = world.findActor(target)->mesh.materialId;
                const auto history = editor.undoStack().cursor();
                const auto historySize = editor.undoStack().size();
                beginAssetDrag(path, grid);
                holdAssetAt(projectPoint(-1.3f, 1, 0), "material_drop_peer_hover");
                checkOutline(peer);
                holdAssetAt(projectPoint(1.3f, 1, 0), (std::string(prefix) + "_hover").c_str());
                checkOutline(target);
                checkInstances(beforeMaterial);
                require(editor.undoStack().cursor() == history && editor.undoStack().size() == historySize,
                    "Hovering Material mutated the instance or its history before release.");
                // Same stationary drag and scene, with only the render-call
                // outline span suppressed. This proves the actual GPU outline
                // changes scene pixels, not just a label or drag tooltip.
                const auto withoutOutline = std::string(prefix) + "_without_outline";
                const auto withOutline = std::string(prefix) + "_with_outline";
                capture(withoutOutline.c_str(), true, false, true, false);
                capture(withOutline.c_str(), true);
                require(scenePixels(output / (withoutOutline + ".bmp"), settings) !=
                    scenePixels(output / (withOutline + ".bmp"), settings),
                    "Material hover target produced no visible GPU outline while selection outline was disabled.");
                releaseAsset((std::string(prefix) + "_assigned").c_str());
                checkOutline(0);
                checkInstances(material);
                require(editor.undoStack().cursor() == history + 1 && editor.undoStack().undoLabel() == "Assign Material",
                    "Material release did not create exactly one Assign Material transaction.");
                const auto sceneFile = output / (std::string(prefix) + ".rascene");
                require(world.save(sceneFile, assets, error), error);
                auto reloadedAssets = assets;
                scene::World reloadedWorld;
                require(reloadedWorld.load(sceneFile, reloadedAssets, error) && reloadedWorld.findActor(target) &&
                    reloadedWorld.findActor(target)->mesh.materialId == material &&
                    reloadedWorld.findActor(peer)->mesh.materialId == 2,
                    "Dropped material reference did not survive isolated scene save/reload.");
                require(editor.undo() && editor.undoStack().cursor() == history,
                    "One Undo did not reverse material assignment.");
                checkInstances(beforeMaterial);
                require(editor.redo() && editor.undoStack().cursor() == history + 1,
                    "Redo did not restore material assignment.");
                checkInstances(material);
                const auto noOpHistory = editor.undoStack().cursor(), noOpSize = editor.undoStack().size();
                beginAssetDrag(path, grid);
                holdAssetAt(projectPoint(1.3f, 1, 0), "material_drop_same_hover");
                checkOutline(target);
                releaseAsset("material_drop_same_noop");
                checkOutline(0);
                checkInstances(material);
                require(editor.undoStack().cursor() == noOpHistory && editor.undoStack().size() == noOpSize,
                    "Assigning the same material added an empty undo transaction.");
                require(editor.undo(), "Could not restore the fixture after material drop.");
                checkInstances(beforeMaterial);
            };
            checkMaterialDrop(engineDragPath, true, assets::EngineDefaultMaterialId, "material_drop_engine_grid");
            world.findActor(target)->mesh.materialId = assets::EngineDefaultMaterialId;
            checkMaterialDrop(projectMaterialPath, false, projectMaterial, "material_drop_project_list");

            for (int scenario = 0; scenario < 4; ++scenario) {
                const auto beforeMaterial = world.findActor(target)->mesh.materialId;
                const auto history = editor.undoStack().cursor(), historySize = editor.undoStack().size();
                beginAssetDrag(projectMaterialPath, scenario % 2 == 0);
                holdAssetAt(projectPoint(1.3f, 1, 0), "material_drop_cancel_target");
                checkOutline(target);
                auto* viewport = findWindow("Scene Viewport##Interaction");
                if (scenario == 0) {
                    pressKey(ImGuiKey_Escape);
                    require(!context->DragDropActive, "Escape retained the Material payload.");
                    checkOutline(0);
                    input.mouse.x += 12;
                    capture("material_drop_escape_still_held", true);
                    require(!context->DragDropActive, "Canceled Material payload restarted while the mouse was held.");
                } else {
                    const auto destination = scenario == 1 ? ImVec2(viewport->WorkRect.Min.x + 12, viewport->WorkRect.Min.y + 50) :
                        scenario == 2 ? itemRect(viewport, viewport->GetID("Move [W]")).GetCenter() :
                        ImVec2(mainOrigin().x - 60, mainOrigin().y - 60);
                    holdAssetAt(destination, scenario == 1 ? "material_drop_empty_hover" :
                        scenario == 2 ? "material_drop_toolbar_hover" : "material_drop_outside_hover");
                    checkOutline(0);
                }
                releaseAsset(scenario == 0 ? "material_drop_escape" : scenario == 1 ? "material_drop_empty" :
                    scenario == 2 ? "material_drop_toolbar" : "material_drop_outside");
                checkOutline(0);
                checkInstances(beforeMaterial);
                require(editor.undoStack().cursor() == history && editor.undoStack().size() == historySize,
                    "Canceled or invalid Material drop changed undo history.");
            }
            {
                // Real imported split mesh: the same Actor has independently
                // assigned slots, and the hit face selects the drag destination.
                const auto source = output / "material-slots-ui.obj";
                const auto writeSlotSource = [&](bool reversed) {
                    std::ofstream stream(source, std::ios::binary | std::ios::trunc);
                    stream << "v -1.4 .25 0\nv -.1 .25 0\nv -.1 1.75 0\nv -1.4 1.75 0\n"
                        "v .1 .25 0\nv 1.4 .25 0\nv 1.4 1.75 0\nv .1 1.75 0\n"
                        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
                    const char* left = "usemtl Left\nf 1/1 2/2 3/3\nf 1/1 3/3 4/4\n";
                    const char* right = "usemtl Right\nf 5/1 6/2 7/3\nf 5/1 7/3 8/4\n";
                    stream << (reversed ? right : left) << (reversed ? left : right);
                    stream.close(); require(stream.good(), "Could not write slot UI source fixture.");
                };
                writeSlotSource(false);
                assets::AssetId splitMeshId = 0;
                require(assets.importObj(source, splitMeshId, error), error);
                auto* splitMesh = assets.findMesh(splitMeshId);
                require(splitMesh && splitMesh->materialSlots.size() == 2 && splitMesh->sections.size() == 2,
                    "OBJ UI fixture did not retain its two source material slots.");
                splitMesh->name = "MaterialSlotsUi";
                for (unsigned channel = 1; channel < assets::MaxMeshUvChannels; ++channel) {
                    splitMesh->uvChannels[channel] = splitMesh->uvChannels[0];
                    splitMesh->uvNames[channel] = "UV " + std::to_string(channel);
                }
                splitMesh->markGeometryModified();
                const auto splitFile = project.contentRoot / "Meshes" / "MaterialSlotsUi.rasset";
                std::filesystem::create_directories(splitFile.parent_path());
                require(assets::writeAssetFile(splitFile, assets::makeMeshAssetFile(*splitMesh), error), error);
                assets.rememberMeshPersisted(splitMeshId, splitFile);
                auto* contentWindow = findWindow("Content & Editors");
                click(itemRect(contentWindow, contentWindow->GetID("Refresh")).GetCenter());
                const auto splitPath = splitFile.lexically_relative(project.contentRoot).generic_string();
                editor.showAssetDetails(splitPath);
                capture("material_slots_asset_details_preroll", true);
                capture("material_slots_asset_details", true, true);
                const auto metadataWindow = [&]() -> ImGuiWindow* {
                    for (auto* window : context->Windows)
                        if (window->Active && std::strstr(window->Name, "##asset-metadata") &&
                            window->ParentWindow && window->ParentWindow->ID == ImHashStr("###AssetDetails")) return window;
                    return nullptr;
                };
                auto* metadata = metadataWindow();
                require(renderedUiText.find("Material slots: 2") != std::string::npos &&
                    renderedUiText.find("UV3:") != std::string::npos && metadata &&
                    metadata->ScrollMax.y <= 1.0f && metadata->InnerRect.GetHeight() >= metadata->ContentSize.y,
                    "Mesh Asset Details did not visibly fit its two slots and four UV channels.");
                const auto savedSlots = splitMesh->materialSlots;
                for (std::size_t slot = 2; slot < 25; ++slot)
                    splitMesh->materialSlots.push_back({"Extra slot " + std::to_string(slot), "test:" + std::to_string(slot), 0});
                splitMesh->markGeometryModified();
                capture("material_slots_many_preroll", true);
                capture("material_slots_many_top", true, true);
                metadata = metadataWindow();
                require(metadata && metadata->ScrollMax.y > 100, "A long mesh slot list has no scrollable metadata region.");
                ImGui::SetScrollY(metadata, metadata->ScrollMax.y);
                capture("material_slots_many_scroll_preroll", true);
                capture("material_slots_many_bottom", true, true);
                require(metadata->Scroll.y > 100 && std::abs(metadata->Scroll.y - metadata->ScrollMax.y) < 1,
                    "The end of a long mesh slot list could not be reached.");
                splitMesh->materialSlots = savedSlots;
                splitMesh->markGeometryModified();
                editor.closeAssetDetails();
                world.actors.clear();
                settings.hardwareRayTracing = false; settings.rayTracing = false;
                settings.shadowMethod = renderer::ShadowMethod::Disabled; settings.debugView = 1;
                const auto splitActor = world.createActor("Two slot instance", splitMeshId, 1);
                const auto splitPeer = world.createActor("Unchanged slot peer", splitMeshId, 2);
                world.findActor(splitPeer)->visible = false;
                capture("material_slots_fixture", true);
                click(projectPoint(-.7f, 1, 0));
                require(editor.selectedActor() == splitActor, "Could not select a multi-slot mesh through its visible face.");
                const auto slotControl = [&](int slot, const char* label) {
                    auto* details = findWindow("Details");
                    const auto slotId = ImHashData(&slot, sizeof(slot), details->ID);
                    return itemRect(details, ImHashStr(label, 0, slotId));
                };
                const auto chooseSlot = [&](int slot, assets::AssetId materialId) {
                    click(slotControl(slot, "##SlotMaterial").GetCenter());
                    require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
                        "A material slot combo did not open.");
                    auto* popup = context->OpenPopupStack.back().Window;
                    const auto* material = assets.findMaterial(materialId);
                    const auto label = material->name + (material->origin == assets::AssetOrigin::Engine ? " (Engine)" : "") +
                        "##" + std::to_string(materialId);
                    click(itemRect(popup, ImHashStr(label.c_str(), 0, popup->ID)).GetCenter());
                };
                const auto slotMaterial = [&](std::size_t slot) {
                    return world.findActor(splitActor)->mesh.materialForSlot(*assets.findMesh(splitMeshId), slot);
                };
                const auto slotHistory = editor.undoStack().cursor();
                chooseSlot(0, 2);
                require(slotMaterial(0) == 2 && slotMaterial(1) == 1 &&
                    editor.undoStack().cursor() == slotHistory + 1, "Slot dropdown changed a peer slot or omitted undo.");
                capture("material_slots_details_override", true, true);
                (void)slotControl(1, "##SlotMaterial");
                capture("material_slots_details_second", true, true);
                click(slotControl(0, "Reset").GetCenter());
                require(slotMaterial(0) == 1 && world.findActor(splitActor)->mesh.materialOverrides.empty(),
                    "Slot Reset did not restore inheritance.");
                require(editor.undo() && slotMaterial(0) == 2, "Slot Reset undo lost the explicit binding.");
                require(editor.redo() && slotMaterial(0) == 1, "Slot Reset redo did not restore inheritance.");
                chooseSlot(0, 2);
                capture("material_slots_before_drop", true);
                const auto slotBefore = scenePixels(output / "material_slots_before_drop.bmp", settings);
                const auto beforeDropHistory = editor.undoStack().cursor();
                beginAssetDrag(engineDragPath, true);
                holdAssetAt(projectPoint(.7f, 1, 0), "material_slots_right_hover");
                require(slotMaterial(0) == 2 && slotMaterial(1) == 1,
                    "Slot drag hover changed material bindings before delivery.");
                releaseAsset("material_slots_right_assigned");
                require(slotMaterial(0) == 2 && slotMaterial(1) == assets::EngineDefaultMaterialId &&
                    world.findActor(splitPeer)->mesh.materialOverrides.empty() &&
                    editor.undoStack().cursor() == beforeDropHistory + 1,
                    "Material drop did not assign only the hit slot of the hit instance.");
                require(scenePixels(output / "material_slots_right_assigned.bmp", settings) != slotBefore,
                    "Slot material assignment did not change rendered pixels.");
                require(editor.undo() && slotMaterial(1) == 1 && slotMaterial(0) == 2,
                    "Slot material drag undo changed the other slot.");
                require(editor.redo() && slotMaterial(1) == assets::EngineDefaultMaterialId,
                    "Slot material drag redo did not restore its binding.");
                const auto slotScene = output / "material-slots-ui.rascene";
                require(world.save(slotScene, assets, error), error);
                auto reloadedAssets = assets;
                scene::World reloaded;
                require(reloaded.load(slotScene, reloadedAssets, error) &&
                    reloaded.findActor(splitActor)->mesh.materialOverrides == world.findActor(splitActor)->mesh.materialOverrides,
                    "Multiple instance material bindings did not survive scene save/reload.");
                writeSlotSource(true);
                require(editor.reimportContentAsset(splitPath, source, error), error);
                editor.closeImportResults();
                splitMesh = assets.findMesh(splitMeshId);
                require(splitMesh->materialSlots[0].sourceKey == "obj:Right" &&
                    splitMesh->materialSlots[1].sourceKey == "obj:Left" &&
                    slotMaterial(0) == assets::EngineDefaultMaterialId && slotMaterial(1) == 2,
                    "Reimport slot reordering attached an instance material to the wrong faces.");
                require(editor.undoStack().size() == 0, "Reordered slots retained unsafe index-based undo history.");
                auto savedAssets = assets;
                scene::World savedBeforeReimport;
                require(savedBeforeReimport.load(slotScene, savedAssets, error) &&
                    savedBeforeReimport.findActor(splitActor)->mesh.materialForSlot(*savedAssets.findMesh(splitMeshId), 0) ==
                        assets::EngineDefaultMaterialId &&
                    savedBeforeReimport.findActor(splitActor)->mesh.materialForSlot(*savedAssets.findMesh(splitMeshId), 1) == 2,
                    "A saved scene did not resolve its slot keys after source reordering.");
                capture("material_slots_after_reimport", true, true);
                std::cout << "PASS: Multi-slot Details assign/reset, actual face-targeted drag, GPU feedback, "
                    "instance isolation, undo/redo, scene persistence and stable-key reimport.\n";
            }
            imgui::uiPreferences() = savedPreferences;
            world = savedWorld;
            settings = savedSettings;
            capture("material_drop_scene_restored", true);
            click(projectPoint(-.35f, 1.35f, 0));
            require(editor.selectedActor() == 1, "Material drag tests did not restore the selected scene Actor.");
            std::cout << "PASS: Engine Grid/Project List Material assignment, target-only GPU hover outline, "
                "shared-mesh and nearest-hit isolation, scene persistence, undo/redo/no-op, cancellation.\n";
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

        require(editor.commands().enabled("actor.addCube"), "Engine Content does not permit creating a cube.");
        bool cubeCreated = false;
        const auto countBeforeCube = world.actors.size();
        for (const auto& command : editor.commands().commands()) if (command.info.id == "actor.addCube") {
            command.action(); cubeCreated = true; break;
        }
        const auto* cubeActor = world.findActor(editor.selectedActor());
        require(cubeCreated && cubeActor && cubeActor->mesh.meshId == assets::EngineCubeMeshId &&
            cubeActor->mesh.materialId == assets::EngineDefaultMaterialId &&
            world.actors.size() == countBeforeCube + 1,
            "New cube actors must use Engine geometry and default material.");
        require(editor.undo() && world.actors.size() == countBeforeCube,
            "Undo did not restore the scene after creating an Engine cube.");
        require(editor.commands().enabled("actor.addSphere"), "Default material does not permit creating a sphere.");
        bool created = false;
        for (const auto& command : editor.commands().commands()) if (command.info.id == "actor.addSphere") {
            command.action(); created = true; break;
        }
        const auto* actor = world.findActor(editor.selectedActor());
        require(created && actor && actor->mesh.meshId == assets::EngineSphereMeshId &&
            actor->mesh.materialId == assets::EngineDefaultMaterialId,
            "New sphere actors must use Engine geometry and default material, regardless of browser selection.");
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
        require(assets.saveMaterials(project.contentRoot / "Materials", error) &&
            assets.saveMeshes(project.contentRoot, error), "Publish earlier material UI fixtures: " + error);
        const auto saveSceneCommand = [&] {
            for (const auto& command : editor.commands().commands())
                if (command.info.id == "scene.save") { command.action(); return; }
            require(false, "Scene save command is missing.");
        };
        saveSceneCommand();
        capture("material_scene_initially_saved", true, true);
        require(renderedUiText.find("Scene *") == std::string::npos, "Material test scene was not saved.");
        click(materialRect("Grid").GetCenter());
        require(editor.materialPreview() && editor.materialPreview()->id == copiedId && editor.materialPreview()->grid,
            "Project copy could not use live material editing.");
        click(materialRect("Apply material").GetCenter());
        capture("material_applied_without_scene_dirty", true, true);
        require(renderedUiText.find("Scene *") == std::string::npos &&
            renderedUiText.find("Material assets have unsaved changes.") != std::string::npos,
            "Applying a material dirtied the scene or lost the asset's unsaved state.");
        const auto copyBeforeSceneSave = contents(copyPath);
        const auto copyTimeBeforeSceneSave = std::filesystem::last_write_time(copyPath);
        saveSceneCommand();
        capture("material_dirty_after_scene_save", true, true);
        require(contents(copyPath) == copyBeforeSceneSave &&
            std::filesystem::last_write_time(copyPath) == copyTimeBeforeSceneSave &&
            renderedUiText.find("Material assets have unsaved changes.") != std::string::npos,
            "Saving the scene wrote the applied material or cleared its unsaved warning.");
        const auto sceneBeforeMaterialSave = contents(editor.scenePath());
        const auto sceneTimeBeforeMaterialSave = std::filesystem::last_write_time(editor.scenePath());
        click(materialRect("Save material asset").GetCenter());
        require(contents(editor.scenePath()) == sceneBeforeMaterialSave &&
            std::filesystem::last_write_time(editor.scenePath()) == sceneTimeBeforeMaterialSave,
            "Saving one material also wrote the scene.");
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
        // Two independently applied materials must keep separate dirty states.
        const auto materialPathFor = [&](assets::AssetId id) {
            for (const auto& entry : editor.assetRegistry().entries())
                if (entry.type == assets::AssetType::Material && entry.id == id) return entry.path;
            throw std::runtime_error("New material was not published in Content.");
        };
        const auto sceneBeforeNewMaterials = contents(editor.scenePath());
        click(materialRect("New material").GetCenter());
        const auto firstNew = assets.materials.back().id;
        const auto firstNewPath = materialPathFor(firstNew);
        require(std::filesystem::is_regular_file(firstNewPath), "New material did not publish its asset immediately.");
        click(materialRect("Grid").GetCenter());
        click(materialRect("Apply material").GetCenter());
        click(materialRect("New material").GetCenter());
        const auto secondNew = assets.materials.back().id;
        const auto secondNewPath = materialPathFor(secondNew);
        const auto secondNewBytes = contents(secondNewPath);
        click(materialRect("Grid").GetCenter());
        click(materialRect("Apply material").GetCenter());
        require(editor.saveMaterialAsset(firstNew, error), "Save the first of two dirty materials: " + error);
        saveSceneCommand();
        capture("material_second_still_dirty", true, true);
        require(contents(secondNewPath) == secondNewBytes &&
            renderedUiText.find("Material assets have unsaved changes.") != std::string::npos &&
            renderedUiText.find("Scene *") == std::string::npos,
            "Saving material A or the scene saved/cleared independently dirty material B.");
        require(editor.saveMaterialAsset(secondNew, error) && contents(secondNewPath) != secondNewBytes,
            "Explicitly saving the second material did not persist its applied values.");
        require(contents(editor.scenePath()) == sceneBeforeNewMaterials,
            "New material or material-only changes altered serialized scene contents.");

        // Failed immediate publication retains the new in-memory asset as dirty
        // so the user can fix the destination and explicitly retry.
        const auto materialsFolder = project.contentRoot / "Materials";
        const auto heldMaterials = project.contentRoot / "Materials-held-by-save-test";
        std::filesystem::rename(materialsFolder, heldMaterials);
        { std::ofstream blocker(materialsFolder); blocker << "isolated test destination blocker"; }
        click(materialRect("New material").GetCenter());
        const auto failedNew = assets.materials.back().id;
        capture("material_new_publish_failed", true, true);
        const bool retainedDirty = renderedUiText.find("Material assets have unsaved changes.") != std::string::npos &&
            renderedUiText.find("New material is unsaved:") != std::string::npos;
        std::filesystem::remove(materialsFolder);
        std::filesystem::rename(heldMaterials, materialsFolder);
        require(retainedDirty && editor.saveMaterialAsset(failedNew, error) &&
            std::filesystem::is_regular_file(materialPathFor(failedNew)),
            "Failed new-material publication lost dirty data or could not retry.");
        std::cout << "PASS: Engine Content navigation, read-only material UI, default Actor assignment, "
            "Copy to Project, independent material/scene dirty states, immediate material publication/failure retry, "
            "save and unchanged engine file.\n";
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
            require(renderedUiText.find("Scene triangles:") != std::string::npos &&
                renderedUiText.find("After clipping (GPU):") != std::string::npos,
                "Profiler is missing the scene total or GPU clipping statistic.");
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
    {
        ImGuiContextScope active(context);
        runImportSmoke(editor, ui, renderer, output, [&](const char* name) { capture(name, true); },
            [&](bool enabled) {
                auto* content = findWindow("Content & Editors");
                click(itemRect(content, ImHashStr("Import options...", 0, content->ID)).GetCenter());
                require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
                    "The Import options button did not open its popup.");
                auto* popup = context->OpenPopupStack.back().Window;
                if (imgui::uiPreferences().importSkipDegenerateTriangles != enabled)
                    click(itemRect(popup, ImHashStr("Skip degenerate triangles", 0, popup->ID)).GetCenter());
                require(imgui::uiPreferences().importSkipDegenerateTriangles == enabled &&
                    imgui::loadUiPreferences(imgui::defaultUiPreferencesPath()).importSkipDegenerateTriangles == enabled,
                    "The real import checkbox did not update and save the shared option.");
                capture(enabled ? "import_options_skip" : "import_options_strict", true);
                pressKey(ImGuiKey_Escape);
            },
            [&](const std::string& path, const char* action, bool grid) {
                const auto* entry = editor.assetRegistry().find(path);
                require(entry != nullptr, "Lifecycle asset menu target is absent.");
                const auto name = entry->name, folder = entry->folder;
                editor.closeAssetDetails();
                editor.closeImportResults();
                editor.selectContentFolder(folder);
                editor.setContentSearch(name);
                editor.setContentTypeFilter(assets::AssetType::Unknown);
                editor.setContentGridView(grid);
                capture("lifecycle_menu_ready", true);
                ImGuiWindow* area = nullptr;
                for (auto* window : context->Windows)
                    if (window->Active && std::strstr(window->Name, "##content-assets")) area = window;
                require(area != nullptr, "Lifecycle Content area is absent.");
                const auto scope = ImHashStr(path.c_str(), 0, area->ID);
                input.mouse = itemRect(area, ImHashStr(grid ? "##asset-tile" : "##asset-row", 0, scope)).GetCenter();
                capture("lifecycle_menu_hover", true);
                input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); };
                capture("lifecycle_menu_press", true);
                input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); };
                capture("lifecycle_menu_open", true);
                capture("lifecycle_menu_stable", true);
                require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
                    "Right-click did not open the asset menu.");
                auto* popup = context->OpenPopupStack.back().Window;
                if (action && *action) click(itemRect(popup, ImHashStr(action, 0, popup->ID)).GetCenter());
            },
            [&](const char* windowName, const char* label) {
                if (!label || !*label) { pressKey(ImGuiKey_Escape); return; }
                auto* window = findWindow(windowName);
                click(itemRect(window, ImHashStr(label, 0, window->ID)).GetCenter());
            });
    }
    {
        // This suite receives the application's isolated project copy. Keep all
        // persistence fixtures there and run last so a new bound scene cannot
        // change the earlier UI/selection/undo assertions.
        ImGuiContextScope active(context);
        const auto invokeCommand = [&](std::string_view id) {
            require(editor.commands().enabled(id), "Persistence command is disabled: " + std::string(id));
            for (const auto& command : editor.commands().commands()) if (command.info.id == id) {
                command.action();
                return;
            }
            require(false, "Persistence command is missing: " + std::string(id));
        };
        const auto originalScenePath = editor.scenePath();
        const auto originalSceneBytes = contents(originalScenePath);
        // Publish any fixture assets created by earlier low-level tests. Normal
        // UI import/new-material actions publish their files themselves.
        require(assets.saveMaterials(project.contentRoot / "Materials", error) &&
            assets.saveMeshes(project.contentRoot, error), "Publish earlier test fixtures: " + error);
        const auto fixtureName = "SavePersistence-" + assets::assetIdText(assets::generateAssetId());
        const auto sourcePath = output / (fixtureName + ".obj");
        {
            std::ofstream stream(sourcePath, std::ios::binary);
            stream << "v -0.5 0 0\nv 0.5 0 0\nv 0 1 0\nf 1 2 3\n";
            stream.close();
            require(stream.good(), "Cannot write the isolated persistence OBJ fixture.");
        }
        assets::AssetId importedId = 0;
        require(assets.importObj(sourcePath, importedId, error), "Persistence fixture import: " + error);
        const auto materialId = assets.createMaterial(fixtureName);
        invokeCommand("actor.addCube"); // Exercise normal dirty-state bookkeeping.
        const auto fixtureActorId = editor.selectedActor();
        auto* fixtureActor = world.findActor(fixtureActorId);
        require(fixtureActor != nullptr, "Persistence fixture Actor was not created.");
        fixtureActor->mesh.meshId = importedId;
        fixtureActor->mesh.materialId = materialId;
        fixtureActor->name = fixtureName;
        const auto meshPath = project.contentRoot / assets::AssetLibrary::meshAssetFileName(*assets.findMesh(importedId));
        const auto materialPath = project.contentRoot / "Materials" /
            assets::AssetLibrary::materialAssetFileName(*assets.findMaterial(materialId));
        require(!std::filesystem::exists(meshPath) && !std::filesystem::exists(materialPath),
            "Persistence fixtures unexpectedly existed before Save As.");
        capture("save_as_dirty", true, true);
        require(renderedUiText.find("Scene *") != std::string::npos,
            "Creating the persistence Actor did not mark the scene unsaved.");
        const auto savedPath = project.contentRoot / "Scenes" / (fixtureName + ".rascene");
        require(!editor.saveSceneAs(savedPath, error) && !error.empty() &&
            editor.scenePath() == originalScenePath && !std::filesystem::exists(savedPath) &&
            !std::filesystem::exists(meshPath) && !std::filesystem::exists(materialPath),
            "Scene-only Save As published unsaved assets or created unresolved references.");
        require(editor.saveMaterialAsset(materialId, error), "Save new material explicitly: " + error);
        require(!editor.saveSceneAs(savedPath, error) && !std::filesystem::exists(savedPath),
            "Unsaved imported mesh was accepted by scene-only Save As.");
        require(assets.saveMeshes(project.contentRoot, error), "Publish the imported mesh fixture: " + error);
        const auto materialBytesBeforeScene = contents(materialPath);
        const auto materialTimeBeforeScene = std::filesystem::last_write_time(materialPath);
        const auto meshBytesBeforeScene = contents(meshPath);
        const auto meshTimeBeforeScene = std::filesystem::last_write_time(meshPath);
        // Both assets now differ from disk. Saving references must retain the
        // persisted versions and leave these independent edits in memory.
        assets.findMaterial(materialId)->roughness = .123f;
        assets.findMesh(importedId)->vertices[0].position[0] -= .25f;
        assets.findMesh(importedId)->markGeometryModified();
        require(editor.saveSceneAs(savedPath, error), "Save As failed: " + error);
        require(editor.scenePath() == savedPath && std::filesystem::exists(meshPath) &&
            std::filesystem::exists(materialPath) && contents(originalScenePath) == originalSceneBytes,
            "Save As did not bind the new scene or preserve the old scene.");
        require(contents(meshPath) == meshBytesBeforeScene && contents(materialPath) == materialBytesBeforeScene &&
            std::filesystem::last_write_time(meshPath) == meshTimeBeforeScene &&
            std::filesystem::last_write_time(materialPath) == materialTimeBeforeScene,
            "Scene-only Save As wrote independently changed assets.");
        capture("save_as_clean", true, true);
        require(renderedUiText.find("Scene saved") != std::string::npos &&
            renderedUiText.find("Scene *") == std::string::npos,
            "Successful Save As left the scene unsaved.");
        require(std::filesystem::remove(sourcePath), "Cannot remove only the isolated persistence OBJ source.");
        const auto savedMeshBytes = contents(meshPath);
        const auto savedMeshTime = std::filesystem::last_write_time(meshPath);
        const auto reloadSaved = [&] {
            assets::AssetLibrary loaded;
            loaded.createDefaults();
            require(loaded.loadEngineMaterials(project.engineRoot / "Content/Materials", error) &&
                loaded.loadEngineMeshes(project.engineRoot / "Content/Meshes", error) &&
                loaded.loadMaterials(project.contentRoot / "Materials", error) &&
                loaded.loadMeshes(project.contentRoot, error), "Reload saved assets: " + error);
            scene::World reloaded;
            require(reloaded.load(savedPath, loaded, error), "Reload saved scene: " + error);
            const auto* actor = reloaded.findActor(fixtureActorId);
            require(actor && actor->mesh.meshId == importedId && actor->mesh.materialId == materialId &&
                loaded.findMesh(importedId) && loaded.findMaterial(materialId),
                "Saved scene does not resolve its new assets after deleting the source OBJ.");
            require(reloaded.actors.size() == world.actors.size(), "Saved scene lost an Actor.");
        };
        reloadSaved();

        invokeCommand("actor.addCube");
        const auto previousSceneBytes = contents(savedPath);
        const auto blockedPath = project.contentRoot / "Scenes" / (fixtureName + "-blocked.rascene");
        require(std::filesystem::create_directory(blockedPath), "Cannot create isolated Save As failure fixture.");
        require(!editor.saveSceneAs(blockedPath, error) && !error.empty() && editor.scenePath() == savedPath &&
            contents(savedPath) == previousSceneBytes && std::filesystem::is_directory(blockedPath),
            "Failed Save As switched the scene path or changed the previous scene.");
        capture("save_as_failed_dirty", true, true);
        require(renderedUiText.find("Scene *") != std::string::npos,
            "Failed Save As cleared the unsaved indicator.");
        invokeCommand("scene.save");
        require(contents(savedPath) != previousSceneBytes, "Save Scene command did not persist the new Actor.");
        capture("save_scene_clean", true, true);
        require(renderedUiText.find("Scene saved") != std::string::npos &&
            renderedUiText.find("Scene *") == std::string::npos,
            "Save Scene command did not clear the unsaved indicator.");
        reloadSaved();
        require(contents(meshPath) == savedMeshBytes && std::filesystem::last_write_time(meshPath) == savedMeshTime,
            "Save Scene wrote the independently changed imported mesh.");
        require(contents(materialPath) == materialBytesBeforeScene &&
            std::filesystem::last_write_time(materialPath) == materialTimeBeforeScene,
            "Save Scene wrote the independently changed material.");

        // A deleted dependency is rejected without recreating it or replacing
        // the old scene. Restoring it permits the scene-only save again.
        const auto missingBackup = meshPath.string() + ".test-hidden";
        std::filesystem::rename(meshPath, missingBackup);
        const auto sceneBeforeMissing = contents(savedPath);
        require(!editor.saveSceneAs(savedPath, error) && contents(savedPath) == sceneBeforeMissing &&
            !std::filesystem::exists(meshPath), "Scene save rewrote a deleted dependency or damaged the scene.");
        std::filesystem::rename(missingBackup, meshPath);

        invokeCommand("actor.addCube");
        const auto sceneBeforeAsset = contents(savedPath);
        require(editor.saveMaterialAsset(materialId, error), "Explicit material save failed: " + error);
        require(contents(savedPath) == sceneBeforeAsset && contents(meshPath) == savedMeshBytes &&
            contents(materialPath) != materialBytesBeforeScene,
            "Saving a material wrote another asset/scene or lost the intended material change.");

        invokeCommand("actor.addCube");
        capture("save_all_ready", true, true);
        auto* toolbar = findWindow("RestArtEngine Editor");
        require(toolbar != nullptr, "Save All toolbar is missing.");
        // EndMenuBar retains the submitted horizontal cursor after the final
        // "Scene *" status. Derive the preceding button's center from that
        // layout, avoiding navigation focus changes from floating tool windows.
        const auto& toolbarStyle = context->Style;
        const float saveAllWidth = ImGui::CalcTextSize("Save All").x + toolbarStyle.FramePadding.x * 2;
        const float saveAllX = toolbar->Pos.x + toolbar->DC.MenuBarOffset.x -
            ImGui::CalcTextSize("Scene *").x - toolbarStyle.ItemSpacing.x * 2 - saveAllWidth * .5f;
        const ImVec2 saveAllCenter{saveAllX, toolbar->MenuBarRect().GetCenter().y};
        require(toolbar->MenuBarRect().Contains(saveAllCenter), "Save All button is outside the menu bar.");
        click(saveAllCenter);
        capture("save_all_clean", true, true);
        require(renderedUiText.find("Scene saved") != std::string::npos &&
            renderedUiText.find("Scene *") == std::string::npos,
            "The real Save All button did not save the dirty scene.");
        reloadSaved();
        require(contents(meshPath) != savedMeshBytes,
            "Save All did not persist the independently edited mesh.");
        assets::AssetFile savedMeshFile;
        assets::MeshAsset savedMesh;
        require(assets::readAssetFile(meshPath, savedMeshFile, error) &&
            assets::readMeshAssetFile(savedMeshFile, savedMesh, error) &&
            savedMesh.vertices[0].position[0] == assets.findMesh(importedId)->vertices[0].position[0],
            "Save All persisted the wrong mesh geometry.");
        require(contents(originalScenePath) == originalSceneBytes && contents(engineMaterialPath) == engineMaterialBytes,
            "Persistence regression modified the original scene or Engine material.");
        std::cout << "PASS: Scene-only Save/Save As leave edited assets untouched, reject unpublished/deleted references, "
            "material-only save preserves scene/mesh, failed Save As keeps path/dirty, Save All persists geometry and reloads.\n";
    }
    {
        // Ordinary dock panels must keep their full image and interaction area
        // after becoming native windows, independently of the main swap chain.
        ImGuiContextScope active(context);
        input.usePlatformZOrder = true;
        const auto savedWorld = world;
        const auto savedSettings = settings;
        const auto savedFolder = editor.contentFolder();
        const auto savedAsset = editor.selectedContentAsset();
        std::size_t layoutBytes = 0;
        const char* layoutData = ImGui::SaveIniSettingsToMemory(&layoutBytes);
        const std::string savedLayout(layoutData, layoutBytes);
        editor.closeDiagnostics(); editor.closeProjectSettings(); editor.closeAssetDetails();
        world.createDefaultScene();
        settings.cameraYaw = .65f; settings.cameraPitch = .4f; settings.cameraDistance = 8;
        settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1; settings.cameraTarget[2] = 0;
        settings.rayTracing = settings.hardwareRayTracing = false;
        settings.debugView = 0;
        editor.resetLayout();
        input.mouse = {-100, -100};
        capture("floating_initial_layout", true);
        capture("floating_initial_layout", true);
        const auto desktop = context->Viewports[0]->GetMainRect();
        const auto platformHandle = [&](const char* name) {
            const auto* window = findWindow(name);
            require(window && window->Active && window->Viewport != context->Viewports[0],
                std::string("Panel did not acquire a native viewport outside the main window: ") + name);
            const auto hwnd = static_cast<HWND>(window->Viewport->PlatformHandle);
            require(hwnd && hwnd != mainHwnd && IsWindow(hwnd), std::string("Panel has no independent HWND: ") + name);
            return hwnd;
        };
        const auto moveNativePanel = [&](const char* name, ImVec2 position, ImVec2 size) {
            const auto hwnd = platformHandle(name);
            require(SetWindowPos(hwnd, nullptr, static_cast<int>(position.x), static_cast<int>(position.y),
                static_cast<int>(size.x), static_cast<int>(size.y), SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                std::string("Cannot move/resize the native test panel: ") + name);
            capture("floating_native_resize", true);
            capture("floating_native_resize", true);
            const auto* window = findWindow(name);
            require(window->WorkRect.GetWidth() > 200 && window->WorkRect.GetHeight() > 150 &&
                window->WorkRect.Min.x > desktop.Max.x,
                std::string("Panel content remained clipped to the main editor: ") + name);
        };
        dragDock(dockTabCenter(findWindow("Content & Editors")), {desktop.Max.x + 170, desktop.Min.y + 100});
        moveNativePanel("Content & Editors", {desktop.Max.x + 40, desktop.Min.y + 430}, {660, 440});
        const auto contentHwnd = platformHandle("Content & Editors");
        savePlatformCapture(findWindow("Content & Editors"), "floating_content_grid_native.bmp");
        const auto sceneProject = [&](float x, float y, float z) {
            using namespace DirectX;
            const float distance = settings.cameraDistance, pitch = settings.cameraPitch;
            const auto target = XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 1);
            const auto eye = XMVectorAdd(target, XMVectorSet(distance * std::cos(pitch) * std::sin(settings.cameraYaw),
                distance * std::sin(pitch), distance * std::cos(pitch) * std::cos(settings.cameraYaw), 0));
            const auto view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
            const auto projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(50.f),
                static_cast<float>(settings.viewportWidth) / settings.viewportHeight, .05f, 100.f);
            const auto rect = findWindow("Scene Viewport##Interaction")->WorkRect;
            const auto point = XMVector3Project(XMVectorSet(x, y, z, 1), rect.Min.x, rect.Min.y,
                static_cast<float>(settings.viewportWidth), static_cast<float>(settings.viewportHeight),
                0, 1, projection, view, XMMatrixIdentity());
            return ImVec2(XMVectorGetX(point), XMVectorGetY(point));
        };
        const auto beginFloatingAssetDrag = [&](const std::string& path, bool grid) {
            SetForegroundWindow(contentHwnd);
            capture("floating_content_native_focus", true);
            capture("floating_content_native_focus", true);
            require(GetForegroundWindow() == contentHwnd, "Native Content did not gain focus before the drag.");
            const auto* entry = editor.assetRegistry().find(path);
            require(entry != nullptr, "Native Content drag asset is not registered: " + path);
            const auto folder = entry->folder, name = entry->name;
            const auto type = entry->type;
            editor.selectContentFolder(folder); editor.setContentSearch(name); editor.setContentTypeFilter(type);
            editor.setContentRecursive(false); editor.setContentGridView(grid); editor.selectContentAsset(path);
            capture("floating_content_source_layout", true);
            capture("floating_content_source_layout", true);
            ImGuiWindow* browser = nullptr;
            for (auto* window : context->Windows)
                if (window->Active && std::strstr(window->Name, "##content-assets")) browser = window;
            require(browser && browser->Viewport != context->Viewports[0], "Content asset child returned to the main viewport.");
            const auto pathId = ImHashStr(path.c_str(), 0, browser->ID);
            const auto sourceId = ImHashStr(grid ? "##asset-tile" : "##asset-row", 0, pathId);
            auto sourceRect = itemRect(browser, sourceId);
            sourceRect.ClipWith(browser->ClipRect);
            // Native windows may overlap after monitor placement constraints.
            // Start on a genuinely exposed part of the item, like a user would,
            // rather than clicking a wide row's potentially covered centre.
            ImVec2 source{};
            bool exposed = false;
            for (const float fraction : {.1f, .5f, .9f}) {
                const ImVec2 point{sourceRect.Min.x + sourceRect.GetWidth() * fraction, sourceRect.GetCenter().y};
                if (ImGui::FindHoveredViewportFromPlatformWindowStack(point) == browser->Viewport) {
                    source = point; exposed = true; break;
                }
            }
            require(exposed && sourceRect.Contains(source), "The native Content item has no exposed drag source.");
            input.mouse = source;
            capture("floating_content_source_hover", true);
            const double nextSingleClick = context->IO.MouseClickedTime[0] + context->IO.MouseDoubleClickTime;
            for (int frame = 0; context->Time <= nextSingleClick && frame < 60; ++frame)
                capture("floating_content_click_settle", true);
            require(!context->IO.MouseDown[0] && context->Time > nextSingleClick, "Floating asset source is still in a prior click.");
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            capture("floating_content_source_press", true);
            require(context->ActiveId == sourceId, "Native Content press did not activate the intended asset widget.");
            input.mouse.x += 18;
            capture("floating_content_source_drag", true);
            if (!context->DragDropActive) {
                savePlatformCapture(findWindow("Content & Editors"), "floating_source_failure_native.bmp");
                std::cerr << "Floating source failure: path=" << path << " grid=" << grid
                    << " source=" << source.x << ',' << source.y << " mouse=" << context->IO.MousePos.x << ',' << context->IO.MousePos.y
                    << " active=" << context->ActiveId << " hovered=" << context->HoveredId
                    << " hoveredWindow=" << (context->HoveredWindow ? context->HoveredWindow->Name : "none")
                    << " mouseDown=" << context->IO.MouseDown[0] << " doubleClick=" << context->IO.MouseDoubleClicked[0]
                    << " blocked=" << ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) << '\n';
            }
            require(context->DragDropActive && context->DragDropPayload.IsDataType("RAE_ASSET") &&
                context->DragDropPayload.DataSize == static_cast<int>(path.size()) &&
                std::memcmp(context->DragDropPayload.Data, path.data(), path.size()) == 0,
                "An external Content widget did not start its actual asset payload.");
        };
        const auto releaseFloatingAsset = [&](ImVec2 destination, const char* prefix) {
            input.mouse = destination;
            capture((std::string(prefix) + "_hover").c_str(), true);
            capture((std::string(prefix) + "_hover").c_str(), true);
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
            capture(prefix, true);
            capture(prefix, true, true);
        };
        const auto dropFloatingMesh = [&](bool grid, const char* prefix) {
            const auto count = world.actors.size(), history = editor.undoStack().cursor();
            beginFloatingAssetDrag("engine://Meshes/Cube.rasset", grid);
            require(world.actors.size() == count, "Native Content created an Actor before delivery.");
            releaseFloatingAsset(sceneProject(2.2f, 0, 0), prefix);
            const auto id = editor.selectedActor();
            const auto* actor = world.findActor(id);
            require(world.actors.size() == count + 1 && actor && actor->mesh.meshId == assets::EngineCubeMeshId &&
                editor.undoStack().cursor() == history + 1 && editor.undoStack().undoLabel() == "Place Mesh" &&
                std::abs(actor->transform.position[0] - 2.2f) < .15f,
                "Native Content-to-scene Mesh delivery lost coordinates, identity or its undo transaction.");
            const auto placed = actor->transform;
            require(editor.undo() && !world.findActor(id) && world.actors.size() == count,
                "Undo did not remove the Mesh dropped from external Content.");
            require(editor.redo() && world.findActor(id) &&
                std::memcmp(&world.findActor(id)->transform, &placed, sizeof(placed)) == 0,
                "Redo did not restore the Mesh dropped across native windows.");
            require(editor.undo(), "Native Mesh drop fixture could not restore its starting scene.");
            capture("floating_mesh_undo", true);
        };
        dropFloatingMesh(true, "floating_grid_to_main_scene");

        dragDock(dockTabCenter(findWindow("Scene Viewport##Interaction")), {desktop.Max.x + 850, desktop.Min.y + 100});
        moveNativePanel("Scene Viewport##Interaction", {desktop.Max.x + 740, desktop.Min.y + 30}, {840, 650});
        const auto sceneHwnd = platformHandle("Scene Viewport##Interaction");
        require(sceneHwnd != contentHwnd && settings.viewportX == 0 && settings.viewportY == 0 &&
            renderer.sceneTextureWidth() == static_cast<unsigned>(settings.viewportWidth) &&
            renderer.sceneTextureHeight() == static_cast<unsigned>(settings.viewportHeight),
            "External Scene is still using the main swap chain rectangle or Content's native surface.");
        const auto firstSize = ImVec2(static_cast<float>(settings.viewportWidth), static_cast<float>(settings.viewportHeight));
        moveNativePanel("Scene Viewport##Interaction", {desktop.Max.x + 740, desktop.Min.y + 30}, {980, 720});
        require(settings.viewportWidth > firstSize.x + 100 && settings.viewportHeight > firstSize.y + 40,
            "Resizing an external Scene did not resize its independent scene texture.");
        // The platform can clamp a wide native Scene window back onto the
        // monitor, covering the Content source even though the requested X
        // positions did not overlap. Arrange the actual windows vertically
        // before injecting cross-window drags, and verify the source is exposed.
        const auto scenePlatformRect = findWindow("Scene Viewport##Interaction")->Viewport->GetMainRect();
        moveNativePanel("Content & Editors", {desktop.Max.x + 40, scenePlatformRect.Max.y + 30}, {660, 440});
        require(!findWindow("Content & Editors")->Viewport->GetMainRect().Overlaps(
            findWindow("Scene Viewport##Interaction")->Viewport->GetMainRect()),
            "Floating drag fixture has overlapping native Scene and Content windows after platform clamping.");
        savePlatformCapture(findWindow("Scene Viewport##Interaction"), "floating_scene_full_native.bmp");
        const auto raw = contents(output / "floating_native_resize.scene.rgba");
        bool variedScene = false;
        for (std::size_t pixel = 4; pixel + 3 < raw.size(); pixel += 4)
            variedScene |= !std::equal(raw.begin(), raw.begin() + 3, raw.begin() + static_cast<std::ptrdiff_t>(pixel));
        require(raw.size() == static_cast<std::size_t>(settings.viewportWidth) * settings.viewportHeight * 4 && variedScene,
            "External Scene readback has incorrect dimensions or no rendered image.");
        verifyLayoutRoundtrip("layout-native-external.ini");
        {
            std::size_t size = 0;
            const char* ini = ImGui::SaveIniSettingsToMemory(&size);
            const std::string nativeLayout(ini, size);
            struct NativeLayoutState { ImGuiID id, viewport; ImVec2ih position, extent, viewportPosition; };
            std::array<NativeLayoutState, 2> expected{};
            const std::array<const char*, 2> names{"Content & Editors", "Scene Viewport##Interaction"};
            for (std::size_t index = 0; index < names.size(); ++index) {
                const auto* state = ImGui::FindWindowSettingsByID(ImHashStr(names[index]));
                require(state && state->ViewportId != context->Viewports[0]->ID, "Native viewport identity was not serialized.");
                expected[index] = {state->ID, state->ViewportId, state->Pos, state->Size, state->ViewportPos};
            }
            auto* restored = ImGui::CreateContext();
            ImGui::SetCurrentContext(restored);
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::LoadIniSettingsFromMemory(nativeLayout.data(), nativeLayout.size());
            bool matched = true;
            for (const auto& state : expected) {
                const auto* loaded = ImGui::FindWindowSettingsByID(state.id);
                matched &= loaded && loaded->ViewportId == state.viewport && loaded->Pos.x == state.position.x &&
                    loaded->Pos.y == state.position.y && loaded->Size.x == state.extent.x && loaded->Size.y == state.extent.y &&
                    loaded->ViewportPos.x == state.viewportPosition.x && loaded->ViewportPos.y == state.viewportPosition.y;
            }
            ImGui::DestroyContext(restored);
            ImGui::SetCurrentContext(context);
            require(matched, "Fresh-context layout reload lost native viewport IDs or external desktop bounds.");
        }
        click(sceneProject(-.35f, 1.35f, 0));
        require(editor.selectedActor() == 1, "Desktop mouse picking missed the sphere in the external Scene.");
        click(itemRect(findWindow("Scene Viewport##Interaction"),
            findWindow("Scene Viewport##Interaction")->GetID("Move [W]")).GetCenter());
        const auto beforeGizmo = world.findActor(1)->transform;
        const auto gizmoHistory = editor.undoStack().cursor();
        const auto start = sceneProject(.55f, 1, 0), end = sceneProject(1.05f, 1, 0);
        drag(start, end, "floating_scene_gizmo_moved");
        require(world.findActor(1)->transform.position[0] > beforeGizmo.position[0] + .2f &&
            editor.undoStack().cursor() == gizmoHistory + 1, "External Scene gizmo did not move the selected Actor in desktop coordinates.");
        require(editor.undo() && std::memcmp(&world.findActor(1)->transform, &beforeGizmo, sizeof(beforeGizmo)) == 0,
            "Undo did not restore the external Scene gizmo transform.");
        capture("floating_scene_gizmo_undo", true);
        dropFloatingMesh(false, "floating_list_to_external_scene");
        const auto beforeMaterial = world.findActor(1)->mesh.materialId;
        const auto materialHistory = editor.undoStack().cursor();
        beginFloatingAssetDrag("engine://Materials/DefaultMaterial.rasset", true);
        releaseFloatingAsset(sceneProject(-.35f, 1.35f, 0), "floating_material_to_external_scene");
        require(world.findActor(1)->mesh.materialId == assets::EngineDefaultMaterialId &&
            editor.undoStack().cursor() == materialHistory + 1 && editor.undoStack().undoLabel() == "Assign Material",
            "Material delivery from external Content did not resolve the external Scene hit.");
        require(editor.undo() && world.findActor(1)->mesh.materialId == beforeMaterial,
            "Undo did not restore the external Scene material assignment.");
        savePlatformCapture(findWindow("Content & Editors"), "floating_content_list_native.bmp");

        const char* diagnosticsName = "Diagnostics##RestArtEngine.Diagnostics";
        editor.showDiagnostics(editor::DiagnosticsTab::Profiler);
        editor.showProjectSettings();
        capture("floating_tools_open", true);
        ImGui::SetWindowPos(diagnosticsName, {desktop.Max.x + 45, desktop.Min.y + 925}, ImGuiCond_Always);
        ImGui::SetWindowSize(diagnosticsName, {650, 480}, ImGuiCond_Always);
        ImGui::SetWindowPos("Project Settings", {desktop.Max.x + 1760, desktop.Min.y + 40}, ImGuiCond_Always);
        ImGui::SetWindowSize("Project Settings", {540, 620}, ImGuiCond_Always);
        capture("floating_tools_external", true);
        capture("floating_tools_external", true);
        const auto diagnosticsHwnd = platformHandle(diagnosticsName);
        const auto projectSettingsHwnd = platformHandle("Project Settings");
        savePlatformCapture(findWindow(diagnosticsName), "floating_diagnostics_native.bmp");
        savePlatformCapture(findWindow("Project Settings"), "floating_project_settings_native.bmp");
        struct RetainedPlatformWindow { const char* name; HWND handle; ImGuiID viewport; RECT bounds; };
        std::array<RetainedPlatformWindow, 4> retained{{
            {"Scene Viewport##Interaction", sceneHwnd}, {"Content & Editors", contentHwnd},
            {diagnosticsName, diagnosticsHwnd}, {"Project Settings", projectSettingsHwnd}}};
        for (auto& state : retained) {
            state.viewport = findWindow(state.name)->ViewportId;
            require(GetWindowRect(state.handle, &state.bounds) != FALSE, "Cannot remember native panel bounds before minimizing.");
        }
        input.mouse = {-100, -100};
        const auto sceneFrameBeforeMinimize = renderer.geometryStatistics().frameId;
        ShowWindow(mainHwnd, SW_MINIMIZE);
        smokeMainSurfaceVisible = false;
        capture("floating_minimized_initial", true, true);
        capture("floating_minimized_steady", true, true);
        require(IsIconic(mainHwnd), "Main test window did not remain minimized during external UI submission.");
        // Ordinary platform windows use the main HWND as owner. Windows may
        // hide those popups while their owner is minimized; complete submission
        // must retain their HWNDs/settings rather than retiring their viewports.
        for (const auto& state : retained) {
            RECT bounds{};
            require(platformHandle(state.name) == state.handle && findWindow(state.name)->ViewportId == state.viewport &&
                GetWindowRect(state.handle, &bounds) && EqualRect(&bounds, &state.bounds),
                "The minimized editor retired or repositioned a hidden owned platform window.");
        }
        require(findWindow("Content & Editors")->Active && findWindow(diagnosticsName)->Active &&
            findWindow("Project Settings")->Active && findWindow("Scene Viewport##Interaction")->Active &&
            !IsWindowVisible(sceneHwnd) && !settings.viewportVisible &&
            renderer.geometryStatistics().frameId == sceneFrameBeforeMinimize,
            "The minimized loop stopped retaining external UI or rendered a hidden owned scene surface.");
        capture("floating_minimized_final", true, true);
        require(!std::filesystem::exists(output / "floating_minimized_final.bmp"),
            "A minimized main-swapchain screenshot was presented as external-window evidence.");
        ShowWindow(mainHwnd, SW_RESTORE);
        smokeMainSurfaceVisible = true;
        input.mouse = {-100, -100};
        capture("floating_main_restored", true);
        capture("floating_main_restored", true);
        for (const auto& state : retained)
            require(platformHandle(state.name) == state.handle && IsWindowVisible(state.handle),
                "Restoring the main editor did not reveal the retained native panels.");
        require(settings.viewportVisible && settings.viewportX == 0 && settings.viewportY == 0 &&
            renderer.geometryStatistics().frameId > sceneFrameBeforeMinimize,
            "Restoring the main editor did not resume the external scene render.");
        savePlatformCapture(findWindow(diagnosticsName), "floating_restored_diagnostics_native.bmp");
        savePlatformCapture(findWindow("Project Settings"), "floating_restored_settings_native.bmp");
        // Retention is verified above. Remove the retained tools before aiming
        // a camera gesture: the OS may clamp those tools over the scene too.
        editor.closeDiagnostics(); editor.closeProjectSettings();
        capture("floating_restored_tools_closed", true);
        capture("floating_restored_tools_closed", true);
        const auto floatingOrbitStart = sceneProject(2.8f, .5f, 0);
        const auto yawBefore = settings.cameraYaw;
        input.mouse = floatingOrbitStart;
        capture("floating_restored_camera_hover", true);
        require(context->HoveredWindow && context->HoveredWindow->Viewport == findWindow("Scene Viewport##Interaction")->Viewport,
            "Restored camera gesture is obstructed by another native window.");
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); };
        capture("floating_restored_camera_press", true);
        require(context->NavWindow && context->NavWindow->Viewport == findWindow("Scene Viewport##Interaction")->Viewport &&
            editor.cameraNavigating(), "Right-button navigation did not acquire the external Scene focus.");
        input.mouse.x += 42;
        capture("floating_restored_camera_drag", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); };
        capture("floating_restored_camera_release", true);
        require(settings.cameraYaw != yawBefore, "The retained external Scene camera stopped working after main-window restore.");
        savePlatformCapture(findWindow("Scene Viewport##Interaction"), "floating_restored_scene_native.bmp");
        savePlatformCapture(findWindow("Content & Editors"), "floating_restored_content_native.bmp");
        editor.closeDiagnostics(); editor.closeProjectSettings();
        capture("floating_tools_closed", true);
        const auto* settingsPanel = findWindow("Viewport settings");
        dragDock(dockTabCenter(findWindow("Scene Viewport##Interaction")), settingsPanel->DockNode->Rect().GetCenter());
        require(findWindow("Scene Viewport##Interaction")->DockId == findWindow("Viewport settings")->DockId &&
            findWindow("Scene Viewport##Interaction")->Viewport == context->Viewports[0],
            "External Scene tab could not be docked back into the main editor.");
        // Native tools can overlap on smaller desktops. Raise the source as
        // a user would before grabbing its tab, then resolve its current bounds.
        SetForegroundWindow(contentHwnd);
        capture("floating_content_redock_focus", true);
        capture("floating_content_redock_focus", true);
        const auto contentTab = dockTabCenter(findWindow("Content & Editors"));
        require(GetForegroundWindow() == contentHwnd &&
            ImGui::FindHoveredViewportFromPlatformWindowStack(contentTab) == findWindow("Content & Editors")->Viewport,
            "Native Content tab is obscured before re-docking.");
        dragDock(contentTab, findWindow("Details")->DockNode->Rect().GetCenter());
        require(findWindow("Content & Editors")->DockId == findWindow("Details")->DockId &&
            findWindow("Content & Editors")->Viewport == context->Viewports[0],
            "External Content tab could not be docked back into the main editor.");
        verifyLayoutRoundtrip("layout-native-redocked.ini");
        world = savedWorld; settings = savedSettings;
        editor.setContentSearch(""); editor.setContentTypeFilter(assets::AssetType::Unknown);
        editor.setContentRecursive(false); editor.setContentGridView(false);
        editor.selectContentFolder(savedFolder); editor.selectContentAsset(savedAsset);
        ImGui::LoadIniSettingsFromMemory(savedLayout.data(), savedLayout.size());
        capture("floating_original_layout_restored", true);
        capture("floating_original_layout_restored", true);
        input.usePlatformZOrder = false;
        std::cout << "PASS: ordinary native Content/Scene dock detach and re-dock, external layout reload, "
            "independent scene targets, desktop picking/gizmo and cross-window Mesh/Material delivery, "
            "plus native-panel retention and hidden-scene GPU suppression while the main window is minimized.\n";
    }
    {
        ImGuiContextScope active(context);
        editor.closeDiagnostics(); editor.closeProjectSettings();
        capture("status_bar_ready", true, true);
        auto* footer = findWindow("Editor Status Bar");
        require(footer != nullptr, "Editor bottom status bar is missing.");
        const auto commandId = footer->GetID("##command");
        const auto typeCommand = [&](const std::string& command) {
            click(itemRect(footer, commandId).GetCenter());
            input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); };
            capture("console_select_text", true);
            input.events = [&](ImGuiIO& io) {
                io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
                io.AddInputCharactersUTF8(command.c_str());
            };
            capture("console_type", true);
        };
        const auto inputText = [&] {
            const auto* state = ImGui::GetInputTextState(commandId);
            require(state && state->TextA.Data, "Console lost its input state.");
            return std::string(state->TextA.Data);
        };
        const auto consoleActorCount = world.actors.size();
        const auto undoCursor = editor.undoStack().cursor();
        typeCommand("actor.addCube");
        pressKey(ImGuiKey_Enter);
        require(world.actors.size() == consoleActorCount + 1 && editor.undoStack().cursor() == undoCursor + 1,
            "Console Enter did not run the normal undoable Actor command.");
        typeCommand("edit.undo"); pressKey(ImGuiKey_Enter);
        require(world.actors.size() == consoleActorCount, "Console Undo did not restore the Actor count.");
        typeCommand("scene.in"); pressKey(ImGuiKey_Tab);
        require(inputText() == "scene.info ", "Console Tab did not complete the command.");
        pressKey(ImGuiKey_Enter);
        require(!editor.console().entries().empty() && !editor.console().entries().back().error,
            "Console scene.info did not produce a successful result.");
        typeCommand("unfinished input"); pressKey(ImGuiKey_DownArrow);
        require(inputText() == "unfinished input", "Down without history browsing erased the command draft.");
        pressKey(ImGuiKey_UpArrow);
        require(inputText().starts_with("scene.info"), "Console Up did not recall history.");
        pressKey(ImGuiKey_DownArrow);
        require(inputText() == "unfinished input", "Console Down did not restore the draft.");
        const auto selected = editor.selectedActor();
        const auto cameraDistance = settings.cameraDistance;
        pressKey(ImGuiKey_Delete); pressKey(ImGuiKey_F); pressKey(ImGuiKey_W);
        require(world.actors.size() == consoleActorCount && editor.selectedActor() == selected && settings.cameraDistance == cameraDistance,
            "Console text input leaked Delete/F/W into the scene.");
        typeCommand("no.such.command"); pressKey(ImGuiKey_Enter);
        capture("console_error_output", true, true);
        require(!editor.console().entries().empty() && editor.console().entries().back().error &&
            findWindow("Output Log###RestArtEngine.OutputLog")->Active,
            "Unknown console command did not open its error output.");
        require(!editor.executeConsoleCommand("select invalid-id").success &&
            editor.executeConsoleCommand("select " + std::to_string(world.actors.front().id)).success,
            "Console argument validation/Actor selection is incorrect.");
        require(editor.executeConsoleCommand("help scene").success, "Console help failed.");
        capture("console_help_output", true);
        // Selection activates the Details tab on a following frame. Settle
        // that queued focus before locating another widget by navigation ID.
        capture("console_selection_settled", true);
        capture("console_selection_settled", true);
        click(itemRect(footer, footer->GetID("Output Log")).GetCenter());
        capture("console_output_closed", true);
        require(!findWindow("Output Log###RestArtEngine.OutputLog")->Active, "Output Log toggle did not close the window.");

        const auto originalTheme = imgui::currentTheme();
        const auto originalStyle = context->Style;
        const auto originalScale = context->IO.FontGlobalScale;
        const ImVec2 originalSize = context->IO.DisplaySize;
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            for (const bool compact : {false, true}) {
                context->Style = originalStyle;
                imgui::setTheme(theme);
                context->IO.FontGlobalScale = originalScale * (compact ? 1.5f : 1.0f);
                if (compact) context->Style.ScaleAllSizes(1.5f);
                const unsigned width = compact ? 680u : 1440u;
                require(renderer.resize(width, 900, error), error);
                input.displaySize = {static_cast<float>(width), 900};
                editor.resetLayout();
                capture("status_bar_layout_warmup", true);
                capture("status_bar_layout_warmup", true);
                const auto* workspace = findWindow("Editor workspace");
                footer = findWindow("Editor Status Bar");
                const auto* viewport = ImGui::GetMainViewport();
                require(workspace && footer && workspace->Pos.y + workspace->Size.y <= footer->Pos.y + 1 &&
                    std::abs(footer->Pos.y + footer->Size.y - viewport->WorkPos.y - viewport->WorkSize.y) <= 1,
                    "Status bar overlaps the dock workspace or leaves the main work area.");
                require(footer->DC.CursorMaxPos.x <= footer->Pos.x + footer->Size.x + 1,
                    "Status bar controls overflow at the tested width/scale.");
                const auto name = std::string("status_bar_") + (theme == imgui::EditorTheme::Black ? "black" : "blue") +
                    (compact ? "_compact" : "_wide");
                capture(name.c_str(), true, true);
                require(renderedUiText.find("Cmd") != std::string::npos && renderedUiText.find("Tools") != std::string::npos,
                    "Bottom console or tools disappeared at narrow width.");
            }
        }
        imgui::setTheme(originalTheme); context->Style = originalStyle; context->IO.FontGlobalScale = originalScale;
        input.displaySize = {}; input.mouse = {-100, -100};
        require(renderer.resize(static_cast<unsigned>(originalSize.x), static_cast<unsigned>(originalSize.y), error), error);
        editor.resetLayout(); capture("status_bar_restored", true);
        std::cout << "PASS: bottom bar layout at wide/compact/150% scale in both themes, real console Enter/Tab/history, "
            "undoable Actor actions, argument/error output, log toggle and scene keyboard isolation.\n";
    }
    {
        // Light Actors use the same command, property, viewport and persistence
        // paths as normal editing. All files stay in the smoke project's copy.
        ImGuiContextScope active(context);
        const auto savedWorld = world;
        const auto savedSettings = settings;
        const auto savedPrimary = editor.selectedActor();
        const auto savedScenePath = editor.scenePath();
        const auto savedSceneBytes = contents(savedScenePath);
        const auto savedTheme = imgui::currentTheme();
        const auto invokeLightCommand = [&](const std::string& command) {
            const auto result = editor.executeConsoleCommand(command);
            require(result.success, "Light editor command failed: " + command + ": " + result.message);
        };
        world = scene::World{};
        settings = renderer::SceneSettings{};
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.cameraDistance = 8;
        world.blackBackground = true;
        const std::array<scene::LightType, 5> types{scene::LightType::Directional, scene::LightType::Sky,
            scene::LightType::Rect, scene::LightType::Spot, scene::LightType::Point};
        const std::array<const char*, 5> commands{"actor.addDirectionalLight", "actor.addSkyLight",
            "actor.addRectLight", "actor.addSpotLight", "actor.addPointLight"};
        std::array<scene::ActorId, 5> ids{};
        for (std::size_t index = 0; index < commands.size(); ++index) {
            invokeLightCommand(commands[index]);
            ids[index] = editor.selectedActor();
            const auto* created = world.findActor(ids[index]);
            require(created && created->light && created->light->type == types[index] &&
                created->mesh.meshId == 0 && created->mesh.materialId == 0 && world.actors.size() == index + 1,
                "Light creation did not create/select the requested component without mesh references.");
            const auto copy = *created;
            require(editor.undo() && !world.findActor(copy.id), "Undo did not remove the created light.");
            require(editor.redo() && editor.selectedActor() == copy.id && world.findActor(copy.id) &&
                world.findActor(copy.id)->light && world.findActor(copy.id)->light->type == types[index] &&
                world.findActor(copy.id)->light->intensity == copy.light->intensity &&
                world.findActor(copy.id)->light->castShadows == copy.light->castShadows,
                "Redo did not restore the light identity, type or properties.");
            auto& placed = *world.findActor(ids[index]);
            placed.transform.position[0] = -3.0f + static_cast<float>(index) * 1.5f;
            placed.light->intensity = 0;
        }
        const auto lightReceiver = world.createActor("Light receiver", assets::EngineSphereMeshId, assets::EngineDefaultMaterialId);
        world.findActor(lightReceiver)->transform.position[1] = 1;
        world.createActor("Light receiver floor", assets::EnginePlaneMeshId, assets::EngineDefaultMaterialId);
        const auto pointId = ids.back();
        world.findActor(pointId)->transform.position[0] = 0;
        world.findActor(pointId)->transform.position[1] = 3;
        world.findActor(pointId)->transform.position[2] = 1;
        world.findActor(pointId)->light->intensity = 5;
        capture("lighting_ui_ready", true);
        capture("lighting_ui_details", true, true);
        require(renderedUiText.find("Point Light") != std::string::npos &&
            renderedUiText.find("Range (world units)") != std::string::npos,
            "Light selection did not expose its real Details controls.");
        auto* lightDetails = findWindow("Details");
        const auto intensityRect = itemRect(lightDetails, lightDetails->GetID("##LightIntensity"));
        capture("lighting_ui_before", true);
        saveScenePixels(renderer, settings, output / "lighting_ui_before.bmp");
        const auto beforePixels = scenePixels(output / "lighting_ui_before.bmp", settings);
        const float beforeIntensity = world.findActor(pointId)->light->intensity;
        const auto beforeCursor = editor.undoStack().cursor();
        const auto shaderRevision = renderer.shaderRevision();
        const auto intensityStart = intensityRect.GetCenter();
        drag(intensityStart, {intensityStart.x + 100, intensityStart.y}, "lighting_ui_intensity");
        const float afterIntensity = world.findActor(pointId)->light->intensity;
        saveScenePixels(renderer, settings, output / "lighting_ui_intensity.bmp");
        const auto afterPixels = scenePixels(output / "lighting_ui_intensity.bmp", settings);
        require(afterIntensity > beforeIntensity && beforePixels != afterPixels &&
            editor.undoStack().cursor() == beforeCursor + 1 && renderer.shaderRevision() == shaderRevision,
            "Dragging light intensity did not change real scene pixels in one transaction without compiling shaders.");
        require(editor.undo() && world.findActor(pointId)->light->intensity == beforeIntensity,
            "Light intensity Undo did not restore the original value.");
        capture("lighting_ui_undo", true);
        saveScenePixels(renderer, settings, output / "lighting_ui_undo.bmp");
        require(scenePixels(output / "lighting_ui_undo.bmp", settings) == beforePixels,
            "Light intensity Undo did not restore the original GPU image.");
        require(editor.redo() && world.findActor(pointId)->light->intensity == afterIntensity,
            "Light intensity Redo did not restore the edited value.");

        // Pick the editor-only light marker after selecting a mesh, then frame
        // the light using ordinary keyboard input over the live scene image.
        invokeLightCommand("select " + std::to_string(lightReceiver));
        capture("lighting_ui_pick_preroll", true);
        click(projectPoint(0, 3, 1));
        require(editor.selectedActor() == pointId, "Clicking the point-light viewport marker did not select the light.");
        input.mouse = projectPoint(0, 3, 1);
        ImGui::FocusWindow(findWindow("Scene Viewport##Interaction"));
        pressKey(ImGuiKey_F);
        require(std::abs(settings.cameraTarget[0]) < .001f &&
            std::abs(settings.cameraTarget[1] - 3) < .001f && std::abs(settings.cameraTarget[2] - 1) < .001f,
            "Frame Selected did not focus the meshless light Actor.");
        pressKey(ImGuiKey_W);
        const auto beforeTransform = world.findActor(pointId)->transform;
        drag(projectPoint(.55f, 3, 1), projectPoint(1.25f, 3, 1), "lighting_ui_gizmo");
        require(std::abs(world.findActor(pointId)->transform.position[0] - beforeTransform.position[0]) > .1f,
            "The light Actor did not move with the viewport translation Gizmo.");
        require(editor.undo() && std::memcmp(&world.findActor(pointId)->transform, &beforeTransform, sizeof(scene::Transform)) == 0,
            "Light Gizmo Undo did not restore its transform.");

        // Save As only needs mesh dependencies for the two receivers. The first
        // Actor is deliberately a light to cover initialization/material state.
        const auto lightScenePath = project.contentRoot / "Scenes" /
            ("LightingGui-" + assets::assetIdText(assets::generateAssetId()) + ".rascene");
        require(editor.saveSceneAs(lightScenePath, error), "Saving the light scene failed: " + error);
        require(editor.openScene(lightScenePath, error), "Reopening the light scene failed: " + error);
        require(world.actors.size() == 7 && world.actors.front().light,
            "Light scene reload lost Actors or changed the first light into a mesh.");
        for (std::size_t index = 0; index < ids.size(); ++index)
            require(world.findActor(ids[index]) && world.findActor(ids[index])->light &&
                world.findActor(ids[index])->light->type == types[index],
                "A light type or Actor ID did not survive editor Save As/reopen.");
        require(world.findActor(pointId)->light->intensity == afterIntensity,
            "Edited light intensity did not survive editor Save As/reopen.");
        invokeLightCommand("select " + std::to_string(ids[2]));
        settings.cameraTarget[0] = 0; settings.cameraTarget[1] = 1; settings.cameraTarget[2] = 0;
        settings.cameraDistance = 8;
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            capture("lighting_ui_theme_preroll", true);
            capture(theme == imgui::EditorTheme::Black ? "lighting_ui_black" : "lighting_ui_blue", true, true);
            require(renderedUiText.find("Rect Light") != std::string::npos &&
                renderedUiText.find("Width / Height (world units)") != std::string::npos,
                "Rectangle light Details disappeared when changing theme.");
        }
        require(contents(savedScenePath) == savedSceneBytes, "Light Save As rewrote the previously open scene.");
        require(editor.openScene(savedScenePath, error), "Restoring the prior scene failed: " + error);
        world = savedWorld;
        settings = savedSettings;
        imgui::setTheme(savedTheme);
        if (world.findActor(savedPrimary)) invokeLightCommand("select " + std::to_string(savedPrimary));
        input.mouse = {-100, -100};
        capture("lighting_ui_restored", true);
        std::cout << "PASS: five light creation commands and identity-preserving Undo/Redo, live intensity GPU pixels, "
            "property Undo/Redo, marker picking, frame selected, translation Gizmo/Undo, Save As/reopen and both themes.\n";
    }
    {
        ImGuiContextScope active(context);
        const auto savedWorld = world;
        const auto savedSettings = settings;
        const auto savedScenePath = editor.scenePath();
        const auto savedBytes = contents(savedScenePath);
        const auto savedTheme = imgui::currentTheme();
        world = scene::World{};
        settings = renderer::SceneSettings{};
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.cameraDistance = 8;
        const auto subject = world.createActor("Camera subject", assets::EngineCubeMeshId, assets::EngineDefaultMaterialId);
        world.findActor(subject)->transform.position[1] = 1;
        world.createActor("Camera floor", assets::EnginePlaneMeshId, assets::EngineDefaultMaterialId);
        const auto freeCamera = renderer::resolveCamera(settings);
        const auto id = editor.createCameraActor(settings, true);
        require(id && world.findActor(id)->camera && !world.findActor(id)->light &&
            world.findActor(id)->mesh.meshId == 0 && editor.selectedActor() == id,
            "Create camera from view did not create/select a meshless Camera Actor.");
        require(std::memcmp(world.findActor(id)->transform.position, freeCamera.pose.position, sizeof(freeCamera.pose.position)) == 0,
            "Created camera did not match the current view position.");
        require(editor.undo() && !world.findActor(id), "Camera creation Undo failed.");
        require(editor.redo() && world.findActor(id)->camera, "Camera creation Redo lost identity/component.");
        capture("camera_ui_ready", true);
        capture("camera_ui_details", true, true);
        require(renderedUiText.find("Vertical FOV (degrees)") != std::string::npos,
            "Camera Details did not expose the live lens controls.");
        auto* cameraDetails = findWindow("Details");
        const float oldFov = world.findActor(id)->camera->verticalFovDegrees;
        const auto fovRect = itemRect(cameraDetails, cameraDetails->GetID("Vertical FOV (degrees)"));
        drag(fovRect.GetCenter(), {fovRect.GetCenter().x + 50, fovRect.GetCenter().y}, "camera_ui_fov");
        const float changedFov = world.findActor(id)->camera->verticalFovDegrees;
        require(changedFov != oldFov, "Real camera FOV widget did not edit lens.");
        require(editor.undo() && world.findActor(id)->camera->verticalFovDegrees == oldFov, "Camera lens Undo failed.");
        require(editor.redo() && world.findActor(id)->camera->verticalFovDegrees == changedFov, "Camera lens Redo failed.");
        const auto dofCheckbox = itemRect(cameraDetails, cameraDetails->GetID("Enable depth of field"));
        click(dofCheckbox.GetCenter());
        require(world.findActor(id)->camera->depthOfField.enabled, "Real DOF checkbox did not enable the lens effect.");
        require(editor.undo() && !world.findActor(id)->camera->depthOfField.enabled, "DOF enable Undo failed.");
        require(editor.redo() && world.findActor(id)->camera->depthOfField.enabled, "DOF enable Redo failed.");
        const auto focusControl = itemRect(cameraDetails, cameraDetails->GetID("Focus distance (m)"));
        const float oldFocus=world.findActor(id)->camera->depthOfField.focusDistance;
        const auto focusHistory=editor.undoStack().cursor();
        drag(focusControl.GetCenter(), {focusControl.GetCenter().x+55,focusControl.GetCenter().y}, "camera_dof_focus");
        const float editedFocus=world.findActor(id)->camera->depthOfField.focusDistance;
        require(editedFocus!=oldFocus && editor.undoStack().cursor()==focusHistory+1,
            "DOF focus drag did not create one lens transaction.");
        require(editor.undo() && world.findActor(id)->camera->depthOfField.focusDistance==oldFocus,"DOF focus Undo failed.");
        require(editor.redo() && world.findActor(id)->camera->depthOfField.focusDistance==editedFocus,"DOF focus Redo failed.");
        capture("camera_dof_details",true,true);
        require(editor.setDefaultCamera(id) && world.defaultCameraActor == id, "Set default camera failed.");
        require(editor.undo() && world.defaultCameraActor == 0, "Default camera Undo failed.");
        require(editor.redo() && world.defaultCameraActor == id, "Default camera Redo failed.");
        world.findActor(id)->camera->aspectRatio = 16.f / 9.f;
        require(editor.setCameraView(settings, id, false), "Cannot view scene camera.");
        capture("camera_ui_view", true, true);
        require(std::abs(float(settings.viewportWidth) / settings.viewportHeight - 16.f / 9.f) < .01f,
            "Fixed camera aspect did not produce its requested image rectangle.");
        const auto beforeView = world.findActor(id)->transform;
        const auto image = editor.cameraImageRect();
        input.mouse = {(image[0]+image[2])*.5f, (image[1]+image[3])*.5f};
        capture("camera_ui_view_hover", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); };
        capture("camera_ui_view_down", true);
        input.mouse.x += 45;
        capture("camera_ui_view_drag", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); };
        capture("camera_ui_view_up", true);
        require(std::memcmp(&beforeView, &world.findActor(id)->transform, sizeof(beforeView)) == 0,
            "Read-only camera view changed the Actor transform.");
        require(editor.setCameraView(settings, id, true), "Cannot pilot scene camera.");
        const auto history = editor.undoStack().cursor();
        input.mouse = {(image[0]+image[2])*.5f, (image[1]+image[3])*.5f};
        capture("camera_ui_pilot_hover", true);
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, true); };
        capture("camera_ui_pilot_down", true);
        for (int step=0; step<5; ++step) { input.mouse.x += 12; capture("camera_ui_pilot_drag", true); }
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(1, false); };
        capture("camera_ui_pilot_up", true);
        require(std::memcmp(&beforeView, &world.findActor(id)->transform, sizeof(beforeView)) != 0 &&
            editor.undoStack().cursor() == history + 1, "Piloting did not produce exactly one transform transaction.");
        require(editor.undo() && std::memcmp(&beforeView, &world.findActor(id)->transform, sizeof(beforeView)) == 0,
            "Piloting Undo did not restore the Camera Actor.");
        editor.returnToFreeCamera(settings);
        const auto restored = renderer::resolveCamera(settings);
        require(std::memcmp(&restored.pose, &freeCamera.pose, sizeof(freeCamera.pose)) == 0,
            "Returning from a scene camera did not restore the free viewport.");
        settings.cameraLens.projection = camera::Projection::Orthographic;
        settings.cameraLens.orthographicHeight = 6;
        capture("camera_ui_ortho", true);
        // The selected Camera's Gizmo projects onto the center in this axial
        // setup. Pick the visible lower front face, away from editor handles.
        click(projectPoint(0,.4f,1));
        require(editor.selectedActor() == subject, "Orthographic image picking does not match rendered geometry.");
        editor.executeConsoleCommand("select " + std::to_string(id));
        editor.showCameraPreview(id);
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            capture("camera_ui_preview_warmup", true);
            capture(theme == imgui::EditorTheme::Black ? "camera_ui_black" : "camera_ui_blue", true, true);
            require(renderedUiText.find("Raster preview") != std::string::npos &&
                renderer.previewTexture(renderer::PreviewSlot::Camera), "Independent camera preview is missing.");
        }
        editor.closeCameraPreview();
        #include "CameraQuadEditorSmoke.inl"
        const auto cameraPath = project.contentRoot / "Scenes" / "CameraGui.rascene";
        require(editor.saveSceneAs(cameraPath,error), "Camera Save As failed: " + error);
        require(editor.openScene(cameraPath,error), "Camera reopen failed: " + error);
        require(world.defaultCameraActor == id && world.findActor(id)->camera &&
            world.findActor(id)->camera->verticalFovDegrees == changedFov &&
            world.findActor(id)->camera->depthOfField.enabled &&
            world.findActor(id)->camera->depthOfField.focusDistance==editedFocus,
            "Camera lens/default reference did not survive editor save/reopen.");
        editor.executeConsoleCommand("select " + std::to_string(id));
        require(editor.executeConsoleCommand("actor.delete").success && !world.findActor(id) && world.defaultCameraActor == 0,
            "Deleting the default Camera left a dangling reference.");
        require(editor.undo() && world.findActor(id)->camera && world.defaultCameraActor == id,
            "Delete Undo did not restore Camera/default reference together.");
        require(contents(savedScenePath) == savedBytes, "Camera Save As overwrote the previous scene.");
        require(editor.openScene(savedScenePath,error),error);
        world = savedWorld; settings = savedSettings; imgui::setTheme(savedTheme);
        input.mouse = {-100,-100}; capture("camera_ui_restored",true);
        std::cout << "PASS: Camera creation, real lens edit/Undo, default reference, fixed aspect, read-only view, "
            "pilot gesture/Undo, free-view restoration, orthographic picking, preview themes, save/reopen and deletion/Undo.\n";
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

