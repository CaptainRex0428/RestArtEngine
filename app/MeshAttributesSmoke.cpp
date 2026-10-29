#include "MeshAttributesSmoke.h"
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
    if (!value) throw std::runtime_error("Mesh attributes: " + message);
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

std::size_t changed(const Pixels& a, const Pixels& b, int tolerance = 2) {
    require(a.size() == b.size(), "Mismatched GPU comparison dimensions.");
    std::size_t result = 0;
    for (std::size_t i = 0; i < a.size(); i += 4)
        if (std::abs(int(a[i]) - int(b[i])) > tolerance ||
            std::abs(int(a[i + 1]) - int(b[i + 1])) > tolerance ||
            std::abs(int(a[i + 2]) - int(b[i + 2])) > tolerance) ++result;
    return result;
}


std::array<int, 3> pixel(const Pixels& pixels, unsigned x, unsigned y) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}
rae::assets::MeshAsset splitQuads() {
    rae::assets::MeshAsset mesh;
    mesh.id = 1001; mesh.name = "Two material sections with four UV channels";
    mesh.primitive = rae::assets::MeshPrimitive::Imported;
    mesh.vertices = {{{-2,-1,0},{0,0,1}},{{-.1f,-1,0},{0,0,1}},{{-.1f,1,0},{0,0,1}},{{-2,1,0},{0,0,1}},
        {{.1f,-1,0},{0,0,1}},{{2,-1,0},{0,0,1}},{{2,1,0},{0,0,1}},{{.1f,1,0},{0,0,1}}};
    mesh.indices = {0,1,2,0,2,3,4,5,6,4,6,7};
    mesh.materialSlots = {{"Left", "fixture/left", 101}, {"Right", "fixture/right", 102}};
    mesh.sections = {{0,6,0}, {6,6,1}};
    for (unsigned channel = 0; channel < 4; ++channel) {
        mesh.uvNames[channel] = "UV" + std::to_string(channel);
        for (unsigned vertex = 0; vertex < 8; ++vertex) {
            const bool right = vertex >= 4;
            const bool highX = vertex % 4 == 1 || vertex % 4 == 2;
            const bool highY = vertex % 4 >= 2;
            mesh.uvChannels[channel].push_back({.08f + channel * .12f + (right ? .2f : 0) + (highX ? .2f : 0),
                .1f + channel * .1f + (highY ? .2f : 0)});
        }
    }
    return mesh;
}
struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};
} // namespace

void runMeshAttributesSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Path = renderer::RenderPath;
    using Method = renderer::GlobalIlluminationMethod;
    using Mode = renderer::GlobalIlluminationMode;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    const auto source = renderer.shaderSource();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::string source;
        unsigned width, height;
        ~Restore() {
            std::string ignored;
            renderer.applyShaderSource(source, ignored);
            renderer.resize(width, height, ignored);
            renderer.resetGlobalIllumination();
        }
    } restore{renderer, source, oldWidth, oldHeight};
    Json report{{"format", "RestArtEngine.MeshAttributesSmoke"}, {"version", 1},
        {"adapter", renderer.adapterName()}, {"checks", Json::array()}, {"passed", false}};
    const auto save = [&] { std::ofstream file(output / "mesh-attributes.json"); file << report.dump(2) << '\n'; };
    const auto pass = [&](const char* text) { report["checks"].push_back(text); std::cout << "PASS: mesh attributes: " << text << '\n'; };
    try {
        std::string error;
        require(renderer.resize(width, height, error), error);
        assets::AssetLibrary library;
        library.meshes.push_back(splitQuads());
        assets::MaterialAsset red;
        red.id = 101; red.name = "Left red"; red.baseColor[0] = .8f; red.baseColor[1] = red.baseColor[2] = .02f;
        red.metallic = 0; red.roughness = .7f;
        auto blue = red; blue.id = 102; blue.name = "Right blue"; std::swap(blue.baseColor[0], blue.baseColor[2]);
        library.materials = {red, blue};
        scene::World world; world.blackBackground = true;
        const auto actor = world.createActor("Shared mesh with independent materials", 1001, 101);
        renderer::SceneSettings settings;
        settings.editorViewport = true; settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = settings.cameraPitch = 0; settings.cameraDistance = 6;
        std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
        settings.shadowMethod = renderer::ShadowMethod::Disabled; settings.debugView = 2;
        const auto render = [&](const assets::MaterialAsset* preview = nullptr) {
            FrameScope frame;
            require(renderer.render(settings, world, library, error, preview), error);
            auto pixels = readScene(renderer);
            require(renderer.present(false, error), error);
            return pixels;
        };
        const auto compile = [&](const std::string& text) {
            require(renderer.applyShaderSource(text, error), "Shader application failed: " + error);
        };
        const std::string identity = "Surface EvaluateMaterial(float3 p, float3 n, Surface m) { return m; }";
        compile(identity);
        Pixels original;
        for (const auto path : {Path::Forward, Path::Deferred}) {
            settings.path = path;
            original = render();
            const auto left = pixel(original, 72, 72), right = pixel(original, 120, 72);
            require(left[0] > left[2] + 100 && right[2] > right[0] + 100, "Section materials are not independently visible.");
            auto draft = blue; draft.baseColor[1] = .8f; draft.baseColor[2] = .02f;
            const auto preview = render(&draft);
            require(pixel(preview, 72, 72) == left && pixel(preview, 120, 72)[1] > 180,
                "A material draft leaked into a different slot or failed to update its slot.");
            require(changed(render(), original, 0) == 0, "Discarding a slot draft did not restore pixels.");
            world.findActor(actor)->mesh.materialOverrides = {102, 101};
            const auto swapped = render();
            require(pixel(swapped,72,72)[2] > 180 && pixel(swapped,120,72)[0] > 180,
                "Per-instance slot overrides failed.");
            world.findActor(actor)->mesh.materialOverrides.clear();
            library.findMaterial(102)->depthTest = assets::MaterialDepthTest::Never;
            const auto hidden = render();
            require(pixel(hidden,72,72) == left && pixel(hidden,120,72)[2] == 0,
                "Per-section depth state affected a sibling slot.");
            library.findMaterial(102)->depthTest = assets::MaterialDepthTest::Less;
            require(changed(render(), original, 0) == 0, "Section depth restore changed pixels.");
        }
        savePixels(output / "material-slots.bmp", original);
        pass("Forward/Deferred section defaults, independent live drafts, Actor overrides and section depth states");

        const auto dispatchStart = renderer.hardwareRayTracingDispatchCount();
        if (renderer.hardwareRayTracingSupported()) {
            settings.hardwareRayTracing = true;
            const auto hardware = render();
            require(renderer.hardwareRayTracingActive(), renderer.hardwareRayTracingStatus());
            require(changed(hardware, original, 2) < 50, "DXR section materials disagree with the raster red/blue fixture.");
            const auto left = pixel(hardware,72,72), right = pixel(hardware,120,72);
            require(left[0] > left[2] + 100 && right[2] > right[0] + 100,
                "DXR collapsed separate section materials into one Actor material.");
            auto draft = blue; draft.baseColor[1] = .8f; draft.baseColor[2] = .02f;
            const auto preview = render(&draft);
            require(pixel(preview,72,72) == left && pixel(preview,120,72)[1] > 180,
                "DXR section material draft affected a sibling or stayed stale.");
            world.findActor(actor)->mesh.materialOverrides = {102,101};
            const auto swapped = render();
            require(pixel(swapped,72,72)[2] > 180 && pixel(swapped,120,72)[0] > 180,
                "DXR did not invalidate per-instance slot material overrides.");
            world.findActor(actor)->mesh.materialOverrides.clear();
            require(changed(render(), hardware, 0) == 0, "DXR slot override restoration changed the original pixels.");
            savePixels(output / "material-slots-dxr.bmp", hardware);
            settings.hardwareRayTracing = false;
            pass("DXR independently resolves red/blue section materials, one-slot live preview and Actor slot overrides");
        }
        for (unsigned channel = 0; channel < 4; ++channel) {
            compile("Surface EvaluateMaterial(float3 p, float3 n, Surface m) { m.albedo = float3(m.uv" +
                std::to_string(channel) + ", m.uvMask == 15u ? 0.5 : 0.0); return m; }");
            settings.hardwareRayTracing = false; settings.path = Path::Forward;
            const auto forward = render();
            settings.path = Path::Deferred;
            const auto deferred = render();
            require(changed(forward, deferred, 1) == 0, "UV material differs between Forward and Deferred.");
            require(pixel(forward,72,72)[2] > 180 && pixel(forward,120,72)[0] > pixel(forward,72,72)[0] + 15,
                "UV mask or independent section UV interpolation is missing.");
            auto& mesh = library.meshes.front();
            mesh.uvChannels[channel][0][0] += .25f; mesh.markGeometryModified();
            const auto edited = render();
            require(changed(edited, forward, 2) > 100, "An in-place UV edit did not invalidate the raster upload.");
            mesh.uvChannels[channel][0][0] -= .25f; mesh.markGeometryModified();
            require(changed(render(), forward, 1) == 0, "Restoring UV did not restore raster pixels.");
            if (renderer.hardwareRayTracingSupported()) {
                settings.hardwareRayTracing = true;
                const auto hardware = render();
                require(renderer.hardwareRayTracingActive(), renderer.hardwareRayTracingStatus());
                require(changed(hardware, forward, 3) < 50, "DXR UV barycentrics disagree with raster interpolation.");
                mesh.uvChannels[channel][0][0] += .25f; mesh.markGeometryModified();
                require(changed(render(), hardware, 2) > 100, "An in-place UV edit did not invalidate DXR UV data.");
                mesh.uvChannels[channel][0][0] -= .25f; mesh.markGeometryModified();
                require(changed(render(), hardware, 1) == 0, "Restoring UV did not restore hardware pixels.");
                savePixels(output / ("uv" + std::to_string(channel) + "-dxr.bmp"), hardware);
            }
            savePixels(output / ("uv" + std::to_string(channel) + "-raster.bmp"), forward);
        }
        pass("UV0-3 and mask are independently readable; raster/DXR barycentrics and same-size UV edit invalidation agree");

        // Query offscreen geometry from a project material. This observes the
        // software ray buffers through actual raster pixels, without recursive
        // material evaluation or assuming a reflection lighting threshold.
        settings.hardwareRayTracing = false; settings.rayTracing = true;
        auto remote = splitQuads(); remote.id = 1002;
        library.meshes.push_back(remote);
        const auto remoteActor = world.createActor("Offscreen UV donor", 1002, 101);
        world.findActor(remoteActor)->transform.position[0] = 10;
        compile("bool TraceScene(float3 o,float3 d,out float t,out float3 n,out int obj,bool shadow);\n"
            "Surface EvaluateMaterial(float3 p,float3 n,Surface m) { float t; float3 hn; int obj;"
            "bool hit=TraceScene(float3(p.x<0?9:11,0,3),float3(0,0,-1),t,hn,obj,false);"
            "m.albedo=0; if(hit && obj < -1) m.albedo=rayTriangles[-obj-2].color.rgb; return m; }");
        const auto raySlots = render();
        require(pixel(raySlots,72,72)[0] > 180 && pixel(raySlots,120,72)[2] > 180,
            "Software rays did not resolve the two donor section materials independently.");
        world.findActor(remoteActor)->mesh.materialOverrides = {102,101};
        const auto raySwapped = render();
        require(pixel(raySwapped,72,72)[2] > 180 && pixel(raySwapped,120,72)[0] > 180,
            "Software ray slot material overrides did not invalidate the scene cache.");
        world.findActor(remoteActor)->mesh.materialOverrides.clear();
        require(changed(render(), raySlots, 0) == 0, "Software ray slot override restore changed the donor image.");
        pass("Software rays resolve both offscreen section materials and update Actor slot overrides");
        compile("bool TraceScene(float3 o,float3 d,out float t,out float3 n,out int obj,bool shadow);\n"
            "Surface EvaluateMaterial(float3 p,float3 n,Surface m) { float t; float3 hn; int obj;"
            "bool hit=TraceScene(float3(9,0,3),float3(0,0,-1),t,hn,obj,false);"
            "m.albedo=0; if(hit && obj < -1) { RayTriangle tri=rayTriangles[-obj-2]; uint token=asuint(tri.color.w);"
            "if(token>0) { uint4 a=rayUvTriangles[token-1]; m.albedo=float3(rayUvVertices[a.x*2+1].zw, a.w==15?0.5:0); }} return m; }");
        const auto rayUv = render();
        require(pixel(rayUv,72,72)[2] > 180, "Software triangle rays did not provide UV3 / mask.");
        library.meshes.back().uvChannels[3][0][0] += .25f;
        library.meshes.back().markGeometryModified();
        require(changed(render(), rayUv, 2) > 1000, "Software ray UV data stayed stale after editing the donor.");
        world.removeActor(remoteActor); library.meshes.pop_back(); settings.rayTracing = false;
        pass("Software triangle rays carry UV3 and the channel mask; offscreen UV edits invalidate their GPU stream");

        compile("Surface EvaluateMaterial(float3 p,float3 n,Surface m) { m.albedo=float3(m.uv3,0.3); return m; }");
        settings.hardwareRayTracing = renderer.hardwareRayTracingSupported();
        const auto beforeFailure = render();
        const auto revision = renderer.shaderRevision();
        require(!renderer.applyShaderSource("invalid shader syntax", error), "Invalid UV shader unexpectedly compiled.");
        require(renderer.shaderRevision() == revision && changed(render(), beforeFailure, 0) == 0,
            "Failed shader compilation replaced the valid UV pipeline.");
        pass("UV-aware material hot compilation fails atomically and retains the previous rendered image");

        if (renderer.hardwareRayTracingSupported()) {
            settings.debugView = 0;
            settings.globalIllumination.method = Method::PathTracing;
            settings.globalIllumination.mode = Mode::Realtime;
            settings.globalIllumination.samplesPerFrame = 1;
            settings.globalIllumination.targetSamples = 8;
            settings.globalIllumination.maxBounces = 2;
            Pixels litBaseline;
            for (unsigned i=0; i<3; ++i) litBaseline = render();
            require(renderer.globalIlluminationStatus().effective == Method::PathTracing &&
                renderer.globalIlluminationStatus().effectiveMode == Mode::Realtime, "Realtime GI did not activate for the multi-slot mesh.");
            auto& mesh = library.meshes.front();
            for (auto& uv : mesh.uvChannels[3]) uv[0] = .02f;
            mesh.markGeometryModified();
            const auto historyBefore = renderer.globalIlluminationStatus().historyRevision;
            const auto edited = render();
            require(renderer.globalIlluminationStatus().historyRevision > historyBefore,
                "UV edits did not reset realtime GI history.");
            require(changed(edited, litBaseline, 4) > 1000, "UV material did not reach the realtime raster receiver.");
            savePixels(output / "uv-realtime-gi.bmp", edited);
            settings.globalIllumination.mode = Mode::Progressive;
            renderer.resetGlobalIllumination();
            render();
            require(renderer.globalIlluminationStatus().effectiveMode == Mode::Progressive,
                "Progressive GI did not activate for UV-bearing sections.");
            pass("Realtime/progressive GI consume UV-bearing sections and UV edits reset history");
        } else report["hardwareSkipped"] = renderer.hardwareRayTracingStatus();
        report["hardwareDispatches"] = renderer.hardwareRayTracingDispatchCount() - dispatchStart;
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation reported errors.");
        report["passed"] = true; save();
    } catch (const std::exception& failure) { report["error"] = failure.what(); save(); throw; }
}
