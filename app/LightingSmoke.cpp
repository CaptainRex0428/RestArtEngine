#include "LightingSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
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
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Pixels = std::vector<std::uint8_t>;
using Mask = std::vector<bool>;
constexpr std::uint32_t width = 160, height = 120;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Lighting: " + message);
}
struct FrameScope {
    FrameScope() { rae::diagnostics::beginFrame(); }
    ~FrameScope() { rae::diagnostics::endFrame(); }
};
Pixels readScene(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "Missing scene texture.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> source, staging;
    require(SUCCEEDED(resource.As(&source)), "Scene output is not a texture.");
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    require(description.Width == width && description.Height == height &&
        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "Unexpected scene dimensions/format.");
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.BindFlags = description.MiscFlags = 0;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&description, nullptr, &staging)), "Cannot allocate GPU staging texture.");
    renderer.context()->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Cannot read GPU scene pixels.");
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
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(bgra.size());
    info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(bgra.size());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    stream.close();
    require(stream.good(), "Cannot save GPU evidence image.");
}
std::size_t count(const Mask& mask) { return static_cast<std::size_t>(std::count(mask.begin(), mask.end(), true)); }
Mask intersection(const Mask& a, const Mask& b) {
    require(a.size() == b.size(), "Incompatible receiver masks.");
    Mask result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) result[i] = a[i] && b[i];
    return result;
}
std::array<double, 3> mean(const Pixels& pixels, const Mask& mask) {
    std::array<double, 3> result{};
    const auto samples = count(mask);
    require(samples > 0, "Empty receiver mask.");
    for (std::size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) for (unsigned channel = 0; channel < 3; ++channel) result[channel] += pixels[i * 4 + channel];
    for (auto& value : result) value /= static_cast<double>(samples);
    return result;
}
double brightness(const Pixels& pixels, const Mask& mask) {
    const auto rgb = mean(pixels, mask);
    return (rgb[0] + rgb[1] + rgb[2]) / 3;
}
std::size_t changed(const Pixels& a, const Pixels& b, const Mask& mask, int tolerance = 3) {
    std::size_t result = 0;
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (!mask[i]) continue;
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (std::abs(int(a[i * 4 + channel]) - int(b[i * 4 + channel])) <= tolerance) continue;
            ++result; break;
        }
    }
    return result;
}
} // namespace

void runLightingSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Type = scene::LightType;
    using Path = renderer::RenderPath;
    using Shadow = renderer::ShadowMethod;
    using Gi = renderer::GlobalIlluminationMethod;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
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
    } restore{renderer, renderer.shaderSource(), renderer.sceneTextureWidth(), renderer.sceneTextureHeight()};
    Json report{{"format", "RestArtEngine.LightingSmoke"}, {"version", 1}, {"adapter", renderer.adapterName()},
        {"passed", false}, {"checks", Json::array()}, {"metrics", Json::object()}};
    const auto saveReport = [&] {
        std::ofstream stream(output / "lighting-report.json", std::ios::binary | std::ios::trunc);
        stream << report.dump(2) << '\n'; stream.close();
        require(stream.good(), "Cannot save lighting report.");
    };
    const auto pass = [&](const std::string& message) {
        report["checks"].push_back(message);
        std::cout << "PASS: lighting: " << message << '\n';
    };
    try {
        std::string error;
        require(renderer.resize(width, height, error), error);
        require(renderer.applyShaderSource("Surface EvaluateMaterial(float3 p, float3 n, Surface m) { return m; }", error), error);
        assets::AssetLibrary assets; assets.createDefaults();
        assets.materials.clear();
        assets::MaterialAsset material;
        material.id = 101; material.name = "Lighting receiver";
        std::fill(std::begin(material.baseColor), std::end(material.baseColor), .65f);
        material.metallic = 0; material.roughness = .85f; material.grid = false;
        assets.materials.push_back(material);
        auto blockerMaterial = material;
        blockerMaterial.id = 102; blockerMaterial.name = "Lighting blocker";
        std::fill(std::begin(blockerMaterial.baseColor), std::end(blockerMaterial.baseColor), .1f);
        assets.materials.push_back(blockerMaterial);
        scene::World world;
        renderer::SceneSettings settings;
        settings.offscreenTarget = true;
        settings.viewportWidth = width; settings.viewportHeight = height;
        settings.cameraYaw = 0; settings.cameraPitch = .45f; settings.cameraDistance = 7;
        settings.cameraTarget[1] = .4f;
        settings.shadowMethod = Shadow::Disabled;
        settings.lightIntensity = 25; // Explicit lights must suppress this legacy fallback, including while hidden.
        const auto render = [&] {
            FrameScope frame;
            require(renderer.render(settings, world, assets, error), error);
            auto pixels = readScene(renderer);
            require(renderer.present(false, error), error);
            return pixels;
        };
        const auto capture = [&](const std::string& name) {
            auto pixels = render();
            savePixels(output / (name + ".bmp"), pixels);
            return pixels;
        };
        const auto receiverMask = [&] {
            const auto oldDebug = settings.debugView;
            settings.debugView = 2; const auto albedo = render();
            settings.debugView = 3; const auto normals = render();
            settings.debugView = oldDebug;
            Mask mask(static_cast<std::size_t>(width) * height);
            for (std::size_t i = 0; i < mask.size(); ++i) {
                const auto offset = i * 4;
                mask[i] = albedo[offset] >= 205 && albedo[offset] <= 215 &&
                    std::abs(int(albedo[offset]) - int(albedo[offset + 1])) <= 1 &&
                    std::abs(int(albedo[offset]) - int(albedo[offset + 2])) <= 1 && normals[offset + 1] >= 254 &&
                    std::abs(int(normals[offset]) - 128) <= 1 && std::abs(int(normals[offset + 2]) - 128) <= 1;
            }
            Mask eroded(mask.size());
            for (std::uint32_t y = 1; y + 1 < height; ++y) for (std::uint32_t x = 1; x + 1 < width; ++x) {
                bool inside = true;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
                    inside = inside && mask[static_cast<std::size_t>(static_cast<int>(y) + dy) * width + static_cast<int>(x) + dx];
                eroded[static_cast<std::size_t>(y) * width + x] = inside;
            }
            require(count(eroded) > 100, "GPU normal/albedo output did not identify the floor.");
            return eroded;
        };
        const auto fixture = [&](Type type, bool sphere = true) {
            world = {};
            world.blackBackground = true;
            const auto floor = world.createActor("Floor", 2, material.id);
            for (float& scale : world.findActor(floor)->transform.scale) scale = .3f;
            if (sphere) {
                const auto ball = world.createActor("Sphere", 1, material.id);
                world.findActor(ball)->transform.position[0] = .9f;
                world.findActor(ball)->transform.position[1] = .6f;
                for (float& scale : world.findActor(ball)->transform.scale) scale = .6f;
            }
            const auto light = world.createLight("Test light", type);
            auto& component = *world.findActor(light)->light;
            component.castShadows = true;
            component.range = 10;
            component.intensity = type == Type::Sky ? 1.f : type == Type::Point || type == Type::Spot ? 25.f : 4.f;
            return light;
        };

        for (const auto type : {Type::Directional, Type::Sky, Type::Rect, Type::Spot, Type::Point}) {
            const std::string name = scene::lightTypeName(type);
            const auto light = fixture(type);
            const auto mask = receiverMask();
            auto& actor = *world.findActor(light);
            auto& component = *actor.light;
            actor.visible = false;
            const auto disabled = capture(name + "-disabled");
            require(brightness(disabled, mask) < 1, name + ": hidden last light incorrectly restored legacy lighting.");
            actor.visible = true;
            const auto originalIntensity = component.intensity;
            component.intensity = 0;
            const auto zero = render();
            require(brightness(zero, mask) < 1, name + ": zero energy still illuminates the receiver.");
            component.intensity = originalIntensity * .25f;
            const auto low = capture(name + "-low");
            component.intensity = originalIntensity;
            settings.path = Path::Forward;
            const auto high = capture(name + "-forward");
            settings.path = Path::Deferred;
            const auto deferred = capture(name + "-deferred");
            const auto pixelsChanged = changed(disabled, high, mask);
            const auto pathDifference = changed(high, deferred, mask, 2);
            report["metrics"][name] = {{"receiverPixels",count(mask)},{"litPixels",pixelsChanged},
                {"lowMean",brightness(low,mask)},{"highMean",brightness(high,mask)},{"pathDifferentPixels",pathDifference}};
            require(pixelsChanged > 30 && brightness(high, mask) > brightness(low, mask) + 2,
                name + ": changing intensity did not update the actual receiver next frame.");
            require(pathDifference <= std::max<std::size_t>(8, count(mask) / 50), name + ": Forward/Deferred lighting disagree.");
            component.color[1] = component.color[2] = 0;
            const auto red = capture(name + "-red");
            const auto rgb = mean(red, mask);
            require(rgb[0] > rgb[1] + 3 && rgb[0] > rgb[2] + 3 && changed(red, deferred, mask) > 30,
                name + ": live color did not reach the receiver.");
            std::fill(std::begin(component.color), std::end(component.color), 1.f);
            settings.rayTracing = true;
            const auto analytic = capture(name + "-software-ray");
            require(brightness(analytic, mask) > 3, name + ": software ray shading lost this light.");
            actor.visible = false;
            require(brightness(render(), mask) < 1, name + ": hidden light remains in software ray shading.");
            settings.rayTracing = false;
            pass(name + " live intensity/color, hidden/zero light, Forward/Deferred and software ray output");
        }

        {
            const auto light = fixture(Type::Point, false);
            const auto mask = receiverMask();
            const auto wide = capture("Point-range-wide");
            world.findActor(light)->light->range = 1;
            const auto shortRange = capture("Point-range-short");
            require(brightness(shortRange, mask) < 1 && changed(wide, shortRange, mask) > 30,
                "Point range does not cut off receiver pixels beyond the authored radius.");
            pass("Point range limits finite lighting influence");
        }
        {
            const auto light = fixture(Type::Spot, false);
            const auto mask = receiverMask();
            auto& actor = *world.findActor(light);
            actor.light->innerConeAngle = 30; actor.light->outerConeAngle = 55;
            const auto wide = capture("Spot-wide");
            actor.light->innerConeAngle = 3; actor.light->outerConeAngle = 8;
            const auto narrow = capture("Spot-narrow");
            require(brightness(wide, mask) > brightness(narrow, mask) + 2 && changed(wide, narrow, mask) > 30,
                "Spot cone angles do not change the visible footprint.");
            actor.transform.rotation[0] = 90;
            const auto away = capture("Spot-aimed-away");
            require(brightness(away, mask) < 1, "Spot rotation did not turn its cone away from the receiver.");
            pass("Spot inner/outer cone and Actor rotation control its footprint");
        }
        {
            const auto light = fixture(Type::Rect, false);
            const auto mask = receiverMask();
            auto& actor = *world.findActor(light);
            actor.light->width = actor.light->height = .4f;
            const auto smallRectImage = capture("Rect-small");
            actor.light->width = actor.light->height = 3;
            const auto large = capture("Rect-large");
            require(brightness(large, mask) > brightness(smallRectImage, mask) + 2 && changed(smallRectImage, large, mask) > 30,
                "Rectangle area does not affect radiance delivered to the floor.");
            actor.transform.rotation[0] = 90;
            const auto back = capture("Rect-back-face");
            require(brightness(back, mask) < 1, "Rectangle emits through its back face.");
            pass("Rectangle dimensions and single-sided Actor orientation affect actual illumination");
        }

        // Compare only the same visible floor pixels; a block's own shading or
        // its camera silhouette cannot masquerade as a light shadow.
        const auto shadowCase = [&](Type type, Shadow shadow, const std::string& label, bool finite) {
            const auto light = fixture(type, false);
            auto& transform = world.findActor(light)->transform;
            transform.rotation[0] = type == Type::Directional ? -65.f : -90.f;
            transform.rotation[1] = 0;
            const auto originalMask = receiverMask();
            settings.shadowMethod = shadow;
            const auto unobstructed = capture(label + "-clear");
            const auto blocker = world.createActor("Shadow blocker", 3, blockerMaterial.id);
            auto& block = world.findActor(blocker)->transform;
            block.position[1] = 1.5f;
            block.scale[0] = block.scale[2] = .7f; block.scale[1] = .08f;
            const auto blockedMask = intersection(originalMask, receiverMask());
            const auto shadowed = capture(label + "-blocked");
            world.findActor(light)->light->castShadows = false;
            const auto noCast = capture(label + "-cast-off");
            world.findActor(light)->light->castShadows = true;
            settings.shadowMethod = Shadow::Disabled;
            const auto globalOff = capture(label + "-global-disabled");
            report["metrics"][label] = {{"receiverPixels",count(blockedMask)},
                {"shadowPixels",changed(shadowed,noCast,blockedMask)},
                {"castOffMean",brightness(noCast,blockedMask)},{"shadowedMean",brightness(shadowed,blockedMask)}};
            require(count(blockedMask) > 100 && changed(shadowed, noCast, blockedMask) > 15 &&
                brightness(noCast, blockedMask) > brightness(shadowed, blockedMask) + .5,
                label + ": opaque geometry does not cast a shadow or Cast shadows has no effect.");
            require(changed(noCast, globalOff, blockedMask, 2) < 8,
                label + ": global Disabled does not suppress the same shadow as Cast shadows off.");
            settings.shadowMethod = shadow;
            if (finite) {
                block.position[1] = 4;
                const auto beyondMask = intersection(originalMask, receiverMask());
                const auto beyond = capture(label + "-block-beyond-light");
                require(changed(unobstructed, beyond, beyondMask, 2) < 8,
                    label + ": geometry beyond the emitter shadows a finite light segment.");
            }
            settings.shadowMethod = Shadow::Disabled;
            pass(label + " occlusion, Cast shadows and global Disabled" + (finite ? ", finite shadow endpoint" : ""));
        };
        shadowCase(Type::Point, Shadow::AnalyticRay, "Point-analytic-shadow", true);
        shadowCase(Type::Spot, Shadow::AnalyticRay, "Spot-analytic-shadow", true);
        shadowCase(Type::Rect, Shadow::AnalyticRay, "Rect-analytic-shadow", true);
        shadowCase(Type::Directional, Shadow::ShadowMap, "Directional-shadowmap", false);

        // Tool-preview rendering intentionally cannot consume the main DXR/GI
        // history. Use the editor's scene texture path for real hardware tests.
        settings.offscreenTarget = false;
        settings.editorViewport = true;
        settings.hardwareRayTracing = true;
        settings.hardwareRayTracingReflections = false;
        settings.hardwareRayTracingShadows = true;
        auto light = fixture(Type::Point, false);
        const auto dispatchStart = renderer.hardwareRayTracingDispatchCount();
        (void)render();
        if (!renderer.hardwareRayTracingSupported()) {
            require(renderer.hardwareRayTracingDispatchCount() == dispatchStart && !renderer.hardwareRayTracingStatus().empty(),
                "Unavailable hardware must report its reason without pretending to dispatch.");
            report["hardwareExecution"] = {{"status","skipped"},{"reason",renderer.hardwareRayTracingStatus()}};
            std::cout << "SKIP: lighting hardware execution: " << renderer.hardwareRayTracingStatus() << '\n';
        } else {
            require(renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() > dispatchStart,
                "Supported hardware did not execute a real dispatch: " + renderer.hardwareRayTracingStatus());
            for (const auto type : {Type::Directional, Type::Sky, Type::Rect, Type::Spot, Type::Point}) {
                const auto id = fixture(type, false);
                const std::string name = scene::lightTypeName(type);
                const auto mask = receiverMask();
                const auto firstDispatch = renderer.hardwareRayTracingDispatchCount();
                const auto lit = capture(name + "-hardware");
                world.findActor(id)->visible = false;
                const auto dark = capture(name + "-hardware-hidden");
                require(renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() > firstDispatch &&
                    brightness(lit, mask) > 3 && brightness(dark, mask) < 1 && changed(lit, dark, mask) > 30,
                    name + ": actual hardware shading lost the light or retained its hidden contribution.");
                pass(name + " actual DXR light and hidden-state receiver pixels");
            }
            shadowCase(Type::Point, Shadow::AnalyticRay, "Point-hardware-shadow", true);
            light = fixture(Type::Point);
            settings.globalIllumination.method = Gi::PathTracing;
            settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
            settings.globalIllumination.samplesPerFrame = 1;
            settings.globalIllumination.targetSamples = 8;
            settings.globalIllumination.maxBounces = 2;
            settings.globalIllumination.indirectDiffuse = true;
            settings.globalIllumination.indirectSpecular = false;
            renderer.resetGlobalIllumination();
            (void)render(); (void)render();
            const auto before = renderer.globalIlluminationStatus();
            require(before.effective == Gi::PathTracing && before.accumulatedSamples == 2,
                "Hardware GI did not accumulate the explicit-light fixture: " + before.reason);
            const auto beforeDispatch = renderer.hardwareRayTracingDispatchCount();
            world.findActor(light)->light->color[0] = .1f;
            world.findActor(light)->light->intensity *= .25f;
            const auto automaticallyReset = capture("Point-gi-light-edit");
            const auto after = renderer.globalIlluminationStatus();
            require(after.historyRevision > before.historyRevision && after.accumulatedSamples == 1 &&
                renderer.hardwareRayTracingDispatchCount() > beforeDispatch,
                "Light edit did not dispatch fresh GI and reset its accumulated history.");
            renderer.resetGlobalIllumination();
            const auto explicitlyReset = capture("Point-gi-fresh-reference");
            const Mask allPixels(static_cast<std::size_t>(width) * height, true);
            require(changed(automaticallyReset, explicitlyReset, allPixels, 1) == 0,
                "Light-edited GI contains stale radiance instead of matching a fresh first frame.");
            settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Realtime;
            renderer.resetGlobalIllumination();
            (void)render(); (void)render();
            const auto realtimeBefore = renderer.globalIlluminationStatus();
            require(realtimeBefore.effective == Gi::PathTracing &&
                realtimeBefore.effectiveMode == renderer::GlobalIlluminationMode::Realtime &&
                realtimeBefore.accumulatedSamples == 2,
                "Explicit lights did not enter realtime GI: " + realtimeBefore.reason);
            world.findActor(light)->transform.position[0] += .5f;
            const auto realtimeEdit = capture("Point-realtime-gi-light-edit");
            const auto realtimeAfter = renderer.globalIlluminationStatus();
            require(realtimeAfter.historyRevision > realtimeBefore.historyRevision &&
                realtimeAfter.accumulatedSamples == 1,
                "Moving a light did not reset realtime GI history.");
            renderer.resetGlobalIllumination();
            const auto realtimeFresh = capture("Point-realtime-gi-fresh-reference");
            require(changed(realtimeEdit, realtimeFresh, allPixels, 1) == 0,
                "Realtime GI retained lighting from before the light moved.");

            // Actor rectangles have a separate progressive area/BRDF MIS path
            // from the four-point preview and the legacy b5 Cornell emitter.
            // Work on an unobstructed neutral floor and inspect only pixels
            // identified by actual GPU albedo/normals, never emitter pixels.
            {
                settings.globalIllumination.mode = renderer::GlobalIlluminationMode::Progressive;
                settings.globalIllumination.samplesPerFrame = 16;
                settings.globalIllumination.targetSamples = 64;
                settings.globalIllumination.maxBounces = 1;
                settings.shadowMethod = Shadow::AnalyticRay;
                const auto rect = fixture(Type::Rect, false);
                world.findActor(rect)->light->intensity = 2;
                world.findActor(rect)->light->range = 1000;
                const auto floorMask = receiverMask();
                const auto areaDispatchStart = renderer.hardwareRayTracingDispatchCount();
                const auto areaCapture = [&](const std::string& name) {
                    const auto initialDispatch = renderer.hardwareRayTracingDispatchCount();
                    Pixels pixels;
                    for (int frame = 0; frame < 5; ++frame) {
                        pixels = render();
                        const auto status = renderer.globalIlluminationStatus();
                        require(renderer.hardwareRayTracingActive() && status.effective == Gi::PathTracing &&
                            status.effectiveMode == renderer::GlobalIlluminationMode::Progressive,
                            "Actor rectangle progressive transport is inactive: " + status.reason);
                        if (status.accumulatedSamples == 64) {
                            require(renderer.hardwareRayTracingDispatchCount() > initialDispatch,
                                "Actor rectangle capture did not execute GPU transport.");
                            savePixels(output / (name + ".bmp"), pixels);
                            return pixels;
                        }
                    }
                    throw std::runtime_error("Lighting: Actor rectangle did not reach its bounded sample budget.");
                };
                // Invert the existing gamma + ACES display transform before
                // testing addition. Adding displayed 8-bit colors would be an
                // invalid test of independent radiance contributions.
                const auto linearMean = [&](const Pixels& pixels, const Mask& mask) {
                    std::array<double, 3> result{};
                    const auto samples = count(mask);
                    require(samples > 100, "Actor rectangle requires a substantial visible floor receiver.");
                    for (std::size_t i = 0; i < mask.size(); ++i) {
                        if (!mask[i]) continue;
                        for (unsigned channel = 0; channel < 3; ++channel) {
                            const auto encoded = pixels[i * 4 + channel];
                            require(encoded < 245, "Actor rectangle receiver saturated; linear comparison would be unreliable.");
                            const double mapped = std::pow(double(encoded) / 255.0, 2.2);
                            const double a = 2.51 - 2.43 * mapped;
                            const double b = .03 - .59 * mapped;
                            result[channel] += (-b + std::sqrt(b * b + .56 * mapped * a)) / (2 * a);
                        }
                    }
                    for (auto& value : result) value /= static_cast<double>(samples);
                    return result;
                };
                const auto relativeDifference = [](double a, double b) {
                    return std::abs(a - b) / std::max(std::max(a, b), .0001);
                };
                const auto single = areaCapture("Rect-progressive-single");
                const auto singleMean = linearMean(single, floorMask);
                report["metrics"]["actorRectProgressive"]["singleLinearMean"] = singleMean;
                report["metrics"]["actorRectProgressive"]["receiverPixels"] = count(floorMask);
                require(brightness(single, floorMask) > 5, "Actor rectangle MIS does not illuminate the actual floor.");
                world.findActor(rect)->visible = false;
                const auto hidden = areaCapture("Rect-progressive-hidden");
                require(brightness(hidden, floorMask) < 1 && changed(single, hidden, floorMask) > 30,
                    "Hidden Actor rectangle remains in progressive transport or restores legacy lighting.");
                world.findActor(rect)->visible = true;
                world.findActor(rect)->transform.rotation[0] = 90;
                const auto back = areaCapture("Rect-progressive-back-face");
                require(brightness(back, floorMask) < 1, "Progressive Actor rectangle emits from its back face.");
                world.findActor(rect)->transform.rotation[0] = -90;

                // An equivalent, already validated Cornell b5 emitter is an
                // independent control for accidental b7 four-point + MIS double
                // counting. Large range makes the Actor cutoff negligible.
                const auto authoredWorld = world;
                world.removeActor(rect);
                world.environment.overrideLighting = true;
                world.environment.directionalIntensity = 0;
                world.environment.environmentIntensity = 0;
                auto& legacyArea = world.environment.areaLight;
                legacyArea.enabled = true;
                legacyArea.halfAxisU[0] = legacyArea.halfAxisV[2] = 1;
                std::fill(std::begin(legacyArea.radiance), std::end(legacyArea.radiance), 2.f);
                const auto legacy = areaCapture("Rect-progressive-equivalent-legacy");
                const auto legacyMean = linearMean(legacy, floorMask);
                report["metrics"]["actorRectProgressive"]["legacyLinearMean"] = legacyMean;
                for (unsigned channel = 0; channel < 3; ++channel)
                    require(relativeDifference(singleMean[channel], legacyMean[channel]) < .08,
                        "Actor rectangle radiance disagrees with equivalent legacy MIS; possible missing or duplicate estimator.");
                world = authoredWorld;
                const auto blocker = world.createActor("Area MIS blocker", 3, blockerMaterial.id);
                auto& block = world.findActor(blocker)->transform;
                block.position[1] = 1.5f;
                block.scale[0] = block.scale[2] = .7f;
                block.scale[1] = .08f;
                const auto visibleFloor = intersection(floorMask, receiverMask());
                const auto blocked = areaCapture("Rect-progressive-shadow");
                world.findActor(rect)->light->castShadows = false;
                const auto castOff = areaCapture("Rect-progressive-cast-off");
                require(changed(blocked, castOff, visibleFloor) > 20 &&
                    brightness(castOff, visibleFloor) > brightness(blocked, visibleFloor) + 1,
                    "Actor rectangle MIS ignores Cast shadows or has no visible receiver shadow.");
                const auto blockedMean = linearMean(blocked, visibleFloor);
                const auto castOffMean = linearMean(castOff, visibleFloor);
                world.removeActor(blocker);
                world.findActor(rect)->light->castShadows = true;

                // Two separated, spectrally independent emitters expose both a
                // missing light-selection factor and incorrect MIS double count.
                auto* redRect = world.findActor(rect);
                redRect->transform.position[0] = -1.1f;
                redRect->light->width = redRect->light->height = 1;
                redRect->light->intensity = 1;
                redRect->light->color[1] = redRect->light->color[2] = 0;
                const auto blueRect = world.createLight("Blue area MIS", Type::Rect);
                auto* blueActor = world.findActor(blueRect);
                blueActor->transform.position[0] = 1.1f;
                blueActor->light->width = blueActor->light->height = 1;
                blueActor->light->range = 1000;
                blueActor->light->intensity = 1;
                blueActor->light->color[0] = blueActor->light->color[1] = 0;
                blueActor->visible = false;
                const auto redOnly = areaCapture("Rect-progressive-red-only");
                world.findActor(rect)->visible = false;
                world.findActor(blueRect)->visible = true;
                const auto blueOnly = areaCapture("Rect-progressive-blue-only");
                world.findActor(rect)->visible = true;
                const auto both = areaCapture("Rect-progressive-red-blue");
                const auto redMean = linearMean(redOnly, floorMask);
                const auto blueMean = linearMean(blueOnly, floorMask);
                const auto bothMean = linearMean(both, floorMask);
                report["metrics"]["actorRectProgressive"]["redLinearMean"] = redMean;
                report["metrics"]["actorRectProgressive"]["blueLinearMean"] = blueMean;
                report["metrics"]["actorRectProgressive"]["bothLinearMean"] = bothMean;
                require(redMean[0] > .001 && redMean[1] < .0001 && redMean[2] < .0001 &&
                    blueMean[2] > .001 && blueMean[0] < .0001 && blueMean[1] < .0001,
                    "Independent rectangle colors do not reach the receiver without channel leakage.");
                require(relativeDifference(bothMean[0], redMean[0]) < .1 &&
                    relativeDifference(bothMean[2], blueMean[2]) < .1 && bothMean[1] < .0001,
                    "Combined rectangle transport loses or duplicates an independent light contribution.");
                require(changed(both, redOnly, floorMask) > 30 && changed(both, blueOnly, floorMask) > 30,
                    "Both rectangle colors must visibly contribute on the same floor receiver.");
                report["metrics"]["actorRectProgressive"] = {{"receiverPixels",count(floorMask)},
                    {"shadowReceiverPixels",count(visibleFloor)},{"singleLinearMean",singleMean},
                    {"legacyLinearMean",legacyMean},{"blockedLinearMean",blockedMean},{"castOffLinearMean",castOffMean},
                    {"redLinearMean",redMean},{"blueLinearMean",blueMean},{"bothLinearMean",bothMean},
                    {"dispatches",renderer.hardwareRayTracingDispatchCount()-areaDispatchStart},{"samplesPerImage",64}};
                pass("Actor Rect progressive MIS: floor radiance, hidden/back-face, shadow switch, legacy estimator agreement and independent red/blue multi-light addition");
            }
            report["hardwareExecution"] = {{"status","passed"},
                {"dispatches",renderer.hardwareRayTracingDispatchCount()-dispatchStart},
                {"historyBefore",before.historyRevision},{"historyAfter",after.historyRevision}};
            pass("Real DXR dispatch, local-light shadow, progressive/realtime GI history reset and fresh GPU image agreement");
        }
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "GPU validation reported an error.");
        restore.restore();
        report["passed"] = true;
        saveReport();
    } catch (const std::exception& failure) {
        report["error"] = failure.what();
        saveReport();
        throw;
    }
}
