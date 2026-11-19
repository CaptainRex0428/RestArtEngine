#include "AntiAliasingSmoke.h"

#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Core/TraceProfiler.h>
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
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Pixels = std::vector<std::uint8_t>;
using Json = nlohmann::json;
constexpr std::uint32_t width = 192, height = 144, referenceScale = 4;
constexpr char identitySource[] =
    "Surface EvaluateMaterial(float3 p, float3 n, Surface m) { return m; }";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Anti-aliasing smoke: " + message);
}

struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};

// Write a top-down 32-bit BMP without introducing graphics-API dependencies.
void savePixels(const std::filesystem::path& path, const Pixels& pixels,
    std::uint32_t imageWidth, std::uint32_t imageHeight) {
    require(pixels.size() == static_cast<std::size_t>(imageWidth) * imageHeight * 4,
        "Invalid evidence image dimensions.");
    std::array<std::uint8_t, 54> header{};
    const auto put = [&](std::size_t at, std::uint32_t value, unsigned bytes = 4) {
        for (unsigned i = 0; i < bytes; ++i) header[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
    };
    header[0] = 'B'; header[1] = 'M';
    put(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put(10, 54); put(14, 40); put(18, imageWidth);
    put(22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(imageHeight)));
    put(26, 1, 2); put(28, 32, 2); put(34, static_cast<std::uint32_t>(pixels.size()));
    auto bgra = pixels;
    for (std::size_t p = 0; p < bgra.size(); p += 4) std::swap(bgra[p], bgra[p + 2]);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    stream.close();
    require(stream.good(), "Cannot save evidence image.");
}

Pixels readScene(rae::renderer::SceneRenderer& renderer, std::uint32_t expectedWidth,
    std::uint32_t expectedHeight) {
    Pixels pixels;
    std::uint32_t actualWidth = 0, actualHeight = 0;
    std::string error;
    require(renderer.readScenePixels(pixels, actualWidth, actualHeight, error), error);
    require(actualWidth == expectedWidth && actualHeight == expectedHeight &&
        pixels.size() == static_cast<std::size_t>(actualWidth) * actualHeight * 4,
        "Readback dimensions differ from the actual scene target.");
    return pixels;
}

std::size_t changedPixels(const Pixels& a, const Pixels& b, int tolerance = 2) {
    require(a.size() == b.size(), "Comparison image dimensions differ.");
    std::size_t changed = 0;
    for (std::size_t p = 0; p < a.size(); p += 4)
        if (std::abs(int(a[p]) - int(b[p])) > tolerance ||
            std::abs(int(a[p + 1]) - int(b[p + 1])) > tolerance ||
            std::abs(int(a[p + 2]) - int(b[p + 2])) > tolerance) ++changed;
    return changed;
}

Pixels downsampleReference(const Pixels& high) {
    require(high.size() == static_cast<std::size_t>(width * referenceScale) * height * referenceScale * 4,
        "Invalid supersampled reference.");
    Pixels result(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x)
        for (unsigned c = 0; c < 4; ++c) {
            unsigned sum = 0;
            for (std::uint32_t dy = 0; dy < referenceScale; ++dy)
                for (std::uint32_t dx = 0; dx < referenceScale; ++dx)
                    sum += high[((static_cast<std::size_t>(y * referenceScale + dy) * width * referenceScale)
                        + x * referenceScale + dx) * 4 + c];
            result[(static_cast<std::size_t>(y) * width + x) * 4 + c] =
                static_cast<std::uint8_t>((sum + referenceScale * referenceScale / 2) /
                    (referenceScale * referenceScale));
        }
    return result;
}

// Build the same edge ROI for every method, from the independent 4x reference.
// Include a one-pixel ring around high contrast edges so blur cannot improve
// the score simply by moving an edge outside the measured pixels.
std::vector<bool> edgeMask(const Pixels& reference) {
    std::vector<bool> result(static_cast<std::size_t>(width) * height, false);
    for (std::uint32_t y = 2; y + 2 < height; ++y)
        for (std::uint32_t x = 2; x + 2 < width; ++x) {
            int low = 255, high = 0;
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const auto at = (static_cast<std::size_t>(int(y) + dy) * width + int(x) + dx) * 4;
                low = std::min(low, int(reference[at])); high = std::max(high, int(reference[at]));
            }
            if (high - low > 32) result[static_cast<std::size_t>(y) * width + x] = true;
        }
    return result;
}

double edgeError(const Pixels& image, const Pixels& reference, const std::vector<bool>& mask) {
    require(image.size() == reference.size(), "Invalid quality comparison dimensions.");
    double error = 0;
    std::size_t count = 0;
    for (std::size_t p = 0; p < mask.size(); ++p) if (mask[p]) {
        for (unsigned c = 0; c < 3; ++c)
            error += std::abs(int(image[p * 4 + c]) - int(reference[p * 4 + c]));
        ++count;
    }
    require(count > 500, "Supersampling fixture has too few visible edges.");
    return error / (count * 3);
}

bool hasPass(const rae::renderer::SceneRenderer& renderer, rae::renderer::RenderPassKind kind) {
    const auto& passes = renderer.activePipeline().passes;
    return std::any_of(passes.begin(), passes.end(), [kind](const auto& pass) { return pass.kind == kind; });
}

rae::assets::MeshAsset edgeMesh() {
    rae::assets::MeshAsset mesh;
    mesh.id = 81001; mesh.name = "AA high-contrast diagonal and subpixel strips";
    mesh.primitive = rae::assets::MeshPrimitive::Imported;
    const auto triangle = [&](float ax, float ay, float bx, float by, float cx, float cy) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({{ax, ay, 0}, {0, 0, 1}});
        mesh.vertices.push_back({{bx, by, 0}, {0, 0, 1}});
        mesh.vertices.push_back({{cx, cy, 0}, {0, 0, 1}});
        mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2});
    };
    triangle(-2.65f, -1.8f, -.15f, -1.67f, -2.41f, 1.79f);
    // Different widths/slopes exercise more than one favorable edge direction.
    for (int i = 0; i < 8; ++i) {
        const float ax = -.03f + i * .35f, ay = -1.75f + i * .05f;
        const float bx = ax - .43f, by = 1.68f - i * .09f;
        const float half = .018f + (i % 3) * .015f;
        triangle(ax - half, ay, ax + half, ay, bx + half, by);
        triangle(ax - half, ay, bx + half, by, bx - half, by);
    }
    return mesh;
}
} // namespace

void runAntiAliasingSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using AA = renderer::AntiAliasingMethod;
    using Pass = renderer::RenderPassKind;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    const auto oldSource = renderer.shaderSource();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    const auto oldValidationErrors = renderer.validationErrorCount();
    const auto oldCaptureOptions = renderer.frameCaptureOptions();
    const auto oldDiagnostics = renderer.diagnosticsEnabled();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::string source;
        std::uint32_t width, height;
        renderer::FrameCaptureOptions captureOptions;
        bool diagnostics;
        bool done = false;
        void restore() {
            std::string error;
            require(renderer.applyShaderSource(source, error), "Restore shader: " + error);
            require(renderer.ensureSceneTargetSize(width, height, error), "Restore targets: " + error);
            renderer.resetGlobalIllumination();
            renderer.setFrameCaptureOptions(captureOptions);
            renderer.setDiagnosticsEnabled(diagnostics);
            done = true;
        }
        ~Restore() { if (!done) try { restore(); } catch (...) {} }
    } restore{renderer, oldSource, oldWidth, oldHeight, oldCaptureOptions, oldDiagnostics};

    Json report{{"format", "RestArtEngine.AntiAliasingSmoke"}, {"version", 1},
        {"adapter", renderer.adapterName()},
        {"backend", renderer::mainRenderBackendKey(renderer.mainBackendSelection().effective)},
        {"width", width}, {"height", height}, {"reference_scale", referenceScale},
        {"quality", Json::array()}, {"checks", Json::array()}, {"passed", false}};
    const auto saveReport = [&] {
        std::ofstream stream(output / "anti-aliasing.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n'; stream.close();
        require(stream.good(), "Cannot save report.");
    };
    const auto pass = [&](const char* message) {
        report["checks"].push_back(message);
        std::cout << "PASS: anti-aliasing: " << message << '\n';
    };

    try {
        std::string error;
        require(renderer.applyShaderSource(identitySource, error), error);
        assets::AssetLibrary library;
        library.meshes.push_back(edgeMesh());
        assets::MaterialAsset white;
        white.id = 81002; white.name = "AA neutral white";
        std::fill(std::begin(white.baseColor), std::end(white.baseColor), 1.f);
        white.metallic = 0; white.roughness = 1; white.twoSided = true;
        library.materials.push_back(white);
        scene::World world;
        world.blackBackground = true;
        const auto actorId = world.createActor("AA edge fixture", library.meshes.front().id, white.id);
        renderer::SceneSettings settings;
        settings.editorViewport = true;
        settings.offscreenTarget = false;
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = settings.cameraPitch = 0;
        settings.cameraDistance = 6;
        std::fill(std::begin(settings.cameraTarget), std::end(settings.cameraTarget), 0.f);
        settings.shadowMethod = renderer::ShadowMethod::Disabled;
        settings.debugView = 1;
        const auto setSize = [&](std::uint32_t w, std::uint32_t h) {
            require(renderer.ensureSceneTargetSize(w, h, error), error);
            settings.viewportWidth = static_cast<int>(w); settings.viewportHeight = static_cast<int>(h);
        };
        const auto render = [&](bool read = true) {
            FrameScope frame;
            require(renderer.render(settings, world, library, error), error);
            auto pixels = read ? readScene(renderer, settings.viewportWidth, settings.viewportHeight) : Pixels{};
            require(renderer.present(false, error), error);
            return pixels;
        };
        const auto accumulate = [&](unsigned frames) {
            Pixels pixels;
            for (unsigned i = 0; i < frames; ++i) pixels = render(i + 1 == frames);
            return pixels;
        };
        const auto expectAA = [&](AA requested, AA effective) {
            const auto status = renderer.antiAliasingStatus();
            require(status.requested == requested && status.effective == effective,
                "Requested/effective AA state disagrees with the render request: " + status.reason);
            require(hasPass(renderer, Pass::AntiAliasing) == (effective != AA::Disabled),
                "Formal pipeline does not match actual AA status.");
            require(hasPass(renderer, Pass::AntiAliasingPresent) == (effective == AA::TAA),
                "Formal pipeline does not present the AA result.");
        };

        for (const auto path : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
            settings.path = path;
            const std::string prefix = path == renderer::RenderPath::Forward ? "forward" : "deferred";
            settings.antiAliasing.method = AA::Disabled;
            setSize(width * referenceScale, height * referenceScale);
            const auto highReference = render();
            expectAA(AA::Disabled, AA::Disabled);
            const auto reference = downsampleReference(highReference);
            savePixels(output / (prefix + "-reference-4x.bmp"), highReference,
                width * referenceScale, height * referenceScale);
            savePixels(output / (prefix + "-reference.bmp"), reference, width, height);
            setSize(width, height);
            const auto disabled = render();
            settings.antiAliasing.method = AA::FXAA;
            const auto fxaa = render();
            expectAA(AA::FXAA, AA::FXAA);
            settings.antiAliasing.method = AA::TAA;
            const auto firstTemporal = render();
            const auto taa = accumulate(47);
            expectAA(AA::TAA, AA::TAA);
            require(renderer.antiAliasingStatus().accumulatedFrames >= 48,
                "Static main-view TAA failed to accumulate 48 frames.");
            require(changedPixels(firstTemporal, taa) > 20, "Temporal accumulation did not change GPU pixels.");
            const auto mask = edgeMask(reference);
            const double offError = edgeError(disabled, reference, mask);
            const double fxaaError = edgeError(fxaa, reference, mask);
            const double taaError = edgeError(taa, reference, mask);
            report["quality"].push_back({{"path", prefix}, {"metric", "reference edge ROI RGB MAE (0..255)"},
                {"edge_pixels", std::count(mask.begin(), mask.end(), true)},
                {"off", offError}, {"fxaa", fxaaError}, {"taa_48_frames", taaError}});
            savePixels(output / (prefix + "-off.bmp"), disabled, width, height);
            savePixels(output / (prefix + "-fxaa.bmp"), fxaa, width, height);
            savePixels(output / (prefix + "-taa-first.bmp"), firstTemporal, width, height);
            savePixels(output / (prefix + "-taa-48.bmp"), taa, width, height);
            saveReport();
            require(offError > 2, "Reference fixture does not expose meaningful raster aliasing.");
            require(fxaaError < offError * .99, prefix + " FXAA did not improve edge MAE against supersampling.");
            require(taaError < offError * .99, prefix + " TAA did not improve edge MAE against supersampling.");
        }
        pass("Forward/Deferred Off, FXAA and 48-frame TAA improve real GPU edges against 4x supersampling");

        // Moving-camera quality must be measured, not inferred from a history
        // counter. Preserve one image stream throughout this slow orbit, then
        // compare its final output to independent raster renders at that pose.
        const auto beforeMotionSettings = settings;
        const auto beforeMotionHistory = renderer.antiAliasingStatus();
        for (unsigned frame = 0; frame < 15; ++frame) {
            settings.cameraYaw += .008f;
            render(false);
        }
        settings.cameraYaw += .008f;
        const auto movingTaa = render();
        const auto afterMotionHistory = renderer.antiAliasingStatus();
        require(afterMotionHistory.historyRevision == beforeMotionHistory.historyRevision &&
            afterMotionHistory.accumulatedFrames == beforeMotionHistory.accumulatedFrames + 16,
            "A continuous camera trajectory repeatedly reset temporal reprojection.");
        settings.antiAliasing.method = AA::Disabled;
        setSize(width * referenceScale, height * referenceScale);
        const auto movingHighReference = render();
        const auto movingReference = downsampleReference(movingHighReference);
        setSize(width, height);
        const auto movingOff = render();
        const auto movingMask = edgeMask(movingReference);
        const double movingOffError = edgeError(movingOff, movingReference, movingMask);
        const double movingTaaError = edgeError(movingTaa, movingReference, movingMask);
        report["quality"].push_back({{"path", "deferred-moving-camera"},
            {"metric", "reference edge ROI RGB MAE (0..255)"},
            {"trajectory_frames", 16}, {"yaw_radians_per_frame", .008},
            {"edge_pixels", std::count(movingMask.begin(), movingMask.end(), true)},
            {"off", movingOffError}, {"taa", movingTaaError},
            {"maximum_taa_to_off_ratio", 1.1}});
        savePixels(output / "moving-taa.bmp", movingTaa, width, height);
        savePixels(output / "moving-off.bmp", movingOff, width, height);
        savePixels(output / "moving-reference.bmp", movingReference, width, height);
        savePixels(output / "moving-reference-4x.bmp", movingHighReference,
            width * referenceScale, height * referenceScale);
        saveReport();
        require(movingTaaError <= movingOffError * 1.1,
            "Moving TAA edge MAE exceeds unfiltered raster by more than 10%; inspect reprojection/ghosting evidence.");
        settings = beforeMotionSettings;
        accumulate(8);
        pass("A 16-frame camera orbit preserves history and limits edge error versus a final-pose GPU reference");

        const auto beforeMove = renderer.antiAliasingStatus();
        settings.cameraYaw += .001f;
        render(false);
        const auto afterMove = renderer.antiAliasingStatus();
        require(afterMove.historyRevision == beforeMove.historyRevision &&
            afterMove.accumulatedFrames > beforeMove.accumulatedFrames,
            "A small camera movement discarded temporal history instead of reprojecting it.");
        settings.cameraYaw += 1.2f;
        render(false);
        require(renderer.antiAliasingStatus().historyRevision > afterMove.historyRevision &&
            renderer.antiAliasingStatus().accumulatedFrames == 1, "Camera cut did not reset history.");
        settings.cameraYaw = 0;
        accumulate(3);
        pass("Small camera motion retains/reprojects history; a camera cut starts a fresh history");

        const auto expectReset = [&](std::uint64_t oldRevision, const char* reason) {
            render(false);
            const auto status = renderer.antiAliasingStatus();
            require(status.historyRevision > oldRevision && status.accumulatedFrames == 1,
                std::string(reason) + " did not invalidate temporal history.");
        };
        auto revision = renderer.antiAliasingStatus().historyRevision;
        world.findActor(actorId)->transform.position[0] += .07f;
        expectReset(revision, "Actor edit");
        revision = renderer.antiAliasingStatus().historyRevision;
        library.materials.front().baseColor[1] = .65f;
        expectReset(revision, "Material parameter edit");
        revision = renderer.antiAliasingStatus().historyRevision;
        ++settings.viewId;
        expectReset(revision, "Main view identity change");
        revision = renderer.antiAliasingStatus().historyRevision;
        setSize(width + 32, height + 24);
        expectReset(revision, "Scene target resize");
        setSize(width, height);
        render(false);
        revision = renderer.antiAliasingStatus().historyRevision;
        settings.cameraLens.verticalFovDegrees += 2;
        expectReset(revision, "Projection change");
        pass("Actor/material edits, view identity, target size and projection invalidate TAA history");

        accumulate(4);
        const auto beforePreview = renderer.antiAliasingStatus();
        {
            FrameScope frame;
            require(renderer.renderViewportPreview(settings, world, library, nullptr, width, height,
                error, renderer::PreviewSlot::Viewport1), error);
            require(renderer.present(false, error), error);
        }
        render(false);
        const auto afterPreview = renderer.antiAliasingStatus();
        require(afterPreview.historyRevision == beforePreview.historyRevision &&
            afterPreview.accumulatedFrames == beforePreview.accumulatedFrames + 1,
            "Tool preview consumed or invalidated the main view's temporal history.");
        // Tool draws keep the main diagnostics intact. Verify their fallback
        // through actual GPU image parity with an explicit spatial request.
        const auto beforeOffscreen = renderer.antiAliasingStatus();
        settings.offscreenTarget = true;
        const auto offscreenTemporalRequest = render();
        settings.antiAliasing.method = AA::FXAA;
        const auto offscreenSpatialRequest = render();
        require(changedPixels(offscreenTemporalRequest, offscreenSpatialRequest, 0) == 0,
            "Offscreen TAA request did not produce the spatial FXAA fallback image.");
        require(renderer.antiAliasingStatus().historyRevision == beforeOffscreen.historyRevision &&
            renderer.antiAliasingStatus().accumulatedFrames == beforeOffscreen.accumulatedFrames &&
            renderer.antiAliasingStatus().effective == AA::TAA,
            "Offscreen fallback replaced the main temporal diagnostics/history.");
        settings.offscreenTarget = false;
        settings.antiAliasing.method = AA::TAA;
        render(false);
        pass("Offscreen TAA matches explicit FXAA pixels and preserves main diagnostics/history");

        const auto shaderBeforeWeight = renderer.shaderRevision();
        revision = renderer.antiAliasingStatus().historyRevision;
        settings.antiAliasing.temporalCurrentWeight = .05f;
        expectReset(revision, "Live temporal weight edit");
        const auto lowWeight = accumulate(23);
        settings.antiAliasing.temporalCurrentWeight = 1.f;
        ++settings.viewId;
        const auto highWeight = accumulate(24);
        require(changedPixels(lowWeight, highWeight, 1) > 20,
            "Changing temporal current weight had no visible GPU effect.");
        require(renderer.shaderRevision() == shaderBeforeWeight, "Weight edit recompiled material shaders.");
        savePixels(output / "taa-weight-005.bmp", lowWeight, width, height);
        savePixels(output / "taa-weight-100.bmp", highWeight, width, height);
        settings.antiAliasing.method = AA::FXAA;
        const auto liveFxaa = render();
        expectAA(AA::FXAA, AA::FXAA);
        settings.antiAliasing.method = AA::Disabled;
        const auto liveOff = render();
        expectAA(AA::Disabled, AA::Disabled);
        require(changedPixels(liveFxaa, liveOff) > 20, "Live AA method switch did not change GPU pixels.");
        require(renderer.shaderRevision() == shaderBeforeWeight, "Method switch recompiled material shaders.");
        pass("AA method and temporal weight affect GPU pixels live without material recompilation");

        settings.antiAliasing.method = AA::TAA;
        accumulate(2);
        renderer.setDiagnosticsEnabled(true);
        renderer.setFrameCaptureOptions({"TAA resolve", true});
        renderer.requestFrameCapture();
        render(false);
        require(renderer.frameCapture().valid, renderer.frameCapture().status);
        const auto capturedResource = [&](const char* fragment) {
            const auto& resources = renderer.frameCapture().resources;
            const auto found = std::find_if(resources.begin(),resources.end(),[&](const auto& item) {
                return item.name.find(fragment) != std::string::npos;
            });
            require(found != resources.end(),std::string("Missing AA capture resource: ")+fragment);
            return static_cast<std::size_t>(found-resources.begin());
        };
        (void)capturedResource("Before / AntiAliasingSource");
        (void)capturedResource("Before / PreviousAntiAliasingHistory");
        (void)capturedResource("After / AntiAliasingHistory");
        const auto& events = renderer.frameCapture().events;
        require(std::any_of(events.begin(),events.end(),[](const auto& event) {
            return event.pixelShader == "PSTAA" && std::any_of(event.constants.begin(),event.constants.end(),
                [](const auto& constant) { return constant.slot==5 && constant.bytes.size()==224; });
        }),"TAA capture did not record the actual shader and AA constant buffer.");
        renderer.setFrameCaptureOptions({"TAA present", true});
        renderer.requestFrameCapture();
        const auto capturedFinal = render();
        float capturedPixel[4]{};
        require(renderer.readCapturedPixel(capturedResource("After / SceneColor"),width/3,height*2/3,capturedPixel,error),error);
        const auto pixelOffset = (height*2/3*width+width/3)*4;
        for(unsigned c=0;c<3;++c)
            require(std::abs(capturedPixel[c]*255-capturedFinal[pixelOffset+c])<1.1f,
                "TAA present capture does not match the actual final GPU image.");
        renderer.setFrameCaptureOptions(oldCaptureOptions);
        renderer.setDiagnosticsEnabled(oldDiagnostics);
        pass("TAA captures actual source/history boundaries, shader constants and final output pixels");

        settings.antiAliasing.method = AA::TAA;
        settings.antiAliasing.temporalCurrentWeight = .1f;
        accumulate(3);
        revision = renderer.antiAliasingStatus().historyRevision;
        require(renderer.applyShaderSource(
            "Surface EvaluateMaterial(float3 p, float3 n, Surface m) { m.albedo *= 0.2; return m; }", error), error);
        require(renderer.shaderRevision() > shaderBeforeWeight, "Valid material candidate was not installed.");
        expectReset(revision, "Successful shader hot compilation");
        settings.antiAliasing.method = AA::FXAA;
        const auto compiledImage = render();
        require(changedPixels(compiledImage, liveFxaa) > 200, "Valid material hot compilation did not change GPU output.");
        const auto acceptedRevision = renderer.shaderRevision();
        require(!renderer.applyShaderSource("Surface EvaluateMaterial( invalid candidate", error),
            "Invalid material shader candidate unexpectedly compiled.");
        require(!error.empty() && renderer.shaderRevision() == acceptedRevision,
            "Failed shader candidate replaced the active shader revision or has no diagnostics.");
        const auto retainedImage = render();
        require(changedPixels(compiledImage, retainedImage, 0) == 0,
            "Failed shader candidate changed the active GPU image.");
        savePixels(output / "hot-compile-retained.bmp", retainedImage, width, height);
        pass("Valid hot compilation updates pixels/resets history; invalid candidates retain revision and pixels");

        if (renderer.hardwareRayTracingSupported()) {
            require(renderer.applyShaderSource(identitySource, error), error);
            settings.debugView = 0;
            settings.hardwareRayTracing = true;
            settings.antiAliasing.method = AA::TAA;
            settings.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
            settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
            settings.globalIllumination.maxBounces = 1;
            settings.globalIllumination.samplesPerFrame = 1;
            settings.globalIllumination.targetSamples = 16;
            const auto dispatchBefore = renderer.hardwareRayTracingDispatchCount();
            render(false);
            require(renderer.globalIlluminationStatus().effective == renderer::GlobalIlluminationMethod::PathTracing,
                "DXR-capable renderer did not execute progressive GI.");
            expectAA(AA::TAA, AA::Disabled);
            require(!renderer.antiAliasingStatus().reason.empty(), "Progressive AA bypass lacks a reason.");
            const auto progressiveRevision = renderer.globalIlluminationStatus().historyRevision;
            settings.antiAliasing.method = AA::FXAA;
            render(false);
            expectAA(AA::FXAA, AA::Disabled);
            require(renderer.globalIlluminationStatus().historyRevision == progressiveRevision,
                "Changing a bypassed AA request reset progressive GI history.");
            settings.antiAliasing.method = AA::TAA;
            settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
            render(false);
            expectAA(AA::TAA, AA::FXAA);
            require(!renderer.antiAliasingStatus().reason.empty(), "Realtime GI fallback lacks a reason.");
            const auto giRevision = renderer.globalIlluminationStatus().historyRevision;
            render(false);
            require(renderer.globalIlluminationStatus().historyRevision == giRevision,
                "AA caused realtime GI history to reset on every frame.");
            require(renderer.hardwareRayTracingDispatchCount() > dispatchBefore,
                "GI policy check did not execute any hardware ray-tracing dispatches.");
            report["dxr"] = "executed";
            pass("Progressive GI bypasses AA; realtime GI explains TAA-to-FXAA fallback without jitter resets");
        } else {
            report["dxr"] = {{"skipped", true}, {"reason", renderer.hardwareRayTracingStatus()}};
            std::cout << "SKIP: anti-aliasing GI policy: " << renderer.hardwareRayTracingStatus() << '\n';
        }

        restore.restore();
        require(renderer.validationErrorCount() == oldValidationErrors,
            "GPU validation reported new errors during anti-aliasing rendering.");
        report["validation_enabled"] = renderer.validationEnabled();
        report["new_validation_errors"] = renderer.validationErrorCount() - oldValidationErrors;
        report["passed"] = true;
        saveReport();
    } catch (const std::exception& failure) {
        report["error"] = failure.what();
        saveReport();
        throw;
    }
}
