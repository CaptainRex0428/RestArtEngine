#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct ID3D11BlendState;
struct ID3D11Buffer;
struct ID3D11DepthStencilState;
struct ID3D11DepthStencilView;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11InputLayout;
struct ID3D11PixelShader;
struct ID3D11RasterizerState;
struct ID3D11RenderTargetView;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;
struct D3D11_VIEWPORT;

namespace rae::renderer {

// Editor-only screen-space selection outline.
//
// The pass is deliberately outside the runtime scene contract: nothing here is
// referenced by the no-world `SceneRenderer::render(settings, error)` path, and
// the mask target is only created when the editor supplies a non-empty
// selection. Ownership stays in the renderer because the outline needs the
// scene depth buffer, the mesh cache and the per-actor constants; the editor
// only decides *which* actors are highlighted and with which colour.
//
// Frame order:
//   1. scene geometry writes colour + depth (forward or deferred path),
//   2. drawMasks() renders the selected silhouettes twice into one RGBA mask:
//      R = fragments that pass the scene depth, G = fragments hidden behind it,
//   3. composite() turns R into a solid ring and G into a dashed hint, using the
//      depth buffer to ignore interior details that share one surface.
class SelectionOutlinePass {
public:
    SelectionOutlinePass();
    ~SelectionOutlinePass();
    SelectionOutlinePass(const SelectionOutlinePass&) = delete;
    SelectionOutlinePass& operator=(const SelectionOutlinePass&) = delete;

    // Creates the constant buffer and the depth/blend/raster/sampler states.
    bool initialize(ID3D11Device* device, std::string& error);
    // Creates or resizes the mask target to match the scene texture. Cheap when
    // the size did not change.
    bool prepare(std::uint32_t width, std::uint32_t height, std::string& error);
    void shutdown();

    // Binds the mask target together with the scene depth and issues the
    // selected draws twice (visible and occluded). Returns false with `error`
    // filled when a mesh upload fails.
    bool drawMasks(ID3D11DeviceContext* context, ID3D11DepthStencilView* sceneDepth,
                   const D3D11_VIEWPORT& viewport,
                   ID3D11VertexShader* geometryShader, ID3D11InputLayout* inputLayout,
                   ID3D11PixelShader* maskShader,
                   ID3D11Buffer* sceneConstants, ID3D11Buffer* objectConstants,
                   const std::function<bool()>& drawSelected, std::string& error);

    // Turns the mask into the outline. `thickness` is the ring radius in pixels
    // of the scene texture.
    void composite(ID3D11DeviceContext* context, ID3D11RenderTargetView* sceneColor,
                   ID3D11ShaderResourceView* sceneDepth, const D3D11_VIEWPORT& viewport,
                   ID3D11VertexShader* fullscreenShader, ID3D11PixelShader* compositeShader,
                   const float color[4], float thickness, bool solidIntersection = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rae::renderer
