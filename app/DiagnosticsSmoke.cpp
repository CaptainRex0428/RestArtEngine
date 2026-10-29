#include "DiagnosticsSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Scene/World.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Pixel = std::array<float, 4>;
using Microsoft::WRL::ComPtr;
void require(bool value, const std::string& text) { if (!value) throw std::runtime_error("Diagnostics: " + text); }
bool floatNear(float a, float b, float tolerance = 0.001f) { return std::abs(a - b) <= tolerance; }
bool same(const Pixel& a, const Pixel& b, float tolerance = 0.00001f) {
    for (std::size_t i = 0; i < a.size(); ++i) if (!floatNear(a[i], b[i], tolerance)) return false;
    return true;
}

// The display texture is already RGBA8. Inspecting it verifies the actual GPU
// conversion, not merely the raw capture or a CPU copy of the display formula.
Pixel displayPixel(rae::renderer::SceneRenderer& renderer, void* handle,
                   const std::filesystem::path& image = {}) {
    require(handle != nullptr, "Captured display SRV is null.");
    auto* view = static_cast<ID3D11ShaderResourceView*>(handle);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source, staging;
    require(SUCCEEDED(resource.As(&source)), "Captured display is not Texture2D.");
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Captured display was not converted to RGBA8.");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Could not create display readback.");
    renderer.context()->CopyResource(staging.Get(), source.Get());
    std::vector<unsigned char> bitmap;
    if (!image.empty()) bitmap.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Display readback Map failed.");
    const auto* center = static_cast<const unsigned char*>(mapped.pData) +
        static_cast<std::size_t>(desc.Height / 2) * mapped.RowPitch + desc.Width / 2 * 4;
    Pixel result{};
    for (unsigned i = 0; i < 4; ++i) result[i] = static_cast<float>(center[i]) / 255.0f;
    if (!image.empty()) {
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
            auto* target = bitmap.data() + static_cast<std::size_t>(y) * desc.Width * 4;
            for (UINT x = 0; x < desc.Width; ++x) {
                target[x * 4] = row[x * 4 + 2]; target[x * 4 + 1] = row[x * 4 + 1];
                target[x * 4 + 2] = row[x * 4]; target[x * 4 + 3] = 255;
            }
        }
    }
    renderer.context()->Unmap(staging.Get(), 0);
    if (!image.empty()) {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(bitmap.size());
        info.biSize = sizeof(info); info.biWidth = static_cast<LONG>(desc.Width);
        info.biHeight = -static_cast<LONG>(desc.Height); info.biPlanes = 1;
        info.biBitCount = 32; info.biCompression = BI_RGB;
        info.biSizeImage = static_cast<DWORD>(bitmap.size());
        std::ofstream stream(image, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
        stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
        stream.write(reinterpret_cast<const char*>(bitmap.data()), static_cast<std::streamsize>(bitmap.size()));
        stream.close();
        require(stream.good(), "Could not save captured resource display BMP.");
    }
    return result;
}
}

void runDiagnosticsSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& outputDirectory,
                         const std::string& standardMaterialSource) {
    using namespace rae;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    const auto oldSource = renderer.shaderSource();
    const bool oldDiagnostics = renderer.diagnosticsEnabled();
    struct Restore {
        renderer::SceneRenderer& renderer;
        const std::string& source;
        std::uint32_t width, height;
        bool enabled, restored = false;
        void restore() {
            std::string error;
            require(renderer.applyShaderSource(source, error), "Could not restore Shader: " + error);
            require(renderer.resize(width, height, error), "Could not restore render size: " + error);
            renderer.setDiagnosticsEnabled(enabled);
            restored = true;
        }
        ~Restore() { if (!restored) try { restore(); } catch (...) {} }
    } restore{renderer, oldSource, oldWidth, oldHeight, oldDiagnostics};
    nlohmann::json report{{"format", "RestArtEngine.DiagnosticsSmoke"}, {"version", 1},
        {"backend", "DX11"}, {"checks", nlohmann::json::array()}, {"passed", false}};
    const auto saveReport = [&] {
        std::ofstream stream(output / "diagnostics-report.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n'; stream.close();
        require(stream.good(), "Could not write diagnostics report.");
    };
    const auto pass = [&](const char* text) {
        report["checks"].push_back(text);
        std::cout << "PASS: Diagnostics " << text << '\n';
    };
    try {
        std::string error;
        require(!standardMaterialSource.empty(), "Missing engine standard material source.");
        require(renderer.applyShaderSource(standardMaterialSource, error), error);
        require(renderer.resize(256, 256, error), error);
        renderer.clearFrameCapture();
        renderer.setDiagnosticsEnabled(true);
        assets::AssetLibrary assets;
        assets.createDefaults();
        auto& material = *assets.findMaterial(1);
        material.baseColor[0] = 0.25f; material.baseColor[1] = 0.5f; material.baseColor[2] = 0.75f;
        material.roughness = 0.33f; material.metallic = 0.72f; material.grid = false;
        const auto baselineMaterial = material;
        scene::World world;
        world.blackBackground = true;
        const auto actor = world.createActor("Diagnostics sphere", 1, material.id);
        renderer::SceneSettings settings;
        settings.editorViewport = true;
        settings.viewportWidth = settings.viewportHeight = 256;
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.cameraYaw = std::atan2(-3.0f, -2.0f);
        settings.cameraPitch = std::atan2(6.0f, std::sqrt(13.0f));
        settings.cameraDistance = 5.0f;
        settings.cameraTarget[0] = settings.cameraTarget[1] = settings.cameraTarget[2] = 0.0f;
        const auto frame = [&](const assets::MaterialAsset* preview = nullptr, bool capture = false, bool toolPreview = false) {
            diagnostics::beginFrame();
            struct End { ~End() { diagnostics::endFrame(); } } end;
            const auto id = diagnostics::currentFrameId();
            const auto oldCapture = renderer.frameCapture().frameId;
            if (capture) renderer.requestFrameCapture();
            if (toolPreview) {
                require(renderer.renderMaterialPreview(material, 64, error), error);
                require(renderer.frameCapture().frameId == oldCapture &&
                    renderer.frameCapture().status.find("pending") != std::string::npos,
                    "Tool preview consumed a pending main-scene capture.");
            }
            require(renderer.render(settings, world, assets, error, preview), error);
            require(renderer.present(false, error), error);
            return id;
        };
        const auto pixel = [&](std::size_t resource, std::uint32_t x = 128, std::uint32_t y = 128) {
            float values[4]{};
            require(renderer.readCapturedPixel(resource, x, y, values, error), error);
            return Pixel{values[0], values[1], values[2], values[3]};
        };
        const auto id = frame(nullptr, true, true);
        require(renderer.frameCapture().valid && renderer.frameCapture().resources.size() == 5,
            "Deferred capture must own SceneColor, three GBuffer textures and Depth.");
        require(renderer.frameCapture().frameId == id && renderer.diagnosticsFrame().frameId == id &&
            renderer.frameCapture().shaderRevision == renderer.shaderRevision(), "Frame/capture/Shader identities differ.");
        bool actorEvent = false;
        for (const auto& event : renderer.frameCapture().events) {
            require(event.shaderRevision == renderer.frameCapture().shaderRevision, "Draw has a different Shader revision.");
            if (event.actorId == actor) {
                actorEvent = true;
                require(event.meshId == 1 && event.materialId == material.id && event.indexCount > 0 &&
                    event.vertexCount > 0 && event.pass == "GBuffer", "Draw event did not identify its sphere/resources/pass.");
            }
        }
        require(actorEvent && renderer.diagnosticsFrame().drawCalls == renderer.frameCapture().events.size(),
            "Main scene event list lost a draw.");
        const auto g0 = pixel(1), g1 = pixel(2), g2 = pixel(3), depth = pixel(4);
        require(floatNear(g0[0], 0.25f) && floatNear(g0[1], 0.5f) && floatNear(g0[2], 0.75f) && floatNear(g0[3], 0.33f),
            "GBuffer0 does not contain evaluated albedo and roughness.");
        require(floatNear(g1[3], 0.72f) && floatNear(g2[3], 1), "Metallic or geometry validity was not stored.");
        const float normalLength = std::sqrt(g1[0] * g1[0] + g1[1] * g1[1] + g1[2] * g1[2]);
        const float inverseLightLength = 1.0f / 7.0f;
        const float alignment = (-3 * g1[0] + 6 * g1[1] - 2 * g1[2]) * inverseLightLength;
        require(floatNear(normalLength, 1, 0.002f) && alignment > 0.99f, "World normal is invalid for the center sphere surface.");
        const float viewDepth = settings.cameraDistance - (-3 * g2[0] + 6 * g2[1] - 2 * g2[2]) * inverseLightLength;
        const float reconstructed = 5.0f / (100.0f - depth[0] * 99.95f);
        require(depth[0] > 0 && depth[0] < 1 && floatNear(viewDepth, reconstructed, 0.02f), "Depth and world position disagree.");
        report["pixels"]["gbuffer0"] = g0; report["pixels"]["gbuffer1"] = g1;
        report["pixels"]["gbuffer2"] = g2; report["pixels"]["depth"] = depth;
        pass("main-frame identity, preview exclusion, actual draw events and raw GBuffer/depth semantics");

        for (std::size_t resource = 0; resource < 5; ++resource) {
            const auto raw = pixel(resource);
            for (unsigned channel = 0; channel < 5; ++channel) {
                const auto actual = displayPixel(renderer, renderer.capturedTexture(resource,
                    static_cast<renderer::DebugTextureChannel>(channel)), output /
                    ("resource-" + std::to_string(resource) + "-channel-" + std::to_string(channel) + ".bmp"));
                Pixel expected = raw;
                if (channel == 0) {
                    for (unsigned component = 0; component < 3; ++component) {
                        if (resource == 1) expected[component] = std::pow(std::clamp(raw[component], 0.0f, 1.0f), 1.0f / 2.2f);
                        if (resource == 2) expected[component] = raw[component] * 0.5f + 0.5f;
                        if (resource == 3) expected[component] = std::abs(raw[component]) * 0.25f - std::floor(std::abs(raw[component]) * 0.25f);
                        if (resource == 4) expected[component] = reconstructed / 100.0f;
                    }
                } else {
                    float value = raw[channel - 1];
                    if (resource == 2 && channel < 4) value = value * 0.5f + 0.5f;
                    if (resource == 3 && channel < 4) value = std::abs(value) * 0.25f - std::floor(std::abs(value) * 0.25f);
                    if (resource == 4 && channel == 1) value = reconstructed / 100.0f;
                    expected[0] = expected[1] = expected[2] = value;
                }
                expected[3] = 1;
                for (auto& component : expected) component = std::clamp(component, 0.0f, 1.0f);
                require(same(actual, expected, 1.5f / 255.0f), "GPU RGB/channel display conversion differs from the stored resource.");
            }
        }
        pass("all five resources and all RGB/R/G/B/A display channels verified by GPU readback");

        auto draft = baselineMaterial;
        draft.baseColor[0] = 0.75f; draft.baseColor[1] = 0.25f; draft.baseColor[2] = 0.125f;
        draft.roughness = 0.67f; draft.metallic = 0.18f;
        frame(&draft, true);
        const auto draftG0 = pixel(1), draftG1 = pixel(2);
        require(floatNear(draftG0[0], draft.baseColor[0]) && floatNear(draftG0[3], draft.roughness) && floatNear(draftG1[3], draft.metallic),
            "Per-frame override did not reach the next captured GBuffer.");
        require(material.baseColor[0] == baselineMaterial.baseColor[0] && material.roughness == baselineMaterial.roughness &&
            material.metallic == baselineMaterial.metallic, "Renderer changed the committed local asset while previewing.");
        pass("next-frame material override reaches GPU without changing committed assets");

        for (const auto path : {renderer::RenderPath::Deferred, renderer::RenderPath::Forward}) {
            settings.path = path;
            for (int mode = 5; mode <= 8; ++mode) {
                settings.debugView = mode; settings.exposure = 1;
                frame(nullptr, true);
                require(renderer.frameCapture().resources.size() == (path == renderer::RenderPath::Deferred ? 5 : 2),
                    "Forward exposed stale GBuffer resources.");
                const auto actual = pixel(0);
                for (unsigned component = 0; component < 3; ++component) {
                    const float f0 = 0.04f * (1 - baselineMaterial.metallic) + baselineMaterial.baseColor[component] * baselineMaterial.metallic;
                    const float expected = mode == 5 ? baselineMaterial.roughness : mode == 6 ? baselineMaterial.metallic :
                        mode == 7 ? std::pow(f0, 1.0f / 2.2f) : viewDepth / 100.0f;
                    require(floatNear(actual[component], expected, 2.0f / 255.0f), "Viewport data mode rendered an incorrect center value.");
                }
                const auto background = pixel(0, 0, 0);
                require(floatNear(background[0], 0) && floatNear(background[1], 0) && floatNear(background[2], 0), "Diagnostic background contains sky values.");
                settings.exposure = 8;
                frame(nullptr, true);
                require(same(pixel(0), actual, 1.0f / 255.0f), "Exposure changed a material/depth data mode.");
                report["viewportPixels"][std::string(path == renderer::RenderPath::Deferred ? "deferred-" : "forward-") +
                    std::to_string(mode)] = actual;
            }
            settings.exposure = 1;
            Pixel diffuse{};
            for (int mode = 9; mode <= 10; ++mode) {
                settings.debugView = mode; frame(nullptr, true);
                const auto actual = pixel(0);
                require(actual[0] > 0.05f && actual[1] > 0.05f && actual[2] > 0.05f,
                    "Direct-light diagnostic did not render a visible surface contribution.");
                if (mode == 9) diffuse = actual;
                else require(!same(diffuse, actual, 0.01f), "Diffuse/specular diagnostics produced indistinguishable results.");
                report["viewportPixels"][std::string(path == renderer::RenderPath::Deferred ? "deferred-" : "forward-") +
                    std::to_string(mode)] = actual;
            }
        }
        pass("Forward/Deferred viewport modes 5..10, data exposure independence and no stale Forward GBuffer");

        settings.path = renderer::RenderPath::Deferred; settings.debugView = 0;
        frame(nullptr, true);
        const auto frozenId = renderer.frameCapture().frameId, frozenRevision = renderer.frameCapture().shaderRevision;
        const auto frozenG0 = pixel(1), frozenDepth = pixel(4);
        const auto frozen = [&] {
            require(renderer.frameCapture().frameId == frozenId && renderer.frameCapture().shaderRevision == frozenRevision &&
                same(pixel(1), frozenG0) && same(pixel(4), frozenDepth), "Frozen capture changed after rendering/resource/Shader work.");
        };
        material.baseColor[0] = 0.95f; material.roughness = 0.81f; material.metallic = 0.05f;
        frame(); frozen();
        require(renderer.renderMaterialPreview(material, 64, error), error); frozen();
        require(renderer.resize(320, 192, error), error); frozen();
        frame(); frozen();
        require(renderer.applyShaderSource(standardMaterialSource + "\n// Diagnostics candidate success\n", error), error); frozen();
        frame(); frozen();
        require(renderer.reloadShaders(error), error); frozen();
        const auto beforeFailedRevision = renderer.shaderRevision();
        require(!renderer.applyShaderSource(standardMaterialSource + "\n#error DiagnosticExpectedCompileFailure\n", error) &&
            !error.empty() && renderer.shaderRevision() == beforeFailedRevision, "Invalid Shader candidate changed the active revision.");
        frozen();
        pass("immutable capture survives material/preview/main rendering, resize, hot compile/reload and failed candidate");

        const auto metadata = output / "frame-metadata.json";
        require(renderer.exportFrameCapture(metadata, error), error);
        std::ifstream metadataInput(metadata);
        const auto exported = nlohmann::json::parse(metadataInput);
        require(exported.at("metadataOnly").get<bool>() && !exported.at("containsImagePayloads").get<bool>() &&
            !exported.at("supportsReplay").get<bool>() && exported.at("frameId").get<std::uint64_t>() == frozenId &&
            exported.at("resources").size() == 5 && !exported.at("events").empty(), "Export misrepresents frame payload/replay or identities.");
        float invalid[4]{};
        require(!renderer.readCapturedPixel(99, 0, 0, invalid, error) && !error.empty(), "Invalid resource index was accepted.");
        require(!renderer.readCapturedPixel(0, 10000, 0, invalid, error) && !error.empty(), "Out-of-range pixel was accepted.");
        renderer.clearFrameCapture();
        require(!renderer.frameCapture().valid && !renderer.capturedTexture(0) &&
            !renderer.readCapturedPixel(0, 0, 0, invalid, error) && !error.empty(), "Clear did not invalidate snapshot access.");
        pass("metadata-only JSON contract, invalid resource/pixel errors and Clear");

        require(renderer.applyShaderSource(standardMaterialSource, error), error);
        require(renderer.resize(256, 256, error), error);
        material = baselineMaterial;
        bool gpuReady = false, unsupported = false;
        for (unsigned attempt = 0; attempt < 48; ++attempt) {
            frame();
            const auto& sample = renderer.diagnosticsFrame();
            if (sample.gpuStatus == renderer::GpuTimingStatus::Unavailable) { unsupported = true; break; }
            if (sample.gpuStatus != renderer::GpuTimingStatus::Ready) continue;
            require(sample.gpuFrameId > 0 && sample.gpuFrameId <= sample.frameId && !sample.gpuPasses.empty() &&
                std::isfinite(sample.gpuTotalMs) && sample.gpuTotalMs >= 0, "Invalid asynchronous GPU sample identity/duration.");
            for (const auto& timing : sample.gpuPasses)
                require(!timing.name.empty() && std::isfinite(timing.gpuMs) && timing.gpuMs >= 0,
                    "GPU pass contains a negative/nonfinite/unidentified duration.");
            report["gpuTiming"] = {{"frameId", sample.gpuFrameId}, {"cpuFrameId", sample.frameId},
                {"totalMs", sample.gpuTotalMs}, {"passCount", sample.gpuPasses.size()}};
            report["gpuTiming"]["passes"] = nlohmann::json::array();
            for (const auto& timing : sample.gpuPasses)
                report["gpuTiming"]["passes"].push_back({{"name", timing.name}, {"gpuMs", timing.gpuMs},
                    {"cpuSubmitMs", timing.cpuMs}, {"drawCalls", timing.drawCalls}});
            gpuReady = true; break;
        }
        require(gpuReady || unsupported, "GPU timing remained pending/disjoint after 48 bounded frames.");
        report["gpuTiming"]["supported"] = gpuReady;
        pass(gpuReady ? "asynchronous GPU result with explicit origin frame and finite nonnegative pass durations" :
            "GPU timestamp queries explicitly unavailable on this device (no zero-ms fabricated sample)");
        {
            // A separate tiny scene makes the hardware pipeline counters exact:
            // no sphere tessellation, tool draws, shadows or selection passes
            // can accidentally stand in for the main view's clipped geometry.
            struct GeometryRestore {
                scene::World& targetWorld;
                renderer::SceneSettings& targetSettings;
                assets::AssetLibrary& targetAssets;
                assets::MaterialAsset& targetMaterial;
                renderer::SceneRenderer& targetRenderer;
                scene::World savedWorld;
                renderer::SceneSettings savedSettings;
                assets::MaterialAsset savedMaterial;
                std::vector<assets::MeshAsset> savedMeshes;
                bool enabled;
                ~GeometryRestore() {
                    targetWorld = std::move(savedWorld);
                    targetSettings = savedSettings;
                    targetMaterial = savedMaterial;
                    targetAssets.meshes = std::move(savedMeshes);
                    targetRenderer.setDiagnosticsEnabled(enabled);
                }
            } geometryRestore{world, settings, assets, material, renderer, world, settings, material,
                assets.meshes, renderer.diagnosticsEnabled()};
            using GeometryStatus = renderer::GeometrySampleStatus;
            const auto statusName = [](GeometryStatus value) {
                switch (value) {
                case GeometryStatus::Pending: return "Pending";
                case GeometryStatus::Ready: return "Ready";
                case GeometryStatus::Unavailable: return "Unavailable";
                case GeometryStatus::NotApplicable: return "NotApplicable";
                case GeometryStatus::NoViewport: return "NoViewport";
                }
                return "Unknown";
            };
            const auto geometryJson = [&](const renderer::SceneGeometryStatistics& sample) {
                return nlohmann::json{{"frameId", sample.frameId}, {"sceneTriangles", sample.sceneTriangles},
                    {"gpuFrameId", sample.gpuFrameId}, {"inputTriangles", sample.inputTriangles},
                    {"postClipTriangles", sample.postClipTriangles}, {"status", statusName(sample.status)},
                    {"statusMessage", sample.statusMessage}};
            };
            auto& geometryReport = report["geometryStatistics"];
            geometryReport = {{"source", "D3D11 pipeline statistics for the main raster view"},
                {"cases", nlohmann::json::array()}, {"gpuReady", false}, {"unavailableCases", 0}};
            bool geometryGpuReady = false;
            const auto checkGeometry = [&](const std::string& name, std::uint64_t expectedScene,
                                           std::uint64_t expectedInput, std::uint64_t expectedClipped) {
                std::uint64_t firstMainFrame = 0;
                auto previousMainFrame = renderer.geometryStatistics().frameId;
                for (unsigned attempt = 0; attempt < 64; ++attempt) {
                    frame(); // Submit and Present real work; never spin/flush GetData.
                    const auto sample = renderer.geometryStatistics();
                    if (!firstMainFrame) firstMainFrame = sample.frameId;
                    require(sample.frameId == previousMainFrame + 1,
                        "Geometry main-frame sequence did not advance exactly once: " + name);
                    previousMainFrame = sample.frameId;
                    require(sample.sceneTriangles == expectedScene, "Wrong per-instance scene triangle count: " + name);
                    if (sample.status == GeometryStatus::Unavailable) {
                        require(!sample.statusMessage.empty(), "Unavailable geometry queries have no explanation.");
                        geometryReport["cases"].push_back({{"name", name}, {"result", "SKIP"},
                            {"reason", sample.statusMessage}, {"firstMainFrame", firstMainFrame},
                            {"frames", attempt + 1}, {"expectedScene", expectedScene},
                            {"expectedInput", expectedInput}, {"expectedPostClip", expectedClipped},
                            {"sample", geometryJson(sample)}});
                        geometryReport["unavailableCases"] = geometryReport["unavailableCases"].get<unsigned>() + 1;
                        std::cout << "SKIP: Geometry " << name << ": " << sample.statusMessage << '\n';
                        return;
                    }
                    require(sample.status == GeometryStatus::Pending || sample.status == GeometryStatus::Ready,
                        "Raster geometry sample has an unexpected status: " + name);
                    if (sample.status != GeometryStatus::Ready || sample.gpuFrameId < firstMainFrame) continue;
                    geometryReport["cases"].push_back({{"name", name}, {"result", "observed"},
                        {"firstMainFrame", firstMainFrame}, {"frames", attempt + 1},
                        {"expectedScene", expectedScene}, {"expectedInput", expectedInput},
                        {"expectedPostClip", expectedClipped}, {"sample", geometryJson(sample)}});
                    require(sample.gpuFrameId > 0 && sample.gpuFrameId <= sample.frameId &&
                        sample.inputTriangles == expectedInput && sample.postClipTriangles == expectedClipped,
                        "Wrong real GPU input/post-clipping count or sample identity: " + name);
                    geometryReport["cases"].back()["result"] = "PASS";
                    geometryGpuReady = true;
                    std::cout << "PASS: Geometry " << name << " (GPU frame " << sample.gpuFrameId << ")\n";
                    return;
                }
                geometryReport["cases"].push_back({{"name", name}, {"result", "FAIL"},
                    {"firstMainFrame", firstMainFrame}, {"frames", 64},
                    {"sample", geometryJson(renderer.geometryStatistics())}});
                require(false, "Geometry GPU query stayed pending/stale after 64 presented frames: " + name);
            };
            assets::AssetId quadMeshId = 0xfffe000000000001ull;
            while (assets.findMesh(quadMeshId) || assets.findMesh(quadMeshId + 1) || assets.findMesh(quadMeshId + 2)) ++quadMeshId;
            assets::MeshAsset quad;
            quad.id = quadMeshId; quad.name = "Geometry statistics quad";
            quad.primitive = assets::MeshPrimitive::Imported;
            quad.vertices = {{{-.5f, -.5f, 0}, {0, 0, 1}}, {{.5f, -.5f, 0}, {0, 0, 1}},
                {{.5f, .5f, 0}, {0, 0, 1}}, {{-.5f, .5f, 0}, {0, 0, 1}}};
            quad.indices = {0, 1, 2, 0, 2, 3};
            assets.meshes.push_back(quad);
            auto unused = quad;
            unused.id = quadMeshId + 1; unused.name = "Unused statistics quad";
            assets.meshes.push_back(std::move(unused));
            auto clipped = quad;
            clipped.id = quadMeshId + 2; clipped.name = "Near-plane split triangle";
            clipped.vertices = {{{-.02f, -.02f, 5.90f}, {0, 0, 1}},
                {{.02f, -.02f, 5.90f}, {0, 0, 1}}, {{0, .02f, 6.02f}, {0, 0, 1}}};
            clipped.indices = {0, 1, 2};
            assets.meshes.push_back(std::move(clipped));
            material.twoSided = true;
            settings = {};
            settings.editorViewport = true;
            settings.viewportWidth = settings.viewportHeight = 256;
            settings.cameraYaw = settings.cameraPitch = 0;
            settings.cameraDistance = 6;
            std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
            settings.shadowMethod = renderer::ShadowMethod::Disabled;
            for (const auto renderPath : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
                const std::string prefix = renderPath == renderer::RenderPath::Forward ? "forward/" : "deferred/";
                settings.path = renderPath;
                settings.shadowMethod = renderer::ShadowMethod::Disabled;
                settings.outlineActors = nullptr; settings.outlineActorCount = 0;
                world = {};
                const auto fixtureActor = world.createActor("Statistics quad", quadMeshId, material.id);
                checkGeometry(prefix + "in-view-and-unused-assets", 2, 2, 2);
                world.findActor(fixtureActor)->transform.position[0] = 1000;
                // CPU frustum rejection prevents submission altogether; with
                // it disabled, the GPU receives and clips both triangles.
                checkGeometry(prefix + "outside-frustum", 2, 0, 0);
                settings.frustumCulling = false;
                checkGeometry(prefix + "outside-frustum-gpu-only", 2, 2, 0);
                settings.frustumCulling = true;
                world.findActor(fixtureActor)->transform.position[0] = 0;
                const auto duplicate = world.duplicateActor(fixtureActor);
                // A depth-hidden identical instance still reaches primitive
                // clipping; these counts do not claim occlusion visibility.
                checkGeometry(prefix + "shared-mesh-duplicate", 4, 4, 4);
                world.findActor(duplicate)->visible = false;
                checkGeometry(prefix + "hidden-instance-stays-in-total", 4, 2, 2);
                world = {};
                checkGeometry(prefix + "empty-scene", 0, 0, 0);
                world.createActor("Missing material", quadMeshId, 0);
                checkGeometry(prefix + "missing-material-stays-in-total", 2, 0, 0);
                world = {};
                const auto outlinedActor = world.createActor("Main-pass isolation", quadMeshId, material.id);
                settings.shadowMethod = renderer::ShadowMethod::ShadowMap;
                checkGeometry(prefix + "shadow-pass-excluded", 2, 2, 2);
                settings.shadowMethod = renderer::ShadowMethod::Disabled;
                settings.outlineActors = &outlinedActor; settings.outlineActorCount = 1;
                checkGeometry(prefix + "outline-pass-excluded", 2, 2, 2);
                settings.outlineActors = nullptr; settings.outlineActorCount = 0;
                renderer.setDiagnosticsEnabled(false);
                checkGeometry(prefix + "profiler-disabled", 2, 2, 2);
                renderer.setDiagnosticsEnabled(true);
                const auto beforePreviews = geometryJson(renderer.geometryStatistics());
                require(renderer.renderMaterialPreview(material, 64, error), error);
                require(geometryJson(renderer.geometryStatistics()) == beforePreviews,
                    "Material tool preview changed main geometry identity/counts/status.");
                require(renderer.renderScenePreview(world, assets, 64, error), error);
                require(geometryJson(renderer.geometryStatistics()) == beforePreviews,
                    "Scene thumbnail preview changed main geometry identity/counts/status.");
                geometryReport["cases"].push_back({{"name", prefix + "tool-previews-excluded"},
                    {"result", "PASS"}, {"sample", beforePreviews}});
                settings.viewportVisible = false;
                world.duplicateActor(outlinedActor);
                const auto beforeHidden = renderer.geometryStatistics().frameId;
                frame();
                const auto hiddenSample = renderer.geometryStatistics();
                require(hiddenSample.status == GeometryStatus::NoViewport && hiddenSample.sceneTriangles == 4 &&
                    hiddenSample.frameId == beforeHidden + 1,
                    "Hidden viewport retained an active GPU sample or stopped updating scene topology.");
                geometryReport["cases"].push_back({{"name", prefix + "hidden-viewport"},
                    {"result", "PASS"}, {"sample", geometryJson(hiddenSample)}});
                settings.viewportVisible = true;
                world = {};
                world.createActor("Near clipping", quadMeshId + 2, material.id);
                checkGeometry(prefix + "near-clip-splits-one-into-two", 1, 1, 2);
            }
            world = {};
            world.createActor("Resize and hardware fixture", quadMeshId, material.id);
            checkGeometry("before-resize", 2, 2, 2);
            require(renderer.resize(272, 240, error), error);
            require(renderer.geometryStatistics().status != GeometryStatus::Ready,
                "Resize exposed a Ready GPU sample from the retired target/query ring.");
            geometryReport["cases"].push_back({{"name", "resize-invalidates-old-sample"},
                {"result", "PASS"}, {"sample", geometryJson(renderer.geometryStatistics())}});
            settings.viewportWidth = 272; settings.viewportHeight = 240;
            checkGeometry("after-resize", 2, 2, 2);
            require(renderer.resize(256, 256, error), error);
            settings.viewportWidth = settings.viewportHeight = 256;
            settings.hardwareRayTracing = true;
            settings.globalIllumination.method = renderer::GlobalIlluminationMethod::Disabled;
            frame();
            if (renderer.hardwareRayTracingSupported()) {
                require(renderer.hardwareRayTracingActive(),
                    "Supported hardware ray tracing failed to activate: " + renderer.hardwareRayTracingStatus());
                const auto hardwareSample = renderer.geometryStatistics();
                require(hardwareSample.status == GeometryStatus::NotApplicable && hardwareSample.sceneTriangles == 2,
                    "Pure hardware image resolve reported raster clipping counts.");
                geometryReport["cases"].push_back({{"name", "hardware-image-resolve"}, {"result", "PASS"},
                    {"sample", geometryJson(hardwareSample)}});
                settings.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
                settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
                settings.globalIllumination.indirectDiffuse = true;
                settings.globalIllumination.samplesPerFrame = 1;
                settings.globalIllumination.targetSamples = 16;
                settings.globalIllumination.maxBounces = 2;
                checkGeometry("realtime-GI-keeps-main-raster", 2, 2, 2);
                const auto giStatus = renderer.globalIlluminationStatus();
                require(renderer.hardwareRayTracingActive() &&
                    giStatus.effective == renderer::GlobalIlluminationMethod::PathTracing &&
                    giStatus.effectiveMode == renderer::GlobalIlluminationMode::Realtime,
                    "Realtime GI statistics case silently used a different rendering mode.");
            } else {
                require(!renderer.hardwareRayTracingActive(), "Unsupported hardware ray tracing reported an active dispatch.");
                const auto reason = renderer.hardwareRayTracingStatus();
                require(!reason.empty(), "Hardware geometry-statistics skip has no explanation.");
                for (const auto* name : {"hardware-image-resolve", "realtime-GI-keeps-main-raster"})
                    geometryReport["cases"].push_back({{"name", name}, {"result", "SKIP"}, {"reason", reason}});
                std::cout << "SKIP: Geometry hardware resolve/Realtime GI: " << reason << '\n';
            }
            geometryReport["gpuReady"] = geometryGpuReady;
            pass(geometryGpuReady ? "real GPU geometry counts, clipping splits, instance topology and main-view sample isolation" :
                "scene instance topology verified; unavailable geometry GPU queries explicitly skipped");
        }
        frame(nullptr, true); // Keep a valid Deferred snapshot for following GUI QA.
        restore.restore();
        require(renderer.validationErrorCount() == 0, "D3D11 validation reported a diagnostic error.");
        report["validationLayerEnabled"] = renderer.validationEnabled();
        report["validationErrors"] = renderer.validationErrorCount();
        report["passed"] = true;
        pass("D3D11 validation errors zero; original active source/target size restored; final frozen capture retained");
        saveReport();
    } catch (const std::exception& exception) {
        report["error"] = exception.what();
        saveReport();
        throw;
    }
}
