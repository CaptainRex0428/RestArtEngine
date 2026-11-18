#include "InspectorUiSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <RestArtEngine/Core/RuntimeDiagnostics.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/EditorTheme.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
using Json = nlohmann::json;
constexpr const char* diagnosticName = "Diagnostics##RestArtEngine.Diagnostics";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Inspector UI: " + message);
}

struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};

struct SyntheticInput {
    ImGuiContext* context = nullptr;
    ImGuiID hook = 0;
    ImVec2 mouse{-100, -100};
    std::function<void(ImGuiIO&)> events;
    bool previousTrickle = false;
    explicit SyntheticInput(ImGuiContext* target) : context(target), previousTrickle(target->IO.ConfigInputTrickleEventQueue) {
        context->IO.ConfigInputTrickleEventQueue = false;
        ImGuiContextHook callback;
        callback.Type = ImGuiContextHookType_NewFramePre;
        callback.UserData = this;
        callback.Callback = [](ImGuiContext* ctx, ImGuiContextHook* activeHook) {
            auto& input = *static_cast<SyntheticInput*>(activeHook->UserData);
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
            if (input.events) { auto apply = std::move(input.events); apply(ctx->IO); }
        };
        hook = ImGui::AddContextHook(context, &callback);
    }
    ~SyntheticInput() {
        if (hook) ImGui::RemoveContextHook(context, hook);
        context->IO.ConfigInputTrickleEventQueue = previousTrickle;
    }
};

Json actorSnapshot(rae::editor::SceneEditor& editor) {
    Json actors = Json::array();
    for (const auto& actor : editor.world().actors) {
        actors.push_back({{"id", actor.id}, {"name", actor.name}, {"visible", actor.visible},
            {"position", std::array{actor.transform.position[0], actor.transform.position[1], actor.transform.position[2]}},
            {"rotation", std::array{actor.transform.rotation[0], actor.transform.rotation[1], actor.transform.rotation[2]}},
            {"scale", std::array{actor.transform.scale[0], actor.transform.scale[1], actor.transform.scale[2]}},
            {"mesh", actor.mesh.meshId}, {"material", actor.mesh.materialId}, {"overrides", actor.mesh.materialOverrides},
            {"isLight", actor.light.has_value()}, {"isCamera", actor.camera.has_value()}});
    }
    return actors;
}
} // namespace

bool runInspectorUiSmoke(rae::renderer::SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
    rae::editor::SceneEditor& editor, const std::filesystem::path& outputDirectory, std::string& error) {
    using namespace rae;
    try {
        require(ui.isInitialized(), "An initialized ImGui layer is required.");
        const auto output = std::filesystem::absolute(outputDirectory);
        std::filesystem::create_directories(output);
        ui.beginFrame(1.0f / 60.0f);
        auto* context = ImGui::GetCurrentContext();
        require(context != nullptr, "The engine ImGui context is missing.");
        const auto* main = ImGui::GetMainViewport();
        const auto hwnd = static_cast<HWND>(main->PlatformHandleRaw ? main->PlatformHandleRaw : main->PlatformHandle);
        ui.endFrame(false);
        require(hwnd && IsWindow(hwnd), "The main platform window is unavailable.");
        RECT client{};
        require(GetClientRect(hwnd, &client) && client.right >= 840 && client.bottom >= 700,
            "The UI fixture needs a main client area of at least 840 x 700.");
        require(renderer.resize(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom), error), error);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);

        ContextScope contextScope(context);
        const auto oldTheme = imgui::currentTheme();
        const auto oldOptions = renderer.frameCaptureOptions();
        const bool oldDiagnostics = renderer.diagnosticsEnabled();
        const char* oldIni = context->IO.IniFilename;
        struct Cleanup {
            renderer::SceneRenderer& renderer;
            imgui::ImGuiLayer& ui;
            editor::SceneEditor& editor;
            imgui::EditorTheme theme;
            renderer::FrameCaptureOptions options;
            bool diagnostics;
            const char* ini;
            ~Cleanup() {
                diagnostics::profiler().stop();
                editor.closeDiagnostics();
                renderer.setDiagnosticsEnabled(diagnostics);
                try { renderer.setFrameCaptureOptions(options); } catch (...) {}
                imgui::setTheme(theme);
                ui.setIniFilename(ini);
            }
        } cleanup{renderer, ui, editor, oldTheme, oldOptions, oldDiagnostics, oldIni};
        ui.setIniFilename(nullptr); // No persistent layout changes from this suite.
        editor.resetLayout();
        SyntheticInput input(context);
        renderer::SceneSettings settings;
        editor.applyStartupCamera(settings);
        std::string renderedText;
        Json report{{"backend", renderer::mainRenderBackendKey(renderer.mainBackendSelection().effective)},
            {"screenshots", Json::array()}, {"screenshotScope", "Actual main-window GPU backbuffer; no secondary-window screenshot claim"}};

        const auto frame = [&](const std::string& evidenceName = std::string{}) {
            struct TraceFrame {
                TraceFrame() { diagnostics::beginFrame(); }
                ~TraceFrame() { diagnostics::endFrame(); }
            } traceFrame;
            diagnostics::TraceScope frameTrace("Inspector UI frame", "Validation");
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                require(message.message != WM_QUIT, "The main window unexpectedly quit.");
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            // Fixed input time separates clicks, while TraceProfiler still uses
            // actual monotonic wall time for its CPU measurements.
            ui.beginFrame(.12f);
            try {
                settings.viewportWidth = settings.viewportHeight = 0;
                settings.editorViewport = false;
                settings.viewportVisible = true;
                bool vsync = false, screenshot = false;
                ImGui::LogToBuffer(0); // Do not force-open trees merely to log text.
                {
                    diagnostics::TraceScope editorTrace("Inspector UI editor", "Editor");
                    editor.draw(settings, renderer, vsync, screenshot);
                }
                renderedText = context->LogBuffer.c_str();
                ImGui::LogFinish();
                {
                    diagnostics::TraceScope submitTrace("Inspector UI submit", "Renderer");
                    require(renderer.render(settings, editor.world(), editor.assets(), error, editor.materialPreview()), error);
                }
                ui.endFrame();
            } catch (...) {
                if (context->WithinFrameScope) {
                    if (context->LogEnabled) ImGui::LogFinish();
                    ui.endFrame(false);
                }
                throw;
            }
            if (!evidenceName.empty()) {
                const auto image = output / (evidenceName + ".bmp");
                require(renderer.saveScreenshot(image.c_str(), error), error);
                require(std::filesystem::file_size(image) > 54, "A main-window screenshot has no pixel payload.");
                std::ofstream text(output / (evidenceName + ".txt"), std::ios::binary);
                text << renderedText; text.close(); require(text.good(), "Cannot save visible UI text.");
                report["screenshots"].push_back(evidenceName + ".bmp");
            }
            require(renderer.present(false, error), error);
        };
        const auto diagnosticWindow = [&]() {
            auto* window = ImGui::FindWindowByName(diagnosticName);
            require(window && window->Active, "The actual Diagnostics window is not active.");
            return window;
        };
        const auto tabSeed = [&]() {
            auto* window = diagnosticWindow();
            const auto* bar = context->TabBars.GetByKey(ImHashStr("Diagnostics tabs", 0, window->ID));
            require(bar && bar->SelectedTabId, "The Diagnostics tab bar has no selected tab.");
            return bar->SelectedTabId;
        };
        const auto locate = [&](ImGuiWindow* window, ImGuiID id) {
            require(window != nullptr, "A widget's actual ImGui window is missing.");
            for (int attempt = 0; attempt < 6; ++attempt) {
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
            throw std::runtime_error("Inspector UI: Could not reveal widget " + std::to_string(id) + " in " + window->Name);
        };
        const auto click = [&](ImVec2 point) {
            input.mouse = point; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); }; frame();
            input.events = [](ImGuiIO& io) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); }; frame();
        };
        const auto clickControl = [&](const char* label) {
            click(locate(diagnosticWindow(), ImHashStr(label, 0, tabSeed())).GetCenter());
        };
        const auto pressKey = [&](ImGuiKey key) {
            input.events = [key](ImGuiIO& io) { io.AddKeyEvent(key, true); }; frame();
            input.events = [key](ImGuiIO& io) { io.AddKeyEvent(key, false); }; frame();
        };
        const auto show = [&](editor::DiagnosticsTab tab, ImVec2 size) {
            editor.showDiagnostics(tab); frame(); frame();
            ImGui::SetWindowDock(diagnosticWindow(), 0, ImGuiCond_Always);
            ImGui::SetWindowViewport(diagnosticWindow(), static_cast<ImGuiViewportP*>(ImGui::GetMainViewport()));
            const auto origin = ImGui::GetMainViewport()->Pos;
            ImGui::SetWindowPos(diagnosticName, {origin.x + 24, origin.y + 48}, ImGuiCond_Always);
            ImGui::SetWindowSize(diagnosticName, size, ImGuiCond_Always);
            ImGui::SetScrollY(diagnosticWindow(), 0);
            frame(); frame();
            require(std::abs(diagnosticWindow()->Size.x - size.x) < 2 && std::abs(diagnosticWindow()->Size.y - size.y) < 2,
                "The diagnostic window did not use the requested real test size.");
        };
        // Establish stable docking/targets, then capture real scene resources.
        editor.showDiagnostics(editor::DiagnosticsTab::Profiler);
        for (int index = 0; index < 4; ++index) frame();
        const auto actorsBefore = actorSnapshot(editor);
        const auto selectionBefore = editor.selection();
        const auto undoSize = editor.undoStack().size(), undoCursor = editor.undoStack().cursor();
        renderer.setFrameCaptureOptions({});
        renderer.requestFrameCapture();
        for (int index = 0; index < 3; ++index) frame();
        require(renderer.frameCapture().valid && !renderer.frameCapture().resources.empty(),
            "Real main-scene capture did not produce inspectable resources.");
        const auto frozenFrame = renderer.frameCapture().frameId;

        show(editor::DiagnosticsTab::Profiler, {760, 660});
        if (diagnostics::profiler().isRecording()) diagnostics::profiler().stop();
        clickControl("Record CPU trace");
        require(diagnostics::profiler().isRecording(), "The Record CPU trace widget did not start recording.");
        for (int index = 0; index < 4; ++index) frame();
        clickControl("Stop CPU trace");
        require(!diagnostics::profiler().isRecording() && diagnostics::profiler().snapshot().eventCount > 0,
            "The Stop CPU trace widget did not retain CPU events.");
        clickControl("CPU timeline and scope aggregates");
        frame("profiler_timeline_interaction");
        require(renderedText.find("CPU thread timeline") != std::string::npos,
            "Real timeline header expansion did not show CPU tracks.");

        diagnostics::runtimeDiagnostics().log(diagnostics::MessageSeverity::Info, "Inspector UI fixture", "INSPECTOR_INFO_ONLY_TOKEN");
        diagnostics::runtimeDiagnostics().log(diagnostics::MessageSeverity::Error, "Inspector UI fixture",
            "INSPECTOR_EXPECTED_ERROR_TOKEN: controlled fixture message, not an application failure");

        for (const auto theme : {imgui::EditorTheme::Black, imgui::EditorTheme::DarkBlue}) {
            imgui::setTheme(theme);
            const std::string suffix = theme == imgui::EditorTheme::Black ? "black" : "blue";
            for (const auto size : {ImVec2{760, 660}, ImVec2{300, 640}}) {
                const std::string width = size.x > 500 ? "wide" : "narrow";
                show(editor::DiagnosticsTab::Profiler, size);
                frame("profiler_" + suffix + "_" + width);
                require(renderedText.find("CPU scopes: frame") != std::string::npos,
                    "The Profiler has no retained CPU frame identity.");
                show(editor::DiagnosticsTab::FrameInspector, size);
                frame("inspector_" + suffix + "_" + width);
                require(renderedText.find("Captured frame") != std::string::npos && renderedText.find("Metadata export contains no images") != std::string::npos,
                    "The Frame Inspector lost frozen identity or export scope text.");
                clickControl("Read pixel");
                frame("inspector_pixel_" + suffix + "_" + width);
                require(renderedText.find("RGBA =") != std::string::npos,
                    "The actual captured-pixel control did not display a GPU numeric sample.");
                show(editor::DiagnosticsTab::Runtime, size);
                frame("runtime_" + suffix + "_" + width);
                require(renderedText.find("Runtime and graphics validation") != std::string::npos &&
                    renderedText.find("process memory") != std::string::npos,
                    "Runtime status/memory scope text is missing.");
                // Reveal the actual message table, including on a 300px tool.
                locate(diagnosticWindow(), ImHashStr("##Runtime filter", 0, tabSeed()));
                ImGui::SetScrollY(diagnosticWindow(), diagnosticWindow()->Scroll.y + 150); frame();
                frame("runtime_messages_" + suffix + "_" + width);
                require(renderedText.find("INSPECTOR_EXPECTED_ERROR_TOKEN") != std::string::npos,
                    "Runtime UI did not show the recorded structured error.");
            }
        }

        show(editor::DiagnosticsTab::FrameInspector, {760, 660});
        clickControl("Capture settings");
        frame("inspector_capture_settings");
        require(renderedText.find("Capture boundary") != std::string::npos && renderedText.find("Resource budget") != std::string::npos,
            "Capture settings did not expand through actual widget input.");
        clickControl("Capture settings");
        locate(diagnosticWindow(), ImHashStr("##Event filter", 0, tabSeed()));
        ImGui::SetScrollY(diagnosticWindow(), diagnosticWindow()->Scroll.y + 150); frame();
        const auto& events = renderer.frameCapture().events;
        const auto detailed = std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.detailedState && !event.constants.empty();
        });
        require(detailed != events.end(), "The fixture has no actual detailed Draw with constant bytes.");
        const auto eventIndex = static_cast<int>(std::distance(events.begin(), detailed));
        const auto tableId = ImHashStr("Draw events", 0, tabSeed());
        auto* table = context->Tables.GetByKey(tableId);
        require(table && table->InnerWindow, "The actual Draw event table is missing.");
        const auto rowSeed = ImHashData(&eventIndex, sizeof(eventIndex), tableId);
        click(locate(table->InnerWindow, ImHashStr(detailed->pass.c_str(), 0, rowSeed)).GetCenter());
        frame("inspector_selected_draw");
        require(renderedText.find("Selected draw state") != std::string::npos && renderedText.find("Vertex shader:") != std::string::npos,
            "Clicking a recorded Draw did not show its actual pipeline and Shader state.");
        clickControl("Constant buffer contents");
        const int firstConstant = 0;
        const auto constantParent = ImHashStr("Constant buffer contents", 0, tabSeed());
        const auto constantSeed = ImHashData(&firstConstant, sizeof(firstConstant), constantParent);
        click(locate(diagnosticWindow(), ImHashStr("##Constant", 0, constantSeed)).GetCenter());
        ImGui::SetScrollY(diagnosticWindow(), diagnosticWindow()->Scroll.y + 130); frame();
        frame("inspector_constant_bytes");
        require(renderedText.find("Float32 interpretation") != std::string::npos && renderedText.find("Raw hex") != std::string::npos,
            "Actual constant-buffer expansion did not display stored bytes.");
        clickControl("Frozen Shader sources");
        const auto sourceSeed = ImHashStr("Material Shader source", 0, tabSeed());
        click(locate(diagnosticWindow(), ImHashStr("Material Shader source", 0, sourceSeed)).GetCenter());
        ImGui::SetScrollY(diagnosticWindow(), diagnosticWindow()->Scroll.y + 130); frame();
        frame("inspector_frozen_shader");
        require(renderedText.find("not fully expanded include trees") != std::string::npos,
            "Frozen source UI lost its explicit source/bytecode debugging boundary.");

        if (renderer.mainBackendSelection().effective != renderer::MainRenderBackend::D3D12) {
            const auto partial = std::find_if(events.begin(), events.end(), [](const auto& event) {
                return !event.detailedState && event.command.find("(native)") != std::string::npos;
            });
            require(partial != events.end(), "The D3D11 fixture did not retain a native partial Draw.");
            // Opening the constant table can grow ImGui's table pool. Pointers
            // into that pool are not stable across those intervening frames.
            const auto* partialTable = context->Tables.GetByKey(tableId);
            require(partialTable && partialTable->InnerWindow, "The Draw table disappeared before partial inspection.");
            auto* partialWindow = partialTable->InnerWindow;
            const auto partialIndex = static_cast<int>(std::distance(events.begin(), partial));
            const auto partialSeed = ImHashData(&partialIndex, sizeof(partialIndex), tableId);
            click(locate(partialWindow, ImHashStr(partial->pass.c_str(), 0, partialSeed)).GetCenter());
            frame("inspector_partial_draw");
            require(renderedText.find("Partial state reason:") != std::string::npos &&
                renderedText.find(partial->pipelineState) != std::string::npos &&
                renderedText.find("Retained partial data:") != std::string::npos,
                "Partial Draw inspection must expose its recorded omission reason and available data.");
        }

        show(editor::DiagnosticsTab::Runtime, {760, 660});
        ImGui::FocusWindow(diagnosticWindow());
        const auto image = editor.cameraImageRect();
        input.mouse = {image[2] - 12, (image[1] + image[3]) * .5f}; frame();
        const auto cameraBefore = settings;
        pressKey(ImGuiKey_F); pressKey(ImGuiKey_Delete);
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_D, true); }; frame();
        input.events = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_D, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); }; frame();
        require(actorSnapshot(editor) == actorsBefore && editor.selection() == selectionBefore &&
            editor.undoStack().size() == undoSize && editor.undoStack().cursor() == undoCursor,
            "Diagnostic interaction changed scene Actors, selection or Undo history.");
        require(settings.cameraYaw == cameraBefore.cameraYaw && settings.cameraPitch == cameraBefore.cameraPitch &&
            settings.cameraDistance == cameraBefore.cameraDistance && std::equal(std::begin(settings.cameraTarget),
                std::end(settings.cameraTarget), std::begin(cameraBefore.cameraTarget)),
            "A focused diagnostics tool allowed camera shortcuts to reach the live scene.");
        require(renderer.frameCapture().frameId == frozenFrame, "Inspection unexpectedly replaced the frozen frame.");
        require(renderer.validationErrorCount() == 0, "The actual UI run produced graphics validation errors.");
        report["passed"] = true;
        report["frozenFrame"] = frozenFrame;
        report["actorCount"] = editor.world().actors.size();
        report["undoUnchanged"] = true;
        report["captureAndPixelWidgets"] = true;
        report["realDrawAndConstantSelection"] = true;
        report["keyboardIsolation"] = true;
        report["validationErrors"] = renderer.validationErrorCount();
        report["limits"] = "Saved actual main-window images require visual review; no secondary GUI pixel, native file-dialog automation, CPU debugger or command-replay claim.";
        std::ofstream manifest(output / "inspector-ui-report.json", std::ios::binary);
        manifest << report.dump(2); manifest.close();
        require(manifest.good(), "Cannot write the UI evidence report.");
        std::cout << "PASS: Inspector UI actual three tabs, both themes and widths, CPU timeline, frozen pixels, Draw/constants/source widgets and keyboard isolation; graphics errors=0.\n";
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
