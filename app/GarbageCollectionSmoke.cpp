#include "GarbageCollectionSmoke.h"
#include <RestArtEngine/Core/GarbageCollector.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const std::string& text) { if (!value) throw std::runtime_error(text); }
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Missing GC capture.");
    return {std::istreambuf_iterator<char>(input), {}};
}
// Keyboard navigation activation follows the real Button/Checkbox handlers,
// without moving the user's desktop cursor. Mouse is used for the menu path.
struct Input {
    ImGuiContext* context{};
    ImGuiID pre{}, post{}, activate{};
    ImVec2 mouse{-100, -100};
    std::function<void(ImGuiIO&)> events;
    explicit Input(ImGuiContext* target) : context(target) {
        context->IO.ConfigInputTrickleEventQueue = false;
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePre;
        hook.UserData = this;
        hook.Callback = [](ImGuiContext* ctx, ImGuiContextHook* h) {
            auto& input = *static_cast<Input*>(h->UserData);
            ctx->IO.AddFocusEvent(true);
            ctx->IO.AddMousePosEvent(input.mouse.x, input.mouse.y);
            if (input.events) { auto event = std::move(input.events); event(ctx->IO); }
        };
        pre = ImGui::AddContextHook(context, &hook);
        hook.Type = ImGuiContextHookType_NewFramePost;
        hook.Callback = [](ImGuiContext* ctx, ImGuiContextHook* h) {
            auto& input = *static_cast<Input*>(h->UserData);
            if (input.activate) {
                ctx->NavActivateId = ctx->NavActivateDownId = ctx->NavActivatePressedId = input.activate;
                ctx->NavActivateFlags = ImGuiActivateFlags_None;
                input.activate = 0;
            }
        };
        post = ImGui::AddContextHook(context, &hook);
    }
    ~Input() { ImGui::RemoveContextHook(context, pre); ImGui::RemoveContextHook(context, post); }
};
}

void runGarbageCollectionSmoke(rae::renderer::SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
    rae::editor::SceneEditor& editor, const std::filesystem::path& outputDirectory) {
    using namespace rae;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    std::string error;
    assets::AssetLibrary assets;
    assets.createDefaults();
    scene::World world;
    world.createDefaultScene();
    auto imported = *assets.findMesh(3);
    imported.id = 9000;
    imported.primitive = assets::MeshPrimitive::Imported;
    assets.meshes.push_back(std::move(imported));
    const auto triangleActor = world.createActor("GC imported mesh", 9000, 1);
    world.findActor(triangleActor)->transform.position[0] = 2;
    renderer::SceneSettings settings;
    settings.rayTracing = true;
    settings.path = renderer::RenderPath::Forward;
    ImGuiContext* context = nullptr;
    const auto sceneFrame = [&](const char* capture, bool transient = false) {
        ui.beginFrame(1.0f / 60.0f);
        context = ImGui::GetCurrentContext();
        ImGui::SetNextWindowPos({16, 48});
        ImGui::SetNextWindowSize({320, 90});
        ImGui::Begin("GC font verification", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Resource reconstruction / 中文字体");
        ImGui::End();
        if (transient) {
            ImGui::Begin("GC inactive cache", nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::BeginTable("GC table", 2)) {
                for (int i = 0; i < 80; ++i) {
                    ImGui::TableNextColumn(); ImGui::Text("Temporary item %d", i);
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }
        require(renderer.render(settings, world, assets, error), error);
        ui.endFrame();
        if (capture) require(renderer.saveScreenshot((output / (std::string(capture) + ".bmp")).c_str(), error), error);
        require(renderer.present(false, error), error);
    };
    sceneFrame(nullptr, true);
    sceneFrame(nullptr);
    sceneFrame(nullptr); // Let focus settle after the temporary window disappears.
    sceneFrame("active-before");
    const auto alive = renderer.cacheStats();
    const auto cpuBefore = ui.memoryStats();
    require(alive.meshCount > 0 && alive.rayGpuBytesEstimate > 0 && cpuBefore.fontAtlasCpuBytes > 0,
        "GC test did not create mesh, triangle and CPU font caches.");
    const auto liveCollection = renderer.collectGarbage(0);
    const auto uiCollection = ui.collectGarbage(0);
    require(liveCollection.objectsReleased == 0 && renderer.cacheStats().meshCount == alive.meshCount,
        "GC removed active render resources.");
    require(uiCollection.cpuBytesReleased >= cpuBefore.fontAtlasCpuBytes && ui.memoryStats().fontAtlasCpuBytes == 0,
        "GC did not release the uploaded font CPU atlas.");
    require(ui.memoryStats().windowTransientCpuBytes < cpuBefore.windowTransientCpuBytes,
        "GC did not compact the inactive window.");
    sceneFrame("active-after");
    require(bytes(output / "active-before.bmp") == bytes(output / "active-after.bmp"),
        "GC changed the active scene or Chinese font output.");

    for (auto& actor : world.actors) actor.visible = false;
    settings.rayTracing = false;
    settings.shadowMethod = renderer::ShadowMethod::Disabled;
    sceneFrame(nullptr);
    const auto retired = renderer.collectGarbage(0);
    require(retired.gpuBytesReleasedEstimate > 0 && renderer.cacheStats().meshCount == 0 &&
        renderer.cacheStats().rayGpuBytesEstimate == 0, "Idle GPU resources were not released.");
    for (auto& actor : world.actors) actor.visible = true;
    settings.rayTracing = true;
    settings.shadowMethod = renderer::ShadowMethod::ShadowMap;
    sceneFrame("restored");
    require(renderer.cacheStats().rayGpuBytesEstimate > 0 && renderer.cacheStats().meshCount == alive.meshCount &&
        bytes(output / "active-before.bmp") == bytes(output / "restored.bmp"), "Released resources did not reconstruct identically.");
    settings.rayTracing = false;
    settings.shadowMethod = renderer::ShadowMethod::AnalyticRay;
    sceneFrame(nullptr);
    renderer.collectGarbage(0);
    require(renderer.cacheStats().rayGpuBytesEstimate > 0, "GC discarded active analytic shadow triangles.");
    settings.shadowMethod = renderer::ShadowMethod::Disabled;
    sceneFrame(nullptr);
    renderer.collectGarbage(0);
    require(renderer.cacheStats().rayGpuBytesEstimate == 0, "Disabled ray consumers retained their expired buffer.");
    std::cout << "PASS: active cache preservation, idle GPU release, identical scene reconstruction, ray consumers, font atlas and inactive ImGui buffers.\n";

    GarbageCollector gc;
    auto policy = gc.settings();
    policy.intervalSeconds = 1;
    policy.memoryIdleSeconds = 0;
    require(gc.setSettings(policy, error), error);
    const auto preferences = output / "gc-ui-settings.json";
    editor.setGarbageCollector(&gc, preferences);
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(context);
    {
        Input input(context);
        const auto frame = [&](const char* capture = nullptr) {
            ui.beginFrame(1.0f / 60.0f);
            bool vsync = false, unusedCapture = false;
            renderer::SceneSettings editorSettings;
            editor.draw(editorSettings, renderer, vsync, unusedCapture);
            require(renderer.render(editorSettings, editor.world(), editor.assets(), error, editor.materialPreview()), error);
            ui.endFrame();
            if (capture) require(renderer.saveScreenshot((output / (std::string(capture) + ".bmp")).c_str(), error), error);
            require(renderer.present(false, error), error);
        };
        const auto click = [&](ImVec2 pos) {
            input.mouse = pos; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); }; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }; frame();
        };
        frame(); frame();
        auto* toolbar = ImGui::FindWindowByName("RestArtEngine Editor");
        require(toolbar != nullptr, "Missing editor toolbar.");
        const float spacing = context->Style.ItemSpacing.x;
        const float x = toolbar->Pos.x + std::max(toolbar->WindowPadding.x, spacing) +
            ImGui::CalcTextSize("Layout").x + ImGui::CalcTextSize("Edit").x + spacing * 4.5f +
            ImGui::CalcTextSize("UI").x * .5f;
        click({x, toolbar->MenuBarRect().GetCenter().y});
        frame();
        require(!context->OpenPopupStack.empty() && context->OpenPopupStack[0].Window, "UI menu did not open.");
        auto* menu = context->OpenPopupStack[0].Window;
        input.activate = menu->GetID("Garbage collection...");
        frame(); frame();
        auto* panel = ImGui::FindWindowByName("Garbage collection");
        require(panel && panel->Active, "GC settings are not reachable from the UI menu.");
        const auto activate = [&](const char* label) {
            ImGui::FocusWindow(panel);
            input.activate = panel->GetID(label);
            frame(); frame();
        };
        activate("Automatic collection");
        require(gc.settings().enabled, "Editing the GC checkbox bypassed Apply.");
        activate("Apply & save");
        require(!gc.settings().enabled, "Apply did not commit the GC checkbox.");
        GarbageCollector reload;
        require(reload.loadSettings(preferences, error) && !reload.settings().enabled && reload.settings().intervalSeconds == 1,
            "GC UI preferences did not round-trip.");
        require(!gc.update(5, [&](double idle) { return renderer.collectGarbage(idle); }), "Disabled automatic GC still ran.");
        activate("Collect now");
        require(gc.stats().collections == 0, "GC ran during widget drawing.");
        require(gc.update(0, [&](double idle) {
            auto memory = renderer.collectGarbage(idle);
            const auto uiMemory = ui.collectGarbage(idle);
            memory.cpuBytesReleased += uiMemory.cpuBytesReleased;
            memory.objectsReleased += uiMemory.objectsReleased;
            return memory;
        }) && gc.stats().collections == 1, "Manual GC did not run when automatic GC was disabled.");
        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            ImGui::SetWindowPos(panel, {360, 150});
            ImGui::SetWindowSize(panel, {480, 650});
            frame(); frame(theme == imgui::EditorTheme::Black ? "gc-black" : "gc-blue");
        }
        ImGui::SetWindowSize(panel, {280, 220});
        frame();
        require(panel->ScrollMax.y > 0, "Narrow GC panel cannot scroll to its actions.");
        ImGui::SetScrollY(panel, panel->ScrollMax.y);
        frame(); frame("gc-narrow");
    }
    editor.setGarbageCollector(nullptr, {});
    ImGui::SetCurrentContext(previous);
    require(renderer.validationErrorCount() == 0, "D3D11 errors during GC validation.");
    std::cout << "PASS: GC menu, draft/apply/save, reload, disabled auto/manual request, themes and narrow scrollable panel.\n"
        << "D3D11 debug layer: " << (renderer.validationEnabled() ? "enabled" : "unavailable")
        << "; validation errors: " << renderer.validationErrorCount() << '\n';
}
