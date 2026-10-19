#include "EditorSmoke.h"
#include <imgui_internal.h>
#include <DirectXMath.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void require(bool value, const std::string& error) { if (!value) throw std::runtime_error(error); }
std::string contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
// Inject after the Win32 backend gathers input, immediately before ImGui consumes it.
// This exercises normal widget behavior without moving the user's desktop cursor.
struct SmokeInput {
    ImGuiContext* context{};
    ImGuiID hook{};
    ImVec2 mouse{-100, -100};
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
            if (input.events) { auto pending = std::move(input.events); pending(ctx->IO); }
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
    world.createDefaultScene(); assets.createDefaults();
    renderer::SceneSettings settings;
    std::string error;
    ImGuiContext* context = nullptr;
    std::string renderedUiText;
    const auto capture = [&](const char* name, bool drawEditor = false, bool recordUiText = false) {
        ui.beginFrame(1.0f / 60.0f);
        context = ImGui::GetCurrentContext();
        settings.viewportWidth = settings.viewportHeight = 0;
        if (drawEditor) {
            bool vsync = false, screenshot = false;
            if (recordUiText) ImGui::LogToBuffer();
            editor.draw(settings, renderer, vsync, screenshot);
            if (recordUiText) {
                renderedUiText = context->LogBuffer.c_str();
                ImGui::LogFinish();
            }
        }
        require(renderer.render(settings, world, assets, error), error);
        ui.endFrame();
        require(renderer.saveScreenshot((output / (std::string(name) + ".bmp")).c_str(), error), error);
        require(renderer.present(false, error), error);
    };

    const std::string originalSource = renderer.shaderSource();
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
    settings.rayTracing = false;
    capture("editor_warmup", true);
    capture("editor", true, true);
    require(renderedUiText.find("Direct-light shadows") != std::string::npos &&
        renderedUiText.find("Shadow method") != std::string::npos &&
        renderedUiText.find("Ray tracing: all mesh actors") != std::string::npos,
        "Editor viewport does not expose the selectable shadow method control.");
    SmokeInput input;
    input.attach(context);
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
    const auto tabBar = [&]() -> ImGuiTabBar* {
        for (int i = 0; i < context->TabBars.GetBufSize(); ++i) {
            auto* bar = context->TabBars.GetByIndex(i);
            if (bar->Window && std::strcmp(bar->Window->Name, "Content & Editors") == 0) return bar;
        }
        throw std::runtime_error("Editor tab bar not found.");
    };
    const auto selectTab = [&](const char* name) {
        auto* bar = tabBar();
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
    selectTab("Shader Editor");
    capture("shader_editor", true);
    const auto buttonRow = [&]() {
        const auto* bar = tabBar();
        return ImVec2(bar->Window->WorkRect.Min.x + 45,
            bar->BarRect.Max.y + context->Style.ItemSpacing.y * 2 + context->FontSize +
            (context->FontSize + context->Style.FramePadding.y * 2) * .5f);
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
    replaceDraft(originalSource);
    click(buttonRow());
    require(renderer.shaderSource() == originalSource,
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
            static_cast<float>(settings.viewportX), static_cast<float>(settings.viewportY),
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
    click({static_cast<float>(settings.viewportX + 30), static_cast<float>(settings.viewportY + 100)});
    require(editor.selectedActor() == 0, "Clicking empty viewport did not clear Actor selection.");
    click(projectPoint(-.35f, 1.35f, 0));
    require(editor.selectedActor() == 1, "Clicking the visible sphere did not select its Actor.");
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
    const ImVec2 panStart{static_cast<float>(settings.viewportX) + settings.viewportWidth * .5f,
        static_cast<float>(settings.viewportY) + settings.viewportHeight * .5f};
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
    const ImVec2 orbitStart{static_cast<float>(settings.viewportX) + settings.viewportWidth * .5f,
        static_cast<float>(settings.viewportY) + settings.viewportHeight * .5f};
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
    capture("actor_deleted", true);
    world.createDefaultScene();
    capture("ui_interaction", true);
    click(projectPoint(-.35f, 1.35f, 0));
    require(editor.selectedActor() == 1, "Could not select the restored sphere Actor.");
    selectTab("Shader Editor");
    replaceDraft(originalSource);
    require(context->IO.WantTextInput && context->ActiveId != 0,
        "Shader source text field did not acquire keyboard input.");
    pressKey(ImGuiKey_Home);
    pressKey(ImGuiKey_Delete);
    require(world.actors.size() == actorCount && world.findActor(1) && editor.selectedActor() == 1,
        "Delete inside Shader Editor incorrectly deleted the selected Actor.");
    replaceDraft(originalSource);
    click(buttonRow());
    require(renderer.shaderSource() == originalSource,
        "Keyboard isolation smoke test did not restore the project shader draft.");
    // Return to a selected sphere with handles for the final preview, without saving.
    selectTab("Material Editor");
    click(projectPoint(-.35f, 1.35f, 0));
    capture("editor", true);
    input.detach();
    ui.shutdown();
    require(renderer.validationErrorCount() == 0, "D3D11 validation reported an editor error.");
    std::cout << "PASS: Actor transforms, mesh/material assignment, shader Apply, failed compile rollback, "
        "binding validation, shader Save/Reload, scene roundtrip, Shader Editor mouse/text Apply, "
        "viewport picking, translation/rotation/local-scale gizmo drags, camera orbit/pan/focus, "
        "Actor deletion and text-input Delete isolation.\n";
    std::cout << "D3D11 debug layer: " << (renderer.validationEnabled() ? "enabled" : "unavailable")
        << "; validation errors: " << renderer.validationErrorCount() << '\n';
}

