#include "HardwareRayTracingSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <imgui_internal.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Editor/SceneEditor.h>
#include <RestArtEngine/ImGui/ImGuiLayer.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Pixels = std::vector<std::uint8_t>;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("Hardware RT: " + message);
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "Cannot read test file.");
    return {std::istreambuf_iterator<char>(input), {}};
}
void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    require(stream.good(), "Cannot write test output.");
}

// Read the actual final scene image, independently of the UI and CPU settings.
Pixels readScene(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Scene output SRV is missing.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, staging;
    require(SUCCEEDED(resource.As(&texture)), "Scene output is not a texture.");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected scene output format.");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)),
        "Cannot create scene readback.");
    renderer.context()->CopyResource(staging.Get(), texture.Get());
    Pixels pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
        "Scene readback failed.");
    for (UINT row = 0; row != desc.Height; ++row)
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * desc.Width * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(desc.Width) * 4);
    renderer.context()->Unmap(staging.Get(), 0);
    return pixels;
}

void savePixels(const std::filesystem::path& path, const Pixels& rgba,
                std::uint32_t width, std::uint32_t height) {
    require(rgba.size() == static_cast<std::size_t>(width) * height * 4, "Invalid image size.");
    auto bgra = rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4)
        std::swap(bgra[pixel], bgra[pixel + 2]);
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(bgra.size());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));
    output.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    output.close();
    require(output.good(), "Cannot save scene image.");
}

std::size_t changedPixels(const Pixels& first, const Pixels& second, unsigned tolerance = 3) {
    require(first.size() == second.size(), "Image comparison dimensions differ.");
    std::size_t changed = 0;
    for (std::size_t pixel = 0; pixel < first.size(); pixel += 4) {
        unsigned maximum = 0;
        for (unsigned channel = 0; channel != 3; ++channel)
            maximum = std::max(maximum, static_cast<unsigned>(std::abs(
                static_cast<int>(first[pixel + channel]) - static_cast<int>(second[pixel + channel]))));
        if (maximum > tolerance) ++changed;
    }
    return changed;
}
std::size_t visiblePixels(const Pixels& pixels) {
    std::size_t visible = 0;
    for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4)
        if (std::max({pixels[pixel], pixels[pixel + 1], pixels[pixel + 2]}) > 8) ++visible;
    return visible;
}

struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext* value) { ImGui::SetCurrentContext(value); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
// Feed ordinary ImGui input after the Win32 backend gathers the real cursor.
struct Input {
    ImGuiContext* context = nullptr;
    ImGuiID hook = 0;
    ImVec2 mouse{-100, -100};
    std::function<void(ImGuiIO&)> event;
    void attach(ImGuiContext* value) {
        context = value;
        context->IO.ConfigInputTrickleEventQueue = false;
        ImGuiContextHook callback;
        callback.Type = ImGuiContextHookType_NewFramePre;
        callback.UserData = this;
        callback.Callback = [](ImGuiContext* ctx, ImGuiContextHook* data) {
            auto& input = *static_cast<Input*>(data->UserData);
            ctx->IO.AddFocusEvent(true);
            ctx->IO.AddMousePosEvent(input.mouse.x, input.mouse.y);
            if (ctx->IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
                ctx->IO.AddMouseViewportEvent(ctx->Viewports[0]->ID);
            if (input.event) { auto pending = std::move(input.event); pending(ctx->IO); }
            bool pressed = false;
            for (const auto& event : ctx->InputEventsQueue)
                pressed |= event.Type == ImGuiInputEventType_MouseButton && event.MouseButton.Down;
            if (pressed && ctx->Viewports[0]->PlatformHandle)
                SetForegroundWindow(static_cast<HWND>(ctx->Viewports[0]->PlatformHandle));
        };
        hook = ImGui::AddContextHook(context, &callback);
    }
    ~Input() { if (hook) ImGui::RemoveContextHook(context, hook); }
};

struct TraceFrame {
    TraceFrame() { rae::diagnostics::beginFrame(); }
    ~TraceFrame() { rae::diagnostics::endFrame(); }
};
} // namespace

void runHardwareRayTracingSmoke(rae::renderer::SceneRenderer& renderer,
    rae::imgui::ImGuiLayer& ui, rae::editor::SceneEditor& editor,
    const rae::project::ProjectPaths& project, const std::filesystem::path& outputDirectory) {
    using namespace rae;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    // The suite deliberately saves project settings. Refuse a user project even
    // if an entry point accidentally forgets to create its isolated copy.
    std::error_code pathError;
    const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), pathError);
    const auto testRoot = std::filesystem::weakly_canonical(project.projectRoot, pathError);
    const auto relative = testRoot.lexically_relative(temporary);
    require(!pathError && !relative.empty() && !relative.is_absolute() && *relative.begin() != "..",
        "Self-test must run on a project copy under the temporary directory.");
    const auto oldSource = renderer.shaderSource();
    const auto oldManifest = contents(project.manifest);
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    struct Restore {
        renderer::SceneRenderer& renderer;
        editor::SceneEditor& editor;
        const std::string& source;
        const std::string& manifest;
        const std::filesystem::path& path;
        std::uint32_t width, height;
        bool done = false;
        void restore() {
            editor.closeProjectSettings();
            std::string error;
            const bool shaderRestored = renderer.applyShaderSource(source, error);
            require(shaderRestored, "Cannot restore shader: " + error);
            const bool sizeRestored = renderer.resize(width, height, error);
            require(sizeRestored, "Cannot restore render size: " + error);
            write(path, manifest);
            done = true;
        }
        ~Restore() { if (!done) try { restore(); } catch (...) {} }
    } restore{renderer, editor, oldSource, oldManifest, project.manifest, oldWidth, oldHeight};

    Json report{{"format", "RestArtEngine.HardwareRayTracingSmoke"}, {"version", 1},
        {"adapter", renderer.adapterName()}, {"hardwareSupported", renderer.hardwareRayTracingSupported()},
        {"checks", Json::array()}, {"pixelChanges", Json::object()}, {"passed", false}};
    const auto saveReport = [&] { write(output / "hardware-rt-report.json", report.dump(2) + '\n'); };
    const auto pass = [&](const char* text) {
        report["checks"].push_back(text);
        std::cout << "PASS: Hardware RT " << text << '\n';
    };
    const auto difference = [&](const char* name, const Pixels& first, const Pixels& second,
                                std::size_t minimum = 16) {
        const auto changed = changedPixels(first, second);
        report["pixelChanges"][name] = changed;
        require(changed >= minimum, std::string(name) + " did not change the GPU image (" +
            std::to_string(changed) + " pixels).");
    };

    try {
        std::string error;
        constexpr std::uint32_t width = 320, height = 240;
        require(renderer.resize(width, height, error), error);
        assets::AssetLibrary assets;
        assets.createDefaults();
        auto& copper = *assets.findMaterial(1);
        copper.roughness = 0.08f;
        copper.metallic = 0.95f;
        auto& ground = *assets.findMaterial(2);
        ground.grid = false;
        ground.roughness = 0.08f;
        ground.metallic = 0.8f;
        ground.baseColor[0] = ground.baseColor[1] = ground.baseColor[2] = 0.6f;
        scene::World world;
        world.createDefaultScene();
        const auto cube = world.createActor("Reflection target", 3, 2);
        auto* cubeActor = world.findActor(cube);
        cubeActor->transform.position[0] = -1.7f;
        cubeActor->transform.position[1] = 0.6f;
        cubeActor->transform.position[2] = 0.25f;
        for (float& scale : cubeActor->transform.scale) scale = 0.6f;
        renderer::SceneSettings settings;
        settings.editorViewport = true;
        settings.viewportWidth = width;
        settings.viewportHeight = height;
        settings.rayTracing = false;
        const auto render = [&](const assets::MaterialAsset* preview = nullptr) {
            TraceFrame frame;
            std::string renderError;
            require(renderer.render(settings, world, assets, renderError, preview), renderError);
            return readScene(renderer);
        };
        const auto dispatchStart = renderer.hardwareRayTracingDispatchCount();
        const auto raster = render();
        require(!renderer.hardwareRayTracingActive() &&
            renderer.hardwareRayTracingDispatchCount() == dispatchStart,
            "Disabled master switch submitted hardware work.");
        pass("disabled master switch uses the existing renderer without DXR dispatch");
        savePixels(output / "raster.bmp", raster, width, height);

        settings.hardwareRayTracing = true;
        const auto hardware = render();
        report["status"] = renderer.hardwareRayTracingStatus();
        if (!renderer.hardwareRayTracingSupported()) {
            require(!renderer.hardwareRayTracingActive() &&
                renderer.hardwareRayTracingDispatchCount() == dispatchStart && settings.hardwareRayTracing &&
                !renderer.hardwareRayTracingStatus().empty(),
                "Unavailable hardware lost the request or failed to report a reason.");
            report["hardwareExecutionSkipped"] = renderer.hardwareRayTracingStatus();
            std::cout << "SKIP: Actual DXR execution: " << renderer.hardwareRayTracingStatus() << '\n';
            pass("unavailable hardware retains requested settings and explains fallback");
        } else {
            require(renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() > dispatchStart,
                "Supported device did not submit an actual hardware dispatch: " + renderer.hardwareRayTracingStatus());
            require(visiblePixels(hardware) > 1000, "Hardware output is empty.");
            savePixels(output / "hardware.bmp", hardware, width, height);
            difference("hardware vs raster", raster, hardware);
            pass("DXR dispatch counter increases and actual GPU scene output is visible");

            settings.hardwareRayTracingReflections = false;
            const auto noReflections = render();
            difference("reflection toggle", hardware, noReflections);
            settings.hardwareRayTracingReflections = true;
            settings.hardwareRayTracingShadows = false;
            const auto noShadows = render();
            difference("shadow toggle", hardware, noShadows);
            settings.hardwareRayTracingShadows = true;
            require(changedPixels(hardware, render(), 0) == 0, "Effect switches did not restore the image.");
            pass("reflection and shadow switches independently change GPU pixels and restore");

            settings.cameraYaw += 0.2f;
            difference("camera update", hardware, render());
            settings.cameraYaw -= 0.2f;
            settings.lightIntensity *= 0.2f;
            difference("light update", hardware, render());
            settings.lightIntensity *= 5.0f;
            settings.exposure *= 0.5f;
            difference("exposure update", hardware, render());
            settings.exposure *= 2.0f;
            auto preview = copper;
            preview.baseColor[0] = 0.03f; preview.baseColor[1] = 0.75f; preview.baseColor[2] = 0.12f;
            difference("material draft", hardware, render(&preview));
            require(assets.findMaterial(1)->baseColor[0] == copper.baseColor[0] &&
                assets.findMaterial(1)->baseColor[1] != preview.baseColor[1],
                "Rendering a material draft mutated the committed asset.");
            require(changedPixels(hardware, render(), 1) == 0, "Discarding the draft did not restore the hardware image.");
            pass("camera, light, exposure and unsaved material drafts update the real hardware image");

            world.findActor(cube)->transform.position[0] += 1.0f;
            const auto moved = render();
            difference("actor movement", hardware, moved);
            require(world.removeActor(cube), "Could not remove fixture actor.");
            difference("actor removal", moved, render());
            world = {};
            world.blackBackground = true;
            require(visiblePixels(render()) == 0, "Removed actors remain in the empty hardware scene.");
            pass("geometry movement, removal and an empty scene update acceleration structure visibility");

            // A single triangle makes facing assertions independent of lights,
            // sphere tessellation, reflected objects and the raster shadow map.
            assets::MeshAsset triangle;
            triangle.id = 9001;
            triangle.name = "Facing fixture";
            triangle.vertices = {{{-1, -1, 0}, {0, 0, 1}}, {{1, -1, 0}, {0, 0, 1}}, {{0, 1, 0}, {0, 0, 1}}};
            triangle.indices = {0, 1, 2};
            assets.meshes.push_back(triangle);
            const auto facingActor = world.createActor("Facing triangle", triangle.id, 1);
            settings.cameraYaw = settings.cameraPitch = 0;
            settings.cameraTarget[1] = 0;
            settings.debugView = 2;
            const auto front = render();
            require(visiblePixels(front) > 100, "A front-facing single-sided triangle is invisible.");
            world.findActor(facingActor)->transform.scale[0] = -1;
            require(visiblePixels(render()) > 100, "Mirrored single-sided geometry lost its visible front.");
            settings.cameraYaw = 3.14159265359f;
            require(visiblePixels(render()) == 0, "Single-sided material renders from its back.");
            assets.findMaterial(1)->twoSided = true;
            require(visiblePixels(render()) > 100, "Two-sided material failed to render its back.");
            pass("single-sided, two-sided and mirrored triangle facing follows material semantics");

            settings.path = renderer::RenderPath::Forward;
            require(renderer.hardwareRayTracingActive() && visiblePixels(render()) > 100,
                "Forward pipeline failed with hardware tracing enabled.");
            settings.path = renderer::RenderPath::Deferred;
            require(visiblePixels(render()) > 100, "Deferred pipeline failed with hardware tracing enabled.");
            const auto stable = render();
            auto changedSource = oldSource;
            const auto insertion = changedSource.rfind("return material;");
            require(insertion != std::string::npos, "Test material shader lacks its return contract.");
            changedSource.insert(insertion, "material.albedo = float3(0.02, 0.85, 0.04);\n    ");
            const auto revision = renderer.shaderRevision();
            require(renderer.applyShaderSource(changedSource, error), error);
            require(renderer.shaderRevision() == revision + 1, "Successful shader update did not advance its revision.");
            const auto compiled = render();
            difference("hot shader update", stable, compiled);
            const auto workingRevision = renderer.shaderRevision();
            require(!renderer.applyShaderSource(changedSource + "\ninvalid shader syntax !!!\n", error) && !error.empty(),
                "Malformed shader source was accepted.");
            require(renderer.shaderRevision() == workingRevision && changedPixels(compiled, render(), 0) == 0,
                "Syntax error replaced the last working hardware pipeline/image.");
            require(!renderer.applyShaderSource(changedSource +
                "\n#ifdef RAE_HARDWARE_RAYTRACING\n#error DXR candidate intentionally invalid\n#endif\n", error) && !error.empty(),
                "DXR-only shader error was accepted after the raster candidate compiled.");
            require(renderer.shaderRevision() == workingRevision && changedPixels(compiled, render(), 0) == 0,
                "DXR candidate failure partially published the raster or hardware shader.");
            require(renderer.applyShaderSource(oldSource, error), error);
            require(changedPixels(stable, render(), 0) == 0, "Fixed shader did not recover the original GPU output.");
            pass("successful shader updates affect DXR pixels; syntax and DXR-only failures preserve both pipelines");

            // A raster project may use pixel derivatives. Rejecting its valid
            // material because the optional compute pipeline cannot compile it
            // would silently narrow the existing material language.
            settings.hardwareRayTracing = false;
            const auto rasterMaterialBaseline = render();
            auto derivativeSource = oldSource;
            const auto derivativeReturn = derivativeSource.rfind("return material;");
            require(derivativeReturn != std::string::npos, "Test material lacks its return contract.");
            derivativeSource.insert(derivativeReturn,
                "material.albedo = float3(0.85, 0.03, 0.7) * (0.8 + 0.2 * saturate(fwidth(worldPosition.x) * 32.0));\n    ");
            const auto rasterRevision = renderer.shaderRevision();
            const auto dispatchBeforeRasterMaterial = renderer.hardwareRayTracingDispatchCount();
            const bool derivativeApplied = renderer.applyShaderSource(derivativeSource, error);
            require(derivativeApplied, "A raster-only derivative material was incorrectly rejected: " + error);
            require(renderer.shaderRevision() == rasterRevision + 1,
                "A valid raster-only material did not advance the shader revision.");
            const auto rasterDerivative = render();
            difference("raster derivative material", rasterMaterialBaseline, rasterDerivative);
            settings.hardwareRayTracing = true;
            const auto derivativeFallback = render();
            require(settings.hardwareRayTracing && !renderer.hardwareRayTracingActive() &&
                renderer.hardwareRayTracingDispatchCount() == dispatchBeforeRasterMaterial,
                "Requesting hardware with an incompatible material dispatched a stale hardware pipeline.");
            require(changedPixels(rasterDerivative, derivativeFallback, 0) == 0,
                "Unavailable hardware replaced the active derivative material image.");
            require(renderer.hardwareRayTracingStatus().find("unavailable") != std::string::npos &&
                renderer.hardwareRayTracingStatus().find("material") != std::string::npos,
                "Hardware-incompatible material has no explicit device-status explanation.");
            require(renderer.applyShaderSource(oldSource, error), error);
            require(changedPixels(stable, render(), 0) == 0 && renderer.hardwareRayTracingActive() &&
                renderer.hardwareRayTracingDispatchCount() > dispatchBeforeRasterMaterial,
                "Restoring a common material did not resume actual hardware rendering.");
            pass("raster derivative materials remain live; incompatible hardware requests explain fallback without stale dispatch");

            // Includes belong to the project material, including files nested
            // in a subdirectory. Both compiler paths must load the same file
            // rather than accidentally resolving an engine file.
            const auto includeDirectory = renderer.materialShaderPath().parent_path() /
                (".HardwareRtSmoke-" + std::to_string(GetCurrentProcessId()));
            require(std::filesystem::create_directory(includeDirectory),
                "Cannot create a unique project-local include fixture.");
            struct IncludeCleanup {
                std::filesystem::path directory;
                ~IncludeCleanup() {
                    std::error_code ignored;
                    std::filesystem::remove(directory / "Nested.hlsli", ignored);
                    std::filesystem::remove(directory / "Color.hlsli", ignored);
                    std::filesystem::remove(directory, ignored);
                }
            } includeCleanup{includeDirectory};
            write(includeDirectory / "Nested.hlsli",
                "#include \"Color.hlsli\"\n");
            write(includeDirectory / "Color.hlsli",
                "float3 HardwareRtSmokeColour() { return float3(0.04, 0.12, 0.9); }\n");
            auto includeSource = oldSource;
            includeSource.insert(includeSource.rfind("return material;"),
                "material.albedo = HardwareRtSmokeColour();\n    ");
            includeSource.insert(0, "#include \"" + includeDirectory.filename().string() + "/Nested.hlsli\"\n");
            const bool includesApplied = renderer.applyShaderSource(includeSource, error);
            require(includesApplied, "Project-relative includes failed: " + error);
            const auto includedImage = render();
            difference("project relative include", stable, includedImage);
            write(includeDirectory / "Color.hlsli",
                "float3 HardwareRtSmokeColour() { return float3(0.9, 0.8, 0.04); }\n");
            require(renderer.applyShaderSource(includeSource, error), error);
            difference("nested include reload", includedImage, render());
            require(renderer.applyShaderSource(oldSource, error), error);
            require(changedPixels(stable, render(), 0) == 0,
                "Removing the include candidate did not restore the original image.");
            pass("project-relative includes, nested includes and edited include reload change the hardware image");

            require(renderer.resize(384, 256, error), error);
            settings.viewportWidth = 384; settings.viewportHeight = 256;
            require(visiblePixels(render()) > 100, "Hardware output failed after resizing.");
            settings.hardwareRayTracing = false;
            const auto stoppedDispatch = renderer.hardwareRayTracingDispatchCount();
            (void)render();
            require(!renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() == stoppedDispatch,
                "Disabling hardware after resize still submits hardware work.");
            settings.hardwareRayTracing = true;
            (void)render();
            require(renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() > stoppedDispatch,
                "Hardware failed to resume after disabling.");
            pass("forward/deferred paths, resize, disable and re-enable retain working output");
        }

        // UI uses the editor's own isolated project and its normal device size.
        require(renderer.resize(oldWidth, oldHeight, error), error);
        auto manifest = Json::parse(oldManifest);
        manifest["hardwareRtSmokeUnknownField"] = {{"preserve", 17}};
        write(project.manifest, manifest.dump(2) + '\n');
        renderer::SceneSettings uiSettings;
        uiSettings.hardwareRayTracing = project.rendering.hardwareRayTracing;
        uiSettings.hardwareRayTracingReflections = project.rendering.hardwareRayTracingReflections;
        uiSettings.hardwareRayTracingShadows = project.rendering.hardwareRayTracingShadows;
        ImGuiContext* context = nullptr;
        std::string uiText;
        const auto uiFrame = [&](const char* screenshot = nullptr) {
            TraceFrame trace;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                require(message.message != WM_QUIT, "Project settings closed the application.");
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            ui.beginFrame(1.0f / 60.0f);
            context = ImGui::GetCurrentContext();
            ImGui::LogToBuffer();
            bool vsync = false, capture = false;
            editor.draw(uiSettings, renderer, vsync, capture);
            uiText = context->LogBuffer.c_str();
            ImGui::LogFinish();
            const bool rendered = renderer.render(uiSettings, editor.world(), editor.assets(), error, editor.materialPreview());
            ui.endFrame();
            require(rendered, error);
            if (screenshot) require(renderer.saveScreenshot((output / screenshot).c_str(), error), error);
            require(renderer.present(false, error), error);
        };
        uiFrame();
        Input input;
        input.attach(context);
        const auto findWindow = [&](const char* name) {
            for (auto* window : context->Windows)
                if (window->ID == ImHashStr(name)) return window;
            return static_cast<ImGuiWindow*>(nullptr);
        };
        const auto findChild = [&](const char* fragment) {
            for (auto* window : context->Windows)
                if (window->Active && std::strstr(window->Name, fragment)) return window;
            return static_cast<ImGuiWindow*>(nullptr);
        };
        const auto itemRect = [&](ImGuiWindow* window, const char* label,
                                  ImGuiNavLayer layer = ImGuiNavLayer_Main, ImGuiID explicitId = 0) {
            require(window != nullptr, std::string("Missing UI control window for ") + label);
            ContextScope scope(context);
            const auto id = explicitId ? explicitId : window->GetID(label);
            ImGui::FocusWindow(window);
            ImGui::SetNavID(id, layer, window->NavRootFocusScopeId, ImRect());
            uiFrame();
            require(context->NavId == id && context->NavIdIsAlive,
                std::string("UI control is not navigable: ") + label);
            // A navigable item may still be clipped. Scroll, submit the new
            // layout and return only the visible hit area, never an old screen
            // coordinate below the child window or over its parent's footer.
            for (int attempt = 0; attempt != 4; ++attempt) {
                auto rectangle = ImGui::WindowRectRelToAbs(window, window->NavRectRel[layer]);
                if (attempt == 0 || !window->ClipRect.Contains(rectangle.GetCenter())) {
                    ImGui::ScrollToRectEx(window, rectangle,
                        ImGuiScrollFlags_AlwaysCenterY | ImGuiScrollFlags_NoScrollParent);
                    uiFrame();
                    require(context->NavId == id && context->NavIdIsAlive,
                        std::string("UI control disappeared while scrolling: ") + label);
                    rectangle = ImGui::WindowRectRelToAbs(window, window->NavRectRel[layer]);
                }
                rectangle.ClipWith(window->ClipRect);
                if (rectangle.GetWidth() > 2.0f && rectangle.GetHeight() > 2.0f &&
                    window->ClipRect.Contains(rectangle.GetCenter())) return rectangle;
                uiFrame();
            }
            throw std::runtime_error(std::string("UI control has no visible hit target after scrolling: ") + label);
        };
        const auto click = [&](ImVec2 position) {
            input.mouse = position;
            uiFrame();
            input.event = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, true); };
            uiFrame();
            input.event = [](ImGuiIO& io) { io.AddMouseButtonEvent(0, false); };
            uiFrame();
            uiFrame();
        };
        // BeginMenuBar pushes its own ID scope, so a menu heading does not use
        // window->GetID(label). Use the same measured horizontal hit target as
        // the existing editor menu regression, then click the actual popup item.
        const auto projectMenuCenter = [&] {
            ContextScope scope(context);
            const auto* toolbar = findWindow("RestArtEngine Editor");
            require(toolbar && toolbar->MenuBarHeight > 0, "Editor has no horizontal project menu bar.");
            float menuX = toolbar->Pos.x + std::max(toolbar->WindowPadding.x, context->Style.ItemSpacing.x);
            for (const char* preceding : {"Layout", "Edit", "UI", "Debug"})
                menuX += ImGui::CalcTextSize(preceding).x + context->Style.ItemSpacing.x * 2.0f;
            return ImVec2(menuX + context->Style.ItemSpacing.x * 0.5f + ImGui::CalcTextSize("Project").x * 0.5f,
                toolbar->MenuBarRect().GetCenter().y);
        }();
        click(projectMenuCenter);
        ImGuiWindow* menu = context->OpenPopupStack.empty() ? nullptr : context->OpenPopupStack[0].Window;
        require(menu != nullptr && uiText.find("Project Settings...") != std::string::npos,
            "Project menu does not expose Project Settings.");
        click(itemRect(menu, "Project Settings...").GetCenter());
        require(editor.projectSettingsOpen(), "Project menu did not open its settings window.");
        uiFrame();
        require(uiText.find("Hardware ray tracing") != std::string::npos &&
            uiText.find("Rendering") != std::string::npos &&
            findChild("ProjectSettingsNavigation") && findChild("ProjectSettingsContent"),
            "Project settings has no Rendering navigation/category or hardware checkbox.");
        require(uiText.find("[ ] Ray tracing") == std::string::npos && uiText.find("[x] Ray tracing") == std::string::npos,
            "The old ambiguous top-level ray-tracing checkbox remains visible.");
        click(itemRect(findChild("ProjectSettingsContent"), "Hardware ray tracing").GetCenter());
        require(uiSettings.hardwareRayTracing != project.rendering.hardwareRayTracing && editor.projectSettingsDirty(),
            "Hardware checkbox failed to update the requested value immediately.");
        if (renderer.hardwareRayTracingSupported() && uiSettings.viewportVisible)
            require(renderer.hardwareRayTracingActive() == uiSettings.hardwareRayTracing,
                "The project checkbox did not update the effective hardware renderer.");
        if (uiSettings.hardwareRayTracing) {
            const auto reflections = uiSettings.hardwareRayTracingReflections;
            click(itemRect(findChild("ProjectSettingsContent"), "Ray-traced reflections").GetCenter());
            require(uiSettings.hardwareRayTracingReflections != reflections,
                "Reflection setting did not update from real UI input.");
        }
        // GI is a project lighting algorithm. Choose it through the real combo
        // and verify dependency activation, sampling feedback and persistence.
        click(itemRect(findChild("ProjectSettingsContent"), "GI method").GetCenter());
        require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
            "GI method did not open its choices.");
        auto* giPopup = context->OpenPopupStack.back().Window;
        const auto pathItem = ImHashStr("Path tracing", 0, giPopup->GetID(1));
        click(itemRect(giPopup, "Path tracing", ImGuiNavLayer_Main, pathItem).GetCenter());
        require(uiSettings.globalIllumination.method == renderer::GlobalIlluminationMethod::PathTracing &&
            uiSettings.hardwareRayTracing && editor.projectSettingsDirty(),
            "GI method failed to request path tracing and activate its hardware dependency.");
        if (renderer.hardwareRayTracingSupported() && uiSettings.viewportVisible) {
            require(renderer.globalIlluminationStatus().effective == renderer::GlobalIlluminationMethod::PathTracing &&
                renderer.globalIlluminationStatus().accumulatedSamples > 0,
                "GI selection did not accumulate real samples.");
            const auto oldHistory = renderer.globalIlluminationStatus().historyRevision;
            click(itemRect(findChild("ProjectSettingsContent"), "Restart accumulation").GetCenter());
            require(renderer.globalIlluminationStatus().historyRevision > oldHistory,
                "Restart accumulation did not invalidate progressive history.");
        }

        click(itemRect(findChild("ProjectSettingsContent"), "GI mode").GetCenter());
        require(!context->OpenPopupStack.empty() && context->OpenPopupStack.back().Window,
            "GI update mode did not open its choices.");
        auto* modePopup = context->OpenPopupStack.back().Window;
        const auto realtimeItem = ImHashStr("Realtime", 0, modePopup->GetID(1));
        click(itemRect(modePopup, "Realtime", ImGuiNavLayer_Main, realtimeItem).GetCenter());
        require(uiSettings.globalIllumination.mode == renderer::GlobalIlluminationMode::Realtime &&
            uiSettings.hardwareRayTracing && editor.projectSettingsDirty(),
            "Realtime GI mode did not update from the actual combo selection.");
        if (renderer.hardwareRayTracingSupported() && uiSettings.viewportVisible)
            require(renderer.globalIlluminationStatus().effective == renderer::GlobalIlluminationMethod::PathTracing &&
                renderer.globalIlluminationStatus().effectiveMode == renderer::GlobalIlluminationMode::Realtime,
                "Realtime GI request did not reach the effective rendering mode.");
        require(uiText.find("History samples") != std::string::npos &&
            uiText.find("Realtime GI updates continuously") != std::string::npos,
            "Realtime configuration did not expose its rolling history controls.");

        const bool legacyReflections = uiSettings.hardwareRayTracingReflections;
        const bool firstDiffuse = uiSettings.globalIllumination.indirectDiffuse;
        const bool firstSpecular = uiSettings.globalIllumination.indirectSpecular;
        click(itemRect(findChild("ProjectSettingsContent"), "Indirect diffuse (color bleeding)").GetCenter());
        require(uiSettings.globalIllumination.indirectDiffuse != firstDiffuse &&
            uiSettings.globalIllumination.indirectSpecular == firstSpecular &&
            uiSettings.hardwareRayTracingReflections == legacyReflections,
            "Indirect diffuse checkbox did not change only its independent GI contribution.");
        click(itemRect(findChild("ProjectSettingsContent"), "Indirect specular (reflections)").GetCenter());
        require(uiSettings.globalIllumination.indirectDiffuse != firstDiffuse &&
            uiSettings.globalIllumination.indirectSpecular != firstSpecular &&
            uiSettings.hardwareRayTracingReflections == legacyReflections,
            "Indirect specular checkbox changed another contribution or the standalone reflection preference.");
        click(itemRect(findChild("ProjectSettingsContent"), "Indirect diffuse (color bleeding)").GetCenter());
        require(uiSettings.globalIllumination.indirectDiffuse == firstDiffuse &&
            uiSettings.globalIllumination.indirectSpecular != firstSpecular,
            "Restoring diffuse also reset the independently edited specular contribution.");
        (void)itemRect(findChild("ProjectSettingsContent"), "GI mode");
        uiFrame("project-settings-realtime.bmp");
        if (renderer.hardwareRayTracingSupported() && uiSettings.viewportVisible) {
            const auto historyBeforeRestart = renderer.globalIlluminationStatus().historyRevision;
            click(itemRect(findChild("ProjectSettingsContent"), "Restart accumulation").GetCenter());
            require(renderer.globalIlluminationStatus().historyRevision > historyBeforeRestart &&
                renderer.globalIlluminationStatus().effectiveMode == renderer::GlobalIlluminationMode::Realtime,
                "Restart accumulation did not restart the continuous GI history.");
        }
        const auto actorCount = editor.world().actors.size();
        const auto selected = editor.selectedActor();
        input.event = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Delete, true); };
        uiFrame();
        input.event = [](ImGuiIO& io) { io.AddKeyEvent(ImGuiKey_Delete, false); };
        uiFrame();
        require(editor.world().actors.size() == actorCount && editor.selectedActor() == selected,
            "Project settings keyboard focus leaked Delete into the scene.");
        uiFrame("project-settings-unsaved.bmp");
        click(itemRect(findWindow("Project Settings"), "Save project settings").GetCenter());
        require(!editor.projectSettingsDirty(), "Save project settings did not clear the unsaved state.");
        project::ProjectPaths reloaded;
        require(project::loadProject(project.manifest, reloaded, error), error);
        require(reloaded.rendering.hardwareRayTracing == uiSettings.hardwareRayTracing &&
            reloaded.rendering.hardwareRayTracingReflections == uiSettings.hardwareRayTracingReflections &&
            reloaded.rendering.hardwareRayTracingShadows == uiSettings.hardwareRayTracingShadows,
            "Saved project rendering values did not survive reloading.");
        require(reloaded.rendering.globalIllumination == uiSettings.globalIllumination,
            "Saved GI algorithm, mode, contributions and sampling values did not survive reloading.");
        require(reloaded.rendering.globalIllumination.mode == renderer::GlobalIlluminationMode::Realtime &&
            reloaded.rendering.globalIllumination.indirectDiffuse == firstDiffuse &&
            reloaded.rendering.globalIllumination.indirectSpecular != firstSpecular,
            "Project reload lost the Realtime mode or independent GI contribution choices.");
        require(Json::parse(contents(project.manifest))["hardwareRtSmokeUnknownField"]["preserve"] == 17,
            "Saving project settings destroyed an unrelated manifest field.");
        uiFrame("project-settings-saved.bmp");
        write(output / "project-settings-ui.txt", uiText);
        editor.closeProjectSettings();
        uiFrame();
        require(!editor.projectSettingsOpen(), "Project settings failed to close.");
        pass("Project menu, GI algorithm/update modes, independent contributions, scrolled restart, focus and project save/reload");

        report["dispatches"] = renderer.hardwareRayTracingDispatchCount() - dispatchStart;
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation reported errors.");
        restore.restore();
        report["passed"] = true;
        saveReport();
        std::cout << "PASS: Hardware RT suite; adapter=" << renderer.adapterName()
            << ", dispatches=" << report["dispatches"] << ", graphics errors=0.\n";
    } catch (const std::exception& failure) {
        report["failure"] = failure.what();
        report["validationErrors"] = renderer.validationErrorCount();
        report["status"] = renderer.hardwareRayTracingStatus();
        try { saveReport(); } catch (...) {}
        throw;
    }
}
