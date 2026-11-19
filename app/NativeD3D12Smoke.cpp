#include "NativeD3D12Smoke.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Json = nlohmann::json;
using Pixels = std::vector<std::uint8_t>;
constexpr std::uint32_t width = 192, height = 144;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Native D3D12 smoke: " + message);
}

struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};

std::size_t visiblePixels(const Pixels& image) {
    std::size_t count = 0;
    for (std::size_t p = 0; p < image.size(); p += 4)
        if (image[p] > 5 || image[p + 1] > 5 || image[p + 2] > 5) ++count;
    return count;
}

std::size_t changedPixels(const Pixels& first, const Pixels& second, int tolerance = 2) {
    require(first.size() == second.size(), "Comparison images have different sizes.");
    std::size_t count = 0;
    for (std::size_t p = 0; p < first.size(); p += 4)
        if (std::abs(int(first[p]) - int(second[p])) > tolerance ||
            std::abs(int(first[p + 1]) - int(second[p + 1])) > tolerance ||
            std::abs(int(first[p + 2]) - int(second[p + 2])) > tolerance) ++count;
    return count;
}

double meanError(const Pixels& first, const Pixels& second) {
    require(first.size() == second.size() && !first.empty(), "Invalid comparison images.");
    double total = 0;
    for (std::size_t p = 0; p < first.size(); p += 4)
        for (unsigned channel = 0; channel < 3; ++channel)
            total += std::abs(int(first[p + channel]) - int(second[p + channel]));
    return total / (first.size() / 4 * 3);
}

void savePixels(const std::filesystem::path& path, const Pixels& pixels,
    std::uint32_t imageWidth, std::uint32_t imageHeight) {
    auto bgra = pixels;
    for (std::size_t p = 0; p < bgra.size(); p += 4) std::swap(bgra[p], bgra[p + 2]);
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(imageWidth);
    info.biHeight = -static_cast<LONG>(imageHeight);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    stream.close();
    require(stream.good(), "Cannot write evidence image.");
}

Pixels read(rae::renderer::SceneRenderer& renderer, rae::renderer::PreviewSlot slot,
    std::uint32_t expectedWidth, std::uint32_t expectedHeight) {
    Pixels pixels;
    std::uint32_t actualWidth = 0, actualHeight = 0;
    std::string error;
    require(renderer.readPreviewPixels(slot, pixels, actualWidth, actualHeight, error), error);
    require(actualWidth == expectedWidth && actualHeight == expectedHeight &&
        pixels.size() == static_cast<std::size_t>(actualWidth) * actualHeight * 4,
        "GPU preview readback dimensions do not match the requested image.");
    require(visiblePixels(pixels) > 100, "GPU output has no visible scene geometry.");
    return pixels;
}

bool hasPass(const rae::renderer::RenderPipeline& pipeline, rae::renderer::RenderPassKind kind) {
    return std::any_of(pipeline.passes.begin(), pipeline.passes.end(),
        [kind](const auto& pass) { return pass.kind == kind; });
}

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        require(message.message != WM_QUIT, "The platform-window fixture unexpectedly quit the application.");
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void runUiSmoke(rae::renderer::SceneRenderer& renderer, rae::imgui::ImGuiLayer& ui,
    const rae::scene::World& world, const rae::assets::AssetLibrary& assets,
    const std::filesystem::path& directory, Json& report) {
    using namespace rae;
    using Slot = renderer::PreviewSlot;
    require(ui.isInitialized(), "Native UI smoke requires an initialized ImGui layer.");
    HWND mainWindow = nullptr;
    {
        FrameScope frame;
        ui.beginFrame(1.0f / 60);
        require((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0,
            "Native UI smoke requires platform windows to be enabled at initialization.");
        const auto* main = ImGui::GetMainViewport();
        mainWindow = static_cast<HWND>(main->PlatformHandleRaw ? main->PlatformHandleRaw : main->PlatformHandle);
        ui.endFrame(false);
    }
    require(mainWindow && IsWindow(mainWindow), "The UI has no live main HWND.");
    // The earlier render tests intentionally resized only the swap chain.
    // Match it to the real UI surface before submitting ImGui's main draw data.
    RECT client{};
    require(GetClientRect(mainWindow, &client) && client.right > 0 && client.bottom > 0,
        "Cannot obtain main UI client dimensions.");
    std::string error;
    require(renderer.resize(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom), error), error);
    struct Cleanup {
        renderer::SceneRenderer& renderer;
        imgui::ImGuiLayer& ui;
        HWND mainWindow;
        bool complete = false;
        ~Cleanup() {
            if (complete) return;
            try {
                ui.endFrame(false);
                if (IsIconic(mainWindow)) ShowWindow(mainWindow, SW_RESTORE);
                // Stop submitting the fixture windows and let ImGui retire
                // their native swap chains before the caller shuts down.
                for (unsigned frame = 0; frame < 3; ++frame) {
                    FrameScope scope;
                    ui.beginFrame(1.0f / 60);
                    ui.endFrame(true);
                    std::string ignored;
                    (void)renderer.present(false, ignored);
                }
            } catch (...) {}
        }
    } cleanup{renderer, ui, mainWindow};
    auto material = assets.materials.front();
    material.grid = false;
    material.metallic = 0;
    material.baseColor[0] = .9f; material.baseColor[1] = .02f; material.baseColor[2] = .03f;
    unsigned previewSize = 128;
    ImGuiViewport* toolViewport = nullptr;
    bool toolOpen = true;
    renderer::SceneSettings sceneSettings;
    sceneSettings.path = renderer::RenderPath::Deferred;
    sceneSettings.shadowMethod = renderer::ShadowMethod::Disabled;
    sceneSettings.editorViewport = true;
    const auto id = [](void* value) { return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(value)); };
    const auto frame = [&](bool renderMain, bool showTool, bool replaceBorrowed, const char* screenshot = nullptr) {
        pumpMessages();
        FrameScope scope;
        ui.beginFrame(1.0f / 60);
        auto* main = ImGui::GetMainViewport();
        require(renderer.renderMaterialPreview(material, previewSize, error, Slot::MaterialEditor), error);
        const auto sceneImage = id(renderer.sceneTexture());
        const auto previewImage = id(renderer.previewTexture(Slot::MaterialEditor));
        require(sceneImage != 0 && previewImage != 0, "Native UI image descriptors are missing.");
        if (renderMain) {
            ImGui::SetNextWindowViewport(main->ID);
            ImGui::SetNextWindowPos({main->Pos.x + 12, main->Pos.y + 48}, ImGuiCond_Always);
            ImGui::SetNextWindowSize({360, 370}, ImGuiCond_Always);
            if (ImGui::Begin("Native D3D12 main smoke", nullptr,
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted("Native scene and material preview");
                ImGui::Image(sceneImage, {192, 144});
                ImGui::Image(previewImage, {128, 128});
            }
            ImGui::End();
        }
        if (showTool && toolOpen) {
            ImGuiWindowClass independent;
            independent.ParentViewportId = 0; // Remains visible when the main owner is minimized.
            independent.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&independent);
            ImGui::SetNextWindowPos({main->Pos.x + main->Size.x + 32, main->Pos.y + 64}, ImGuiCond_Once);
            ImGui::SetNextWindowSize({320, 350}, ImGuiCond_Always);
            if (ImGui::Begin("Native D3D12 detached smoke", &toolOpen,
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextUnformatted("Independent material tool");
                ImGui::Image(previewImage, {160, 160});
                ImGui::Image(sceneImage, {128, 96});
            }
            toolViewport = ImGui::GetWindowViewport();
            ImGui::End();
        }
        if (replaceBorrowed) {
            // Both ImGui draw lists already borrow the previous descriptor.
            // Replacing the scratch target and the same preview slot must keep
            // those resources alive through main AND secondary queue submits.
            require(renderer.ensureSceneTargetSize(renderer.sceneTextureWidth() + 32,
                renderer.sceneTextureHeight() + 16, error), error);
            require(renderer.renderMaterialPreview(material, 144, error, Slot::MaterialEditor), error);
            previewSize = 160;
            require(renderer.renderMaterialPreview(material, previewSize, error, Slot::MaterialEditor), error);
        }
        if (renderMain) {
            sceneSettings.viewportWidth = static_cast<int>(renderer.sceneTextureWidth());
            sceneSettings.viewportHeight = static_cast<int>(renderer.sceneTextureHeight());
            require(renderer.render(sceneSettings, world, assets, error), error);
        }
        ui.endFrame(renderMain);
        if (renderMain) {
            if (screenshot) {
                const auto file = directory / screenshot;
                require(renderer.saveScreenshot(file.c_str(), error), error);
            }
            require(renderer.present(false, error), error);
        } else {
            renderer.releaseRetiredSceneTargets();
        }
    };
    for (unsigned warmup = 0; warmup < 3; ++warmup) frame(true, true, false);
    require(toolViewport && toolViewport->PlatformHandle && toolViewport->RendererUserData,
        "The detached tool did not create native platform and D3D12 renderer resources.");
    const auto toolWindow = static_cast<HWND>(toolViewport->PlatformHandleRaw
        ? toolViewport->PlatformHandleRaw : toolViewport->PlatformHandle);
    require(IsWindow(toolWindow) && toolWindow != mainWindow && IsWindowVisible(toolWindow),
        "The detached tool has no visible independent HWND.");
    RECT mainBounds{}, toolBounds{};
    require(GetWindowRect(mainWindow, &mainBounds), "Cannot obtain main platform-window bounds.");
    // A small desktop can clamp a newly created ImGui window to its monitor.
    // Exercise the engine's native move path to make the outside-main fixture
    // deterministic without changing the main window's user placement.
    require(SetWindowPos(toolWindow, nullptr, mainBounds.right + 32, mainBounds.top + 64,
        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
        "Cannot position the native tool outside the main window.");
    frame(true, true, false);
    require(GetWindowRect(toolWindow, &toolBounds) &&
        toolBounds.left > mainBounds.right, "The tool fixture was not placed outside the main window.");
    frame(true, true, true, "native-ui-main.bmp");
    require(renderer.validationErrorCount() == 0, "UI borrowing/replacement produced GPU validation errors.");

    ShowWindow(mainWindow, SW_MINIMIZE);
    require(IsIconic(mainWindow), "Could not minimize the main UI window.");
    material.baseColor[0] = .02f; material.baseColor[1] = .9f; material.baseColor[2] = .03f;
    frame(false, true, false);
    const auto green = read(renderer, Slot::MaterialEditor, 160, 160);
    material.baseColor[0] = .03f; material.baseColor[1] = .02f; material.baseColor[2] = .9f;
    frame(false, true, true);
    const auto blue = read(renderer, Slot::MaterialEditor, 160, 160);
    require(changedPixels(green, blue) > 100,
        "Material GPU preview did not update during minimized-main UI frames.");
    require(IsIconic(mainWindow) && IsWindow(toolWindow) && IsWindowVisible(toolWindow) &&
        ui.hasVisiblePlatformWindows() && toolViewport->RendererUserData,
        "Minimizing the main window hid or destroyed the independent tool.");
    require(renderer.validationErrorCount() == 0,
        "Minimized-main platform submission produced GPU state or descriptor-lifetime errors.");
    savePixels(directory / "minimized-material-preview.bmp", blue, 160, 160);
    (void)ui.collectGarbage(0);
    (void)renderer.collectGarbage(0);
    require(read(renderer, Slot::MaterialEditor, 160, 160) == blue,
        "Garbage collection changed a live minimized-tool preview.");

    ShowWindow(mainWindow, SW_RESTORE);
    frame(true, true, false);
    toolOpen = false;
    for (unsigned closing = 0; closing < 3; ++closing) frame(true, false, false);
    require(!IsIconic(mainWindow) && !IsWindow(toolWindow),
        "Restoring main/closing the detached tool did not complete its native lifetime.");
    cleanup.complete = true;
    report["ui"] = {{"nativeSecondaryWindow", true}, {"minimizedMainSubmission", true},
        {"borrowedImageReplacement", true}, {"minimizedPreviewChangedPixels", changedPixels(green, blue)},
        {"evidenceBoundary", "Native HWND/renderer lifetime, GPU preview readback and debug-layer checks. Main screenshot excludes the secondary window; no secondary UI pixel readback or visual acceptance."}};
}
} // namespace

bool runNativeD3D12Smoke(rae::renderer::SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, const std::filesystem::path& artifactDirectory, std::string& error) {
    using namespace rae;
    using Path = renderer::RenderPath;
    using Shadow = renderer::ShadowMethod;
    using Slot = renderer::PreviewSlot;
    error.clear();
    Json report{{"format", "RestArtEngine.NativeD3D12Smoke"}, {"version", 1},
        {"adapter", renderer.adapterName()}, {"passed", false}, {"checks", Json::array()},
        {"notCovered", {"depth of field", "selection outline", "Frame Inspector frozen resources",
            "DXR hardware preview on adapters without DXR 1.1"}}};
    const auto persist = [&] {
        std::ofstream stream(artifactDirectory / "native-d3d12-report.json", std::ios::trunc);
        stream << report.dump(2) << '\n';
        stream.close();
        require(stream.good(), "Cannot write smoke report.");
    };
    const auto pass = [&](const char* message) {
        report["checks"].push_back(message);
        std::cout << "PASS: native D3D12: " << message << '\n';
    };
    const auto oldSource = renderer.shaderSource();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    const bool oldDiagnostics = renderer.diagnosticsEnabled();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::string source;
        std::uint32_t width, height;
        bool diagnostics, done = false;
        void run() {
            std::string error;
            if (renderer.shaderSource() != source)
                require(renderer.applyShaderSource(source, error), "Restoring shader: " + error);
            require(renderer.resize(width, height, error), "Restoring dimensions: " + error);
            renderer.setDiagnosticsEnabled(diagnostics);
            renderer.clearFrameCapture();
            done = true;
        }
        ~Restore() { if (!done) try { run(); } catch (...) {} }
    } restore{renderer, oldSource, oldWidth, oldHeight, oldDiagnostics};
    try {
        std::filesystem::create_directories(artifactDirectory);
        require(renderer.mainBackendReady() &&
            renderer.mainBackendSelection().effective == renderer::MainRenderBackend::D3D12 &&
            !renderer.mainBackendSelection().fallback,
            "The smoke test requires an actual native D3D12 renderer without fallback.");
        require(renderer.graphicsDevice() != nullptr, "Missing active RHI device.");
        require(renderer.resize(320, 240, error), error);
        assets::AssetLibrary assets;
        assets.createDefaults();
        scene::World world;
        world.createDefaultScene(assets);
        world.blackBackground = true;
        renderer::SceneSettings settings;
        settings.path = Path::Forward;
        settings.shadowMethod = Shadow::Disabled;
        renderer.setDiagnosticsEnabled(true);
        const auto renderMain = [&](const char* screenshot = nullptr) {
            FrameScope frame;
            require(renderer.render(settings, world, assets, error), error);
            if (screenshot) {
                const auto path = artifactDirectory / screenshot;
                require(renderer.saveScreenshot(path.c_str(), error), error);
            }
            require(renderer.present(false, error), error);
        };
        const auto renderPreview = [&](const char* name = nullptr) {
            FrameScope frame;
            require(renderer.renderViewportPreview(settings, world, assets, nullptr,
                width, height, error, Slot::Viewport0), error);
            auto pixels = read(renderer, Slot::Viewport0, width, height);
            if (name) savePixels(artifactDirectory / (std::string(name) + ".bmp"), pixels, width, height);
            return pixels;
        };

        renderMain("forward-main.bmp");
        require(renderer.activePipeline().path == Path::Forward &&
            hasPass(renderer.activePipeline(), renderer::RenderPassKind::ForwardGeometry),
            "Forward geometry pass was not declared/executed.");
        const auto forward = renderPreview("forward");
        settings.path = Path::Deferred;
        renderMain("deferred-main.bmp");
        require(renderer.activePipeline().path == Path::Deferred &&
            hasPass(renderer.activePipeline(), renderer::RenderPassKind::GBuffer) &&
            hasPass(renderer.activePipeline(), renderer::RenderPassKind::DeferredLighting),
            "Deferred G-buffer/lighting passes are missing.");
        const auto deferred = renderPreview("deferred");
        report["forwardDeferredMeanError"] = meanError(forward, deferred);
        require(meanError(forward, deferred) < 8.0,
            "Forward and Deferred shading disagree substantially for the same scene.");
        require(renderer.diagnosticsFrame().drawCalls > 0 &&
            !renderer.diagnosticsFrame().passes.empty() &&
            renderer.diagnosticsFrame().gpuStatus != renderer::GpuTimingStatus::Unavailable,
            "Native D3D12 must retain CPU diagnostics and actual timestamp query availability.");
        require(renderer.geometryStatistics().sceneTriangles > 0 &&
            renderer.geometryStatistics().status == renderer::GeometrySampleStatus::Unavailable &&
            !renderer.geometryStatistics().statusMessage.empty(),
            "CPU topology must survive with an explicit unavailable GPU query status.");
        pass("Real Forward/Deferred GPU images and public screenshots; CPU diagnostics remain available.");

        settings.debugView = 8;
        const auto depth = renderPreview("linear-depth");
        require(changedPixels(deferred, depth) > 100, "Linear-depth output did not differ from lit shading.");
        settings.debugView = 0;
        struct ShadowCase { Shadow method; renderer::RenderFeature feature; const char* name; };
        const std::array shadowCases{
            ShadowCase{Shadow::ShadowMap, renderer::RenderFeature::ShadowMapPCF, "pcf"},
            ShadowCase{Shadow::ShadowMapPCSS, renderer::RenderFeature::ShadowMapPCSS, "pcss"},
            ShadowCase{Shadow::ShadowMapCSM2, renderer::RenderFeature::ShadowMapCSM2, "csm2"},
            ShadowCase{Shadow::GTAO, renderer::RenderFeature::AmbientOcclusionGTAO, "gtao"},
            ShadowCase{Shadow::ContactShadow, renderer::RenderFeature::ContactShadows, "contact"}};
        for (const auto& test : shadowCases) {
            settings.shadowMethod = test.method;
            renderMain();
            require(renderer.renderFeatures().isActive(test.feature) &&
                hasPass(renderer.activePipeline(), renderer::RenderPassKind::ShadowMap),
                std::string(test.name) + " was requested without an active shadow pipeline.");
            const auto image = renderPreview(test.name);
            const auto changed = changedPixels(deferred, image);
            report["shadowChangedPixels"][test.name] = changed;
            require(changed > 5, std::string(test.name) + " produced no visible shadow change from disabled.");
        }
        settings.shadowMethod = Shadow::Disabled;
        pass("Depth plus PCF, PCSS, CSM2, GTAO and contact shadows produce real GPU pixels.");

        auto material = assets.materials.front();
        material.grid = false;
        material.metallic = 0;
        material.baseColor[0] = .9f; material.baseColor[1] = .03f; material.baseColor[2] = .02f;
        Pixels materialRed, cameraFirst;
        {
            FrameScope frame;
            require(renderer.renderMaterialPreview(material, 128, error, Slot::MaterialEditor), error);
            materialRed = read(renderer, Slot::MaterialEditor, 128, 128);
            auto camera = renderer::resolveCamera(settings);
            require(renderer.renderCameraPreview(world, assets, camera, width, height, error), error);
            cameraFirst = read(renderer, Slot::Camera, width, height);
            camera.pose.position[0] += 1.3f;
            require(renderer.renderCameraPreview(world, assets, camera, width, height, error,
                1.0f, Slot::Viewport3), error);
            const auto cameraMoved = read(renderer, Slot::Viewport3, width, height);
            require(changedPixels(cameraFirst, cameraMoved) > 100, "Camera movement did not change visible geometry.");
            material.baseColor[0] = .02f; material.baseColor[1] = .85f; material.baseColor[2] = .04f;
            require(renderer.renderMaterialPreview(material, 128, error, Slot::AssetDetails), error);
            const auto materialGreen = read(renderer, Slot::AssetDetails, 128, 128);
            require(changedPixels(materialRed, materialGreen) > 100, "Material edit did not affect GPU preview.");
            require(read(renderer, Slot::MaterialEditor, 128, 128) == materialRed &&
                read(renderer, Slot::Camera, width, height) == cameraFirst,
                "Rendering another preview slot overwrote an existing image.");
            savePixels(artifactDirectory / "material-red.bmp", materialRed, 128, 128);
            savePixels(artifactDirectory / "material-green.bmp", materialGreen, 128, 128);
        }
        pass("Live material/camera changes and independent same-frame preview images.");

        settings.debugView = 1;
        const auto beforeShader = renderPreview();
        auto changedSource = oldSource;
        const std::string marker = "return material;";
        const auto position = changedSource.rfind(marker);
        require(changedSource.find("EvaluateMaterial") != std::string::npos && position != std::string::npos,
            "The active material source does not contain the expected EvaluateMaterial return; source was not modified.");
        changedSource.replace(position, marker.size(),
            "material.albedo = float3(0.03, 0.85, 0.12);\n    return material;");
        const auto priorRevision = renderer.shaderRevision();
        require(renderer.applyShaderSource(changedSource, error), error);
        require(renderer.shaderRevision() > priorRevision && renderer.shaderSource() == changedSource,
            "Successful hot compilation did not publish its candidate source/revision.");
        const auto afterShader = renderPreview("hot-compiled");
        require(changedPixels(beforeShader, afterShader) > 100, "Valid hot compilation did not alter GPU output.");
        const auto acceptedRevision = renderer.shaderRevision();
        require(!renderer.applyShaderSource(changedSource + "\n deliberate invalid HLSL ;", error) && !error.empty(),
            "Invalid HLSL unexpectedly succeeded or omitted diagnostics.");
        report["rejectedShaderDiagnostic"] = error;
        require(renderer.shaderRevision() == acceptedRevision && renderer.shaderSource() == changedSource &&
            renderPreview() == afterShader, "Rejected hot compilation changed the live pipeline or pixels.");
        require(renderer.applyShaderSource(oldSource, error), error);
        require(renderPreview() == beforeShader, "Restoring the original shader did not restore its GPU image.");
        settings.debugView = 0;
        pass("Valid hot compilation changes pixels; invalid source retains the previous pipeline and image.");

        require(renderer.ensureSceneTargetSize(512, 384, error), error);
        require(renderer.renderViewportPreview(settings, world, assets, nullptr, 512, 384,
            error, Slot::Viewport1), error);
        (void)read(renderer, Slot::Viewport1, 512, 384);
        require(renderer.resize(256, 192, error), error);
        require(renderer.ensureSceneTargetSize(448, 336, error), error);
        require(renderer.renderViewportPreview(settings, world, assets, nullptr, 448, 336,
            error, Slot::Viewport2), error);
        (void)read(renderer, Slot::Viewport2, 448, 336);
        require(read(renderer, Slot::MaterialEditor, 128, 128) == materialRed &&
            read(renderer, Slot::Camera, width, height) == cameraFirst,
            "Resizing the main/scratch target invalidated independent preview images.");
        pass("Detached scene targets exceed swap-chain size; resize preserves independent images.");

        renderer.requestFrameCapture();
        renderMain("resized-main.bmp");
        require(hasPass(renderer.activePipeline(), renderer::RenderPassKind::FrameCapture) &&
            renderer.frameCapture().valid && !renderer.frameCapture().resources.empty(),
            "Native D3D12 Inspector must produce real frozen resources.");
        report["inspectorStatus"] = renderer.frameCapture().status;
        const auto cacheBefore = renderer.cacheStats().meshCount;
        require(cacheBefore >= 2, "Main scene mesh cache was not populated.");
        (void)renderer.collectGarbage(0);
        require(renderer.cacheStats().meshCount >= cacheBefore,
            "GC evicted geometry used by the latest main scene frame.");
        require(read(renderer, Slot::MaterialEditor, 128, 128) == materialRed &&
            read(renderer, Slot::Camera, width, height) == cameraFirst,
            "GC changed live preview resources.");
        const auto beforeGc = renderPreview();
        (void)renderer.collectGarbage(0);
        require(renderPreview() == beforeGc, "GPU scene output changed after garbage collection.");
        pass("Inspector freezes native D3D12 resources; GC retains current scene and preview resources.");

        runUiSmoke(renderer, ui, world, assets, artifactDirectory, report);
        pass("Native secondary UI, image replacement and minimized-main submissions complete without GPU validation errors.");

        if (renderer.hardwareRayTracingSupported()) {
            const auto savedSettings = settings;
            settings.path = Path::Deferred;
            settings.shadowMethod = Shadow::Disabled;
            settings.hardwareRayTracing = true;
            settings.hardwareRayTracingReflections = true;
            settings.hardwareRayTracingShadows = true;
            settings.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
            settings.debugView = 0;
            renderMain("native-dxr-preview.bmp");
            require(renderer.hardwareRayTracingActive() &&
                hasPass(renderer.activePipeline(), renderer::RenderPassKind::HardwareTransport),
                "Native DXR did not execute the hardware transport pass: " + renderer.hardwareRayTracingStatus());
            // Editor viewports are a sub-rectangle of the scene target. Resolve
            // the hardware image at the requested offset and require the same
            // image as a viewport at (0,0); copying at the target origin would
            // shift/leave the right band stale.
            require(renderer.ensureSceneTargetSize(320, 240, error), error);
            auto editorSettings = settings;
            editorSettings.editorViewport = true;
            editorSettings.offscreenTarget = false;
            editorSettings.viewportVisible = true;
            editorSettings.viewportWidth = 200;
            editorSettings.viewportHeight = 140;
            editorSettings.viewportX = 0;
            editorSettings.viewportY = 0;
            settings.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
            const auto captureEditorViewport = [&](std::uint32_t x, std::uint32_t y,
                std::vector<std::uint8_t>& pixels, std::uint32_t& width, std::uint32_t& height) {
                editorSettings.viewportX = x;
                editorSettings.viewportY = y;
                FrameScope frame;
                require(renderer.render(editorSettings, world, assets, error), error);
                require(renderer.readScenePixels(pixels, width, height, error), error);
            };
            const auto compareViewportOffset = [&](renderer::AntiAliasingMethod method, const char* name) {
                editorSettings.antiAliasing.method = method;
                std::vector<std::uint8_t> originScene, offsetScene;
                std::uint32_t originWidth = 0, originHeight = 0, offsetWidth = 0, offsetHeight = 0;
                captureEditorViewport(0, 0, originScene, originWidth, originHeight);
                captureEditorViewport(64, 48, offsetScene, offsetWidth, offsetHeight);
                require(originWidth == offsetWidth && originHeight == offsetHeight,
                    "Editor viewport renders changed the scene target size.");
                savePixels(artifactDirectory / (std::string("native-dxr-editor-") + name + "-offset.bmp"),
                    offsetScene, offsetWidth, offsetHeight);
                std::size_t mismatches = 0;
                // Spatial filters sample a small neighbourhood; the pixels at the
                // viewport border see different off-viewport leftovers between
                // the two renders, so compare the interior for AA variants.
                const std::uint32_t border = method == renderer::AntiAliasingMethod::Disabled ? 0u : 4u;
                for (std::uint32_t y = border; y < 140 - border; ++y) {
                    for (std::uint32_t x = border; x < 200 - border; ++x) {
                        const auto origin = (static_cast<std::size_t>(y) * originWidth + x) * 4;
                        const auto offset = (static_cast<std::size_t>(y + 48) * offsetWidth + (x + 64)) * 4;
                        if (std::memcmp(originScene.data() + origin, offsetScene.data() + offset, 4) != 0)
                            ++mismatches;
                    }
                }
                require(mismatches <= (method == renderer::AntiAliasingMethod::Disabled ? 0u : 8u),
                    std::string("Native DXR editor viewport image is shifted (") +
                    name + "): " + std::to_string(mismatches) + " mismatched pixels.");
                return mismatches;
            };
            const auto noAaMismatches = compareViewportOffset(renderer::AntiAliasingMethod::Disabled, "no-aa");
            const auto fxaaMismatches = compareViewportOffset(renderer::AntiAliasingMethod::FXAA, "fxaa");
            report["dxrViewport"] = {{"offset", {64, 48}}, {"size", {200, 140}},
                {"mismatches", {{"disabled", noAaMismatches}, {"fxaa", fxaaMismatches}}}};
            settings.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
            settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
            settings.globalIllumination.indirectDiffuse = true;
            settings.globalIllumination.indirectSpecular = true;
            settings.globalIllumination.samplesPerFrame = 1;
            settings.globalIllumination.targetSamples = 8;
            settings.globalIllumination.maxBounces = 2;
            for (int frame = 0; frame < 4; ++frame) renderMain(frame == 3 ? "native-dxr-realtime-gi.bmp" : nullptr);
            const auto gi = renderer.globalIlluminationStatus();
            require(gi.effective == renderer::GlobalIlluminationMethod::PathTracing,
                "Native realtime GI did not become effective: " + gi.reason);
            report["dxr"] = {{"active", true}, {"status", renderer.hardwareRayTracingStatus()},
                {"realtimeGiSamples", gi.accumulatedSamples}, {"realtimeGiReason", gi.reason}};
            settings = savedSettings;
            pass("Native DXR hardware preview and realtime GI render on the raster D3D12 device.");
        } else {
            report["dxr"] = {{"active", false}, {"reason", renderer.hardwareRayTracingStatus()}};
        }

        const auto statistics = renderer.rhiStatistics();
        require(statistics.renderPasses > 0 && statistics.indexedDrawCalls > 0 && statistics.bufferUploads > 0,
            "The native RHI did not report real rendering/upload work.");
        report["rhi"] = {{"passes", statistics.renderPasses}, {"draws", statistics.drawCalls},
            {"indexedDraws", statistics.indexedDrawCalls}, {"uploads", statistics.bufferUploads}};
        restore.run();
        report["validationEnabled"] = renderer.validationEnabled();
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation reported errors.");
        report["passed"] = true;
        persist();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        report["failure"] = error;
        try { persist(); } catch (...) {}
        return false;
    }
}
