#include <RestArtEngine/Renderer/SelectionOutline.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace rae::renderer {
namespace {

// Mirrors `OutlineConstants : register(b4)` in Scene.hlsl. Colour is authored by
// the editor (theme accent or a custom pick); params carry the ring radius and
// the mask texel size so the composite pass stays resolution independent.
struct alignas(16) OutlineConstants {
    float color[4];
    float params[4];
};
static_assert(sizeof(OutlineConstants) == 32, "OutlineConstants must stay 32 bytes to match Scene.hlsl.");

bool failed(HRESULT result, const char* operation, std::string& error) {
    if (SUCCEEDED(result)) return false;
    char code[16]{};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    error = std::string(operation) + " failed (HRESULT 0x" + code + ").";
    return true;
}

} // namespace

struct SelectionOutlinePass::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> mask;
    ComPtr<ID3D11RenderTargetView> maskTarget;
    ComPtr<ID3D11ShaderResourceView> maskView;
    ComPtr<ID3D11Buffer> constants;
    // Two depth states split the selected geometry into visible (R) and occluded
    // (G) fragments; the blend states only write the channel each pass owns.
    ComPtr<ID3D11DepthStencilState> visibleDepth;
    ComPtr<ID3D11DepthStencilState> hiddenDepth;
    ComPtr<ID3D11DepthStencilState> noDepth;
    ComPtr<ID3D11BlendState> writeVisible;
    ComPtr<ID3D11BlendState> writeHidden;
    // The composite paints the ring over the scene, so transparent pixels must
    // not overwrite it: alpha blending is required for both the solid and the
    // dashed parts.
    ComPtr<ID3D11BlendState> compositeBlend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11SamplerState> sampler;
    std::uint32_t width = 0, height = 0;
};

SelectionOutlinePass::SelectionOutlinePass() : impl_(std::make_unique<Impl>()) {}
SelectionOutlinePass::~SelectionOutlinePass() { shutdown(); }

bool SelectionOutlinePass::initialize(ID3D11Device* device, std::string& error) {
    shutdown();
    error.clear();
    if (!device) { error = "Selection outline requires a device."; return false; }
    auto& state = *impl_;
    state.device = device;

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(OutlineConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (failed(device->CreateBuffer(&buffer, nullptr, &state.constants), "Create outline constants", error)) return false;

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    depth.StencilEnable = FALSE;
    if (failed(device->CreateDepthStencilState(&depth, &state.visibleDepth), "Create outline visible depth state", error)) return false;
    depth.DepthFunc = D3D11_COMPARISON_GREATER;
    if (failed(device->CreateDepthStencilState(&depth, &state.hiddenDepth), "Create outline hidden depth state", error)) return false;
    depth.DepthEnable = FALSE;
    if (failed(device->CreateDepthStencilState(&depth, &state.noDepth), "Create outline composite depth state", error)) return false;

    const auto createWriteMask = [&](UINT mask, ComPtr<ID3D11BlendState>& result) {
        D3D11_BLEND_DESC description{};
        description.RenderTarget[0].BlendEnable = FALSE;
        description.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(mask);
        return !failed(device->CreateBlendState(&description, &result), "Create outline mask write state", error);
    };
    if (!createWriteMask(D3D11_COLOR_WRITE_ENABLE_RED, state.writeVisible)) return false;
    if (!createWriteMask(D3D11_COLOR_WRITE_ENABLE_GREEN, state.writeHidden)) return false;
    D3D11_BLEND_DESC compositeDescription{};
    compositeDescription.RenderTarget[0].BlendEnable = TRUE;
    compositeDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    compositeDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    compositeDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    compositeDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    compositeDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    compositeDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    compositeDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (failed(device->CreateBlendState(&compositeDescription, &state.compositeBlend), "Create outline composite blend state", error)) return false;

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (failed(device->CreateRasterizerState(&raster, &state.raster), "Create outline rasterizer", error)) return false;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (failed(device->CreateSamplerState(&sampler, &state.sampler), "Create outline sampler", error)) return false;
    return true;
}

bool SelectionOutlinePass::prepare(std::uint32_t width, std::uint32_t height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.device) { error = "Selection outline is not initialized."; return false; }
    if (width == 0 || height == 0) { error = "Selection outline needs a nonzero size."; return false; }
    if (state.mask && state.width == width && state.height == height) return true;

    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (failed(state.device->CreateTexture2D(&description, nullptr, &texture), "Create outline mask", error)) return false;
    ComPtr<ID3D11RenderTargetView> target;
    if (failed(state.device->CreateRenderTargetView(texture.Get(), nullptr, &target), "Create outline mask target", error)) return false;
    ComPtr<ID3D11ShaderResourceView> view;
    if (failed(state.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "Create outline mask view", error)) return false;
    state.mask = std::move(texture);
    state.maskTarget = std::move(target);
    state.maskView = std::move(view);
    state.width = width;
    state.height = height;
    return true;
}

void SelectionOutlinePass::shutdown() {
    auto& state = *impl_;
    state.mask.Reset();
    state.maskTarget.Reset();
    state.maskView.Reset();
    state.constants.Reset();
    state.visibleDepth.Reset();
    state.hiddenDepth.Reset();
    state.noDepth.Reset();
    state.writeVisible.Reset();
    state.writeHidden.Reset();
    state.compositeBlend.Reset();
    state.raster.Reset();
    state.sampler.Reset();
    state.device.Reset();
    state.width = state.height = 0;
}

bool SelectionOutlinePass::drawMasks(ID3D11DeviceContext* context, ID3D11DepthStencilView* sceneDepth,
                                     const D3D11_VIEWPORT& viewport,
                                     ID3D11VertexShader* geometryShader, ID3D11InputLayout* inputLayout,
                                     ID3D11PixelShader* maskShader,
                                     ID3D11Buffer* sceneConstants, ID3D11Buffer* objectConstants,
                                     const std::function<bool()>& drawSelected, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!context || !state.maskTarget || !sceneDepth) { error = "Selection outline mask is not ready."; return false; }
    // The composite pass binds the mask and depth as SRVs; release both slots
    // before either resource becomes a render target / depth target again.
    ID3D11ShaderResourceView* unusedSlots[2] = {nullptr, nullptr};
    context->PSSetShaderResources(5, 2, unusedSlots);
    context->OMSetRenderTargets(1, state.maskTarget.GetAddressOf(), sceneDepth);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(state.maskTarget.Get(), zero);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(state.raster.Get());
    context->IASetInputLayout(inputLayout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(geometryShader, nullptr, 0);
    context->PSSetShader(maskShader, nullptr, 0);
    ID3D11Buffer* buffers[] = {sceneConstants, objectConstants};
    context->VSSetConstantBuffers(0, 2, buffers);
    context->PSSetConstantBuffers(0, 2, buffers);
    // Visible fragments: same depth as the scene -> R channel.
    context->OMSetDepthStencilState(state.visibleDepth.Get(), 0);
    context->OMSetBlendState(state.writeVisible.Get(), nullptr, 0xffffffff);
    if (!drawSelected()) return false;
    // Occluded fragments: behind the scene -> G channel, drawn as a dashed hint.
    context->OMSetDepthStencilState(state.hiddenDepth.Get(), 0);
    context->OMSetBlendState(state.writeHidden.Get(), nullptr, 0xffffffff);
    if (!drawSelected()) return false;
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void SelectionOutlinePass::composite(ID3D11DeviceContext* context, ID3D11RenderTargetView* sceneColor,
                                     ID3D11ShaderResourceView* sceneDepth, const D3D11_VIEWPORT& viewport,
                                     ID3D11VertexShader* fullscreenShader, ID3D11PixelShader* compositeShader,
                                     const float color[4], float thickness, bool outlineOccluders) {
    auto& state = *impl_;
    if (!context || !sceneColor || !state.maskView || !sceneDepth) return;
    OutlineConstants data{};
    std::memcpy(data.color, color, sizeof(data.color));
    data.params[0] = thickness;
    data.params[1] = state.width ? 1.0f / static_cast<float>(state.width) : 0.0f;
    data.params[2] = state.height ? 1.0f / static_cast<float>(state.height) : 0.0f;
    // 1 = symmetric depth test (also outline front occluders).
    data.params[3] = outlineOccluders ? 1.0f : 0.0f;
    context->UpdateSubresource(state.constants.Get(), 0, nullptr, &data, 0, 0);
    context->OMSetRenderTargets(1, &sceneColor, nullptr);
    context->RSSetViewports(1, &viewport);
    context->RSSetState(state.raster.Get());
    context->IASetInputLayout(nullptr);
    context->VSSetShader(fullscreenShader, nullptr, 0);
    context->PSSetShader(compositeShader, nullptr, 0);
    context->OMSetDepthStencilState(state.noDepth.Get(), 0);
    context->OMSetBlendState(state.compositeBlend.Get(), nullptr, 0xffffffff);
    ID3D11Buffer* constants[] = {state.constants.Get()};
    context->PSSetConstantBuffers(4, 1, constants);
    ID3D11ShaderResourceView* views[] = {state.maskView.Get(), sceneDepth};
    context->PSSetShaderResources(5, 2, views);
    ID3D11SamplerState* sampler = state.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* unused[2] = {nullptr, nullptr};
    context->PSSetShaderResources(5, 2, unused);
    context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
}

} // namespace rae::renderer
