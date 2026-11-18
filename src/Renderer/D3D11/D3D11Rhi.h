#pragma once

#include <RestArtEngine/Renderer/Rhi.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11ShaderResourceView;
struct ID3D11VertexShader;
struct ID3D11PixelShader;
struct ID3D11InputLayout;

namespace rae::renderer::d3d11 {

[[nodiscard]] std::shared_ptr<rhi::Device> createRhiDevice(ID3D11Device*, ID3D11DeviceContext*);
[[nodiscard]] rhi::TextureHandle importTexture(rhi::Device&, ID3D11Texture2D*, std::string name);

// Borrowed native views for explicitly unmigrated D3D11 interoperation only.
[[nodiscard]] ID3D11Buffer* nativeBuffer(const rhi::BufferHandle&);
[[nodiscard]] ID3D11Texture2D* nativeTexture(const rhi::TextureHandle&);
[[nodiscard]] ID3D11RenderTargetView* nativeColorTarget(const rhi::TextureHandle&);
[[nodiscard]] ID3D11DepthStencilView* nativeDepthTarget(const rhi::TextureHandle&, std::uint32_t layer = 0);
[[nodiscard]] ID3D11ShaderResourceView* nativeShaderResource(const rhi::TextureHandle&);
[[nodiscard]] ID3D11VertexShader* nativeVertexShader(const rhi::ShaderHandle&);
[[nodiscard]] ID3D11PixelShader* nativePixelShader(const rhi::ShaderHandle&);
[[nodiscard]] ID3D11InputLayout* nativeInputLayout(const rhi::GraphicsPipelineHandle&);

} // namespace rae::renderer::d3d11
