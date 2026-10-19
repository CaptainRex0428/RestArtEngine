#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct HWND__;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace rae::scene { class World; }
namespace rae::assets { class AssetLibrary; }

namespace rae::renderer {

enum class RenderPath { Forward, Deferred };

// Direct-light shadowing strategy. Raster variants share the depth resource;
// GTAO and ContactShadow additionally use the Deferred G-buffer and fall back
// to the raster map in Forward rendering. AnalyticRay traces built-in
// primitives plus the common mesh-triangle scene.
enum class ShadowMethod {
    ShadowMap,
    AnalyticRay,
    Disabled,
    ShadowMapHard,
    ShadowMapPCF9,
    ShadowMapPCSS,
    ShadowMapCSM2,
    GTAO,
    ContactShadow,
};

struct SceneSettings {
    RenderPath path = RenderPath::Deferred;
    ShadowMethod shadowMethod = ShadowMethod::ShadowMap;
    // Analytic GPU ray tracing for every visible MeshAsset (exact built-in
    // intersections plus structured mesh triangles), not hardware DXR.
    bool rayTracing = false;
    float roughness = 0.25f;
    float metallic = 0.65f;
    float baseColor[3] = {0.85f, 0.28f, 0.08f};
    float lightIntensity = 5.0f;
    float cameraYaw = 0.65f;
    float cameraPitch = 0.3f;
    float cameraDistance = 6.0f;
    // Orbit focus point. Yaw/pitch/distance are relative to this target, so
    // middle-mouse panning translates it and the camera follows.
    float cameraTarget[3] = {0.0f, 1.0f, 0.0f};
    // Orbit pitch limits. Negative pitch lets the camera look at the scene from
    // below; the pole margin keeps the look-at up vector from degenerating.
    static constexpr float minPitch = -1.55f;
    static constexpr float maxPitch = 1.55f;
    float exposure = 1.0f;
    // Lit=0, Unlit=1, Albedo=2, World normals=3, World position=4.
    int debugView = 0;
    int viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
};

// Native D3D11 preview renderer. The broader RHI remains separate until its
// resource/command interfaces are implemented by the other graphics APIs.
class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // Two-stage startup so the caller can present a loading screen between the
    // fast device creation and the slower shader compile. `initialize` performs
    // both stages and keeps the historical one-call behaviour.
    bool initializeDevice(HWND__* window, std::uint32_t width, std::uint32_t height,
                          std::string& error);
    bool initializePipeline(std::string& error);
    bool initialize(HWND__* window, std::uint32_t width, std::uint32_t height,
                    std::string& error);
    bool resize(std::uint32_t width, std::uint32_t height, std::string& error);
    bool render(const SceneSettings& settings, std::string& error);
    bool render(const SceneSettings& settings, const scene::World& world,
                const assets::AssetLibrary& assets, std::string& error);
    // Clears the swap-chain target without drawing a scene. Used by the
    // project manager, which only needs the GPU surface for its UI.
    bool clear(const float color[4], std::string& error);
    void setShaderRoot(std::filesystem::path root);
    // Engine pipeline source and project material source have separate ownership.
    void setMaterialShaderPath(std::filesystem::path path);
    std::filesystem::path materialShaderPath() const;
    bool applyShaderSource(const std::string& materialSource, std::string& diagnostics);
    std::string shaderSource() const;
    std::uint64_t shaderRevision() const;
    bool reloadShaders(std::string& diagnostics);
    bool saveShaderSource(std::string& error);
    bool rayTracingSupported(const scene::World& world, const assets::AssetLibrary& assets) const;
    std::string rayTracingStatus(const scene::World& world, const assets::AssetLibrary& assets) const;
    bool present(bool vsync, std::string& error);
    void shutdown();
    ID3D11Device* device() const;
    ID3D11DeviceContext* context() const;
    std::string adapterName() const;
    bool validationEnabled() const;
    std::uint32_t validationErrorCount() const;
    // Captures the current backbuffer before Present, including any UI drawn.
    bool saveScreenshot(const wchar_t* path, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rae::renderer
