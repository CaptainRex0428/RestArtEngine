#pragma once

#include <RestArtEngine/Core/GarbageCollector.h>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct HWND__;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace rae::scene { class World; }
namespace rae::assets { class AssetLibrary; struct MaterialAsset; }

namespace rae::renderer {

// Logical cache allocations, not driver residency or process working set.
struct RendererCacheStats {
    std::uint64_t meshCount = 0;
    std::uint64_t meshGpuBytesEstimate = 0;
    std::uint64_t rayTriangleCount = 0;
    std::uint64_t rayGpuBytesEstimate = 0;
};

enum class RenderPath { Forward, Deferred };

// Each tool owns its preview image; rendering another slot never replaces it.
enum class PreviewSlot { MaterialEditor, AssetDetails, SceneThumbnail };

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
    // Editor panels composite the scene texture through their own window draw
    // list. An inactive dock tab must not fall back to fullscreen rendering.
    bool editorViewport = false;
    // Render into the offscreen scene target while keeping editor-only branches
    // (outline, viewport rect tracking) disabled. Used by tool previews.
    bool offscreenTarget = false;
    bool viewportVisible = true;
    // Editor-only selection outline. `outlineActors` is borrowed for the render
    // call only; runtime callers leave it null, so the outline pass is never
    // created outside the editor.
    const std::uint64_t* outlineActors = nullptr;
    std::uint32_t outlineActorCount = 0;
    // rgb + alpha, normally the active theme accent.
    float outlineColor[4] = {0.992f, 0.471f, 0.016f, 1.0f};
    // Ring radius in scene-texture pixels.
    float outlineThickness = 2.0f;
    // Outline geometry in front of the selection too (symmetric depth test).
    bool outlineFrontObjects = false;

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
    // Binds the renderer to another window, rebuilding only the swap chain and
    // window-sized targets. The device, shaders and GPU caches are reused.
    bool attachWindow(HWND__* window, std::uint32_t width, std::uint32_t height, std::string& error);
    // Releases the swap chain and window-sized targets while the HWND is alive.
    void detachWindow();
    bool render(const SceneSettings& settings, std::string& error);
    // Renders a single lit sphere with the given material parameters into an
    // offscreen texture and returns it for UI display. The main scene targets are
    // reused and then overwritten by the frame's regular render call.
    bool renderMaterialPreview(const assets::MaterialAsset& material, std::uint32_t size, std::string& error,
                               PreviewSlot slot = PreviewSlot::MaterialEditor);
    // Same preview target with an explicit orbit camera for the material ball.
    bool renderMaterialPreview(const assets::MaterialAsset& material, std::uint32_t size,
                               float yaw, float pitch, float distance, std::string& error,
                               PreviewSlot slot = PreviewSlot::MaterialEditor);
    // Renders the given scene into the preview target with a camera framed to the
    // visible actors; used to produce scene thumbnails.
    bool renderScenePreview(const scene::World& world, const assets::AssetLibrary& assets,
                            std::uint32_t size, std::string& error,
                            PreviewSlot slot = PreviewSlot::SceneThumbnail);
    // Borrowed SRV until a successful resize of the same slot or shutdown.
    // Requested size is bounded to the available scene target dimensions.
    [[nodiscard]] void* previewTexture(PreviewSlot slot) const;
    bool readPreviewPixels(PreviewSlot slot, std::vector<std::uint8_t>& pixels,
                           std::uint32_t& width, std::uint32_t& height, std::string& error) const;
    // Compatibility helpers always address the MaterialEditor slot.
    [[nodiscard]] void* materialPreviewTexture() const;
    // Copies the material editor preview image back to CPU memory (RGBA8).
    bool readMaterialPreviewPixels(std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                                   std::uint32_t& height, std::string& error) const;
    // Optional per-frame material override, matched by asset ID across raster
    // and ray paths. Borrowed only for this call; the asset library is unchanged.
    bool render(const SceneSettings& settings, const scene::World& world,
                const assets::AssetLibrary& assets, std::string& error,
                const assets::MaterialAsset* materialOverride = nullptr);
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
    // Main/render thread only, after UI submission/capture/Present (or between
    // frames while minimized). Retains caches used by the latest scene frame.
    rae::GarbageCollectionMemoryStats collectGarbage(double maxIdleSeconds);
    [[nodiscard]] RendererCacheStats cacheStats() const;
    void shutdown();
    ID3D11Device* device() const;
    ID3D11DeviceContext* context() const;
    // D3D11 SRV borrowed until resize/shutdown. Available before the UI builds
    // its draw list; render() fills it before the UI submits that draw list.
    void* sceneTexture() const;
    std::uint32_t sceneTextureWidth() const;
    std::uint32_t sceneTextureHeight() const;
    std::string adapterName() const;
    bool validationEnabled() const;
    std::uint32_t validationErrorCount() const;
    // Captures the current backbuffer before Present, including any UI drawn.
    bool saveScreenshot(const wchar_t* path, std::string& error);
    // Writes BMP at the current position of a borrowed binary output stream.
    // Caller owns flushing/closing it and checking the final close result.
    bool saveScreenshot(std::FILE* file, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
private:
    bool renderPreviewFrame(const SceneSettings& settings, const scene::World& world,
                            const assets::AssetLibrary& assets, const assets::MaterialAsset* materialOverride,
                            std::uint32_t size, std::string& error, PreviewSlot slot);
};

} // namespace rae::renderer
