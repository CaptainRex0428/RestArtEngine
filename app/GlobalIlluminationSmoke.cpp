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
Pixels readScene(rae::renderer::SceneRenderer& renderer,
                 std::uint32_t* actualWidth = nullptr, std::uint32_t* actualHeight = nullptr) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Scene image is unavailable.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, staging;
    require(SUCCEEDED(resource.As(&texture)), "Scene image is not a texture.");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    require(desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected scene format.");
    if (actualWidth) *actualWidth = desc.Width;
    if (actualHeight) *actualHeight = desc.Height;
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
Pixels readSceneRegion(rae::renderer::SceneRenderer& renderer, std::uint32_t x, std::uint32_t y,
                       std::uint32_t width, std::uint32_t height) {
    std::uint32_t textureWidth = 0, textureHeight = 0;
    const auto image = readScene(renderer, &textureWidth, &textureHeight);
    require(x <= textureWidth && y <= textureHeight && width <= textureWidth - x && height <= textureHeight - y,
        "Requested viewport region is outside the actual scene texture.");
    Pixels region(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t row = 0; row < height; ++row)
        std::memcpy(region.data() + static_cast<std::size_t>(row) * width * 4,
            image.data() + (static_cast<std::size_t>(y + row) * textureWidth + x) * 4,
            static_cast<std::size_t>(width) * 4);
    return region;
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
std::size_t visiblePixels(const Pixels& image, int threshold = 3) {
    std::size_t result = 0;
    for (std::size_t pixel = 0; pixel < image.size(); pixel += 4)
        if (std::max({static_cast<int>(image[pixel]), static_cast<int>(image[pixel + 1]),
                      static_cast<int>(image[pixel + 2])}) > threshold) ++result;
    return result;
}
struct TraceFrame {
    TraceFrame() { rae::diagnostics::beginFrame(); }
    ~TraceFrame() { rae::diagnostics::endFrame(); }
};

Mask erodeMask(const Mask& mask, std::uint32_t width, std::uint32_t height, int radius) {
    require(mask.size() == static_cast<std::size_t>(width) * height && radius >= 0,
        "Invalid receiver mask dimensions.");
    Mask result(mask.size());
    for (int y = radius; y < static_cast<int>(height) - radius; ++y)
        for (int x = radius; x < static_cast<int>(width) - radius; ++x) {
            bool inside = true;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx)
                    inside &= mask[static_cast<std::size_t>(y + dy) * width + static_cast<std::size_t>(x + dx)] != 0;
            result[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = inside ? 1 : 0;
        }
    return result;
}

struct NeighbourMasks { Mask redSphere, blueCube; };
NeighbourMasks neighbourReceiverMasks(const Pixels& albedo, const Pixels& normals,
                                      std::uint32_t width, std::uint32_t height) {
    require(albedo.size() == normals.size() && albedo.size() == static_cast<std::size_t>(width) * height * 4,
        "Invalid neighbour receiver debug images.");
    NeighbourMasks masks{Mask(albedo.size() / 4), Mask(albedo.size() / 4)};
    for (std::size_t pixel = 0; pixel < masks.redSphere.size(); ++pixel) {
        const auto p = pixel * 4;
        const int r = albedo[p], g = albedo[p + 1], b = albedo[p + 2];
        const bool red = r > 180 && r > g + 70 && r > b + 70;
        const bool blue = b > 180 && b > r + 70 && b > g + 50;
        // +X-facing, camera-visible part of the sphere looks toward the cube.
        // The cube's -X face looks toward the sphere. These are GPU normals,
        // not screen rectangles that might also include a donor or the floor.
        masks.redSphere[pixel] = red && normals[p] >= 160 && normals[p + 1] >= 60 &&
            normals[p + 1] <= 195 && normals[p + 2] >= 150 ? 1 : 0;
        masks.blueCube[pixel] = blue && normals[p] <= 1 &&
            std::abs(static_cast<int>(normals[p + 1]) - 128) <= 1 &&
            std::abs(static_cast<int>(normals[p + 2]) - 128) <= 1 ? 1 : 0;
    }
    masks.redSphere = erodeMask(masks.redSphere, width, height, 2);
    masks.blueCube = erodeMask(masks.blueCube, width, height, 2);
    require(maskCount(masks.redSphere) >= 40 && maskCount(masks.blueCube) >= 40,
        "GPU albedo/normal masks do not contain enough inner coloured receiver pixels (" +
        std::to_string(maskCount(masks.redSphere)) + "/" + std::to_string(maskCount(masks.blueCube)) + ").");
    return masks;
}

double channelDifference(const Pixels& high, const Pixels& low, const Mask& mask, unsigned channel) {
    return meanColour(high, mask)[channel] - meanColour(low, mask)[channel];
}
std::size_t increasedChannelPixels(const Pixels& high, const Pixels& low, const Mask& mask, unsigned channel) {
    require(high.size() == low.size() && high.size() == mask.size() * 4 && channel < 3,
        "Invalid coloured-receiver comparison.");
    std::size_t result = 0;
    for (std::size_t pixel = 0; pixel < mask.size(); ++pixel)
        if (mask[pixel] && high[pixel * 4 + channel] > low[pixel * 4 + channel]) ++result;
    return result;
}

// Fixed receivers and fixed geometry isolate changes caused by their neighbour's
// reflectance. The donor's target channel changes; the receiving material does
// not. A neutral-floor-only test cannot establish this coloured-surface contract.
void runNeighbourColourTransport(rae::renderer::SceneRenderer& renderer,
                                const std::filesystem::path& output, Json& report) {
    using namespace rae;
    using Method = renderer::GlobalIlluminationMethod;
    using Mode = renderer::GlobalIlluminationMode;
    constexpr std::uint32_t width = 320, height = 240;
    const auto previousWidth = renderer.sceneTextureWidth(), previousHeight = renderer.sceneTextureHeight();
    struct RestoreSize {
        renderer::SceneRenderer& renderer;
        std::uint32_t width, height;
        ~RestoreSize() {
            try { std::string ignored; renderer.resize(width, height, ignored); renderer.resetGlobalIllumination(); }
            catch (...) {}
        }
    } restore{renderer, previousWidth, previousHeight};
    std::string error;
    require(renderer.resize(width, height, error), error);
    assets::AssetLibrary assets;
    assets.createDefaults();
    assets.materials.clear();
    constexpr assets::AssetId redId = 201, blueId = 202, floorId = 203;
    const std::array<float, 3> redPaint{.75f, .04f, .04f}, bluePaint{.04f, .1f, .75f};
    const std::array<float, 3> redLow{.04f, .04f, .04f}, blueLow{.04f, .1f, .04f};
    const auto material = [&](assets::AssetId id, const char* name, const std::array<float, 3>& colour, float roughness) {
        assets::MaterialAsset value;
        value.id = id; value.name = name; value.metallic = 0; value.roughness = roughness; value.grid = false;
        std::copy(colour.begin(), colour.end(), value.baseColor);
        assets.materials.push_back(value);
    };
    material(redId, "Painted red sphere", redPaint, .4f);
    material(blueId, "Painted blue cube", bluePaint, .4f);
    material(floorId, "Neutral neighbour floor", {.4f, .4f, .4f}, .85f);
    const auto setColour = [&](assets::AssetId id, const std::array<float, 3>& colour) {
        std::copy(colour.begin(), colour.end(), assets.findMaterial(id)->baseColor);
    };
    const auto painted = [&] {
        setColour(redId, redPaint); setColour(blueId, bluePaint);
        for (const auto id : {redId, blueId}) {
            assets.findMaterial(id)->metallic = 0;
            assets.findMaterial(id)->roughness = .4f;
        }
    };
    scene::World world;
    world.blackBackground = true;
    const auto sphere = world.createActor("Coloured receiver sphere", 1, redId);
    world.findActor(sphere)->transform.position[0] = -.8f;
    world.findActor(sphere)->transform.position[1] = .75f;
    for (auto& scale : world.findActor(sphere)->transform.scale) scale = .75f;
    const auto cube = world.createActor("Coloured receiver cube", 3, blueId);
    world.findActor(cube)->transform.position[0] = .8f;
    world.findActor(cube)->transform.position[1] = .75f;
    world.findActor(cube)->transform.scale[0] = .7f;
    world.findActor(cube)->transform.scale[1] = .75f;
    world.findActor(cube)->transform.scale[2] = .7f;
    const auto floor = world.createActor("Coloured receiver neutral floor", 2, floorId);
    for (auto& scale : world.findActor(floor)->transform.scale) scale = .25f;
    renderer::SceneSettings settings;
    settings.editorViewport = true;
    settings.viewportWidth = width; settings.viewportHeight = height;
    settings.cameraYaw = -.35f; settings.cameraPitch = .18f; settings.cameraDistance = 5.5f;
    settings.cameraTarget[1] = .75f;
    settings.hardwareRayTracing = true;
    settings.hardwareRayTracingReflections = false;
    settings.hardwareRayTracingShadows = true;
    settings.globalIllumination.method = Method::PathTracing;
    settings.globalIllumination.mode = Mode::Progressive;
    settings.globalIllumination.indirectDiffuse = true;
    settings.globalIllumination.indirectSpecular = false;
    settings.globalIllumination.samplesPerFrame = 16;
    settings.globalIllumination.targetSamples = 512;
    settings.globalIllumination.maxBounces = 4;
    const auto render = [&] {
        TraceFrame frame;
        std::string renderError;
        require(renderer.render(settings, world, assets, renderError), renderError);
        return readScene(renderer);
    };
    const auto masksForCurrentMaterials = [&] {
        settings.debugView = 2;
        const auto albedo = render();
        settings.debugView = 3;
        const auto normals = render();
        settings.debugView = 0;
        return neighbourReceiverMasks(albedo, normals, width, height);
    };
    const auto capture = [&](const char* name, std::uint32_t samples) {
        settings.globalIllumination.targetSamples = samples;
        renderer.resetGlobalIllumination();
        Pixels image;
        for (std::uint32_t frame = 0; frame <= samples; ++frame) {
            image = render();
            const auto status = renderer.globalIlluminationStatus();
            require(status.effective == Method::PathTracing && status.effectiveMode == settings.globalIllumination.mode,
                "Neighbour transport became unavailable: " + status.reason);
            if (status.accumulatedSamples >= samples) break;
        }
        require(renderer.globalIlluminationStatus().accumulatedSamples == samples,
            "Neighbour transport did not reach the requested sample count.");
        savePixels(output / (std::string(name) + ".bmp"), image, width, height);
        return image;
    };
    auto& metrics = report["metrics"]["colouredReceivers"];
    const auto receiverMasks = masksForCurrentMaterials();
    metrics["paintedRedSpherePixels"] = maskCount(receiverMasks.redSphere);
    metrics["paintedBlueCubePixels"] = maskCount(receiverMasks.blueCube);
    Pixels maskImage(static_cast<std::size_t>(width) * height * 4, 0);
    for (std::size_t pixel = 0; pixel < receiverMasks.redSphere.size(); ++pixel) {
        maskImage[pixel * 4] = receiverMasks.redSphere[pixel] ? 255 : 0;
        maskImage[pixel * 4 + 2] = receiverMasks.blueCube[pixel] ? 255 : 0;
        maskImage[pixel * 4 + 3] = 255;
    }
    savePixels(output / "neighbours-receiver-masks.bmp", maskImage, width, height);

    settings.globalIllumination.indirectDiffuse = false;
    const auto directBothHigh = capture("neighbours-direct-only", 64);
    setColour(blueId, blueLow);
    const auto directBlueLow = capture("neighbours-direct-blue-donor-low", 64);
    setColour(blueId, bluePaint); setColour(redId, redLow);
    const auto directRedLow = capture("neighbours-direct-red-donor-low", 64);
    metrics["directOnlyRedReceiverMae"] = meanError(directBothHigh, directBlueLow, receiverMasks.redSphere);
    metrics["directOnlyBlueReceiverMae"] = meanError(directBothHigh, directRedLow, receiverMasks.blueCube);
    require(metrics["directOnlyRedReceiverMae"].get<double>() == 0 &&
        metrics["directOnlyBlueReceiverMae"].get<double>() == 0,
        "Changing a neighbour's colour affected the fixed receiver without indirect contributions.");

    painted();
    settings.globalIllumination.indirectDiffuse = true;
    const auto paintedBothHigh = capture("neighbours-painted-indirect", 512);
    setColour(blueId, blueLow);
    const auto paintedBlueLow = capture("neighbours-painted-blue-donor-low", 512);
    setColour(blueId, bluePaint); setColour(redId, redLow);
    const auto paintedRedLow = capture("neighbours-painted-red-donor-low", 512);
    const auto blueToRed = channelDifference(paintedBothHigh, paintedBlueLow, receiverMasks.redSphere, 2);
    const auto redToBlue = channelDifference(paintedBothHigh, paintedRedLow, receiverMasks.blueCube, 0);
    metrics["progressiveBlueToRedReceiverBlueDelta"] = blueToRed;
    metrics["progressiveRedToBlueReceiverRedDelta"] = redToBlue;
    metrics["paintedRedReceiverMeanRgb"] = meanColour(paintedBothHigh, receiverMasks.redSphere);
    metrics["paintedBlueReceiverMeanRgb"] = meanColour(paintedBothHigh, receiverMasks.blueCube);
    require(blueToRed > .0002 && redToBlue > .0002,
        "Progressive GI did not produce the correct bidirectional colour change on fixed coloured receivers (" +
        std::to_string(blueToRed) + "/" + std::to_string(redToBlue) + ").");
    report["checks"].push_back("progressive bidirectional donor-colour transfer changes fixed painted receivers; direct-only receiver controls stay exact");
    std::cout << "PASS: GI coloured receiver progressive transfer (blue->red=" << blueToRed << ", red->blue=" << redToBlue << ").\n";

    painted(); setColour(redId, {.75f, 0, 0});
    const auto pureRedMask = masksForCurrentMaterials().redSphere;
    const auto saturatedRedHigh = capture("neighbours-saturated-red-blue-donor-high", 256);
    setColour(blueId, blueLow);
    const auto saturatedRedLow = capture("neighbours-saturated-red-blue-donor-low", 256);
    const auto redForeignIncreases = increasedChannelPixels(saturatedRedHigh, saturatedRedLow, pureRedMask, 2);
    metrics["saturatedRedForeignBlueIncreasedPixels"] = redForeignIncreases;
    metrics["saturatedRedForeignBlueMeanDelta"] = channelDifference(saturatedRedHigh, saturatedRedLow, pureRedMask, 2);
    require(redForeignIncreases == 0 && metrics["saturatedRedForeignBlueMeanDelta"].get<double>() == 0,
        "A zero-blue diffuse red receiver reflected additional blue instead of absorbing it.");
    painted(); setColour(blueId, {0, 0, .75f});
    const auto pureBlueMask = masksForCurrentMaterials().blueCube;
    const auto saturatedBlueHigh = capture("neighbours-saturated-blue-red-donor-high", 256);
    setColour(redId, redLow);
    const auto saturatedBlueLow = capture("neighbours-saturated-blue-red-donor-low", 256);
    const auto blueForeignIncreases = increasedChannelPixels(saturatedBlueHigh, saturatedBlueLow, pureBlueMask, 0);
    metrics["saturatedBlueForeignRedIncreasedPixels"] = blueForeignIncreases;
    metrics["saturatedBlueForeignRedMeanDelta"] = channelDifference(saturatedBlueHigh, saturatedBlueLow, pureBlueMask, 0);
    require(blueForeignIncreases == 0 && metrics["saturatedBlueForeignRedMeanDelta"].get<double>() == 0,
        "A zero-red diffuse blue receiver reflected additional red instead of absorbing it.");
    report["checks"].push_back("pure saturated diffuse receivers absorb their zero-reflectance foreign channels exactly instead of receiving an artificial colour boost");
    std::cout << "PASS: GI pure saturated receiver absorption; increased foreign-channel pixels=0/0.\n";

    painted();
    settings.globalIllumination.mode = Mode::Realtime;
    settings.globalIllumination.indirectDiffuse = false;
    const auto realtimeDirectHigh = capture("neighbours-realtime-direct-only", 64);
    setColour(blueId, blueLow);
    const auto realtimeDirectBlueLow = capture("neighbours-realtime-direct-blue-donor-low", 64);
    setColour(blueId, bluePaint); setColour(redId, redLow);
    const auto realtimeDirectRedLow = capture("neighbours-realtime-direct-red-donor-low", 64);
    metrics["realtimeDirectOnlyRedReceiverMae"] = meanError(realtimeDirectHigh, realtimeDirectBlueLow, receiverMasks.redSphere);
    metrics["realtimeDirectOnlyBlueReceiverMae"] = meanError(realtimeDirectHigh, realtimeDirectRedLow, receiverMasks.blueCube);
    require(metrics["realtimeDirectOnlyRedReceiverMae"].get<double>() == 0 &&
        metrics["realtimeDirectOnlyBlueReceiverMae"].get<double>() == 0,
        "Realtime donor colour changed a fixed receiver without indirect contributions.");
    painted();
    settings.globalIllumination.indirectDiffuse = true;
    const auto realtimeBothHigh = capture("neighbours-realtime-painted-indirect", 256);
    setColour(blueId, blueLow);
    const auto realtimeBlueLow = capture("neighbours-realtime-blue-donor-low", 256);
    setColour(blueId, bluePaint); setColour(redId, redLow);
    const auto realtimeRedLow = capture("neighbours-realtime-red-donor-low", 256);
    const auto realtimeBlueToRed = channelDifference(realtimeBothHigh, realtimeBlueLow, receiverMasks.redSphere, 2);
    const auto realtimeRedToBlue = channelDifference(realtimeBothHigh, realtimeRedLow, receiverMasks.blueCube, 0);
    metrics["realtimeBlueToRedReceiverBlueDelta"] = realtimeBlueToRed;
    metrics["realtimeRedToBlueReceiverRedDelta"] = realtimeRedToBlue;
    require(realtimeBlueToRed > .0002 && realtimeRedToBlue > .0002,
        "Realtime GI did not transfer neighbour colour in both fixed coloured receiver masks (" +
        std::to_string(realtimeBlueToRed) + "/" + std::to_string(realtimeRedToBlue) + ").");
    report["checks"].push_back("realtime painted-neighbour transport is causal in both eroded inner receiver masks");
    std::cout << "PASS: GI coloured receiver realtime transfer (blue->red=" << realtimeBlueToRed << ", red->blue=" << realtimeRedToBlue << ").\n";

    // These are the reported material values, not modified user assets. Pure
    // foreign diffuse reflectance is zero; high metallic further reduces the
    // diffuse lobe. Record the measured strength rather than demanding a boost.
    const std::array<float, 3> userRed{.9812f, 0, 0}, userBlue{0, .1294f, 1};
    setColour(redId, userRed); setColour(blueId, userBlue);
    assets.findMaterial(redId)->metallic = .313f; assets.findMaterial(redId)->roughness = .638f;
    assets.findMaterial(blueId)->metallic = .867f; assets.findMaterial(blueId)->roughness = .744f;
    settings.globalIllumination.mode = Mode::Progressive;
    const auto userMasks = masksForCurrentMaterials();
    settings.globalIllumination.indirectDiffuse = false;
    settings.globalIllumination.indirectSpecular = false;
    const auto userDirect = capture("neighbours-reported-materials-direct-only", 64);
    settings.globalIllumination.indirectDiffuse = true;
    settings.globalIllumination.indirectSpecular = true;
    const auto userBothHigh = capture("neighbours-reported-materials-indirect", 512);
    setColour(blueId, {0, .1294f, 0});
    const auto userBlueLow = capture("neighbours-reported-materials-blue-donor-low", 512);
    setColour(blueId, userBlue); setColour(redId, {0, 0, 0});
    const auto userRedLow = capture("neighbours-reported-materials-red-donor-low", 512);
    metrics["reportedBlueToRedReceiverBlueDelta"] = channelDifference(userBothHigh, userBlueLow, userMasks.redSphere, 2);
    metrics["reportedRedToBlueReceiverRedDelta"] = channelDifference(userBothHigh, userRedLow, userMasks.blueCube, 0);
    metrics["reportedRedReceiverMeanRgb"] = meanColour(userBothHigh, userMasks.redSphere);
    metrics["reportedBlueReceiverMeanRgb"] = meanColour(userBothHigh, userMasks.blueCube);
    metrics["reportedRedReceiverDirectMeanRgb"] = meanColour(userDirect, userMasks.redSphere);
    metrics["reportedBlueReceiverDirectMeanRgb"] = meanColour(userDirect, userMasks.blueCube);
    metrics["reportedMaterialValues"] = {{"red", {{"baseColor", userRed}, {"metallic", .313}, {"roughness", .638}}},
        {"blue", {{"baseColor", userBlue}, {"metallic", .867}, {"roughness", .744}}}};
    metrics["reportedForeignDiffuseReflectance"] = {{"redReceiverBlue", 0}, {"blueReceiverRed", 0}};
    metrics["reportedNormalIncidenceForeignSpecularF0"] = {{"redReceiverBlue", .04 * (1 - .313)},
        {"blueReceiverRed", .04 * (1 - .867)}};
    report["checks"].push_back("reported high-metal pure-colour materials are measured separately without requiring exaggerated foreign-colour transfer");
    std::cout << "PASS: GI reported material reference images/metrics recorded without changing user assets or lighting gain.\n";
}
void runAreaLightTransport(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& output, Json& report) {
    using namespace rae;
    constexpr std::uint32_t width = 160, height = 120;
    std::string error;
    require(renderer.resize(width, height, error), error);
    scene::World world;
    world.blackBackground = true;
    world.environment.overrideLighting = true;
    world.environment.directionalIntensity = world.environment.environmentIntensity = 0;
    auto& light = world.environment.areaLight;
    light.enabled = true;
    light.center[1] = 3;
    light.halfAxisU[0] = light.halfAxisV[2] = .7f;
    std::fill(std::begin(light.radiance), std::end(light.radiance), 10.0f);
    assets::AssetLibrary assets;
    assets.createDefaults();
    assets.materials.clear();
    assets::MaterialAsset diffuse;
    diffuse.id = 901; diffuse.name = "Area light neutral receiver";
    diffuse.metallic = 0; diffuse.roughness = .8f; diffuse.grid = false;
    std::fill(std::begin(diffuse.baseColor), std::end(diffuse.baseColor), .65f);
    assets.materials.push_back(diffuse);
    auto mirror = diffuse;
    mirror.id = 902; mirror.name = "Area light mirror";
    mirror.metallic = 1; mirror.roughness = .04f;
    std::fill(std::begin(mirror.baseColor), std::end(mirror.baseColor), .95f);
    assets.materials.push_back(mirror);
    renderer::SceneSettings settings;
    settings.editorViewport = true;
    settings.viewportWidth = width; settings.viewportHeight = height;
    settings.hardwareRayTracing = true;
    settings.cameraYaw = 0; settings.cameraPitch = .1f; settings.cameraDistance = 6;
    settings.cameraTarget[1] = 1.3f;
    settings.globalIllumination.method = renderer::GlobalIlluminationMethod::PathTracing;
    settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
    settings.globalIllumination.samplesPerFrame = 16;
    settings.globalIllumination.targetSamples = 128;
    settings.globalIllumination.maxBounces = 1;
    const auto render = [&] {
        TraceFrame frame;
        require(renderer.render(settings, world, assets, error), error);
        return readScene(renderer);
    };
    const auto capture = [&](const char* name) {
        Pixels pixels;
        for (unsigned frame = 0; frame < 10; ++frame) {
            pixels = render();
            const auto status = renderer.globalIlluminationStatus();
            require(status.effective == renderer::GlobalIlluminationMethod::PathTracing,
                "Rectangle test needs active progressive DXR: " + status.reason);
            if (status.accumulatedSamples >= settings.globalIllumination.targetSamples) {
                savePixels(output / (std::string(name) + ".bmp"), pixels, width, height);
                return pixels;
            }
        }
        throw std::runtime_error("Rectangle test did not reach its sample budget.");
    };
    const auto front = capture("area-emitter-front");
    require(visiblePixels(front, 180) > 30, "Rectangle emitter is not visible to primary rays.");
    light.halfAxisV[2] *= -1;
    const auto back = capture("area-emitter-back");
    require(visiblePixels(back) == 0, "Rectangle emitter radiates from its back face.");
    light.halfAxisV[2] *= -1;
    const auto blocker = world.createActor("Primary occluder", 3, diffuse.id);
    auto& blockTransform = world.findActor(blocker)->transform;
    blockTransform.position[1] = 2; blockTransform.position[2] = 2;
    blockTransform.scale[0] = blockTransform.scale[1] = 3;
    blockTransform.scale[2] = .1f;
    const auto blocked = capture("area-emitter-occluded");
    require(visiblePixels(blocked) == 0, "Rectangle emitter shines through opaque foreground geometry.");
    world.removeActor(blocker);
    const auto floor = world.createActor("Diffuse floor", 2, diffuse.id);
    std::fill(std::begin(world.findActor(floor)->transform.scale), std::end(world.findActor(floor)->transform.scale), .3f);
    settings.debugView = 2; const auto floorAlbedo = render();
    settings.debugView = 3; const auto floorNormals = render();
    const auto receiver = floorMask(floorAlbedo, floorNormals, width, height);
    require(maskCount(receiver) > 100, "Rectangle test did not identify its floor receiver.");
    settings.debugView = 0;
    const auto lit = capture("area-floor-lit");
    const auto litMean = meanColour(lit, receiver);
    require(litMean[0] > .12, "Rectangle light does not illuminate diffuse surfaces.");
    const auto ceiling = world.createActor("Geometry behind emitter", 3, diffuse.id);
    auto& ceilingTransform = world.findActor(ceiling)->transform;
    ceilingTransform.position[1] = 3.2f;
    ceilingTransform.scale[0] = ceilingTransform.scale[2] = 3;
    ceilingTransform.scale[1] = .05f;
    const auto behind = capture("area-floor-light-backstop");
    require(meanError(lit, behind, receiver) < .001,
        "Geometry beyond the sampled rectangle incorrectly shadows its finite light segment.");
    world.findActor(ceiling)->transform.position[1] = 1.5f;
    const auto shadowed = capture("area-floor-shadowed");
    settings.debugView = 2; const auto shadowAlbedo = render();
    settings.debugView = 3; const auto shadowNormals = render();
    const auto shadowReceiver = floorMask(shadowAlbedo, shadowNormals, width, height);
    settings.debugView = 0;
    require(maskCount(shadowReceiver) > 30 && meanColour(shadowed, shadowReceiver)[0] < litMean[0] * .5,
        "Opaque geometry did not shadow the floor from the rectangle.");
    settings.hardwareRayTracingShadows = false;
    const auto noShadows = capture("area-floor-shadows-disabled");
    require(meanColour(noShadows, shadowReceiver)[0] > meanColour(shadowed, shadowReceiver)[0] + .1,
        "Progressive hardware shadow switch did not disable rectangle shadows.");
    settings.hardwareRayTracingShadows = true;
    world.removeActor(ceiling);
    world.removeActor(floor);
    const auto sphere = world.createActor("Mirror sphere", 1, mirror.id);
    world.findActor(sphere)->transform.position[1] = 1.1f;
    settings.debugView = 2;
    const auto sphereAlbedo = render();
    settings.debugView = 0;
    const auto specular = capture("area-mirror-highlight");
    std::size_t highlightedSpherePixels = 0;
    for (std::size_t pixel = 0; pixel < specular.size(); pixel += 4)
        if (sphereAlbedo[pixel] > 240 && sphereAlbedo[pixel + 1] > 240 && sphereAlbedo[pixel + 2] > 240 &&
            specular[pixel] > 60 && specular[pixel + 1] > 60 && specular[pixel + 2] > 60)
            ++highlightedSpherePixels;
    require(highlightedSpherePixels > 3, "Near-mirror sphere has no actual reflected rectangle highlight.");
    const auto revision = renderer.globalIlluminationStatus().historyRevision;
    std::fill(std::begin(light.radiance), std::end(light.radiance), 2.0f);
    (void)render();
    require(renderer.globalIlluminationStatus().historyRevision > revision &&
        renderer.globalIlluminationStatus().accumulatedSamples == 16,
        "Rectangle radiance edit did not restart accumulation immediately.");
    light.enabled = false;
    const auto disabled = capture("area-disabled");
    require(visiblePixels(disabled) == 0, "Disabled rectangle or zero environment still lights the scene.");
    settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
    light.enabled = true;
    (void)render();
    require(renderer.globalIlluminationStatus().reason.find("require hardware progressive") != std::string::npos,
        "Unsupported realtime area lighting did not explain its limitation.");
    report["metrics"]["areaLightFloorMean"] = litMean;
    report["metrics"]["areaLightPrimaryPixels"] = visiblePixels(front, 180);
    report["metrics"]["areaLightMirrorHighlightPixels"] = highlightedSpherePixels;
    report["checks"].push_back("single-sided rectangle primary visibility, occlusion, finite shadow segment, diffuse/mirror transport, radiance reset and mode limitation");
    std::cout << "PASS: GI rectangle emitter, diffuse/mirror MIS, finite shadows, live history reset and explicit mode limitation.\n";
}
} // namespace

void runGlobalIlluminationSmoke(rae::renderer::SceneRenderer& renderer,
    const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Method = renderer::GlobalIlluminationMethod;
    using Mode = renderer::GlobalIlluminationMode;
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
        settings.hardwareRayTracingReflections = false; // Independent preview-only reflection option.
        settings.hardwareRayTracingShadows = true;
        settings.globalIllumination.method = Method::PathTracing;
        settings.globalIllumination.mode = Mode::Progressive;
        settings.globalIllumination.indirectDiffuse = true;
        settings.globalIllumination.indirectSpecular = false; // Isolate diffuse interreflection.
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
            renderer.globalIlluminationStatus().requestedMode == Mode::Progressive &&
            renderer.globalIlluminationStatus().effectiveMode == Mode::Progressive &&
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
        settings.globalIllumination.indirectSpecular = true;
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
        settings.globalIllumination.indirectSpecular = false;
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

        // Realtime is a separate consumer contract: an indirect-light buffer is
        // combined with the selected raster path's direct light. It keeps
        // sampling after its bounded history fills, rather than freezing a
        // complete progressive camera image.
        require(renderer.resize(width, height, error), error);
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = .35f; settings.cameraPitch = .3f; settings.cameraDistance = 7;
        settings.cameraTarget[1] = 1;
        settings.exposure = 1; settings.lightIntensity = 5;
        settings.path = renderer::RenderPath::Deferred;
        settings.hardwareRayTracingReflections = false;
        settings.globalIllumination.mode = Mode::Realtime;
        settings.globalIllumination.method = Method::PathTracing;
        settings.globalIllumination.indirectDiffuse = true;
        settings.globalIllumination.indirectSpecular = false;
        settings.globalIllumination.samplesPerFrame = 16;
        settings.globalIllumination.targetSamples = 256;
        settings.globalIllumination.maxBounces = 4;
        std::copy(red.begin(), red.end(), assets.findMaterial(leftMaterial)->baseColor);
        const auto realtimeFrames = [&](std::uint32_t frames, bool reset = false,
                                         const assets::MaterialAsset* draft = nullptr) {
            require(frames > 0, "Realtime fixture requires at least one rendered frame.");
            if (reset) renderer.resetGlobalIllumination();
            Pixels image;
            for (std::uint32_t frame = 0; frame < frames; ++frame) image = render(draft);
            if (settings.globalIllumination.indirectDiffuse || settings.globalIllumination.indirectSpecular)
                require(renderer.globalIlluminationStatus().effective == Method::PathTracing &&
                    renderer.globalIlluminationStatus().requestedMode == Mode::Realtime &&
                    renderer.globalIlluminationStatus().effectiveMode == Mode::Realtime,
                    "Realtime transport became unavailable: " + renderer.globalIlluminationStatus().reason);
            return image;
        };
        const auto realtimeRed = realtimeFrames(16, true);
        std::copy(blue.begin(), blue.end(), assets.findMaterial(leftMaterial)->baseColor);
        const auto realtimeBlue = realtimeFrames(16, true);
        const auto realtimeRedMean = meanColour(realtimeRed, mask);
        const auto realtimeBlueMean = meanColour(realtimeBlue, mask);
        const auto realtimeTransfer = (realtimeRedMean[0] - realtimeRedMean[2]) -
            (realtimeBlueMean[0] - realtimeBlueMean[2]);
        report["metrics"]["realtimeRedWallFloorMeanRgb"] = realtimeRedMean;
        report["metrics"]["realtimeBlueWallFloorMeanRgb"] = realtimeBlueMean;
        report["metrics"]["realtimeDiffuseRedBlueTransfer"] = realtimeTransfer;
        savePixels(output / "realtime-red-wall.bmp", realtimeRed, width, height);
        savePixels(output / "realtime-blue-wall.bmp", realtimeBlue, width, height);
        require(realtimeTransfer > .002,
            "Realtime diffuse transport did not transfer wall colour to the GPU-selected neutral floor.");

        settings.globalIllumination.indirectDiffuse = false;
        const auto realtimeBlueDirect = realtimeFrames(16, true);
        std::copy(red.begin(), red.end(), assets.findMaterial(leftMaterial)->baseColor);
        const auto realtimeRedDirect = realtimeFrames(16, true);
        const auto realtimeDirectError = meanError(realtimeBlueDirect, realtimeRedDirect, mask);
        const auto realtimeDiffuseEffect = meanError(realtimeBlue, realtimeBlueDirect, mask);
        report["metrics"]["realtimeDirectWallChangeFloorMae"] = realtimeDirectError;
        report["metrics"]["realtimeDiffuseContributionFloorMae"] = realtimeDiffuseEffect;
        require(realtimeDirectError < .0005 && realtimeDiffuseEffect > .001,
            "Realtime diffuse contribution switch failed to isolate wall-colour transport from direct light.");
        require(visiblePixels(realtimeRedDirect) > 100,
            "Disabling both indirect contributions removed the raster path's direct lighting.");
        settings.lightIntensity = 0;
        const auto realtimeNoLight = realtimeFrames(1);
        report["metrics"]["realtimeBothContributionsOffNoLightVisiblePixels"] = visiblePixels(realtimeNoLight);
        require(visiblePixels(realtimeNoLight) == 0,
            "Disabling both GI contributions fell back to local ambient/environment lighting.");
        settings.lightIntensity = 5;
        settings.globalIllumination.indirectDiffuse = true;
        (void)realtimeFrames(16, true);
        pass("realtime neutral-floor colour transfer follows indirect diffuse; both contributions off preserve direct light without ambient fallback");

        // Continue from a full 256-sample window. New estimates must still
        // arrive while the history count stays bounded and the camera is still.
        const auto fullRealtimeStatus = renderer.globalIlluminationStatus();
        const auto fullRealtimeImage = readScene(renderer);
        const auto realtimeDispatches = renderer.hardwareRayTracingDispatchCount();
        const auto rollingRealtime = realtimeFrames(4);
        const auto rollingStatus = renderer.globalIlluminationStatus();
        const auto rollingPixels = changedPixels(fullRealtimeImage, rollingRealtime, 0);
        report["metrics"]["realtimeRollingChangedPixels"] = rollingPixels;
        require(fullRealtimeStatus.accumulatedSamples == 256 && rollingStatus.accumulatedSamples == 256 &&
            rollingStatus.historyRevision == fullRealtimeStatus.historyRevision &&
            renderer.hardwareRayTracingDispatchCount() >= realtimeDispatches + 4 && rollingPixels > 8,
            "Realtime GI stopped producing new samples/images after filling its history window.");
        savePixels(output / "realtime-rolling-history.bmp", rollingRealtime, width, height);
        pass("realtime continues real dispatches and GPU image updates after its bounded history reaches the cap");
        settings.exposure = .5f;
        const auto realtimeDarker = render();
        require(renderer.globalIlluminationStatus().historyRevision == rollingStatus.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 256 &&
            changedPixels(rollingRealtime, realtimeDarker) > 100,
            "Realtime exposure editing reset radiance history or failed to change the GPU output.");
        settings.exposure = 1;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == rollingStatus.historyRevision,
            "Restoring realtime exposure invalidated indirect history.");
        pass("realtime exposure updates the final image while continuing the existing indirect history");

        // A primary metal surface has no diffuse lobe. Its indirect-specular
        // switch can therefore be checked without confusing diffuse colour
        // transfer with reflections or the legacy hardware preview checkbox.
        const auto realtimeDiffuseSphere = *assets.findMaterial(sphereMaterial);
        require(renderer.resize(320, 240, error), error);
        settings.viewportWidth = 320; settings.viewportHeight = 240;
        settings.cameraDistance = 4.8f; settings.cameraTarget[1] = .7f; settings.cameraPitch = .22f;
        auto* realtimeMetal = assets.findMaterial(sphereMaterial);
        realtimeMetal->metallic = 1; realtimeMetal->roughness = .12f;
        realtimeMetal->baseColor[0] = realtimeMetal->baseColor[1] = realtimeMetal->baseColor[2] = .88f;
        settings.globalIllumination.indirectDiffuse = false;
        settings.globalIllumination.indirectSpecular = true;
        settings.hardwareRayTracingReflections = false;
        const auto realtimeSpecular = realtimeFrames(16, true);
        savePixels(output / "realtime-indirect-specular-on.bmp", realtimeSpecular, 320, 240);
        settings.hardwareRayTracingReflections = true;
        const auto realtimeLegacySwitch = realtimeFrames(16, true);
        require(changedPixels(realtimeSpecular, realtimeLegacySwitch, 0) == 0,
            "Legacy preview reflection setting unexpectedly controls realtime GI specular transport.");
        settings.globalIllumination.indirectSpecular = false;
        const auto realtimeSpecularOff = realtimeFrames(16, true);
        const auto realtimeSpecularPixels = changedPixels(realtimeSpecular, realtimeSpecularOff);
        report["metrics"]["realtimeIndirectSpecularChangedPixels"] = realtimeSpecularPixels;
        require(realtimeSpecularPixels > 100,
            "Disabling GI indirect specular did not remove indirect reflections from the metal fixture.");
        savePixels(output / "realtime-indirect-specular-off.bmp", realtimeSpecularOff, 320, 240);
        settings.globalIllumination.indirectSpecular = true;
        require(changedPixels(realtimeSpecular, realtimeFrames(16, true), 0) == 0,
            "Restoring indirect specular did not restore its same-seed realtime image.");
        *assets.findMaterial(sphereMaterial) = realtimeDiffuseSphere;
        settings.hardwareRayTracingReflections = false;
        settings.globalIllumination.indirectDiffuse = true;
        settings.globalIllumination.indirectSpecular = false;
        settings.cameraDistance = 7; settings.cameraTarget[1] = 1; settings.cameraPitch = .3f;
        require(renderer.resize(width, height, error), error);
        settings.viewportWidth = width; settings.viewportHeight = height;
        pass("realtime GI indirect specular controls metal transport independently of diffuse and the legacy reflection switch");

        settings.globalIllumination.samplesPerFrame = 2;
        settings.globalIllumination.targetSamples = 16;
        (void)realtimeFrames(8, true);
        const auto realtimeFreshAfter = [&](const char* name, auto change,
                                             const assets::MaterialAsset* draft = nullptr) {
            const auto before = renderer.globalIlluminationStatus().historyRevision;
            change();
            const auto moved = render(draft);
            const auto automatic = renderer.globalIlluminationStatus();
            require(automatic.historyRevision > before && automatic.accumulatedSamples == 2,
                std::string("Realtime ") + name + " did not clear obsolete samples on its first frame.");
            renderer.resetGlobalIllumination();
            const auto clean = render(draft);
            require(changedPixels(moved, clean, 0) == 0,
                std::string("Realtime ") + name + " blended old history into the changed scene.");
            report["metrics"]["realtimeFreshHistory"][name] = automatic.historyRevision;
            return moved;
        };
        Pixels movingCamera;
        for (int step = 1; step <= 3; ++step)
            movingCamera = realtimeFreshAfter(("camera step " + std::to_string(step)).c_str(), [&] {
                settings.cameraYaw = .35f + .06f * static_cast<float>(step);
            });
        savePixels(output / "realtime-camera-moving-first-frame.bmp", movingCamera, width, height);
        const auto stoppedCamera = renderer.globalIlluminationStatus();
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == stoppedCamera.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 4,
            "Realtime camera history failed to continue when motion stopped.");
        (void)realtimeFreshAfter("camera restore", [&] { settings.cameraYaw = .35f; });
        Pixels movingObject;
        for (int step = 1; step <= 3; ++step)
            movingObject = realtimeFreshAfter(("object step " + std::to_string(step)).c_str(), [&] {
                world.findActor(sphere)->transform.position[0] = .15f * static_cast<float>(step);
            });
        savePixels(output / "realtime-object-moving-first-frame.bmp", movingObject, width, height);
        (void)realtimeFreshAfter("object restore", [&] { world.findActor(sphere)->transform.position[0] = 0; });
        const auto normalLighting = realtimeFrames(8, true);
        const auto dimLighting = realtimeFreshAfter("light change", [&] { settings.lightIntensity = 1.5f; });
        require(changedPixels(normalLighting, dimLighting) > 100, "Realtime light editing failed to update actual scene pixels.");
        (void)realtimeFreshAfter("light restore", [&] { settings.lightIntensity = 5; });
        const auto realtimeCommitted = *assets.findMaterial(floorMaterial);
        auto realtimeDraft = realtimeCommitted;
        realtimeDraft.baseColor[0] = .12f; realtimeDraft.baseColor[1] = .8f; realtimeDraft.baseColor[2] = .2f;
        const auto beforeRealtimeDraft = realtimeFrames(8, true);
        const auto realtimeDraftImage = realtimeFreshAfter("material draft", [] {}, &realtimeDraft);
        require(changedPixels(beforeRealtimeDraft, realtimeDraftImage) > 100 &&
            assets.findMaterial(floorMaterial)->baseColor[0] == realtimeCommitted.baseColor[0],
            "Realtime material preview did not update GPU pixels or modified the committed asset.");
        (void)realtimeFreshAfter("discard material draft", [] {});
        (void)realtimeFreshAfter("hide object", [&] { world.findActor(sphere)->visible = false; });
        (void)realtimeFreshAfter("show object", [&] { world.findActor(sphere)->visible = true; });
        (void)realtimeFreshAfter("indirect specular enabled", [&] { settings.globalIllumination.indirectSpecular = true; });
        (void)realtimeFreshAfter("indirect diffuse disabled", [&] { settings.globalIllumination.indirectDiffuse = false; });
        (void)realtimeFreshAfter("indirect diffuse restored", [&] { settings.globalIllumination.indirectDiffuse = true; });
        (void)realtimeFreshAfter("indirect specular disabled", [&] { settings.globalIllumination.indirectSpecular = false; });
        (void)realtimeFreshAfter("bounce depth", [&] { settings.globalIllumination.maxBounces = 1; });
        (void)realtimeFreshAfter("bounce depth restore", [&] { settings.globalIllumination.maxBounces = 4; });
        pass("realtime camera/object motion, stopped motion, lighting, material preview/discard and visibility produce fresh first-frame images without old history");

        // Debug data and tool previews are separate consumers. Neither is a
        // radiance sample, and neither may corrupt the main camera's history.
        (void)realtimeFrames(3, true);
        const auto beforeRealtimeDebug = renderer.globalIlluminationStatus();
        settings.debugView = 3;
        const auto realtimeNormals = render();
        require(renderer.globalIlluminationStatus().accumulatedSamples == beforeRealtimeDebug.accumulatedSamples &&
            renderer.globalIlluminationStatus().historyRevision == beforeRealtimeDebug.historyRevision,
            "Realtime debug output consumed or replaced radiance history.");
        settings.debugView = 0;
        (void)render();
        require(renderer.globalIlluminationStatus().accumulatedSamples == beforeRealtimeDebug.accumulatedSamples + 2 &&
            renderer.globalIlluminationStatus().historyRevision == beforeRealtimeDebug.historyRevision,
            "Returning from realtime debug did not resume the main history.");
        const auto beforeRealtimeTool = renderer.globalIlluminationStatus();
        require(renderer.renderMaterialPreview(realtimeCommitted, 64, error), error);
        require(renderer.globalIlluminationStatus().accumulatedSamples == beforeRealtimeTool.accumulatedSamples &&
            renderer.globalIlluminationStatus().historyRevision == beforeRealtimeTool.historyRevision,
            "Tool preview changed realtime main-scene samples.");
        (void)render();
        require(renderer.globalIlluminationStatus().accumulatedSamples == beforeRealtimeTool.accumulatedSamples + 2 &&
            renderer.globalIlluminationStatus().historyRevision == beforeRealtimeTool.historyRevision,
            "Realtime history failed to continue after the tool preview.");
        settings.debugView = 3;
        settings.globalIllumination.indirectDiffuse = false;
        require(changedPixels(realtimeNormals, render(), 0) == 0,
            "GI contribution toggles contaminated the world-normal debug image.");
        settings.debugView = 0;
        settings.globalIllumination.indirectDiffuse = true;
        pass("realtime debug images and offscreen previews remain isolated from radiance history and lighting contributions");

        settings.globalIllumination.samplesPerFrame = 16;
        settings.globalIllumination.targetSamples = 64;
        for (const auto path : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
            settings.path = path;
            settings.globalIllumination.indirectDiffuse = true;
            const auto withIndirect = realtimeFrames(4, true);
            settings.globalIllumination.indirectDiffuse = false;
            const auto directOnly = realtimeFrames(4, true);
            const auto pathError = meanError(withIndirect, directOnly, mask);
            const auto pathName = path == renderer::RenderPath::Forward ? "forward" : "deferred";
            report["metrics"][std::string("realtimeRasterConsumerFloorMae_") + pathName] = pathError;
            require(pathError > .001, std::string("The ") + pathName + " path did not consume actual realtime indirect lighting.");
            savePixels(output / (std::string("realtime-") + pathName + "-gi.bmp"), withIndirect, width, height);
            savePixels(output / (std::string("realtime-") + pathName + "-direct.bmp"), directOnly, width, height);
        }
        settings.path = renderer::RenderPath::Deferred;
        settings.globalIllumination.indirectDiffuse = true;
        settings.globalIllumination.samplesPerFrame = 2;
        settings.globalIllumination.targetSamples = 16;
        (void)realtimeFrames(8, true);
        const auto beforeModeChange = renderer.globalIlluminationStatus().historyRevision;
        settings.globalIllumination.mode = Mode::Progressive;
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision > beforeModeChange &&
            renderer.globalIlluminationStatus().accumulatedSamples == 2,
            "Changing realtime to progressive mode reused incompatible history.");
        const auto progressiveRoundtrip = advanceTo(16);
        require(changedPixels(progressiveRoundtrip, render(), 0) == 0,
            "Progressive mode did not stop at its sample cap after a realtime roundtrip.");
        const auto beforeRealtimeReturn = renderer.globalIlluminationStatus().historyRevision;
        settings.globalIllumination.mode = Mode::Realtime;
        const auto realtimeRoundtrip = render();
        require(renderer.globalIlluminationStatus().historyRevision > beforeRealtimeReturn &&
            renderer.globalIlluminationStatus().accumulatedSamples == 2,
            "Returning to realtime mode reused a progressive camera image as indirect history.");
        renderer.resetGlobalIllumination();
        require(changedPixels(realtimeRoundtrip, render(), 0) == 0,
            "Realtime mode roundtrip produced a stale first-frame image.");
        pass("Forward and Deferred each consume realtime indirect light; realtime/progressive roundtrips reset incompatible histories");

        const auto beforeRealtimeResize = renderer.globalIlluminationStatus().historyRevision;
        require(renderer.resize(192, 128, error), error);
        settings.viewportWidth = 192; settings.viewportHeight = 128;
        const auto resizedRealtime = render();
        require(renderer.globalIlluminationStatus().historyRevision > beforeRealtimeResize &&
            renderer.globalIlluminationStatus().accumulatedSamples == 2,
            "Realtime resize retained an incompatible indirect-light image.");
        renderer.resetGlobalIllumination();
        require(changedPixels(resizedRealtime, render(), 0) == 0,
            "Resized realtime output contains radiance from the old image dimensions.");
        const auto realtimeBeforeShader = realtimeFrames(8, true);
        const auto realtimeShaderHistory = renderer.globalIlluminationStatus().historyRevision;
        require(renderer.applyShaderSource(changedSource, error), error);
        const auto realtimeChangedShader = render();
        require(renderer.globalIlluminationStatus().historyRevision > realtimeShaderHistory &&
            renderer.globalIlluminationStatus().accumulatedSamples == 2 &&
            changedPixels(realtimeBeforeShader, realtimeChangedShader) > 100,
            "Realtime shader update failed to discard stale indirect lighting and update actual pixels.");
        renderer.resetGlobalIllumination();
        require(changedPixels(realtimeChangedShader, render(), 0) == 0,
            "Realtime shader replacement blended the old shader's indirect-light history.");
        const auto realtimeWorkingHistory = renderer.globalIlluminationStatus();
        const auto realtimeWorkingRevision = renderer.shaderRevision();
        const bool invalidRealtimeApplied = renderer.applyShaderSource(changedSource +
            "\n#ifdef RAE_HARDWARE_RAYTRACING\n#error Intentional realtime GI candidate failure\n#endif\n", error);
        require(!invalidRealtimeApplied && !error.empty() && renderer.shaderRevision() == realtimeWorkingRevision &&
            renderer.shaderSource() == changedSource &&
            renderer.globalIlluminationStatus().historyRevision == realtimeWorkingHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == realtimeWorkingHistory.accumulatedSamples,
            "Invalid realtime shader candidate changed active source or history.");
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision == realtimeWorkingHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 4,
            "Realtime history did not continue after a rejected shader candidate.");
        require(renderer.applyShaderSource(oldSource, error), error);
        (void)render();
        require(renderer.globalIlluminationStatus().historyRevision > realtimeWorkingHistory.historyRevision &&
            renderer.globalIlluminationStatus().accumulatedSamples == 2,
            "Repairing the realtime shader did not start compatible indirect history.");
        pass("realtime resize and valid shader updates start clean GPU history; failed candidates leave the active shader and history running");

        const auto boundarySettings = settings;
        const auto boundaryWidth = renderer.sceneTextureWidth(), boundaryHeight = renderer.sceneTextureHeight();
        require(renderer.resize(width, height, error), error);
        settings.viewportX = settings.viewportY = 0;
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.globalIllumination.samplesPerFrame = 16;
        settings.globalIllumination.targetSamples = 1;
        renderer.resetGlobalIllumination();
        const auto oneSampleDispatches = renderer.hardwareRayTracingDispatchCount();
        std::array<Pixels, 3> requestedBatch16;
        std::uint64_t oneSampleHistory = 0;
        for (std::size_t frame = 0; frame < requestedBatch16.size(); ++frame) {
            requestedBatch16[frame] = render();
            const auto status = renderer.globalIlluminationStatus();
            require(status.effective == Method::PathTracing && status.effectiveMode == Mode::Realtime &&
                status.accumulatedSamples == 1 && (frame == 0 || status.historyRevision == oneSampleHistory),
                "Realtime SPF=16/target=1 failed to maintain a bounded one-sample history.");
            oneSampleHistory = status.historyRevision;
        }
        const auto oneSampleChanged = changedPixels(requestedBatch16.front(), requestedBatch16.back(), 0);
        report["metrics"]["realtimeOneSampleWindowChangedPixels"] = oneSampleChanged;
        require(renderer.hardwareRayTracingDispatchCount() >= oneSampleDispatches + requestedBatch16.size() && oneSampleChanged > 8,
            "Realtime one-sample history stopped dispatching or updating its image.");
        // With a one-sample window a requested batch of sixteen must behave
        // exactly like a batch of one, including its deterministic sequence.
        settings.globalIllumination.samplesPerFrame = 1;
        renderer.resetGlobalIllumination();
        for (std::size_t frame = 0; frame < requestedBatch16.size(); ++frame)
            require(changedPixels(requestedBatch16[frame], render(), 0) == 0 &&
                renderer.globalIlluminationStatus().accumulatedSamples == 1,
                "A batch larger than the realtime history window changed its sample weighting or sequence.");
        settings.globalIllumination.samplesPerFrame = 16;
        const auto beforeOneSampleMotion = renderer.globalIlluminationStatus().historyRevision;
        const auto boundaryYaw = settings.cameraYaw;
        settings.cameraYaw += .08f;
        const auto oneSampleMotion = render();
        require(renderer.globalIlluminationStatus().historyRevision > beforeOneSampleMotion &&
            renderer.globalIlluminationStatus().accumulatedSamples == 1,
            "Motion did not reset a realtime one-sample history with a larger requested batch.");
        renderer.resetGlobalIllumination();
        require(changedPixels(oneSampleMotion, render(), 0) == 0,
            "Realtime one-sample motion blended old history instead of producing its fresh same-seed image.");
        settings.cameraYaw = boundaryYaw;
        savePixels(output / "realtime-one-sample-window.bmp", requestedBatch16.back(), width, height);
        pass("realtime SPF larger than a one-sample target stays bounded, continues new samples and resets motion without invalid weights");

        // Compare actual scene readbacks, cropped using the texture's queried
        // dimensions rather than assuming that the viewport owns the texture.
        // Keeping Grid disabled removes resolution-dependent material footprints
        // from this test; geometry, direct light and GI must otherwise align.
        require(std::all_of(assets.materials.begin(), assets.materials.end(), [](const auto& material) { return !material.grid; }),
            "Viewport alignment fixture must not contain pixel-footprint grid shading.");
        constexpr std::uint32_t largerWidth = 257, largerHeight = 193, offsetX = 37, offsetY = 29;
        settings.globalIllumination.samplesPerFrame = 16;
        settings.globalIllumination.targetSamples = 64;
        for (const auto path : {renderer::RenderPath::Forward, renderer::RenderPath::Deferred}) {
            const auto pathName = path == renderer::RenderPath::Forward ? "forward" : "deferred";
            settings.path = path;
            settings.viewportX = settings.viewportY = 0;
            settings.viewportWidth = width; settings.viewportHeight = height;
            require(renderer.resize(width, height, error), error);
            (void)realtimeFrames(4, true);
            const auto origin = readSceneRegion(renderer, 0, 0, width, height);
            require(renderer.resize(largerWidth, largerHeight, error), error);
            (void)realtimeFrames(4, true);
            const auto largerOrigin = readSceneRegion(renderer, 0, 0, width, height);
            settings.viewportX = offsetX; settings.viewportY = offsetY;
            (void)realtimeFrames(4, true);
            const auto offset = readSceneRegion(renderer, offsetX, offsetY, width, height);
            const auto largerOriginChanges = changedPixels(origin, largerOrigin, 0);
            const auto offsetChanges = changedPixels(largerOrigin, offset, 0);
            report["metrics"][std::string("realtimeLargerTargetOriginChangedPixels_") + pathName] = largerOriginChanges;
            report["metrics"][std::string("realtimeViewportOffsetChangedPixels_") + pathName] = offsetChanges;
            savePixels(output / (std::string("realtime-viewport-origin-") + pathName + ".bmp"), origin, width, height);
            savePixels(output / (std::string("realtime-viewport-larger-origin-") + pathName + ".bmp"), largerOrigin, width, height);
            savePixels(output / (std::string("realtime-viewport-offset-") + pathName + ".bmp"), offset, width, height);
            std::uint32_t actualWidth = 0, actualHeight = 0;
            const auto offsetFull = readScene(renderer, &actualWidth, &actualHeight);
            require(actualWidth == largerWidth && actualHeight == largerHeight,
                "Readback did not use the actual larger render-target dimensions.");
            savePixels(output / (std::string("realtime-viewport-full-") + pathName + ".bmp"), offsetFull, actualWidth, actualHeight);
            require(largerOriginChanges == 0 && offsetChanges == 0,
                std::string("Realtime ") + pathName + " GI is misaligned after target resize or nonzero viewport origin (" +
                std::to_string(largerOriginChanges) + "/" + std::to_string(offsetChanges) + " changed pixels).");
        }
        require(renderer.resize(boundaryWidth, boundaryHeight, error), error);
        settings = boundarySettings;
        (void)render();
        pass("realtime Forward/Deferred viewport crops match their zero-origin image exactly inside larger render targets");

        runNeighbourColourTransport(renderer, output, report);
        runAreaLightTransport(renderer, output, report);

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
