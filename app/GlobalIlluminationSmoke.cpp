#include "GlobalIlluminationSmoke.h"
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
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Pixels = std::vector<std::uint8_t>;
using Mask = std::vector<std::uint8_t>;
using Colour = std::array<double, 3>;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error("GI: " + message);
}
void write(const std::filesystem::path& path, const std::string& value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    stream.close();
    require(stream.good(), "Cannot write validation output.");
}
Pixels readScene(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Scene image is unavailable.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, staging;
    require(SUCCEEDED(resource.As(&texture)), "Scene image is not a texture.");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected scene format.");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&desc, nullptr, &staging)), "Cannot allocate readback.");
    Pixels pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    renderer.context()->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Cannot map scene readback.");
    for (UINT row = 0; row < desc.Height; ++row)
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * desc.Width * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(desc.Width) * 4);
    renderer.context()->Unmap(staging.Get(), 0);
    return pixels;
}
void savePixels(const std::filesystem::path& path, const Pixels& rgba,
                std::uint32_t width, std::uint32_t height) {
    require(rgba.size() == static_cast<std::size_t>(width) * height * 4, "Invalid screenshot dimensions.");
    auto bgra = rgba;
    for (std::size_t pixel = 0; pixel < bgra.size(); pixel += 4) std::swap(bgra[pixel], bgra[pixel + 2]);
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
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    stream.close();
    require(stream.good(), "Cannot save GPU image.");
}

Mask floorMask(const Pixels& albedo, const Pixels& normals, std::uint32_t width, std::uint32_t height) {
    require(albedo.size() == normals.size() && albedo.size() == static_cast<std::size_t>(width) * height * 4,
        "Debug mask images differ in size.");
    Mask mask(static_cast<std::size_t>(width) * height);
    // Neutral .65 floor, encoded by the Albedo debug view. The white sphere is
    // brighter and its curved normals do not survive the 3x3 erosion below.
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel) {
        const auto at = pixel * 4;
        const int red = albedo[at], green = albedo[at + 1], blue = albedo[at + 2];
        const bool neutral = red >= 205 && red <= 215 && std::abs(red - green) <= 1 && std::abs(red - blue) <= 1;
        const bool up = std::abs(static_cast<int>(normals[at]) - 128) <= 1 && normals[at + 1] >= 254 &&
            std::abs(static_cast<int>(normals[at + 2]) - 128) <= 1;
        mask[pixel] = neutral && up ? 1 : 0;
    }
    Mask eroded(mask.size());
    for (std::uint32_t y = 1; y + 1 < height; ++y)
        for (std::uint32_t x = 1; x + 1 < width; ++x) {
            bool inside = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    inside &= mask[static_cast<std::size_t>(static_cast<int>(y) + dy) * width +
                        static_cast<std::uint32_t>(static_cast<int>(x) + dx)] != 0;
            eroded[static_cast<std::size_t>(y) * width + x] = inside ? 1 : 0;
        }
    return eroded;
}
std::size_t maskCount(const Mask& mask) {
    return static_cast<std::size_t>(std::count(mask.begin(), mask.end(), std::uint8_t{1}));
}
Colour meanColour(const Pixels& image, const Mask& mask) {
    require(image.size() == mask.size() * 4 && maskCount(mask) != 0, "Empty or invalid floor mask.");
    Colour mean{};
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel)
        if (mask[pixel]) for (unsigned channel = 0; channel < 3; ++channel) mean[channel] += image[pixel * 4 + channel];
    for (auto& value : mean) value /= static_cast<double>(maskCount(mask)) * 255.0;
    return mean;
}
double meanError(const Pixels& first, const Pixels& second, const Mask& mask) {
    require(first.size() == second.size() && first.size() == mask.size() * 4 && maskCount(mask) != 0,
        "Invalid image comparison/mask.");
    double difference = 0;
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel)
        if (mask[pixel]) for (unsigned channel = 0; channel < 3; ++channel)
            difference += std::abs(static_cast<int>(first[pixel * 4 + channel]) - static_cast<int>(second[pixel * 4 + channel]));
    return difference / (static_cast<double>(maskCount(mask)) * 3.0 * 255.0);
}
std::size_t changedPixels(const Pixels& first, const Pixels& second, int tolerance = 2) {
    require(first.size() == second.size(), "Image sizes differ.");
    std::size_t result = 0;
    for (std::size_t pixel = 0; pixel < first.size(); pixel += 4) {
        bool changed = false;
        for (unsigned channel = 0; channel < 3; ++channel)
            changed |= std::abs(static_cast<int>(first[pixel + channel]) - static_cast<int>(second[pixel + channel])) > tolerance;
        if (changed) ++result;
    }
    return result;
}
struct TraceFrame {
    TraceFrame() { rae::diagnostics::beginFrame(); }
    ~TraceFrame() { rae::diagnostics::endFrame(); }
};
} // namespace

void runGlobalIlluminationSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Method = renderer::GlobalIlluminationMethod;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    const auto oldSource = renderer.shaderSource();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    struct Restore {
        renderer::SceneRenderer& renderer;
        const std::string& source;
        std::uint32_t width, height;
        bool done = false;
        void restore() {
            std::string error;
            const bool applied = renderer.applyShaderSource(source, error);
            require(applied, "Cannot restore shader source: " + error);
            const bool resized = renderer.resize(width, height, error);
            require(resized, "Cannot restore render dimensions: " + error);
            renderer.resetGlobalIllumination();
            done = true;
        }
        ~Restore() { if (!done) try { restore(); } catch (...) {} }
    } restore{renderer, oldSource, oldWidth, oldHeight};
    Json report{{"format", "RestArtEngine.GlobalIlluminationSmoke"}, {"version", 1},
        {"adapter", renderer.adapterName()}, {"checks", Json::array()}, {"metrics", Json::object()}, {"passed", false}};
    const auto saveReport = [&] { write(output / "gi-report.json", report.dump(2) + '\n'); };
    const auto pass = [&](const char* message) {
        report["checks"].push_back(message);
        std::cout << "PASS: GI " << message << '\n';
    };

    try {
        std::string error;
        constexpr std::uint32_t width = 160, height = 120;
        require(renderer.resize(width, height, error), error);
        assets::AssetLibrary assets;
        assets.createDefaults();
        assets.materials.clear();
        const auto addMaterial = [&](assets::AssetId id, const char* name, float r, float g, float b) {
            assets::MaterialAsset material;
            material.id = id; material.name = name;
            material.baseColor[0] = r; material.baseColor[1] = g; material.baseColor[2] = b;
            material.metallic = 0; material.roughness = 0.9f; material.grid = false;
            material.twoSided = false;
            assets.materials.push_back(material);
        };
        constexpr assets::AssetId floorMaterial = 101, leftMaterial = 102, rightMaterial = 103,
            backMaterial = 104, sphereMaterial = 105;
        addMaterial(floorMaterial, "Neutral diffuse floor", .65f, .65f, .65f);
        addMaterial(leftMaterial, "Red diffuse wall", .75f, .03f, .02f);
        addMaterial(rightMaterial, "Blue diffuse wall", .02f, .03f, .75f);
        addMaterial(backMaterial, "Neutral back wall", .72f, .72f, .72f);
        addMaterial(sphereMaterial, "White sphere", .82f, .82f, .82f);
        scene::World world;
        world.blackBackground = true;
        const auto floor = world.createActor("Neutral floor", 2, floorMaterial);
        for (float& scale : world.findActor(floor)->transform.scale) scale = .25f;
        const auto leftWall = world.createActor("Red left wall", 3, leftMaterial);
        world.findActor(leftWall)->transform.position[0] = -2.0f;
        world.findActor(leftWall)->transform.position[1] = 1.0f;
        world.findActor(leftWall)->transform.scale[0] = .08f;
        world.findActor(leftWall)->transform.scale[1] = 1.0f;
        world.findActor(leftWall)->transform.scale[2] = 2.0f;
        const auto rightWall = world.createActor("Blue right wall", 3, rightMaterial);
        world.findActor(rightWall)->transform.position[0] = 2.0f;
        world.findActor(rightWall)->transform.position[1] = 1.0f;
        world.findActor(rightWall)->transform.scale[0] = .08f;
        world.findActor(rightWall)->transform.scale[1] = 1.0f;
        world.findActor(rightWall)->transform.scale[2] = 2.0f;
        const auto backWall = world.createActor("Neutral back wall", 3, backMaterial);
        world.findActor(backWall)->transform.position[1] = 1.0f;
        world.findActor(backWall)->transform.position[2] = -2.0f;
        world.findActor(backWall)->transform.scale[0] = 2.0f;
        world.findActor(backWall)->transform.scale[1] = 1.0f;
        world.findActor(backWall)->transform.scale[2] = .08f;
        const auto sphere = world.createActor("Neutral sphere", 1, sphereMaterial);
        world.findActor(sphere)->transform.position[1] = .65f;
        for (float& scale : world.findActor(sphere)->transform.scale) scale = .65f;
        renderer::SceneSettings settings;
        settings.editorViewport = true;
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = .35f; settings.cameraPitch = .3f; settings.cameraDistance = 7.0f;
        settings.cameraTarget[1] = 1.0f;
        settings.hardwareRayTracing = true;
        settings.hardwareRayTracingReflections = false; // Isolate diffuse interreflection.
        settings.hardwareRayTracingShadows = true;
        settings.globalIllumination.method = Method::PathTracing;
        settings.globalIllumination.samplesPerFrame = 1;
        settings.globalIllumination.targetSamples = 256;
        settings.globalIllumination.maxBounces = 4;
        const auto render = [&](const assets::MaterialAsset* preview = nullptr) {
            TraceFrame frame;
            std::string renderError;
            require(renderer.render(settings, world, assets, renderError, preview), renderError);
            return readScene(renderer);
        };
        const auto dispatchStart = renderer.hardwareRayTracingDispatchCount();
        (void)render();
        if (!renderer.hardwareRayTracingSupported()) {
            const auto status = renderer.globalIlluminationStatus();
            require(status.requested == Method::PathTracing && status.effective == Method::Disabled &&
                status.accumulatedSamples == 0 && !status.reason.empty() &&
                renderer.hardwareRayTracingDispatchCount() == dispatchStart,
                "Unsupported hardware did not preserve the request and explain the skipped GI execution.");
            report["hardwareExecutionSkipped"] = status.reason;
            report["validationErrors"] = renderer.validationErrorCount();
            require(renderer.validationErrorCount() == 0, "Fallback produced graphics validation errors.");
            restore.restore();
            report["passed"] = true;
            saveReport();
            std::cout << "SKIP: GI hardware execution: " << status.reason << '\n';
            return;
        }
        require(renderer.hardwareRayTracingActive() && renderer.globalIlluminationStatus().effective == Method::PathTracing &&
            renderer.hardwareRayTracingDispatchCount() > dispatchStart,
            "Actual path tracing did not activate: " + renderer.globalIlluminationStatus().reason);

        settings.debugView = 2;
        const auto albedo = render();
        settings.debugView = 3;
        const auto normals = render();
        const auto mask = floorMask(albedo, normals, width, height);
        report["metrics"]["floorPixels"] = maskCount(mask);
        require(maskCount(mask) >= 100, "GPU albedo/normal mask did not identify enough neutral floor pixels.");
        Pixels maskImage(mask.size() * 4, 255);
        for (std::size_t pixel = 0; pixel < mask.size(); ++pixel)
            for (unsigned channel = 0; channel != 3; ++channel) maskImage[pixel * 4 + channel] = mask[pixel] ? 255 : 0;
        savePixels(output / "neutral-floor-mask.bmp", maskImage, width, height);
        settings.debugView = 0;
        renderer.resetGlobalIllumination();
        const auto sample1 = render();
        require(renderer.globalIlluminationStatus().accumulatedSamples == 1, "First progressive frame did not contain one sample.");
        savePixels(output / "path-001.bmp", sample1, width, height);
        settings.globalIllumination.samplesPerFrame = 15;
        const auto sample16 = render();
        require(renderer.globalIlluminationStatus().accumulatedSamples == 16, "Progressive sample accounting is incorrect.");
        savePixels(output / "path-016.bmp", sample16, width, height);
        settings.globalIllumination.samplesPerFrame = 16;
        const auto advanceTo = [&](std::uint32_t target, const assets::MaterialAsset* preview = nullptr) {
            Pixels image;
            for (std::uint32_t frame = 0; frame <= target + 1; ++frame) {
                image = render(preview);
                const auto status = renderer.globalIlluminationStatus();
                require(status.effective == Method::PathTracing, "GI became unavailable: " + status.reason);
                if (status.accumulatedSamples >= target) {
                    require(status.accumulatedSamples == target, "Progressive render exceeded its expected sample count.");
                    return image;
                }
            }
            throw std::runtime_error("GI: Progressive renderer did not reach its target.");
        };
        const auto sample64 = advanceTo(64);
        const auto redMulti = advanceTo(256);
        savePixels(output / "path-256.bmp", redMulti, width, height);
        const auto error1 = meanError(sample1, redMulti, mask);
        const auto error16 = meanError(sample16, redMulti, mask);
        const auto error64 = meanError(sample64, redMulti, mask);
        report["metrics"]["floorMae1To256"] = error1;
        report["metrics"]["floorMae16To256"] = error16;
        report["metrics"]["floorMae64To256"] = error64;
        require(error1 > .0001 && error64 < error1 * .9,
            "The neutral floor image did not converge toward its 256-sample reference.");
        const auto cappedHistory = renderer.globalIlluminationStatus().historyRevision;
        for (int frame = 0; frame < 3; ++frame)
            require(changedPixels(redMulti, render(), 0) == 0 &&
                renderer.globalIlluminationStatus().accumulatedSamples == 256 &&
                renderer.globalIlluminationStatus().historyRevision == cappedHistory,
                "Reaching the sample cap did not freeze accumulated radiance.");
        pass("real DXR progressive samples converge on a GPU-selected neutral floor and stop at the sample cap");

        settings.globalIllumination.maxBounces = 1;
        const auto redDirect = advanceTo(256);
        const std::array<float, 3> red{.75f, .03f, .02f}, blue{.02f, .03f, .75f};
        std::copy(blue.begin(), blue.end(), assets.findMaterial(leftMaterial)->baseColor);
        const auto blueDirect = advanceTo(256);
        const auto directError = meanError(redDirect, blueDirect, mask);
        report["metrics"]["directOnlyWallChangeFloorMae"] = directError;
        require(directError < .0005, "Changing a wall colour unexpectedly changes direct-only neutral-floor lighting.");
        settings.globalIllumination.maxBounces = 4;
        const auto blueMulti = advanceTo(256);
        const auto redMean = meanColour(redMulti, mask), blueMean = meanColour(blueMulti, mask);
        const auto transfer = (redMean[0] - redMean[2]) - (blueMean[0] - blueMean[2]);
        report["metrics"]["redWallFloorMeanRgb"] = redMean;
        report["metrics"]["blueWallFloorMeanRgb"] = blueMean;
        report["metrics"]["diffuseRedBlueTransfer"] = transfer;
        report["metrics"]["indirectSpecularEnabled"] = false;
        savePixels(output / "red-wall-direct.bmp", redDirect, width, height);
        savePixels(output / "blue-wall-direct.bmp", blueDirect, width, height);
        savePixels(output / "blue-wall-indirect.bmp", blueMulti, width, height);
        require(transfer > .003,
            "Multi-bounce lighting did not transfer the wall's red/blue colour onto the neutral floor.");
        pass("wall colour affects neutral-floor diffuse indirect lighting while direct-only lighting stays unchanged");

        // Separate GGX evidence uses a closer camera and a larger image so a
        // reviewer can inspect reflected wall edges and highlight spread. A
        // pixel difference alone cannot establish physically plausible blur.
        const auto diffuseSphere = *assets.findMaterial(sphereMaterial);
        require(renderer.resize(320, 240, error), error);
        settings.viewportWidth = 320; settings.viewportHeight = 240;
        settings.cameraDistance = 4.8f; settings.cameraTarget[1] = .7f; settings.cameraPitch = .22f;
        settings.hardwareRayTracingReflections = true;
        std::copy(red.begin(), red.end(), assets.findMaterial(leftMaterial)->baseColor);
        auto* metal = assets.findMaterial(sphereMaterial);
        metal->baseColor[0] = metal->baseColor[1] = metal->baseColor[2] = .88f;
        metal->metallic = 1;
        metal->roughness = .08f;
        const auto polished = advanceTo(256);
        savePixels(output / "ggx-metal-roughness-008.bmp", polished, 320, 240);
        const auto polishedHistory = renderer.globalIlluminationStatus().historyRevision;
        metal->roughness = .55f;
        const auto roughMetal = advanceTo(256);
        savePixels(output / "ggx-metal-roughness-055.bmp", roughMetal, 320, 240);
        const auto roughPixels = changedPixels(polished, roughMetal);
        report["metrics"]["roughGgxChangedPixels"] = roughPixels;
        require(roughPixels > 100 && renderer.globalIlluminationStatus().historyRevision > polishedHistory,
            "Metal roughness failed to reset history and change actual GGX transport.");
        metal->roughness = .08f;
        require(changedPixels(polished, advanceTo(256), 0) == 0,
            "Restoring metal roughness did not restore the converged GPU image.");
        *assets.findMaterial(sphereMaterial) = diffuseSphere;
        std::copy(blue.begin(), blue.end(), assets.findMaterial(leftMaterial)->baseColor);
        settings.hardwareRayTracingReflections = false;
        settings.cameraDistance = 7; settings.cameraTarget[1] = 1; settings.cameraPitch = .3f;
        require(renderer.resize(width, height, error), error);
        settings.viewportWidth = width; settings.viewportHeight = height;
        require(changedPixels(blueMulti, advanceTo(256), 0) == 0,
            "Restoring the diffuse fixture did not restore its converged image.");
        report["visualReview"] = {"ggx-metal-roughness-008.bmp", "ggx-metal-roughness-055.bmp"};
        pass("GGX metal roughness changes converged GPU pixels and restores; reflection-spread images saved for visual review");

        // Exposure only resolves the accumulated HDR history; it must not cause
        // another stochastic lighting estimate or lose a converged image.
        const auto exposureHistory = renderer.globalIlluminationStatus();
        settings.exposure = .5f;
        const auto darker = render();
        require(renderer.globalIlluminationStatus().historyRevision == exposureHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == exposureHistory.accumulatedSamples &&
            changedPixels(blueMulti, darker) > 100, "Exposure changed history or failed to update the actual output.");
        settings.exposure = 1;
        require(changedPixels(blueMulti, render(), 0) == 0, "Restoring exposure did not restore the converged image.");
        settings.globalIllumination.targetSamples = 320;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == exposureHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 272,
            "Increasing target samples discarded converged history.");
        settings.globalIllumination.samplesPerFrame = 8;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == exposureHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 280,
            "Changing samples per frame discarded existing history.");
        settings.globalIllumination.targetSamples = 16;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision > exposureHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 8,
            "Lowering the target below accumulated samples did not start a bounded new history.");
        pass("exposure preserves radiance, increased sample budgets continue history, lower caps restart safely");

        std::copy(red.begin(), red.end(), assets.findMaterial(leftMaterial)->baseColor);
        settings.globalIllumination.targetSamples = 8;
        settings.globalIllumination.samplesPerFrame = 1;
        (void)advanceTo(8);
        const auto expectReset = [&](const char* label, auto change, const assets::MaterialAsset* preview = nullptr) {
            const auto before = renderer.globalIlluminationStatus().historyRevision;
            change();
            const auto image = render(preview);
            const auto status = renderer.globalIlluminationStatus();
            require(status.historyRevision > before && status.accumulatedSamples == 1,
                std::string(label) + " failed to invalidate stale radiance.");
            report["metrics"]["historyReset"][label] = status.historyRevision;
            return image;
        };
        const auto originalYaw = settings.cameraYaw;
        (void)expectReset("camera", [&] { settings.cameraYaw += .12f; });
        (void)expectReset("camera restore", [&] { settings.cameraYaw = originalYaw; });
        (void)expectReset("light", [&] { settings.lightIntensity *= .7f; });
        (void)expectReset("light restore", [&] { settings.lightIntensity = 5; });
        (void)expectReset("transform", [&] { world.findActor(sphere)->transform.position[0] = .3f; });
        (void)expectReset("transform restore", [&] { world.findActor(sphere)->transform.position[0] = 0; });
        const auto committed = *assets.findMaterial(floorMaterial);
        const auto committedImage = advanceTo(8);
        auto preview = committed;
        preview.baseColor[0] = .2f; preview.baseColor[1] = .75f; preview.baseColor[2] = .3f;
        (void)expectReset("material draft", [] {}, &preview);
        require(assets.findMaterial(floorMaterial)->baseColor[0] == committed.baseColor[0] &&
            assets.findMaterial(floorMaterial)->baseColor[1] == committed.baseColor[1], "GI preview mutated the committed material.");
        (void)expectReset("discard material draft", [] {});
        require(changedPixels(committedImage, advanceTo(8), 0) == 0,
            "Discarding the preview did not recover the same sampled GPU image.");
        (void)expectReset("visibility", [&] { world.findActor(sphere)->visible = false; });
        (void)expectReset("visibility restore", [&] { world.findActor(sphere)->visible = true; });
        const auto beforePreview = renderer.globalIlluminationStatus();
        require(renderer.renderMaterialPreview(committed, 64, error), error);
        const auto afterPreview = renderer.globalIlluminationStatus();
        require(afterPreview.accumulatedSamples == beforePreview.accumulatedSamples &&
            afterPreview.historyRevision == beforePreview.historyRevision && afterPreview.effective == beforePreview.effective,
            "Tool preview consumed or replaced main-scene GI history.");
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == beforePreview.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == beforePreview.accumulatedSamples + 1,
            "Main scene did not continue its history after a tool preview.");
        pass("camera, light, geometry, material draft/discard and visibility reset; offscreen previews preserve main history");

        settings.globalIllumination.samplesPerFrame = 16;
        settings.globalIllumination.targetSamples = 16;
        settings.path = renderer::RenderPath::Forward;
        renderer.resetGlobalIllumination();
        const auto forward = advanceTo(16);
        settings.path = renderer::RenderPath::Deferred;
        renderer.resetGlobalIllumination();
        const auto deferred = advanceTo(16);
        require(changedPixels(forward, deferred, 0) == 0, "Forward/Deferred changed the same-seed path tracing result.");
        settings.globalIllumination.method = Method::Disabled;
        (void)render();
        require(renderer.globalIlluminationStatus().effective == Method::Disabled &&
            renderer.globalIlluminationStatus().accumulatedSamples == 0 && renderer.hardwareRayTracingActive(),
            "Disabling GI did not return to the hardware preview.");
        settings.globalIllumination.method = Method::PathTracing;
        settings.hardwareRayTracing = false;
        const auto pausedDispatches = renderer.hardwareRayTracingDispatchCount();
        (void)render();
        require(renderer.globalIlluminationStatus().requested == Method::PathTracing &&
            renderer.globalIlluminationStatus().effective == Method::Disabled &&
            !renderer.globalIlluminationStatus().reason.empty() &&
            renderer.hardwareRayTracingDispatchCount() == pausedDispatches,
            "Disabled hardware did not retain the GI request and explain its inactive state.");
        settings.hardwareRayTracing = true;
        settings.globalIllumination.samplesPerFrame = 1;
        (void)render();
        require(renderer.globalIlluminationStatus().effective == Method::PathTracing &&
            renderer.globalIlluminationStatus().accumulatedSamples == 1 &&
            renderer.hardwareRayTracingDispatchCount() > pausedDispatches,
            "Re-enabling hardware did not start actual path tracing from fresh history.");
        const auto beforeResize = renderer.globalIlluminationStatus().historyRevision;
        require(renderer.resize(192, 128, error), error);
        settings.viewportWidth = 192; settings.viewportHeight = 128;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision > beforeResize &&
            renderer.globalIlluminationStatus().accumulatedSamples == 1, "Resize retained incompatible GI history.");
        pass("Forward/Deferred agree; GI/hardware switches and resize retain requests and restart compatible history");

        settings.globalIllumination.targetSamples = 16;
        settings.globalIllumination.samplesPerFrame = 16;
        renderer.resetGlobalIllumination();
        const auto beforeShader = advanceTo(16);
        const auto shaderHistory = renderer.globalIlluminationStatus().historyRevision;
        auto changedSource = oldSource;
        const auto insertion = changedSource.rfind("return material;");
        require(insertion != std::string::npos, "Test material source has no return contract.");
        changedSource.insert(insertion, "material.albedo *= float3(0.3, 0.8, 0.95);\n    ");
        const auto shaderRevision = renderer.shaderRevision();
        require(renderer.applyShaderSource(changedSource, error), error);
        const auto changed = advanceTo(16);
        require(renderer.shaderRevision() == shaderRevision + 1 &&
            renderer.globalIlluminationStatus().historyRevision > shaderHistory &&
            changedPixels(beforeShader, changed) > 100, "Successful shader update failed to reset history and change the GPU output.");
        const auto workingHistory = renderer.globalIlluminationStatus();
        const auto workingShaderRevision = renderer.shaderRevision();
        const bool invalidApplied = renderer.applyShaderSource(changedSource +
            "\n#ifdef RAE_HARDWARE_RAYTRACING\n#error Intentional GI candidate failure\n#endif\n", error);
        require(!invalidApplied && !error.empty(), "Invalid GI candidate was accepted.");
        require(renderer.shaderRevision() == workingShaderRevision && renderer.shaderSource() == changedSource &&
            changedPixels(changed, render(), 0) == 0 &&
            renderer.globalIlluminationStatus().historyRevision == workingHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == workingHistory.accumulatedSamples,
            "Failed shader candidate changed source, converged image or history.");
        require(renderer.applyShaderSource(oldSource, error), error);
        require(changedPixels(beforeShader, advanceTo(16), 0) == 0,
            "Repairing the shader did not restore the same-seed converged image.");
        pass("valid shader changes reset progressive lighting; invalid hardware candidates preserve source, history and pixels");

        report["dispatches"] = renderer.hardwareRayTracingDispatchCount() - dispatchStart;
        report["historyRevision"] = renderer.globalIlluminationStatus().historyRevision;
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation reported errors.");
        restore.restore();
        report["passed"] = true;
        saveReport();
        std::cout << "PASS: GI suite; adapter=" << renderer.adapterName() << ", dispatches=" << report["dispatches"]
            << ", graphics errors=0.\n";
    } catch (const std::exception& failure) {
        report["failure"] = failure.what();
        report["validationErrors"] = renderer.validationErrorCount();
        report["status"] = renderer.globalIlluminationStatus().reason;
        try { saveReport(); } catch (...) {}
        throw;
    }
}
