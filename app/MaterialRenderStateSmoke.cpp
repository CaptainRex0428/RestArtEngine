#include "MaterialRenderStateSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Assets/MeshQuery.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Pixels = std::vector<std::uint8_t>;
constexpr std::uint32_t width = 192, height = 144;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("Material render states: " + message);
}

Pixels readScene(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Missing scene output.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source, staging;
    require(SUCCEEDED(resource.As(&source)), "Scene output is not a texture.");
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    require(description.Width == width && description.Height == height &&
        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected scene output dimensions or format.");
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.BindFlags = description.MiscFlags = 0;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&description, nullptr, &staging)), "Create readback failed.");
    renderer.context()->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "GPU readback failed.");
    Pixels pixels(static_cast<std::size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * width * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch, width * 4);
    renderer.context()->Unmap(staging.Get(), 0);
    return pixels;
}

void savePixels(const std::filesystem::path& path, const Pixels& pixels) {
    auto bgra = pixels;
    for (std::size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    info.biSize = sizeof(info);
    info.biWidth = width; info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(bgra.size());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    stream.close();
    require(stream.good(), "Cannot save evidence image.");
}

std::array<int, 3> center(const Pixels& pixels) {
    const auto offset = (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}
bool nearPixel(const std::array<int, 3>& a, const std::array<int, 3>& b, int tolerance = 2) {
    for (unsigned i = 0; i < 3; ++i) if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}
std::size_t changed(const Pixels& a, const Pixels& b, int tolerance = 2) {
    require(a.size() == b.size(), "Mismatched GPU comparison dimensions.");
    std::size_t result = 0;
    for (std::size_t i = 0; i < a.size(); i += 4)
        if (std::abs(int(a[i]) - int(b[i])) > tolerance ||
            std::abs(int(a[i + 1]) - int(b[i + 1])) > tolerance ||
            std::abs(int(a[i + 2]) - int(b[i + 2])) > tolerance) ++result;
    return result;
}

rae::assets::MeshAsset makeQuad() {
    rae::assets::MeshAsset mesh;
    mesh.id = 1001; mesh.name = "Render-state authored +Z quad";
    mesh.primitive = rae::assets::MeshPrimitive::Imported;
    mesh.vertices = {{{-1, -1, 0}, {0, 0, 1}}, {{1, -1, 0}, {0, 0, 1}},
        {{1, 1, 0}, {0, 0, 1}}, {{-1, 1, 0}, {0, 0, 1}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};
} // namespace

void runMaterialRenderStateSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Cull = assets::MaterialCullMode;
    using Depth = assets::MaterialDepthTest;
    using Path = renderer::RenderPath;
    using Method = renderer::GlobalIlluminationMethod;
    using Mode = renderer::GlobalIlluminationMode;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    const auto oldSource = renderer.shaderSource();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::string source;
        std::uint32_t oldWidth, oldHeight;
        bool done = false;
        void restore() {
            std::string error;
            require(renderer.applyShaderSource(source, error), "Restore shader: " + error);
            require(renderer.resize(oldWidth, oldHeight, error), "Restore dimensions: " + error);
            renderer.resetGlobalIllumination();
            done = true;
        }
        ~Restore() { if (!done) try { restore(); } catch (...) {} }
    } restore{renderer, oldSource, oldWidth, oldHeight};
    Json report{{"format", "RestArtEngine.MaterialRenderStateSmoke"}, {"version", 1},
        {"adapter", renderer.adapterName()}, {"checks", Json::array()}, {"passed", false}};
    const auto saveReport = [&] {
        std::ofstream stream(output / "material-render-states.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n';
        stream.close();
        require(stream.good(), "Cannot save test report.");
    };
    const auto pass = [&](const char* text) {
        report["checks"].push_back(text);
        std::cout << "PASS: material render states: " << text << '\n';
    };

    try {
        std::string error;
        require(renderer.resize(width, height, error), error);
        require(renderer.applyShaderSource(
            "Surface EvaluateMaterial(float3 p, float3 n, Surface m) { return m; }", error), error);
        assets::AssetLibrary library;
        library.meshes.push_back(makeQuad());
        assets::MaterialAsset red;
        red.id = 101; red.name = "Render-state red";
        red.baseColor[0] = .75f; red.baseColor[1] = .025f; red.baseColor[2] = .02f;
        red.metallic = 0; red.roughness = .8f;
        auto blue = red;
        blue.id = 102; blue.name = "Render-state blue";
        std::swap(blue.baseColor[0], blue.baseColor[2]);
        library.materials = {red, blue};
        scene::World world;
        world.blackBackground = true;
        const auto redActor = world.createActor("Red base", 1001, 101);
        renderer::SceneSettings settings;
        settings.editorViewport = true;
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = settings.cameraPitch = 0;
        settings.cameraDistance = 6;
        std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.debugView = 2;
        const auto render = [&](const assets::MaterialAsset* preview = nullptr) {
            FrameScope frame;
            require(renderer.render(settings, world, library, error, preview), error);
            auto pixels = readScene(renderer);
            require(renderer.present(false, error), error);
            return pixels;
        };

        // Keep the material parameter separate from geometry: switching cull
        // modes must not rebuild a mesh or depend on its authored winding changing.
        for (const auto path : {Path::Forward, Path::Deferred}) {
            settings.path = path;
            for (bool mirrored : {false, true}) {
                for (bool back : {false, true}) {
                    auto& transform = world.findActor(redActor)->transform;
                    transform.rotation[1] = back ? 180.f : 0.f;
                    transform.scale[0] = mirrored ? -1.f : 1.f;
                    for (const auto mode : {Cull::Off, Cull::Front, Cull::Back}) {
                        library.findMaterial(101)->setCullMode(mode);
                        settings.debugView = 2;
                        const auto pixels = render();
                        const auto value = center(pixels);
                        const bool visible = mode == Cull::Off || (back ? mode == Cull::Front : mode == Cull::Back);
                        require(visible ? value[0] > 150 && value[0] > value[2] + 100 : nearPixel(value, {0, 0, 0}),
                            "Raster Cull Off/Front/Back disagrees with the authored front or mirrored transform.");
                        if (visible) {
                            settings.debugView = 3;
                            require(nearPixel(center(render()), {128, 128, 255}), "Rendered back-face normals do not face the camera.");
                        }
                    }
                }
            }
        }
        pass("Forward and Deferred Cull Off/Front/Back, mirrored Actors and back-face shading normals");

        // Exercise the CPU picking accelerator directly using the same authored
        // surface. Existing callers that omit the new flag retain Cull Back.
        {
            assets::MeshQueryCache queries;
            assets::MeshRayHit hit;
            const assets::MeshRay front{{0, 0, 3}, {0, 0, -1}}, back{{0, 0, -3}, {0, 0, 1}};
            const auto& mesh = library.meshes.front();
            require(queries.intersect(mesh, front, 10, false, 1e-8f, hit), "Default picking lost the authored front.");
            require(!queries.intersect(mesh, back, 10, false, 1e-8f, hit), "Default picking accepted a culled back.");
            require(!queries.intersect(mesh, front, 10, false, 1e-8f, hit, true), "Cull Front picking accepted the front.");
            require(queries.intersect(mesh, back, 10, false, 1e-8f, hit, true), "Cull Front picking rejected the back.");
            require(queries.intersect(mesh, front, 10, true, 1e-8f, hit, true) &&
                queries.intersect(mesh, back, 10, true, 1e-8f, hit, true), "Cull Off picking did not override face filtering.");
        }
        pass("CPU picking Cull Off/Front/Back preserves old callers and matches authored sides");

        world.findActor(redActor)->transform = {};
        library.findMaterial(101)->setCullMode(Cull::Back);
        settings.debugView = 2;
        const auto blueActor = world.createActor("Blue comparison", 1001, 102);
        const std::array<Depth, 9> tests{Depth::Never, Depth::Less, Depth::Equal, Depth::LessEqual,
            Depth::Greater, Depth::NotEqual, Depth::GreaterEqual, Depth::Always, Depth::Disabled};
        for (const auto path : {Path::Forward, Path::Deferred}) {
            settings.path = path;
            for (int relation : {-1, 0, 1}) {
                // Positive Z is closer to the camera; the red reference at Z=0
                // is drawn first and establishes the depth buffer value.
                world.findActor(blueActor)->transform.position[2] = static_cast<float>(relation) * .4f;
                for (const auto test : tests) {
                    library.findMaterial(102)->depthTest = test;
                    bool blueWins = false;
                    switch (test) {
                    case Depth::Never: break;
                    case Depth::Less: blueWins = relation > 0; break;
                    case Depth::Equal: blueWins = relation == 0; break;
                    case Depth::LessEqual: blueWins = relation >= 0; break;
                    case Depth::Greater: blueWins = relation < 0; break;
                    case Depth::NotEqual: blueWins = relation != 0; break;
                    case Depth::GreaterEqual: blueWins = relation <= 0; break;
                    case Depth::Always: case Depth::Disabled: blueWins = true; break;
                    }
                    const auto value = center(render());
                    require(blueWins ? value[2] > value[0] + 100 : value[0] > value[2] + 100,
                        "GPU depth comparison selected the wrong overlapping surface (mode=" +
                        std::to_string(static_cast<int>(test)) + ", relative Z=" + std::to_string(relation) + ").");
                }
            }
            library.findMaterial(102)->depthTest = Depth::Less;
            world.findActor(blueActor)->transform.position[2] = -.4f;
            library.findMaterial(101)->depthWrite = false;
            require(center(render())[2] > 150, "ZWrite Off still occludes the later surface.");
            library.findMaterial(101)->depthWrite = true;
            require(center(render())[0] > 150, "Restoring ZWrite On did not restore occlusion.");
        }
        pass("all nine depth modes select expected nearer/equal/farther surfaces in both raster paths; ZWrite restores");

        // Submission culling must affect the query's input count while leaving
        // the final scene image unchanged. Readback flushes the test stream, but
        // production statistics still use their ordinary asynchronous polling.
        world.findActor(blueActor)->transform.position[0] = 100;
        world.findActor(blueActor)->transform.position[2] = 0;
        const auto readCount = [&](std::uint64_t expected) {
            const auto firstFrame = renderer.geometryStatistics().frameId + 1;
            Pixels last;
            for (unsigned frame = 0; frame < 16; ++frame) {
                last = render();
                const auto& statistics = renderer.geometryStatistics();
                if (statistics.status == renderer::GeometrySampleStatus::Ready &&
                    statistics.gpuFrameId >= firstFrame && statistics.inputTriangles == expected) return last;
            }
            throw std::runtime_error("Material render states: Main-view GPU submission count did not reach " + std::to_string(expected));
        };
        for (const auto path : {Path::Forward, Path::Deferred}) {
            settings.path = path;
            settings.frustumCulling = false;
            const auto unculled = readCount(4);
            settings.frustumCulling = true;
            const auto culled = readCount(2);
            require(changed(unculled, culled, 0) == 0, "Frustum culling changed visible raster pixels.");
        }
        report["frustumInputTriangles"] = {{"disabled", 4}, {"enabled", 2}};
        pass("camera frustum culling reduces actual GPU input primitives without changing visible pixels");

        {
            auto savedLibrary = std::move(library);
            auto savedWorld = std::move(world);
            const auto savedSettings = settings;
            library = {};
            library.createDefaults();
            auto importedPlane = *library.findMesh(2);
            importedPlane.id = 1002;
            importedPlane.primitive = assets::MeshPrimitive::Imported;
            library.meshes.push_back(std::move(importedPlane));
            world = {};
            world.blackBackground = true;
            const auto donorActor = world.createActor("Raised reflected red plane", 2, 1);
            auto* donorTransform = &world.findActor(donorActor)->transform;
            donorTransform->position[1] = 2;
            donorTransform->position[2] = -3;
            std::fill(std::begin(donorTransform->scale), std::end(donorTransform->scale), .15f);
            const auto groundActor = world.createActor("Analytic reflection receiver", 2, 2);
            auto* groundTransform = &world.findActor(groundActor)->transform;
            std::fill(std::begin(groundTransform->scale), std::end(groundTransform->scale), .4f);
            auto* donor = library.findMaterial(1);
            donor->baseColor[0] = .8f; donor->baseColor[1] = .15f; donor->baseColor[2] = .04f;
            donor->roughness = .7f; donor->metallic = 0; donor->grid = false;
            auto* ground = library.findMaterial(2);
            ground->baseColor[0] = ground->baseColor[1] = ground->baseColor[2] = .8f;
            ground->roughness = .05f; ground->metallic = 1; ground->grid = false;
            ground->setCullMode(Cull::Back);
            settings.cameraPitch = .4f;
            settings.debugView = 0;
            settings.rayTracing = true;
            for (const auto path : {Path::Forward, Path::Deferred}) for (const bool triangles : {false, true}) {
                settings.path = path;
                world.findActor(donorActor)->mesh.meshId = triangles ? 1002 : 2;
                donor->setCullMode(Cull::Back);
                const auto single = center(render());
                donor->setCullMode(Cull::Off);
                const auto both = center(render());
                donor->setCullMode(Cull::Front);
                const auto front = render();
                require(!nearPixel(single, both, 4), "Secondary-ray fixture did not isolate the reflected back face.");
                require(nearPixel(center(front), both), "Cull Front reflection differs from the equivalent Cull Off back face.");
                if (path == Path::Deferred)
                    savePixels(output / (triangles ? "triangle-cull-front-reflection.bmp" : "analytic-cull-front-reflection.bmp"), front);
            }
            library = std::move(savedLibrary);
            world = std::move(savedWorld);
            settings = savedSettings;
        }
        pass("analytic and generic-triangle reflection rays accept Cull Front back faces in both raster paths");

        {
            auto savedLibrary = std::move(library);
            auto savedWorld = std::move(world);
            const auto savedSettings = settings;
            library = {};
            library.createDefaults();
            library.meshes.push_back(makeQuad());
            world = {};
            world.blackBackground = true;
            world.createActor("Ray-distance GPU probe", 1001, 1);
            const auto closedActor = world.createActor("Offscreen closed primitive", 1, 2);
            world.findActor(closedActor)->transform.position[0] = 10;
            library.findMaterial(1)->setCullMode(Cull::Off);
            settings.debugView = 2;
            settings.rayTracing = true;
            settings.hardwareRayTracing = false;
            // The probe ray misses the visible quad and hits only the remote
            // closed object. Encoding its actual hit distance in a rendered
            // pixel observes near/far root selection independently of lighting.
            for (bool shadow : {false, true}) {
                const std::string probe =
                    "bool TraceScene(float3 o, float3 d, out float t, out float3 n, out int object, bool shadow);\n"
                    "Surface EvaluateMaterial(float3 p, float3 n, Surface m) {\n"
                    "float t; float3 hitNormal; int object;\n"
                    "bool hit = TraceScene(float3(10,0,3), float3(0,0,-1), t, hitNormal, object, " +
                    std::string(shadow ? "true" : "false") +
                    "); m.albedo = hit ? float3(t / 8.0, 0, 1) : float3(0,0,0); return m; }";
                require(renderer.applyShaderSource(probe, error), "Compile ray-distance probe: " + error);
                for (const assets::AssetId meshId : {assets::AssetId{1}, assets::AssetId{3}}) {
                    world.findActor(closedActor)->mesh.meshId = meshId;
                    for (const auto mode : {Cull::Off, Cull::Front, Cull::Back}) {
                        library.findMaterial(2)->setCullMode(mode);
                        const auto value = center(render());
                        const double actualDistance = std::pow(value[0] / 255.0, 2.2) * 8.0;
                        const bool useFarSurface = mode != Cull::Off && (shadow != (mode == Cull::Front));
                        require(value[2] > 250 && std::abs(actualDistance - (useFarSurface ? 4.0 : 2.0)) < .06,
                            "Closed analytic primitive selected the wrong cull/shadow near/far root.");
                    }
                }
            }
            require(renderer.applyShaderSource(
                "Surface EvaluateMaterial(float3 p, float3 n, Surface m) { return m; }", error), error);
            library = std::move(savedLibrary);
            world = std::move(savedWorld);
            settings = savedSettings;
        }
        pass("GPU hit-distance probes verify sphere/cube near/far roots for ordinary and reversed shadow rays");

        world.removeActor(blueActor);
        settings.hardwareRayTracing = true;
        settings.frustumCulling = true;
        settings.debugView = 2;
        (void)render();
        if (renderer.hardwareRayTracingSupported()) {
            require(renderer.hardwareRayTracingActive(), "Available DXR did not activate: " + renderer.hardwareRayTracingStatus());
            const auto initialDispatches = renderer.hardwareRayTracingDispatchCount();
            for (bool mirrored : {false, true}) for (bool back : {false, true}) {
                auto& transform = world.findActor(redActor)->transform;
                transform.rotation[1] = back ? 180.f : 0.f;
                transform.scale[0] = mirrored ? -1.f : 1.f;
                for (const auto mode : {Cull::Off, Cull::Front, Cull::Back}) {
                    library.findMaterial(101)->setCullMode(mode);
                    const auto value = center(render());
                    require(renderer.hardwareRayTracingActive(), "DXR became unavailable during face-state changes.");
                    const bool visible = mode == Cull::Off || (back ? mode == Cull::Front : mode == Cull::Back);
                    require(visible ? value[0] > 150 : nearPixel(value, {0, 0, 0}),
                        "DXR Cull Off/Front/Back disagrees with raster/authoring semantics.");
                }
            }
            report["faceStateDispatches"] = renderer.hardwareRayTracingDispatchCount() - initialDispatches;
            pass("actual DXR candidate filtering follows Cull Off/Front/Back and mirrored Actor changes");

            world.findActor(redActor)->transform = {};
            library.findMaterial(101)->setCullMode(Cull::Back);
            settings.debugView = 0;
            settings.globalIllumination.method = Method::PathTracing;
            settings.globalIllumination.mode = Mode::Realtime;
            settings.globalIllumination.samplesPerFrame = 2;
            settings.globalIllumination.targetSamples = 8;
            settings.globalIllumination.maxBounces = 3;
            (void)render();
            require(renderer.globalIlluminationStatus().effective == Method::PathTracing, "Realtime GI failed to activate.");
            for (const auto test : {Depth::Never, Depth::Equal, Depth::Greater, Depth::NotEqual,
                Depth::GreaterEqual, Depth::Always, Depth::Disabled}) {
                const auto dispatches = renderer.hardwareRayTracingDispatchCount();
                library.findMaterial(101)->depthTest = test;
                (void)render();
                const auto status = renderer.globalIlluminationStatus();
                require(!renderer.hardwareRayTracingActive() && status.requested == Method::PathTracing &&
                    status.effective == Method::Disabled && !status.reason.empty() &&
                    renderer.hardwareRayTracingDispatchCount() == dispatches,
                    "Nonstandard raster depth state produced mismatched hardware/GI visibility or lost its request.");
            }
            library.findMaterial(101)->depthTest = Depth::Less;
            library.findMaterial(101)->depthWrite = false;
            (void)render();
            require(!renderer.hardwareRayTracingActive() && renderer.globalIlluminationStatus().effective == Method::Disabled,
                "ZWrite Off incorrectly retained hardware GI.");
            library.findMaterial(101)->depthWrite = true;
            (void)render();
            require(renderer.hardwareRayTracingActive() && renderer.globalIlluminationStatus().effective == Method::PathTracing &&
                renderer.globalIlluminationStatus().accumulatedSamples == settings.globalIllumination.samplesPerFrame,
                "Restoring standard depth did not reactivate GI with fresh history.");
            pass("nonstandard depth states retain hardware/GI requests, explain inactivity and restore fresh accumulation");

            // A mirror looks back toward a red donor wholly BEHIND the camera.
            // Its raster draw can be culled, but its DXR geometry must survive.
            auto* mirror = library.findMaterial(101);
            mirror->baseColor[0] = mirror->baseColor[1] = mirror->baseColor[2] = .8f;
            mirror->metallic = 1; mirror->roughness = .045f;
            auto* donor = library.findMaterial(102);
            donor->baseColor[0] = .75f; donor->baseColor[1] = .025f; donor->baseColor[2] = .02f;
            const auto donorActor = world.createActor("Behind-camera reflected donor", 1001, 102);
            auto* actor = world.findActor(donorActor);
            actor->transform.position[2] = 8;
            actor->transform.rotation[1] = 180;
            actor->transform.scale[0] = actor->transform.scale[1] = 3;
            for (const bool realtime : {false, true}) {
                settings.globalIllumination.method = realtime ? Method::PathTracing : Method::Disabled;
                settings.frustumCulling = false;
                renderer.resetGlobalIllumination();
                const auto unculled = render();
                settings.frustumCulling = true;
                renderer.resetGlobalIllumination();
                const auto culled = render();
                require(changed(unculled, culled, 1) == 0, "Camera frustum culling removed a DXR/GI contributor.");
                actor->visible = false;
                renderer.resetGlobalIllumination();
                const auto absent = render();
                require(changed(culled, absent, 4) > 30, "Behind-camera donor did not affect reflected GPU pixels.");
                actor->visible = true;
                savePixels(output / (realtime ? "realtime-gi-offscreen-donor.bmp" : "hardware-offscreen-donor.bmp"), culled);
                report[realtime ? "realtimeOffscreenDonorPixels" : "hardwareOffscreenDonorPixels"] = changed(culled, absent, 4);
            }
            pass("offscreen contributors remain visible in hardware reflection and realtime GI when camera culling is enabled");
        } else {
            report["hardwareSkipped"] = renderer.hardwareRayTracingStatus();
            std::cout << "SKIP: material render states hardware checks: " << renderer.hardwareRayTracingStatus() << '\n';
        }
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "GPU validation reported errors.");
        restore.restore();
        report["passed"] = true;
        saveReport();
    } catch (const std::exception& failure) {
        report["error"] = failure.what();
        saveReport();
        throw;
    }
}
