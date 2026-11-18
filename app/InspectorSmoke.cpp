#include "InspectorSmoke.h"

#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Renderer/FrameCaptureArchive.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using Pixel = std::array<float, 4>;
using Json = nlohmann::json;
constexpr std::uint32_t Width = 256, Height = 256;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Frame Inspector smoke: " + message);
}
bool same(const Pixel& a, const Pixel& b, float tolerance = .00001f) {
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}
struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};
std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Could not open a saved capture fixture.");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(static_cast<bool>(stream), "Could not write a damaged capture fixture.");
}
bool hasWord(const std::string& text, const char* lower, const char* title) {
    return text.find(lower) != std::string::npos || text.find(title) != std::string::npos;
}
} // namespace

bool runInspectorSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& captureDirectory, std::string& error) {
    using namespace rae;
    error.clear();
    Json report{{"format", "RestArtEngine.FrameInspectorSmoke"}, {"version", 1},
        {"backend", renderer::mainRenderBackendKey(renderer.mainBackendSelection().effective)},
        {"checks", Json::array()}, {"passed", false}, {"supportsReplay", false}};
    const auto output = std::filesystem::absolute(captureDirectory);
    const auto saveReport = [&] {
        std::ofstream stream(output / "inspector-report.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n';
        stream.close();
        require(static_cast<bool>(stream), "Could not write the inspector report.");
    };
    const auto passed = [&](const char* message) {
        report["checks"].push_back(message);
        std::cout << "PASS: Frame Inspector " << message << '\n';
    };
    const auto originalWidth = renderer.sceneTextureWidth(), originalHeight = renderer.sceneTextureHeight();
    const auto originalOptions = renderer.frameCaptureOptions();
    const auto originalDiagnostics = renderer.diagnosticsEnabled();
    const auto originalRecording = diagnostics::profiler().isRecording();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::uint32_t width, height;
        renderer::FrameCaptureOptions options;
        bool diagnostics, recording;
        ~Restore() {
            try {
                renderer.setFrameCaptureOptions(options);
                renderer.setDiagnosticsEnabled(diagnostics);
                if (!recording) rae::diagnostics::profiler().stop();
                std::string ignored;
                if (width && height) renderer.resize(width, height, ignored);
            } catch (...) { /* Preserve the self-test's original result. */ }
        }
    } restore{renderer, originalWidth, originalHeight, originalOptions, originalDiagnostics, originalRecording};

    try {
        std::filesystem::create_directories(output);
        require(renderer.mainBackendReady(), "The main backend is not initialized.");
        require(renderer.resize(Width, Height, error), error);
        renderer.clearFrameCapture();
        renderer.setDiagnosticsEnabled(true);
        renderer.setFrameCaptureOptions({});
        if (!originalRecording) diagnostics::profiler().start();

        assets::AssetLibrary assets;
        assets.createDefaults();
        auto* material = assets.findMaterial(1);
        auto* mesh = assets.findMesh(1);
        require(material && mesh && mesh->indices.size() >= 6, "Default in-memory fixture assets are unavailable.");
        material->baseColor[0] = .25f; material->baseColor[1] = .5f; material->baseColor[2] = .75f;
        material->roughness = .33f; material->metallic = .72f; material->grid = false;
        const auto materialId = material->id;
        // Preserve the complete mesh while exercising a real nonzero firstIndex
        // and material section. Both sections intentionally use the same asset.
        const auto totalIndices = static_cast<std::uint32_t>(mesh->indices.size());
        const auto split = (totalIndices / 6) * 3;
        mesh->materialSlots = {{"First half", "first", materialId}, {"Second half", "second", materialId}};
        mesh->sections = {{0, split, 0}, {split, totalIndices - split, 1}};
        mesh->markGeometryModified();
        const auto meshId = mesh->id;
        scene::World world;
        world.blackBackground = true;
        const auto actor = world.createActor("Inspector center sphere", meshId, materialId);
        const auto secondActor = world.createActor("Inspector offset sphere", meshId, materialId);
        world.findActor(secondActor)->transform.position[0] = 2.4f;
        renderer::SceneSettings settings;
        settings.path = renderer::RenderPath::Deferred;
        settings.editorViewport = true;
        settings.viewportWidth = Width; settings.viewportHeight = Height;
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.frustumCulling = false;
        settings.cameraYaw = std::atan2(-3.f, -2.f);
        settings.cameraPitch = std::atan2(6.f, std::sqrt(13.f));
        settings.cameraDistance = 5.f;
        settings.cameraTarget[0] = settings.cameraTarget[1] = settings.cameraTarget[2] = 0;
        settings.cameraLens.nearClip = .2f;
        settings.cameraLens.farClip = 80.f;

        const auto frame = [&](bool capture = false, bool preview = false) {
            FrameScope applicationFrame;
            diagnostics::TraceScope scope("Inspector self-test frame", "Test");
            const auto id = diagnostics::currentFrameId();
            const auto oldCaptureFrame = renderer.frameCapture().frameId;
            if (capture) renderer.requestFrameCapture();
            if (preview) {
                require(renderer.renderMaterialPreview(*material, 64, error), error);
                require(renderer.frameCapture().frameId == oldCaptureFrame &&
                    hasWord(renderer.frameCapture().status, "pending", "Pending"),
                    "An offscreen material preview consumed the pending main-scene capture.");
            }
            require(renderer.render(settings, world, assets, error), error);
            require(renderer.present(false, error), error);
            return id;
        };
        const auto pixel = [&](std::size_t resource, std::uint32_t x = Width / 2, std::uint32_t y = Height / 2) {
            const auto& images = renderer.frameCapture().resources;
            std::ostringstream description;
            description << "resource=" << resource << ", x=" << x << ", y=" << y;
            if (resource < images.size()) {
                const auto& image = images[resource];
                description << ", name=" << image.name << ", format=" << image.format << ", layer=" << image.layer
                    << ", size=" << image.width << 'x' << image.height << ", pass=" << image.pass
                    << ", boundary=" << image.boundary;
            }
            float rgba[4]{};
            const bool read = renderer.readCapturedPixel(resource, x, y, rgba, error);
            const auto readError = error;
            description << ", RGBA=[" << rgba[0] << ", " << rgba[1] << ", " << rgba[2] << ", " << rgba[3] << ']';
            if (!read || std::any_of(std::begin(rgba), std::end(rgba), [](const auto value) { return !std::isfinite(value); }))
                report["failedPixel"] = description.str();
            require(read, "Captured texel readback failed: " + readError + "; " + description.str());
            for (const auto value : rgba)
                require(std::isfinite(value), "Captured raw texel contains a non-finite value; " + description.str());
            return Pixel{rgba[0], rgba[1], rgba[2], rgba[3]};
        };
        const auto resource = [&](const std::string& fragment) {
            const auto& resources = renderer.frameCapture().resources;
            const auto found = std::find_if(resources.begin(), resources.end(), [&](const auto& item) {
                return item.name.find(fragment) != std::string::npos;
            });
            require(found != resources.end(), "Captured resource not found: " + fragment);
            return static_cast<std::size_t>(found - resources.begin());
        };
        const auto verifyDisplays = [&] {
            const auto& capture = renderer.frameCapture();
            for (std::size_t index = 0; index < capture.resources.size(); ++index) {
                const auto& image = capture.resources[index];
                const auto x = image.width / 2, y = image.height / 2;
                const auto raw = pixel(index, x, y);
                Pixel rgb = raw;
                for (std::size_t c = 0; c < 3; ++c) {
                    if (image.displayKind == 1) rgb[c] = std::pow(std::clamp(raw[c], 0.f, 1.f), 1.f / 2.2f);
                    if (image.displayKind == 2) rgb[c] = raw[c] * .5f + .5f;
                    if (image.displayKind == 3) rgb[c] = std::abs(raw[c]) * .25f - std::floor(std::abs(raw[c]) * .25f);
                    if (image.displayKind == 4) {
                        const auto n = capture.nearPlane, f = capture.farPlane;
                        rgb[c] = (capture.orthographic ? n + raw[0] * (f - n) : n * f / (f - raw[0] * (f - n))) / 100.f;
                    }
                }
                for (unsigned channel = 0; channel < 5; ++channel) {
                    Pixel expected = rgb;
                    if (channel) {
                        float component = raw[channel - 1];
                        if ((image.displayKind == 2 || image.displayKind == 3) && channel < 4) component = rgb[channel - 1];
                        if (image.displayKind == 4 && channel == 1) component = rgb[0];
                        expected = {component, component, component, 1};
                    }
                    expected[3] = 1;
                    for (auto& component : expected) component = std::clamp(component, 0.f, 1.f);
                    float actual[4]{};
                    require(renderer.readCapturedDisplayPixel(index, static_cast<renderer::DebugTextureChannel>(channel),
                        x, y, actual, error), error);
                    require(same({actual[0], actual[1], actual[2], actual[3]}, expected, 1.5f / 255.f),
                        "GPU display readback disagrees with stored resource channels: resource=" + std::to_string(index) +
                        ", channel=" + std::to_string(channel));
                }
            }
        };

        // Warm the pipelines before the frame whose timing/pixels are captured.
        frame();
        const auto capturedFrame = frame(true, true);
        const auto initial = renderer.frameCapture();
        require(initial.valid && initial.frameId == capturedFrame && initial.width == Width && initial.height == Height,
            "The immutable capture must identify its actual main application frame and dimensions.");
        require(initial.resources.size() >= 5 && initial.shaderRevision == renderer.shaderRevision() &&
            initial.renderPath == "Deferred" && !initial.backend.empty(),
            "The final Deferred capture is missing its resources or pipeline/backend identities.");
        require(std::abs(initial.nearPlane - settings.cameraLens.nearClip) < .00001f &&
            std::abs(initial.farPlane - settings.cameraLens.farClip) < .00001f,
            "Capture lens metadata was replaced by hard-coded depth conversion constants.");
        const auto albedoIndex = resource("GBuffer0");
        const auto baseline = pixel(albedoIndex);
        require(same(baseline, Pixel{.25f, .5f, .75f, .33f}, .002f),
            "Actual captured GBuffer0 does not contain the fixture's evaluated albedo/roughness.");
        report["capturedFrameId"] = capturedFrame;
        report["rawAlbedoRoughness"] = baseline;
        verifyDisplays();
        passed("all five resources and RGB/R/G/B/A display channels verified from actual GPU conversion pixels");
        report["resourceCount"] = initial.resources.size();
        report["drawCount"] = initial.events.size();
        passed("next-main-frame capture, preview exclusion, raw material texel and captured camera lens");

        std::set<std::uint64_t> eventIds, objectVersions;
        std::vector<std::vector<std::uint8_t>> objectValues;
        bool firstIndexObserved = false, secondActorObserved = false, bindingsObserved = false;
        for (const auto& event : initial.events) {
            require(event.eventId != 0 && eventIds.insert(event.eventId).second && event.passIndex < initial.passes.size(),
                "Draw events need unique nonzero IDs and valid captured Pass indices.");
            if (!event.actorId) continue;
            require(event.detailedState && event.meshId == meshId && event.materialId == materialId &&
                event.shaderRevision == initial.shaderRevision && !event.actorName.empty() &&
                !event.meshName.empty() && !event.materialName.empty() &&
                !event.vertexShader.empty() && !event.pipelineState.empty() &&
                event.viewport[2] > 0 && event.viewport[3] > 0,
                "A scene mesh Draw is missing its captured identity, PSO, Shader or viewport.");
            firstIndexObserved |= event.actorId == actor && event.firstIndex == split && event.materialSlot == 1;
            secondActorObserved |= event.actorId == secondActor;
            bindingsObserved |= !event.bindings.empty();
            for (const auto& constants : event.constants) {
                if (constants.slot != 1 || constants.bytes.empty()) continue;
                objectVersions.insert(constants.version);
                if (std::find(objectValues.begin(), objectValues.end(), constants.bytes) == objectValues.end())
                    objectValues.push_back(constants.bytes);
            }
        }
        require(firstIndexObserved && secondActorObserved && bindingsObserved && objectVersions.size() >= 2 && objectValues.size() >= 2,
            "Detailed Draw capture lost firstIndex/materialSlot, a second Actor, resource bindings or per-Draw constant versions.");
        require(!initial.materialShaderSource.empty() && !initial.pipelineShaderSource.empty(),
            "An offline inspection needs the captured Shader source identities/content.");
        require(std::any_of(initial.cpuTrace.events.begin(), initial.cpuTrace.events.end(), [&](const auto& event) {
            return event.frameId == capturedFrame && event.durationUs >= 0;
        }), "Captured CPU trace contains no completed scope from the captured application frame.");
        require(!initial.passes.empty() && std::all_of(initial.passes.begin(), initial.passes.end(), [](const auto& pass) {
            return std::isfinite(pass.cpuMs) && pass.cpuMs >= 0 && std::isfinite(pass.cpuStartMs) && pass.cpuStartMs >= 0;
        }), "Pass timings need finite frame-relative CPU data.");
        report["constantVersions"] = objectVersions.size();
        report["capturedCpuEvents"] = initial.cpuTrace.events.size();
        passed("unique Draw/Pass identities, real section offsets, binding/PSO/Shader details and immutable constant versions");

        for (unsigned attempt = 0; attempt < 24 &&
            renderer.frameCapture().gpuStatus != renderer::GpuTimingStatus::Ready; ++attempt) frame();
        require(renderer.frameCapture().gpuStatus == renderer::GpuTimingStatus::Ready &&
            renderer.frameCapture().gpuFrameId == capturedFrame && !renderer.frameCapture().gpuPasses.empty(),
            "The frozen capture must retain its own completed GPU timing, not a prior Live frame's sample.");
        const auto timed = renderer.diagnosticsFrame();
        require(timed.gpuStatus == renderer::GpuTimingStatus::Ready && timed.gpuFrameId != 0 &&
            timed.gpuFrameId <= timed.frameId && std::isfinite(timed.gpuTotalMs) && timed.gpuTotalMs >= 0 &&
            !timed.gpuPasses.empty(),
            "Real GPU timestamp results did not become Ready with an originating frame ID.");
        for (const auto& pass : timed.gpuPasses)
            require(std::isfinite(pass.gpuMs) && pass.gpuMs >= 0, "A Ready GPU Pass result is invalid.");
        report["gpuFrameId"] = timed.gpuFrameId;
        report["gpuTotalMs"] = timed.gpuTotalMs;
        report["gpuPasses"] = Json::array();
        for (const auto& pass : timed.gpuPasses) report["gpuPasses"].push_back({{"name", pass.name}, {"ms", pass.gpuMs}});
        passed("real GPU timestamp completion with original GPU frame identity and finite Pass times");

        const auto archivePath = output / "original-frame.raframe";
        require(renderer.saveFrameCapture(archivePath, error), error);
        require(renderer.exportFrameCapture(output / "frame-metadata.json", error), error);
        material->baseColor[0] = .85f; material->roughness = .81f;
        frame();
        require(renderer.renderMaterialPreview(*material, 80, error), error);
        require(renderer.resize(320, 192, error), error);
        frame();
        renderer.collectGarbage(0);
        require(renderer.frameCapture().frameId == capturedFrame && same(pixel(albedoIndex), baseline),
            "Later live rendering, preview, resize or GC changed a frozen capture.");
        for (unsigned channel = 0; channel < 5; ++channel)
            require(renderer.capturedTexture(albedoIndex, static_cast<renderer::DebugTextureChannel>(channel)) != nullptr,
                "Captured RGB/channel display allocation failed for the actual backend.");
        passed("frozen resources survive live material changes, independent preview, resize, GC and channel display creation");

        require(renderer.resize(Width, Height, error), error);
        frame(true);
        require(!same(pixel(resource("GBuffer0")), baseline, .01f),
            "A fresh capture did not observe the actual live material change.");
        require(renderer.loadFrameCapture(archivePath, error), error);
        require(renderer.frameCapture().valid && renderer.frameCapture().offline &&
            renderer.frameCapture().frameId == capturedFrame && same(pixel(resource("GBuffer0")), baseline),
            "Offline archive loading failed to restore original raw resource pixels independently of the live scene.");
        verifyDisplays();
        const auto originalPayload = readBytes(archivePath);
        require(originalPayload.size() > 64, "The saved bundle has no actual resource payloads.");
        auto corrupted = originalPayload;
        corrupted.back() ^= 0x80;
        writeBytes(output / "damaged-payload.raframe", corrupted);
        require(!renderer.loadFrameCapture(output / "damaged-payload.raframe", error) && !error.empty() &&
            renderer.frameCapture().valid && renderer.frameCapture().frameId == capturedFrame &&
            same(pixel(resource("GBuffer0")), baseline),
            "A damaged image payload must fail without replacing the last valid capture.");
        renderer::captureArchive::Archive malformed;
        require(renderer::captureArchive::load(archivePath, malformed, error), error);
        auto metadata = Json::parse(malformed.metadataJson);
        metadata["version"] = 999;
        malformed.metadataJson = metadata.dump();
        require(renderer::captureArchive::save(output / "unsupported-metadata.raframe", malformed, error), error);
        require(!renderer.loadFrameCapture(output / "unsupported-metadata.raframe", error) && !error.empty() &&
            renderer.frameCapture().frameId == capturedFrame && same(pixel(resource("GBuffer0")), baseline),
            "Unsupported metadata schema must fail transactionally even when archive checksums are valid.");
        passed("resource-bearing offline roundtrip and transactional rejection of damaged payload and unsupported metadata");

        renderer::FrameCaptureOptions limited;
        limited.budgetBytes = 1;
        renderer.setFrameCaptureOptions(limited);
        frame(true);
        require(renderer.frameCapture().valid && renderer.frameCapture().frameId == capturedFrame &&
            same(pixel(resource("GBuffer0")), baseline) && hasWord(renderer.frameCapture().status, "failed", "Failed"),
            "A resource-budget failure must retain the preceding valid capture and report failure.");
        renderer.setFrameCaptureOptions({});
        require(renderer.resize(Width, Height, error), error);
        frame();
        passed("explicit capture budget rejection retains the previous frozen resource set");

        const auto lighting = std::find_if(renderer.activePipeline().passes.begin(), renderer.activePipeline().passes.end(),
            [](const auto& pass) { return pass.kind == renderer::RenderPassKind::DeferredLighting; });
        require(lighting != renderer.activePipeline().passes.end(), "The fixture has no Deferred lighting Pass.");
        const auto lightingName = std::string(lighting->name);
        renderer.setFrameCaptureOptions({lightingName, true});
        frame(true);
        require(renderer.frameCapture().valid, "Selected-Pass capture failed.");
        bool before = false, after = false;
        for (std::size_t i = 0; i < renderer.frameCapture().resources.size(); ++i) {
            const auto& image = renderer.frameCapture().resources[i];
            require(image.pass == lightingName, "A selected-Pass resource has the wrong Pass identity.");
            before |= hasWord(image.boundary, "before", "Before");
            after |= hasWord(image.boundary, "after", "After");
            pixel(i, image.width / 2, image.height / 2);
        }
        require(before && after, "Initialized selected-Pass outputs must provide distinct Before and After resource identities.");
        require(renderer.saveFrameCapture(output / "lighting-boundaries.raframe", error), error);
        passed("selected lighting Pass Before/After resources retain exact stage identities and readable raw pixels");

        renderer.setFrameCaptureOptions({});
        settings.shadowMethod = renderer::ShadowMethod::ShadowMapCSM2;
        frame();
        const auto shadow = std::find_if(renderer.activePipeline().passes.begin(), renderer.activePipeline().passes.end(),
            [](const auto& pass) { return pass.kind == renderer::RenderPassKind::ShadowMap; });
        require(shadow != renderer.activePipeline().passes.end(), "CSM2 did not execute a Shadow Pass.");
        const auto shadowName = std::string(shadow->name);
        renderer.setFrameCaptureOptions({shadowName, false});
        frame(true);
        require(renderer.frameCapture().valid, "Selected shadow array capture failed.");
        std::set<std::uint32_t> layers;
        for (std::size_t i = 0; i < renderer.frameCapture().resources.size(); ++i) {
            const auto& image = renderer.frameCapture().resources[i];
            require(image.pass == shadowName, "A shadow snapshot has the wrong logical Pass identity.");
            if (image.format.find("32") == std::string::npos) continue;
            layers.insert(image.layer);
            const auto raw = pixel(i, image.width / 2, image.height / 2);
            require(raw[0] >= 0 && raw[0] <= 1, "Captured shadow device depth is outside [0,1]: layer=" +
                std::to_string(image.layer) + ", depth=" + std::to_string(raw[0]) + '.');
        }
        require(layers.contains(0) && layers.contains(1), "CSM2 capture did not preserve both actual array subresources.");
        require(renderer.saveFrameCapture(output / "shadow-cascades.raframe", error), error);
        passed("CSM2 shadow capture stores both depth-array layers with independent subresource metadata");

        if (renderer.mainBackendSelection().effective != renderer::MainRenderBackend::D3D12) {
            renderer.setFrameCaptureOptions({});
            settings.outlineActors = &actor;
            settings.outlineActorCount = 1;
            frame(true);
            bool nativeDraw = false;
            for (const auto& event : renderer.frameCapture().events) {
                if (event.command.find("(native)") == std::string::npos) continue;
                nativeDraw = true;
                require(!event.detailedState && event.vertexShader.empty() && event.pixelShader.empty(),
                    "Native stages must not inherit the previous RHI pass's Shader identity or detailed state.");
            }
            require(nativeDraw, "Selection outline did not exercise a native Draw inspection gap.");
            settings.outlineActors = nullptr;
            settings.outlineActorCount = 0;
            passed("native outline Draws explicitly omit unobserved bindings and prior-Pass Shader identities");
        }

        renderer.clearFrameCapture();
        float invalid[4]{};
        require(!renderer.frameCapture().valid && !renderer.capturedTexture(0) &&
            !renderer.readCapturedPixel(0, 0, 0, invalid, error),
            "Clear must invalidate capture resources and subsequent pixel reads.");
        require(renderer.validationErrorCount() == 0, "The graphics debug layer reported errors during inspection.");
        report["graphicsValidationErrors"] = renderer.validationErrorCount();
        report["graphicsValidationEnabled"] = renderer.validationEnabled();
        report["debugMessageCount"] = renderer.debugMessages().size();
        report["passed"] = true;
        passed("Clear invalidation and zero graphics debug-layer errors");
        saveReport();
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        report["error"] = error;
        try {
            const auto messages = renderer.debugMessages();
            report["graphicsMessages"] = messages;
            report["graphicsValidationErrors"] = renderer.validationErrorCount();
            for (const auto& message : messages) std::cerr << "Graphics debug: " << message << '\n';
        } catch (const std::exception& diagnosticError) {
            report["graphicsMessageError"] = diagnosticError.what();
        }
        try { saveReport(); } catch (...) {}
        std::cerr << error << '\n';
        return false;
    }
}
