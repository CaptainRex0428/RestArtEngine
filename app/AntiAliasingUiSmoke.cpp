#include "AntiAliasingUiSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
using Json = nlohmann::json;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("AA UI: " + message);
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read isolated project manifest.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    output.close();
    require(bool(output), "Cannot write AA UI evidence.");
}
struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
struct SyntheticInput {
    ImGuiContext* context;
    ImGuiID hook = 0;
    ImVec2 mouse{-100, -100};
    std::function<void(ImGuiIO&)> events;
    bool oldTrickle;
    explicit SyntheticInput(ImGuiContext* target) : context(target), oldTrickle(target->IO.ConfigInputTrickleEventQueue) {
        context->IO.ConfigInputTrickleEventQueue = false;
        ImGuiContextHook callback;
        callback.Type = ImGuiContextHookType_NewFramePre;
        callback.UserData = this;
        callback.Callback = [](ImGuiContext* ctx, ImGuiContextHook* active) {
            auto& input = *static_cast<SyntheticInput*>(active->UserData);
            ctx->IO.AddFocusEvent(true);
            ctx->IO.AddMousePosEvent(input.mouse.x, input.mouse.y);
            ImGuiViewportP* hovered = nullptr;
            for (int index = ctx->Viewports.Size - 1; index >= 0; --index) {
                auto* candidate = ctx->Viewports[index];
                if (!(candidate->Flags & (ImGuiViewportFlags_NoInputs | ImGuiViewportFlags_IsMinimized)) &&
                    candidate->GetMainRect().Contains(input.mouse)) { hovered = candidate; break; }
            }
            if (ctx->IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
                ctx->IO.AddMouseViewportEvent(hovered ? hovered->ID : 0);
            if (input.events) { auto event = std::move(input.events); event(ctx->IO); }
        };
        hook = ImGui::AddContextHook(context, &callback);
    }
    ~SyntheticInput() {
        if (hook) ImGui::RemoveContextHook(context, hook);
        context->IO.ConfigInputTrickleEventQueue = oldTrickle;
    }
};
}

bool runAntiAliasingUiSmoke(rae::renderer::SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
    rae::editor::SceneEditor& editor, const rae::project::ProjectPaths& project,
    const std::filesystem::path& outputDirectory, std::string& error) {
    using namespace rae;
    const auto output = std::filesystem::absolute(outputDirectory);
    Json report{{"passed", false}, {"backend", renderer::mainRenderBackendKey(renderer.mainBackendSelection().effective)},
        {"checks", Json::array()}, {"screenshots", Json::array()},
        {"screenshotScope", "Actual main-window GPU output; Project Settings held in the main viewport"}};
    try {
        std::filesystem::create_directories(output);
        require(ui.isInitialized(), "The initialized editor UI is required.");
        const auto originalManifest = read(project.manifest);
        const auto originalJson = Json::parse(originalManifest);
        project::ProjectPaths loaded;
        require(project::loadProject(project.manifest, loaded, error), error);
        const auto originalRendering = loaded.rendering;
        ui.beginFrame(1.0f / 60.0f);
        auto* context = ImGui::GetCurrentContext();
        require(context != nullptr, "ImGui context is unavailable.");
        const auto* main = ImGui::GetMainViewport();
        const auto hwnd = static_cast<HWND>(main->PlatformHandleRaw ? main->PlatformHandleRaw : main->PlatformHandle);
        ui.endFrame(false);
        ContextScope contextScope(context);
        RECT client{};
        require(hwnd && GetClientRect(hwnd, &client) && client.right >= 840 && client.bottom >= 700,
            "The test surface requires at least 840 x 700 client pixels.");
        require(renderer.resize(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom), error), error);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        const auto oldTheme = imgui::currentTheme();
        const auto oldIni = context->IO.IniFilename;
        struct Cleanup {
            imgui::ImGuiLayer& ui;
            editor::SceneEditor& editor;
            imgui::EditorTheme theme;
            const char* ini;
            const std::filesystem::path& manifest;
            const std::string& original;
            ~Cleanup() {
                editor.closeProjectSettings();
                imgui::setTheme(theme);
                ui.setIniFilename(ini);
                // Tests run in a disposable project; also restore its input for
                // repeatable independent suites in the same process.
                std::ofstream restore(manifest, std::ios::binary | std::ios::trunc);
                restore << original;
            }
        } cleanup{ui, editor, oldTheme, oldIni, project.manifest, originalManifest};
        ui.setIniFilename(nullptr);
        editor.resetLayout();
        editor.closeDiagnostics();
        SyntheticInput input(context);
        renderer::SceneSettings settings;
        settings.hardwareRayTracing = originalRendering.hardwareRayTracing;
        settings.hardwareRayTracingReflections = originalRendering.hardwareRayTracingReflections;
        settings.hardwareRayTracingShadows = originalRendering.hardwareRayTracingShadows;
        settings.globalIllumination = originalRendering.globalIllumination;
        settings.antiAliasing = originalRendering.antiAliasing;
        editor.applyStartupCamera(settings);
        const auto shaderRevision = renderer.shaderRevision();
        const auto startupBackend = renderer.mainBackendSelection().requested;
        std::string visibleText;
        const auto frame = [&](const char* evidence = nullptr) {
            diagnostics::beginFrame();
            struct EndTrace { ~EndTrace() { diagnostics::endFrame(); } } trace;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                require(message.message != WM_QUIT, "The test window unexpectedly quit.");
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            ui.beginFrame(.12f);
            try {
                settings.viewportWidth = settings.viewportHeight = 0;
                settings.editorViewport = false; settings.viewportVisible = true;
                bool vsync = false, screenshot = false;
                ImGui::LogToBuffer(0);
                editor.draw(settings, renderer, vsync, screenshot);
                visibleText = context->LogBuffer.c_str();
                ImGui::LogFinish();
                require(renderer.render(settings, editor.world(), editor.assets(), error, editor.materialPreview()), error);
                ui.endFrame();
            } catch (...) {
                if (context->WithinFrameScope) {
                    if (context->LogEnabled) ImGui::LogFinish();
                    ui.endFrame(false);
                }
                throw;
            }
            if (evidence) {
                const auto image = output / (std::string(evidence) + ".bmp");
                require(renderer.saveScreenshot(image.c_str(), error), error);
                require(std::filesystem::file_size(image) > 54, "Screenshot has no pixels.");
                write(output / (std::string(evidence) + ".txt"), visibleText);
                report["screenshots"].push_back(image.filename().string());
            }
            require(renderer.present(false, error), error);
        };
        editor.showProjectSettings(); frame(); frame();
        const auto settingsWindow = [&]() {
            auto* window = ImGui::FindWindowByName("Project Settings");
            require(window && window->Active, "Project Settings is not active.");
            return window;
        };
        const auto contentWindow = [&]() {
            for (auto* window : context->Windows)
                if (window->Active && window->ParentWindow == settingsWindow() &&
                    std::string(window->Name).find("ProjectSettingsContent") != std::string::npos) return window;
            throw std::runtime_error("AA UI: Rendering content child is missing.");
        };
        const auto locate = [&](ImGuiWindow* window, ImGuiID id) {
            require(window != nullptr, "Missing widget window.");
            for (int attempt = 0; attempt < 7; ++attempt) {
                ImGui::FocusWindow(window);
                ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
                frame();
                if (context->NavId != id || !context->NavIdIsAlive) continue;
                auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
                for (auto* parent = window; parent; parent = parent->ParentWindow)
                    ImGui::ScrollToRectEx(parent, rect, ImGuiScrollFlags_KeepVisibleEdgeY);
                frame();
                rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
                if (context->NavId == id && context->NavIdIsAlive && window->ClipRect.Contains(rect.GetCenter())) return rect;
            }
            throw std::runtime_error("AA UI: Cannot reveal actual control " + std::to_string(id));
        };
        const auto click = [&](ImVec2 position) {
            input.mouse = position; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); }; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }; frame(); frame();
        };
        const auto resizeSettings = [&](ImVec2 size) {
            auto* window = settingsWindow();
            ImGui::SetWindowDock(window, 0, ImGuiCond_Always);
            ImGui::SetWindowViewport(window, static_cast<ImGuiViewportP*>(ImGui::GetMainViewport()));
            const auto origin = ImGui::GetMainViewport()->Pos;
            ImGui::SetWindowPos("Project Settings", {origin.x + 24, origin.y + 42}, ImGuiCond_Always);
            ImGui::SetWindowSize("Project Settings", size, ImGuiCond_Always);
            frame(); frame();
            require(std::abs(settingsWindow()->Size.x - size.x) < 2, "Settings width did not match the requested size.");
        };
        const auto selectMethod = [&](const char* label, renderer::AntiAliasingMethod method) {
            auto* content = contentWindow();
            click(locate(content, content->GetID("##AntiAliasingMethod")).GetCenter());
            require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
                "Anti-aliasing combo did not open.");
            auto* popup = context->OpenPopupStack.back().Window;
            const auto item = ImHashStr(label, 0, popup->GetID(static_cast<int>(method)));
            click(locate(popup, item).GetCenter());
            require(settings.antiAliasing.method == method && renderer.antiAliasingStatus().requested == method,
                "The real combo did not update CPU and renderer AA requests.");
            require(renderer.shaderRevision() == shaderRevision, "Changing AA recompiled the material shader.");
        };
        resizeSettings({760, 650});
        selectMethod("Off", renderer::AntiAliasingMethod::Disabled);
        require(renderer.antiAliasingStatus().effective == renderer::AntiAliasingMethod::Disabled,
            "Off still executes anti-aliasing.");
        selectMethod("FXAA", renderer::AntiAliasingMethod::FXAA);
        require(renderer.antiAliasingStatus().effective == renderer::AntiAliasingMethod::FXAA,
            "FXAA request did not execute spatial AA.");
        selectMethod("TAA", renderer::AntiAliasingMethod::TAA);
        const auto taa = renderer.antiAliasingStatus();
        require(taa.effective == renderer::AntiAliasingMethod::TAA || !taa.reason.empty(),
            "TAA fallback has no explanation.");
        report["checks"].push_back("Actual Off / FXAA / TAA combo updates renderer requests without shader compilation");
        auto* content = contentWindow();
        const auto slider = locate(content, content->GetID("##TemporalCurrentWeight"));
        input.mouse = {slider.Min.x + slider.GetWidth() * .25f, slider.GetCenter().y}; frame();
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); }; frame();
        const auto pressedWeight = settings.antiAliasing.temporalCurrentWeight;
        input.mouse.x = slider.Min.x + slider.GetWidth() * .7f; frame();
        const auto draggedWeight = settings.antiAliasing.temporalCurrentWeight;
        require(draggedWeight > pressedWeight && editor.projectSettingsDirty(),
            "Slider dragging did not update the unsaved AA weight while held.");
        require(renderer.shaderRevision() == shaderRevision && renderer.antiAliasingStatus().requested == renderer::AntiAliasingMethod::TAA,
            "Live slider editing changed the shader revision or requested method.");
        input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); }; frame(); frame();
        report["pressedWeight"] = pressedWeight; report["draggedWeight"] = draggedWeight;
        report["effectiveTaaRequest"] = renderer::antiAliasingMethodName(renderer.antiAliasingStatus().effective);
        report["checks"].push_back("Weight changes while mouse is held; CPU draft and renderer request stay live");
        const auto revealSlider = [&]() {
            auto* child = contentWindow();
            const auto rect = locate(child, child->GetID("##TemporalCurrentWeight"));
            require(rect.Min.x >= child->ClipRect.Min.x && rect.Max.x <= child->ClipRect.Max.x,
                "Weight slider is horizontally clipped.");
        };
        imgui::setTheme(imgui::EditorTheme::Black); revealSlider(); frame("aa-settings-black");
        imgui::setTheme(imgui::EditorTheme::DarkBlue); revealSlider(); frame("aa-settings-darkblue");
        resizeSettings({560, 600}); revealSlider(); frame("aa-settings-narrow");
        require(visibleText.find("Current frame weight") != std::string::npos &&
            visibleText.find("Effective:") != std::string::npos, "AA controls or status are absent in the narrow layout.");
        auto* window = settingsWindow();
        click(locate(window, window->GetID("Save project settings")).GetCenter());
        require(!editor.projectSettingsDirty(), "Save did not clear the AA dirty state.");
        require(project::loadProject(project.manifest, loaded, error), error);
        auto expected = originalRendering;
        expected.antiAliasing = settings.antiAliasing;
        require(loaded.rendering == expected, "AA save lost the selected method/weight or altered backend/GI requests.");
        const auto savedJson = Json::parse(read(project.manifest));
        for (auto it = originalJson.begin(); it != originalJson.end(); ++it)
            if (it.key() != "rendering") require(savedJson.at(it.key()) == it.value(), "AA save changed unrelated project metadata.");
        require(renderer.mainBackendSelection().requested == startupBackend && renderer.shaderRevision() == shaderRevision,
            "Saving AA changed the running backend or shader.");
        frame("aa-settings-saved");
        report["checks"].push_back("Save via actual button round-trips AA and preserves backend, GI and project metadata");
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation errors.");
        report["passed"] = true;
        write(output / "aa-ui-report.json", report.dump(2) + '\n');
        error.clear();
        std::cout << "PASS: AA Project Settings real combo, live weight, save and both-theme screenshots.\n";
        return true;
    } catch (const std::exception& failure) {
        error = failure.what(); report["failure"] = error;
        try { std::filesystem::create_directories(output); write(output / "aa-ui-report.json", report.dump(2) + '\n'); } catch (...) {}
        return false;
    }
}
