#pragma once

#include <RestArtEngine/Renderer/GlobalIllumination.h>
#include <RestArtEngine/Renderer/Lighting.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;
namespace rae::scene { class World; }
namespace rae::assets { class AssetLibrary; struct MaterialAsset; }

namespace rae::renderer::d3d12 {

// Internal DX11/DX12 bridge. Native resource ownership stays in this module;
// neither project data nor scene/assets depend on the bridge or on DirectX.
struct Frame {
    std::array<float, 44> sceneConstants{};
    std::array<float, 16> inverseViewProjection{};
    // Focus metres, aperture radius metres, enabled, reserved. Only progressive
    // path sampling consumes this lens; realtime DOF is a display operation.
    std::array<float, 4> thinLens{};
    LightingConstants lighting;
    // Centre/enabled, half-axis U/environment scale, half-axis V/reserved,
    // emitted radiance/reserved. Kept outside the locked raster b0 contract.
    std::array<float, 16> areaLight{0,0,0,0, 0,0,0,1, 0,0,0,0, 0,0,0,0};
    std::array<float, 4> probeGrid{}; // X, Y, Z, hysteresis
    std::array<float, 4> probeOrigin{}; // x, y, z, spacing
    std::uint32_t probeRayCount = 16;
    std::uint32_t width = 0, height = 0;
    bool reflections = true, shadows = true;
    bool probeUpdate = false;
    // Reflection-only frame used by raster SSGI; the resolved texture is
    // exposed through nativeIndirect()/indirectLighting().
    bool reflectionOnly = false;
    GlobalIlluminationSettings globalIllumination;
    // Changed by the owning scene renderer for explicit restart or re-enable.
    std::uint64_t historyEpoch = 0;
    std::uint64_t viewId = 1;
};

class HardwareRayTracing {
public:
    HardwareRayTracing();
    ~HardwareRayTracing();
    HardwareRayTracing(const HardwareRayTracing&) = delete;
    HardwareRayTracing& operator=(const HardwareRayTracing&) = delete;

    bool initialize(ID3D11Device*, ID3D11DeviceContext*, std::string& error);
    // Native D3D12 mode: use an existing raster device/queue and expose D3D12
    // resources instead of DX11 shared textures. No D3D11 device is created.
    bool initializeNative(ID3D12Device*, ID3D12CommandQueue*, std::string& error);
    [[nodiscard]] bool nativeMode() const;
    // Valid after the first successful native render. Resources are owned by
    // this module and remain alive until resize/shutdown.
    [[nodiscard]] ID3D12Resource* nativeOutput() const;
    [[nodiscard]] ID3D12Resource* nativeIndirect() const;
    // Ray-traced DDGI irradiance atlas, RGBA16F 1024x32. Native mode exposes
    // the D3D12 resource; hybrid mode exposes the shared DX11 texture.
    [[nodiscard]] ID3D12Resource* nativeProbeAtlas() const;
    [[nodiscard]] ID3D11Texture2D* probeAtlas() const;
    [[nodiscard]] ID3D12Resource* nativeProbeDepth() const;
    [[nodiscard]] ID3D11Texture2D* probeDepth() const;
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> probeAtlasSize() const;
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> nativeOutputSize() const;
    [[nodiscard]] bool supported() const;
    // Builds, reflects and creates a complete candidate before publishing it.
    // The exact in-memory scene/material sources participate in this compile.
    bool preparePipeline(const std::filesystem::path& shaderRoot,
                         const std::filesystem::path& materialPath,
                         const std::string& sceneSource,
                         const std::string& materialSource, std::string& error);
    bool render(const Frame&, const scene::World&, const assets::AssetLibrary&,
                const assets::MaterialAsset* materialOverride, std::string& error);
    // Borrowed until the next render/resize or shutdown. A successful render
    // queues a DX11 wait, so subsequent copies on the borrowed context are safe.
    [[nodiscard]] ID3D11Texture2D* output() const;
    // Linear indirect lighting for the raster scene. Null unless the latest
    // successful render produced a lit realtime GI frame. The caller must
    // unbind its SRV before the next render; the bridge orders GPU ownership.
    [[nodiscard]] ID3D11ShaderResourceView* indirectLighting() const;
    [[nodiscard]] std::string status() const;
    [[nodiscard]] std::vector<std::string> debugMessages() const;
    [[nodiscard]] std::uint32_t validationErrorCount() const;
    [[nodiscard]] std::uint64_t dispatchCount() const;
    [[nodiscard]] GlobalIlluminationStatus globalIlluminationStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rae::renderer::d3d12
