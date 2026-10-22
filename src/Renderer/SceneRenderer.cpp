#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "RestArtEngine/Renderer/SceneRenderer.h"
#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Scene/World.h"
#include <d3d11shader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <new>

namespace rae::renderer {
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX;

bool failed(HRESULT hr, const char* operation, std::string& error) {
    if (SUCCEEDED(hr)) return false;
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex
           << static_cast<unsigned long>(hr) << ").";
    error = stream.str();
    return true;
}

struct Vertex { XMFLOAT3 position; XMFLOAT3 normal; };
struct alignas(16) SceneConstants {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 cameraPosition;
    XMFLOAT4 baseColor;
    XMFLOAT4 lightDirectionIntensity;
    XMFLOAT4 parameters;
    XMFLOAT4 viewportDebug;
};
static_assert(sizeof(SceneConstants) == 144);
struct alignas(16) ObjectConstants {
    XMFLOAT4X4 worldMatrix, normalMatrix;
    XMFLOAT4 color, parameters;
};
struct alignas(16) RayObject {
    XMFLOAT4X4 inverseWorld, normalTransform;
    XMFLOAT4 color, properties;
};
struct alignas(16) RayConstants { XMFLOAT4 info; RayObject objects[32]; };
struct alignas(16) RayTriangle {
    XMFLOAT3 position0;
    XMFLOAT3 position1;
    XMFLOAT3 position2;
    XMFLOAT3 normal;
    XMFLOAT4 color;
    XMFLOAT4 properties;
};
struct alignas(16) ShadowConstants {
    XMFLOAT4X4 lightViewProjection;
    XMFLOAT4X4 lightViewProjectionCascade1;
    XMFLOAT4 parameters; // texel size, depth bias, normal bias, filter/cascade mode
    XMFLOAT4 cascadeParameters; // split distance, enabled, reserved, reserved
};
static_assert(sizeof(ObjectConstants) == 160 && sizeof(RayConstants) == 5136 && sizeof(RayTriangle) == 80);
static_assert(sizeof(ShadowConstants) == 160);
XMMATRIX actorMatrix(const scene::Actor& actor) {
    const auto& t = actor.transform;
    return XMMatrixScaling(t.scale[0], t.scale[1], t.scale[2]) *
        XMMatrixRotationRollPitchYaw(XMConvertToRadians(t.rotation[0]), XMConvertToRadians(t.rotation[1]), XMConvertToRadians(t.rotation[2])) *
        XMMatrixTranslation(t.position[0], t.position[1], t.position[2]);
}
bool validTransform(const scene::Actor& actor) {
    const auto& t = actor.transform;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(t.position[i]) || !std::isfinite(t.rotation[i]) || !std::isfinite(t.scale[i]) || std::abs(t.scale[i]) < 0.0001f) return false;
    return true;
}
std::string pathUtf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
bool readText(const std::filesystem::path& path, std::string& result, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot read shader: " + pathUtf8(path); return false; }
    result.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) { error = "Failed reading shader: " + pathUtf8(path); return false; }
    return true;
}

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(result.size() - 1);
    return result;
}

// Own include buffers until D3DCompile closes them; nested includes resolve from
// their actual parent file. The editor overrides only Standard.hlsli in memory.
class ShaderIncludes final : public ID3DInclude {
public:
    std::filesystem::path root, materialPath;
    const std::string& material;
    std::unordered_map<const void*, std::pair<std::filesystem::path, std::unique_ptr<std::string>>> opened;
    ShaderIncludes(std::filesystem::path path, std::filesystem::path projectMaterial, const std::string& source)
        : root(std::move(path)), materialPath(std::move(projectMaterial)), material(source) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID* data, UINT* bytes) override {
        auto base = root / "Private";
        if (const auto it = opened.find(parent); it != opened.end()) base = it->second.first.parent_path();
        auto path = std::filesystem::absolute(base / std::filesystem::path(std::u8string(name, name + std::strlen(name)))).lexically_normal();
        auto source = std::make_unique<std::string>();
        std::string error;
        if (path == std::filesystem::absolute(root / "Materials/Standard.hlsli").lexically_normal() || path == materialPath) {
            path = materialPath;
            *source = material;
        } else if (!readText(path, *source, error)) return E_FAIL;
        if (source->size() >= 3 && static_cast<unsigned char>((*source)[0]) == 0xef &&
            static_cast<unsigned char>((*source)[1]) == 0xbb && static_cast<unsigned char>((*source)[2]) == 0xbf) source->erase(0, 3);
        // Diagnostics name the project file; nested includes use its actual parent.
        *source = "#line 1 \"" + pathUtf8(path) + "\"\n" + *source;
        *data = source->data(); *bytes = static_cast<UINT>(source->size());
        opened.emplace(*data, std::make_pair(path, std::move(source)));
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID data) override { opened.erase(data); return S_OK; }
};

bool validateShader(ID3DBlob* bytecode, const char* entry, std::string& error) {
    ComPtr<ID3D11ShaderReflection> reflection;
    if (failed(D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_PPV_ARGS(&reflection)), "Reflect shader", error)) return false;
    D3D11_SHADER_DESC desc{}; reflection->GetDesc(&desc);
    struct Binding { const char* name; D3D_SHADER_INPUT_TYPE type; UINT slot, size; };
    const Binding bindings[] = {
        {"SceneConstants", D3D_SIT_CBUFFER, 0, sizeof(SceneConstants)},
        {"ObjectConstants", D3D_SIT_CBUFFER, 1, sizeof(ObjectConstants)},
        {"RayConstants", D3D_SIT_CBUFFER, 2, sizeof(RayConstants)},
        {"ShadowConstants", D3D_SIT_CBUFFER, 3, sizeof(ShadowConstants)},
        {"albedoBuffer", D3D_SIT_TEXTURE, 0, 0}, {"normalBuffer", D3D_SIT_TEXTURE, 1, 0}, {"positionBuffer", D3D_SIT_TEXTURE, 2, 0},
        {"shadowMap", D3D_SIT_TEXTURE, 3, 0}, {"gbufferSampler", D3D_SIT_SAMPLER, 0, 0},
        {"shadowSampler", D3D_SIT_SAMPLER, 1, 0},
        {"rayTriangles", D3D_SIT_STRUCTURED, 4, 0}
    };
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{}; reflection->GetResourceBindingDesc(i, &binding);
        const Binding* expected = nullptr;
        for (const auto& candidate : bindings) if (std::strcmp(candidate.name, binding.Name) == 0) expected = &candidate;
        if (!expected || binding.Type != expected->type || binding.BindPoint != expected->slot || binding.BindCount != 1) {
            error = std::string(entry) + ": incompatible resource binding " + binding.Name +
                " (type=" + std::to_string(static_cast<unsigned>(binding.Type)) +
                ", slot=" + std::to_string(binding.BindPoint) +
                ", count=" + std::to_string(binding.BindCount) + ")."; return false;
        }
        if (std::strcmp(binding.Name, "shadowMap") == 0) {
            if ((binding.Dimension != D3D_SRV_DIMENSION_TEXTURE2D && binding.Dimension != D3D_SRV_DIMENSION_TEXTURE2DARRAY) ||
                binding.ReturnType != D3D_RETURN_TYPE_FLOAT) {
                error = std::string(entry) + ": shadowMap requires Texture2D<float> or Texture2DArray<float>."; return false;
            }
        } else if (expected->type == D3D_SIT_TEXTURE &&
                   (binding.Dimension != D3D_SRV_DIMENSION_TEXTURE2D || binding.ReturnType != D3D_RETURN_TYPE_FLOAT)) {
            error = std::string(entry) + ": G-buffer resources require Texture2D<float4>: " + binding.Name; return false;
        }
        if (expected->type == D3D_SIT_STRUCTURED && binding.Dimension != D3D_SRV_DIMENSION_BUFFEREX && binding.Dimension != D3D_SRV_DIMENSION_BUFFER) {
            error = std::string(entry) + ": rayTriangles requires a structured buffer SRV."; return false;
        }
        if (expected->size) {
            D3D11_SHADER_BUFFER_DESC cb{};
            reflection->GetConstantBufferByName(binding.Name)->GetDesc(&cb);
            if (cb.Size != expected->size) { error = std::string(entry) + ": incompatible constant buffer size: " + binding.Name; return false; }
        }
    }
    struct Variable { const char* buffer; const char* name; UINT offset, size; };
    const Variable variables[] = {
        {"SceneConstants","viewProjection",0,64}, {"SceneConstants","cameraPosition",64,16},
        {"SceneConstants","baseColor",80,16}, {"SceneConstants","lightDirectionIntensity",96,16},
        {"SceneConstants","parameters",112,16}, {"SceneConstants","viewportDebug",128,16},
        {"ObjectConstants","worldMatrix",0,64}, {"ObjectConstants","normalMatrix",64,64},
        {"ObjectConstants","objectColor",128,16}, {"ObjectConstants","objectParameters",144,16},
        {"RayConstants","rayInfo",0,16}, {"RayConstants","rayObjects",16,5120},
        {"ShadowConstants","lightViewProjection",0,64},
        {"ShadowConstants","lightViewProjectionCascade1",64,64},
        {"ShadowConstants","shadowParameters",128,16},
        {"ShadowConstants","cascadeParameters",144,16}
    };
    for (const auto& variable : variables) {
        auto* cb = reflection->GetConstantBufferByName(variable.buffer);
        D3D11_SHADER_BUFFER_DESC cbDesc{};
        if (FAILED(cb->GetDesc(&cbDesc))) continue; // A completely unused buffer is optimized away.
        D3D11_SHADER_VARIABLE_DESC actual{};
        if (FAILED(cb->GetVariableByName(variable.name)->GetDesc(&actual)) || actual.StartOffset != variable.offset || actual.Size != variable.size) {
            error = std::string(entry) + ": incompatible constant layout: " + variable.name; return false;
        }
        auto* variableType = cb->GetVariableByName(variable.name)->GetType();
        D3D11_SHADER_TYPE_DESC type{}; variableType->GetDesc(&type);
        if (variable.size <= 64) {
            const bool matrix = variable.size == 64;
            if (type.Type != D3D_SVT_FLOAT || type.Elements != 0 || type.Columns != 4 ||
                type.Rows != (matrix ? 4u : 1u) || type.Class != (matrix ? D3D_SVC_MATRIX_ROWS : D3D_SVC_VECTOR)) {
                error = std::string(entry) + ": incompatible constant type: " + variable.name; return false;
            }
        } else {
            if (type.Class != D3D_SVC_STRUCT || type.Elements != 32 || type.Members != 4) {
                error = std::string(entry) + ": RayObject requires the 32-element renderer structure."; return false;
            }
            const char* members[] = {"inverseWorld", "normalTransform", "color", "properties"};
            const UINT offsets[] = {0, 64, 128, 144};
            for (UINT i = 0; i < 4; ++i) {
                D3D11_SHADER_TYPE_DESC member{};
                if (FAILED(variableType->GetMemberTypeByName(members[i])->GetDesc(&member)) || member.Offset != offsets[i] ||
                    member.Type != D3D_SVT_FLOAT || member.Columns != 4 || member.Rows != (i < 2 ? 4u : 1u) ||
                    member.Class != (i < 2 ? D3D_SVC_MATRIX_ROWS : D3D_SVC_VECTOR)) {
                    error = std::string(entry) + ": incompatible RayObject member: " + members[i]; return false;
                }
            }
        }
    }
    if (std::strcmp(entry, "VSMain") == 0) {
        if (desc.InputParameters != 2) { error = "VSMain requires POSITION and NORMAL float3 inputs."; return false; }
        for (UINT i = 0; i < 2; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC input{}; reflection->GetInputParameterDesc(i, &input);
            if ((std::strcmp(input.SemanticName, "POSITION") != 0 && std::strcmp(input.SemanticName, "NORMAL") != 0) ||
                input.SemanticIndex != 0 || input.Mask != 7 || input.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) {
                error = "VSMain input contract changed (POSITION/NORMAL float3 required)."; return false;
            }
        }
    }
    if (entry[0] == 'P' && entry[1] == 'S') {
        const UINT outputs = std::strcmp(entry, "PSGBuffer") == 0 ? 3u : 1u;
        if (desc.OutputParameters != outputs) { error = std::string(entry) + ": incompatible render-target count."; return false; }
        for (UINT i = 0; i < outputs; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC output{}; reflection->GetOutputParameterDesc(i, &output);
            if (output.SystemValueType != D3D_NAME_TARGET || output.SemanticIndex != i || output.Mask != 15 ||
                output.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) {
                error = std::string(entry) + ": output must be float4 SV_TARGET with consecutive indices."; return false;
            }
        }
    }
    return true;
}

bool compile(const std::filesystem::path& root, const std::filesystem::path& materialPath,
             const std::string& sceneSource, const std::string& materialSource,
             const char* entry, const char* target, ComPtr<ID3DBlob>& bytecode, std::string& error) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
    ShaderIncludes includes(root, materialPath, materialSource);
    ComPtr<ID3DBlob> diagnostics;
    const auto sourceName = pathUtf8(root / "Private/Scene.hlsl");
    const HRESULT hr = D3DCompile(sceneSource.data(), sceneSource.size(), sourceName.c_str(), nullptr,
        &includes, entry, target, flags, 0, &bytecode, &diagnostics);
    if (FAILED(hr)) {
        error = std::string("Shader ") + entry + ": ";
        if (diagnostics) error.append(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
        else error += "D3DCompile failed.";
        return false;
    }
    return validateShader(bytecode.Get(), entry, error);
}
} // namespace

struct SceneRenderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11InfoQueue> infoQueue;
    ComPtr<ID3D11RenderTargetView> backbuffer;
    ComPtr<ID3D11RenderTargetView> sceneTarget;
    ComPtr<ID3D11ShaderResourceView> sceneView;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11Texture2D> shadowTexture;
    ComPtr<ID3D11DepthStencilView> shadowDepth;
    std::array<ComPtr<ID3D11DepthStencilView>, 2> shadowDepthSlices;
    ComPtr<ID3D11ShaderResourceView> shadowView;
    std::array<ComPtr<ID3D11RenderTargetView>, 3> gbufferTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> gbufferViews;
    ComPtr<ID3D11VertexShader> geometryVS, fullscreenVS;
    ComPtr<ID3D11PixelShader> forwardPS, gbufferPS, deferredPS, backgroundPS;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constants, objectConstants, rayConstants, shadowConstants;
    ComPtr<ID3D11Buffer> rayTriangleBuffer;
    ComPtr<ID3D11ShaderResourceView> rayTriangleView;
    std::uint64_t rayTriangleHash = 0;
    UINT rayTriangleCount = 0;
    UINT raySceneActorCount = 0;
    struct GpuMesh { ComPtr<ID3D11Buffer> vertices, indices; UINT count = 0; std::uint64_t hash = 0; };
    std::unordered_map<assets::AssetId, GpuMesh> meshes;
    std::filesystem::path shaderRoot = "Shaders";
    std::filesystem::path materialOverride;
    std::filesystem::path materialPath() const {
        return std::filesystem::absolute(materialOverride.empty() ? shaderRoot / "Materials/Standard.hlsli" : materialOverride).lexically_normal();
    }
    std::string sceneSource, materialSource;
    std::uint64_t revision = 0;
    assets::AssetLibrary defaultAssets;
    scene::World defaultWorld;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depthEnabled, depthDisabled;
    ComPtr<ID3D11SamplerState> sampler, shadowSampler;
    static constexpr UINT shadowMapSize = 2048;
    static constexpr UINT shadowCascadeCount = 2;
    std::uint32_t width = 0, height = 0;
    std::string adapter;

    bool createTargets(std::string& error) {
        ComPtr<ID3D11Texture2D> texture;
        if (failed(swapChain->GetBuffer(0, IID_PPV_ARGS(&texture)), "Get backbuffer", error)) return false;
        if (failed(device->CreateRenderTargetView(texture.Get(), nullptr, &backbuffer),
                   "Create backbuffer view", error)) return false;

        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        texture.Reset();
        if (failed(device->CreateTexture2D(&description, nullptr, &texture), "Create depth", error)) return false;
        if (failed(device->CreateDepthStencilView(texture.Get(), nullptr, &depth), "Create depth view", error)) return false;

        // Keep scene color separate from the swap chain so docked and floating
        // editor windows composite it with normal UI clipping and z ordering.
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texture.Reset();
        if (failed(device->CreateTexture2D(&description, nullptr, &texture), "Create scene color", error)) return false;
        if (failed(device->CreateRenderTargetView(texture.Get(), nullptr, &sceneTarget), "Create scene color target", error)) return false;
        if (failed(device->CreateShaderResourceView(texture.Get(), nullptr, &sceneView), "Create scene color SRV", error)) return false;

        // A persistent directional shadow map. Keep this independent from the
        // camera depth buffer so it can be sampled by both forward and deferred
        // lighting passes.
        D3D11_TEXTURE2D_DESC shadowDescription{};
        shadowDescription.Width = shadowMapSize;
        shadowDescription.Height = shadowMapSize;
        shadowDescription.MipLevels = 1;
        shadowDescription.ArraySize = shadowCascadeCount;
        shadowDescription.Format = DXGI_FORMAT_R32_TYPELESS;
        shadowDescription.SampleDesc.Count = 1;
        shadowDescription.Usage = D3D11_USAGE_DEFAULT;
        shadowDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (failed(device->CreateTexture2D(&shadowDescription, nullptr, &shadowTexture), "Create shadow map", error)) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC shadowDsvDescription{};
        shadowDsvDescription.Format = DXGI_FORMAT_D32_FLOAT;
        shadowDsvDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        shadowDsvDescription.Texture2DArray.MipSlice = 0;
        shadowDsvDescription.Texture2DArray.FirstArraySlice = 0;
        shadowDsvDescription.Texture2DArray.ArraySize = 1;
        for (UINT cascade = 0; cascade < shadowCascadeCount; ++cascade) {
            shadowDsvDescription.Texture2DArray.FirstArraySlice = cascade;
            if (failed(device->CreateDepthStencilView(shadowTexture.Get(), &shadowDsvDescription, &shadowDepthSlices[cascade]), "Create shadow map depth view", error)) return false;
        }
        // Keep the legacy view member as an alias for the first cascade. The
        // render path binds the explicit slice views when producing CSM data.
        shadowDepth = shadowDepthSlices[0];
        D3D11_SHADER_RESOURCE_VIEW_DESC shadowSrvDescription{};
        shadowSrvDescription.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrvDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowSrvDescription.Texture2DArray.MostDetailedMip = 0;
        shadowSrvDescription.Texture2DArray.MipLevels = 1;
        shadowSrvDescription.Texture2DArray.FirstArraySlice = 0;
        shadowSrvDescription.Texture2DArray.ArraySize = shadowCascadeCount;
        if (failed(device->CreateShaderResourceView(shadowTexture.Get(), &shadowSrvDescription, &shadowView), "Create shadow map shader view", error)) return false;

        description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (std::size_t i = 0; i < gbufferTargets.size(); ++i) {
            texture.Reset();
            if (failed(device->CreateTexture2D(&description, nullptr, &texture), "Create G-buffer", error)) return false;
            if (failed(device->CreateRenderTargetView(texture.Get(), nullptr, &gbufferTargets[i]), "Create G-buffer target", error)) return false;
            if (failed(device->CreateShaderResourceView(texture.Get(), nullptr, &gbufferViews[i]), "Create G-buffer SRV", error)) return false;
        }
        return true;
    }

    bool createPipeline(std::string& error) {
        ComPtr<ID3DBlob> shader;
        if (!compile(shaderRoot, materialPath(), sceneSource, materialSource, "VSMain", "vs_5_0", shader, error)) return false;
        if (failed(device->CreateVertexShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &geometryVS), "Create geometry VS", error)) return false;
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (failed(device->CreateInputLayout(layout, 2, shader->GetBufferPointer(), shader->GetBufferSize(), &inputLayout), "Create input layout", error)) return false;
        shader.Reset();
        if (!compile(shaderRoot, materialPath(), sceneSource, materialSource, "VSFullscreen", "vs_5_0", shader, error)) return false;
        if (failed(device->CreateVertexShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &fullscreenVS), "Create fullscreen VS", error)) return false;
        const auto pixelShader = [&](const char* entry, ComPtr<ID3D11PixelShader>& result) {
            shader.Reset();
            return compile(shaderRoot, materialPath(), sceneSource, materialSource, entry, "ps_5_0", shader, error) &&
                !failed(device->CreatePixelShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &result), entry, error);
        };
        if (!pixelShader("PSForward", forwardPS) || !pixelShader("PSGBuffer", gbufferPS) ||
            !pixelShader("PSDeferred", deferredPS) || !pixelShader("PSBackground", backgroundPS)) return false;

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(SceneConstants);
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (failed(device->CreateBuffer(&buffer, nullptr, &constants), "Create scene constants", error)) return false;

        buffer.ByteWidth = sizeof(ObjectConstants);
        if (failed(device->CreateBuffer(&buffer, nullptr, &objectConstants), "Create object constants", error)) return false;
        buffer.ByteWidth = sizeof(RayConstants);
        if (failed(device->CreateBuffer(&buffer, nullptr, &rayConstants), "Create ray constants", error)) return false;
        buffer.ByteWidth = sizeof(ShadowConstants);
        if (failed(device->CreateBuffer(&buffer, nullptr, &shadowConstants), "Create shadow constants", error)) return false;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (failed(device->CreateRasterizerState(&raster, &rasterizer), "Create rasterizer", error)) return false;
        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = TRUE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDescription.DepthFunc = D3D11_COMPARISON_LESS;
        if (failed(device->CreateDepthStencilState(&depthDescription, &depthEnabled), "Create depth state", error)) return false;
        depthDescription.DepthEnable = FALSE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (failed(device->CreateDepthStencilState(&depthDescription, &depthDisabled), "Create no-depth state", error)) return false;
        D3D11_SAMPLER_DESC samplerDescription{};
        samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDescription.AddressU = samplerDescription.AddressV = samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
        if (failed(device->CreateSamplerState(&samplerDescription, &sampler), "Create G-buffer sampler", error)) return false;
        D3D11_SAMPLER_DESC shadowSamplerDescription{};
        shadowSamplerDescription.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSamplerDescription.AddressU = shadowSamplerDescription.AddressV = shadowSamplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        shadowSamplerDescription.BorderColor[0] = shadowSamplerDescription.BorderColor[1] = shadowSamplerDescription.BorderColor[2] = shadowSamplerDescription.BorderColor[3] = 1.0f;
        shadowSamplerDescription.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        shadowSamplerDescription.MinLOD = 0.0f;
        shadowSamplerDescription.MaxLOD = 0.0f;
        if (failed(device->CreateSamplerState(&shadowSamplerDescription, &shadowSampler), "Create shadow sampler", error)) return false;
        return true;
    }

    bool prepareMesh(const assets::MeshAsset& mesh, std::string& error) {
        if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
            mesh.vertices.size() > std::numeric_limits<UINT>::max() / sizeof(assets::MeshVertex) ||
            mesh.indices.size() > std::numeric_limits<UINT>::max() / sizeof(std::uint32_t)) {
            error = "Invalid or oversized mesh: " + mesh.name; return false;
        }
        std::uint64_t hash = 1469598103934665603ull;
        const auto hashBytes = [&hash](const void* pointer, std::size_t size) {
            const auto* bytes = static_cast<const unsigned char*>(pointer);
            for (std::size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        };
        hashBytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(assets::MeshVertex));
        hashBytes(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
        if (const auto it = meshes.find(mesh.id); it != meshes.end() && it->second.hash == hash) return true;
        for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) { error = "Mesh index out of bounds: " + mesh.name; return false; }
        GpuMesh gpu; gpu.hash = hash; gpu.count = static_cast<UINT>(mesh.indices.size());
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(assets::MeshVertex));
        description.Usage = D3D11_USAGE_IMMUTABLE; description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = mesh.vertices.data();
        if (failed(device->CreateBuffer(&description, &initial, &gpu.vertices), "Create mesh vertices", error)) return false;
        description.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
        description.BindFlags = D3D11_BIND_INDEX_BUFFER; initial.pSysMem = mesh.indices.data();
        if (failed(device->CreateBuffer(&description, &initial, &gpu.indices), "Create mesh indices", error)) return false;
        meshes[mesh.id] = std::move(gpu); return true;
    }

    bool prepareRayTriangles(const scene::World& world, const assets::AssetLibrary& assets,
                             std::string& error) {
        std::vector<RayTriangle> triangles;
        // Grow with the scene. The count check below only protects D3D11
        // structured-buffer addressability; no scene triangle budget drops
        // geometry.
        try {
            triangles.reserve(16384);
        } catch (const std::bad_alloc&) {
            error = "Unable to allocate ray triangle staging data.";
            return false;
        }
        raySceneActorCount = 0;
        unsigned analyticOrdinal = 0;
        for (const auto& actor : world.actors) {
            if (!actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            const auto* material = assets.findMaterial(actor.mesh.materialId);
            if (!mesh || !material) continue;
            ++raySceneActorCount;
            // Built-in primitives retain their exact analytic intersections
            // for the first 32 actors. Additional built-in actors fall back
            // to the same triangle path as imported and future mesh types, so
            // every visible actor remains traceable.
            const bool builtin = mesh->primitive == assets::MeshPrimitive::Sphere ||
                mesh->primitive == assets::MeshPrimitive::Plane ||
                mesh->primitive == assets::MeshPrimitive::Cube;
            if (builtin && analyticOrdinal++ < 32) continue;
            if (mesh->indices.size() % 3 != 0) {
                error = "Ray scene mesh has a non-triangulated index buffer: " + mesh->name;
                return false;
            }
            const XMMATRIX matrix = actorMatrix(actor);
            for (std::size_t index = 0; index < mesh->indices.size(); index += 3) {
                const auto i0 = mesh->indices[index], i1 = mesh->indices[index + 1], i2 = mesh->indices[index + 2];
                if (i0 >= mesh->vertices.size() || i1 >= mesh->vertices.size() || i2 >= mesh->vertices.size()) {
                    error = "Ray scene mesh index out of bounds: " + mesh->name;
                    return false;
                }
                const auto& v0 = mesh->vertices[i0];
                const auto& v1 = mesh->vertices[i1];
                const auto& v2 = mesh->vertices[i2];
                for (const auto* vertex : {&v0, &v1, &v2}) {
                    for (const float value : vertex->position) if (!std::isfinite(value)) {
                        error = "Ray scene mesh contains a non-finite position: " + mesh->name;
                        return false;
                    }
                }
                const XMVECTOR p0 = XMVector3TransformCoord(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(v0.position)), matrix);
                const XMVECTOR p1 = XMVector3TransformCoord(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(v1.position)), matrix);
                const XMVECTOR p2 = XMVector3TransformCoord(XMLoadFloat3(reinterpret_cast<const XMFLOAT3*>(v2.position)), matrix);
                const XMVECTOR crossProduct = XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0));
                if (XMVectorGetX(XMVector3LengthSq(crossProduct)) < 1e-12f) continue;
                // RayConstants stores the count as a UINT-sized D3D11 SRV
                // element count. Reject only an unrepresentable allocation;
                // this is an API safety check, not a scene complexity budget.
                if (triangles.size() >= std::numeric_limits<UINT>::max() / sizeof(RayTriangle)) {
                    error = "Ray scene is too large for a D3D11 structured buffer: " + mesh->name;
                    return false;
                }
                RayTriangle triangle{};
                XMStoreFloat3(&triangle.position0, p0);
                XMStoreFloat3(&triangle.position1, p1);
                XMStoreFloat3(&triangle.position2, p2);
                XMStoreFloat3(&triangle.normal, XMVector3Normalize(crossProduct));
                triangle.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2], 1.0f);
                triangle.properties = XMFLOAT4(material->roughness, material->metallic, 3.0f, material->grid ? 1.0f : 0.0f);
                try {
                    triangles.push_back(triangle);
                } catch (const std::bad_alloc&) {
                    error = "Unable to allocate ray triangle staging data.";
                    return false;
                }
            }
        }
        std::uint64_t hash = 1469598103934665603ull;
        for (const auto& triangle : triangles) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(&triangle);
            for (std::size_t i = 0; i < sizeof(triangle); ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        }
        if (hash == rayTriangleHash && rayTriangleCount == triangles.size()) return true;
        if (triangles.empty()) {
            rayTriangleHash = hash;
            rayTriangleCount = 0;
            rayTriangleBuffer.Reset(); rayTriangleView.Reset();
            return true;
        }
        D3D11_BUFFER_DESC description{};
        const auto byteWidth = triangles.size() * sizeof(RayTriangle);
        if (byteWidth > std::numeric_limits<UINT>::max()) {
            error = "Ray scene is too large for a D3D11 structured buffer.";
            return false;
        }
        description.ByteWidth = static_cast<UINT>(byteWidth);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = sizeof(RayTriangle);
        D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = triangles.data();
        ComPtr<ID3D11Buffer> newRayTriangleBuffer;
        if (failed(device->CreateBuffer(&description, &initial, &newRayTriangleBuffer), "Create ray triangle buffer", error)) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        view.BufferEx.FirstElement = 0;
        view.BufferEx.NumElements = static_cast<UINT>(triangles.size());
        ComPtr<ID3D11ShaderResourceView> newRayTriangleView;
        if (failed(device->CreateShaderResourceView(newRayTriangleBuffer.Get(), &view, &newRayTriangleView), "Create ray triangle view", error)) return false;
        rayTriangleHash = hash;
        rayTriangleCount = static_cast<UINT>(triangles.size());
        rayTriangleBuffer = std::move(newRayTriangleBuffer);
        rayTriangleView = std::move(newRayTriangleView);
        return true;
    }

    bool drawGeometry(ID3D11PixelShader* pixelShader, const scene::World& world,
                      const assets::AssetLibrary& assets, std::string& error) {
        const UINT stride = sizeof(assets::MeshVertex), offset = 0;
        context->IASetInputLayout(inputLayout.Get());
        context->VSSetShader(geometryVS.Get(), nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(depthEnabled.Get(), 0);
        // Mesh assets can be edited or reloaded in-place with the same ID. Check
        // content once per unique visible mesh each frame; shared actors reuse it.
        std::unordered_set<assets::AssetId> preparedMeshes;
        preparedMeshes.reserve(std::min(world.actors.size(), assets.meshes.size()));
        for (const auto& actor : world.actors) {
            if (!actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            const auto* material = assets.findMaterial(actor.mesh.materialId);
            if (!mesh || !material) continue;
            if (preparedMeshes.insert(mesh->id).second && !prepareMesh(*mesh, error)) return false;
            auto& gpu = meshes.at(mesh->id);
            ObjectConstants data{};
            const auto matrix = actorMatrix(actor);
            XMStoreFloat4x4(&data.worldMatrix, matrix);
            XMStoreFloat4x4(&data.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, matrix)));
            data.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2], 1);
            data.parameters = XMFLOAT4(material->roughness, material->metallic, 0, material->grid ? 1.0f : 0.0f);
            context->UpdateSubresource(objectConstants.Get(), 0, nullptr, &data, 0, 0);
            ID3D11Buffer* vertexBuffer = gpu.vertices.Get();
            context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
            context->IASetIndexBuffer(gpu.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->DrawIndexed(gpu.count, 0, 0);
        }
        return true;
    }

    void drawFullscreen(ID3D11PixelShader* pixelShader) {
        context->IASetInputLayout(nullptr);
        context->VSSetShader(fullscreenVS.Get(), nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(depthDisabled.Get(), 0);
        context->Draw(3, 0);
    }
};

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}
SceneRenderer::~SceneRenderer() { shutdown(); }

bool SceneRenderer::initializeDevice(HWND__* window, std::uint32_t width,
                                     std::uint32_t height, std::string& error) {
    shutdown();
    error.clear();
    if (!window || width == 0 || height == 0) {
        error = "A valid window and nonzero dimensions are required.";
        return false;
    }
    auto& state = *impl_;
    state.width = width;
    state.height = height;
    ComPtr<IDXGIFactory1> factory;
    if (failed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory", error)) return false;
    struct Adapter { ComPtr<IDXGIAdapter1> object; DXGI_ADAPTER_DESC1 description; };
    std::vector<Adapter> adapters;
    for (UINT index = 0;; ++index) {
        Adapter adapter;
        if (FAILED(factory->EnumAdapters1(index, &adapter.object))) break;
        if (!adapter.object || FAILED(adapter.object->GetDesc1(&adapter.description))) continue;
        if (!(adapter.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) adapters.push_back(std::move(adapter));
    }
    std::stable_sort(adapters.begin(), adapters.end(), [](const Adapter& a, const Adapter& b) {
        return a.description.DedicatedVideoMemory > b.description.DedicatedVideoMemory;
    });
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferDesc.Width = width;
    swap.BufferDesc.Height = height;
    swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap.SampleDesc.Count = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 2;
    swap.OutputWindow = window;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    const auto create = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = D3D11CreateDeviceAndSwapChain(adapter, type, nullptr, flags,
            featureLevels, 1, D3D11_SDK_VERSION, &swap, &state.swapChain,
            &state.device, nullptr, &state.context);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            state.swapChain.Reset(); state.device.Reset(); state.context.Reset();
            hr = D3D11CreateDeviceAndSwapChain(adapter, type, nullptr,
                flags & ~D3D11_CREATE_DEVICE_DEBUG, featureLevels, 1, D3D11_SDK_VERSION,
                &swap, &state.swapChain, &state.device, nullptr, &state.context);
        }
        return hr;
    };
    HRESULT result = E_FAIL;
    for (auto& adapter : adapters) {
        result = create(adapter.object.Get(), D3D_DRIVER_TYPE_UNKNOWN);
        if (SUCCEEDED(result)) { state.adapter = utf8(adapter.description.Description); break; }
    }
    if (FAILED(result)) {
        result = create(nullptr, D3D_DRIVER_TYPE_WARP);
        if (SUCCEEDED(result)) state.adapter = "Microsoft WARP (software rasterizer)";
    }
    if (failed(result, "Create D3D11 device and swap chain", error)) return false;
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    state.device.As(&state.infoQueue);
    state.defaultAssets.createDefaults(); state.defaultWorld.createDefaultScene();
    if (!state.createTargets(error)) { shutdown(); return false; }
    return true;
}

bool SceneRenderer::initializePipeline(std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.device) {
        error = "Create the render device before compiling the shader pipeline.";
        return false;
    }
    if (!readText(state.shaderRoot / "Private/Scene.hlsl", state.sceneSource, error) ||
        !readText(state.materialPath(), state.materialSource, error) ||
        !state.createPipeline(error))
        return false;
    state.revision = 1;
    return true;
}

bool SceneRenderer::initialize(HWND__* window, std::uint32_t width,
                               std::uint32_t height, std::string& error) {
    if (!initializeDevice(window, width, height, error)) return false;
    if (!initializePipeline(error)) { shutdown(); return false; }
    return true;
}

bool SceneRenderer::resize(std::uint32_t width, std::uint32_t height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.swapChain) { error = "Renderer is not initialized."; return false; }
    if (width == 0 || height == 0 || (width == state.width && height == state.height)) return true;
    state.context->ClearState();
    state.context->Flush();
    state.backbuffer.Reset(); state.depth.Reset();
    state.sceneTarget.Reset(); state.sceneView.Reset();
    state.shadowTexture.Reset(); state.shadowDepth.Reset();
    for (auto& view : state.shadowDepthSlices) view.Reset();
    state.shadowView.Reset();
    for (auto& view : state.gbufferTargets) view.Reset();
    for (auto& view : state.gbufferViews) view.Reset();
    if (failed(state.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize swap chain", error)) return false;
    state.width = width; state.height = height;
    return state.createTargets(error);
}

bool SceneRenderer::render(const SceneSettings& settings, std::string& error) {
    auto* material = impl_->defaultAssets.findMaterial(1);
    if (material) {
        std::copy_n(settings.baseColor, 3, material->baseColor);
        material->roughness = settings.roughness; material->metallic = settings.metallic;
    }
    return render(settings, impl_->defaultWorld, impl_->defaultAssets, error);
}

bool SceneRenderer::clear(const float color[4], std::string& error) {
    error.clear();
    if (!impl_->context || !impl_->backbuffer) {
        error = "Renderer is not initialized.";
        return false;
    }
    ID3D11RenderTargetView* target = impl_->backbuffer.Get();
    impl_->context->OMSetRenderTargets(1, &target, nullptr);
    impl_->context->ClearRenderTargetView(target, color);
    return true;
}

bool SceneRenderer::render(const SceneSettings& settings, const scene::World& world,
                           const assets::AssetLibrary& assets, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.device || !state.backbuffer) { error = "Renderer has no render target."; return false; }
    const float editorClear[4] = {0.025f, 0.035f, 0.055f, 1.0f};
    if (settings.editorViewport && (!settings.viewportVisible || settings.viewportWidth < 1 || settings.viewportHeight < 1))
        return clear(editorClear, error);
    const int viewportX = settings.viewportWidth > 0 ? std::clamp(settings.viewportX, 0, static_cast<int>(state.width) - 1) : 0;
    const int viewportY = settings.viewportHeight > 0 ? std::clamp(settings.viewportY, 0, static_cast<int>(state.height) - 1) : 0;
    const int viewportWidth = settings.viewportWidth > 0 ? std::clamp(settings.viewportWidth, 1, static_cast<int>(state.width) - viewportX) : static_cast<int>(state.width);
    const int viewportHeight = settings.viewportHeight > 0 ? std::clamp(settings.viewportHeight, 1, static_cast<int>(state.height) - viewportY) : static_cast<int>(state.height);
    const float pitch = std::clamp(settings.cameraPitch, SceneSettings::minPitch, SceneSettings::maxPitch);
    const float distance = std::clamp(settings.cameraDistance, 2.0f, 30.0f);
    const float yaw = settings.cameraYaw;
    const XMVECTOR cameraTargetVec = XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 1.0f);
    const XMVECTOR eye = XMVectorAdd(cameraTargetVec, XMVectorSet(
        distance * std::cos(pitch) * std::sin(yaw),
        distance * std::sin(pitch),
        distance * std::cos(pitch) * std::cos(yaw), 0.0f));
    const XMMATRIX view = XMMatrixLookAtRH(eye, cameraTargetVec, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(50.0f),
        static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight), 0.05f, 100.0f);
    const bool wantsRayScene = settings.rayTracing || settings.shadowMethod == ShadowMethod::AnalyticRay;
    if (wantsRayScene && !state.prepareRayTriangles(world, assets, error)) return false;
    SceneConstants data{};
    XMStoreFloat4x4(&data.viewProjection, view * projection);
    XMStoreFloat4(&data.cameraPosition, eye);
    // cameraPosition.w is unused by the shaders; reuse it as the "solid black
    // backdrop" flag so the scene format can switch the sky gradient off
    // without changing the locked SceneConstants layout.
    data.cameraPosition.w = world.blackBackground ? 1.0f : 0.0f;
    data.baseColor = XMFLOAT4(settings.baseColor[0], settings.baseColor[1], settings.baseColor[2], 1);
    XMStoreFloat4(&data.lightDirectionIntensity, XMVector3Normalize(XMVectorSet(-3, 6, -2, 0)));
    data.lightDirectionIntensity.w = settings.lightIntensity;
    data.parameters = XMFLOAT4(std::clamp(settings.roughness, 0.04f, 1.0f),
        std::clamp(settings.metallic, 0.0f, 1.0f), settings.exposure,
        (settings.rayTracing && rayTracingSupported(world, assets)) ? 1.0f : 0.0f);
    data.viewportDebug = XMFLOAT4(static_cast<float>(state.width), static_cast<float>(state.height),
        static_cast<float>(settings.debugView), static_cast<float>(settings.shadowMethod == ShadowMethod::Disabled ? 2 :
            settings.shadowMethod == ShadowMethod::AnalyticRay ? 1 : 0));
    state.context->UpdateSubresource(state.constants.Get(), 0, nullptr, &data, 0, 0);
    const XMVECTOR lightDirection = XMVector3Normalize(XMVectorSet(-3, 6, -2, 0));
    const XMVECTOR lightTarget = XMVectorSet(0, 0, 0, 1);
    // The shader's light vector points from the surface toward the light, so
    // place the shadow camera along that same direction from the target.
    const XMVECTOR lightPosition = XMVectorAdd(lightTarget, XMVectorScale(lightDirection, 28.0f));
    const XMVECTOR lightUp = XMVectorSet(0, 1, 0, 0);
    const XMMATRIX lightView = XMMatrixLookAtRH(lightPosition, lightTarget,
        std::abs(XMVectorGetY(lightDirection)) > 0.95f ? XMVectorSet(1, 0, 0, 0) : lightUp);
    const bool useCsm = settings.shadowMethod == ShadowMethod::ShadowMapCSM2;
    const XMMATRIX lightProjection = XMMatrixOrthographicRH(30.0f, 30.0f, 0.1f, 60.0f);
    ShadowConstants shadowData{};
    XMStoreFloat4x4(&shadowData.lightViewProjection, lightView * lightProjection);
    const float shadowFilter = settings.shadowMethod == ShadowMethod::ShadowMapHard ? 1.0f :
        settings.shadowMethod == ShadowMethod::ShadowMapPCF9 ? 2.0f :
        settings.shadowMethod == ShadowMethod::ShadowMapPCSS ? 3.0f :
        useCsm ? 4.0f : 0.0f;
    shadowData.parameters = XMFLOAT4(1.0f / static_cast<float>(Impl::shadowMapSize), 0.0015f, 0.008f, shadowFilter);
    const float shadowFeature = settings.shadowMethod == ShadowMethod::GTAO ? 1.0f :
        settings.shadowMethod == ShadowMethod::ContactShadow ? 2.0f : 0.0f;
    shadowData.cascadeParameters = XMFLOAT4(10.0f, useCsm ? 1.0f : 0.0f, shadowFeature, 0);
    XMStoreFloat4x4(&shadowData.lightViewProjectionCascade1, lightView * lightProjection);
    if (useCsm) {
        const XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(cameraTargetVec, eye));
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
        const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
        const float aspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
        const float tanHalfFov = std::tan(XMConvertToRadians(50.0f) * 0.5f);
        const float splitDistances[3] = {0.05f, 10.0f, 100.0f};
        const auto makeCorners = [&](float distance, std::array<XMVECTOR, 4>& corners) {
            const XMVECTOR center = XMVectorAdd(eye, XMVectorScale(forward, distance));
            const float halfHeight = tanHalfFov * distance;
            const float halfWidth = halfHeight * aspect;
            corners[0] = XMVectorAdd(XMVectorAdd(center, XMVectorScale(right, -halfWidth)), XMVectorScale(up, halfHeight));
            corners[1] = XMVectorAdd(XMVectorAdd(center, XMVectorScale(right, halfWidth)), XMVectorScale(up, halfHeight));
            corners[2] = XMVectorAdd(XMVectorAdd(center, XMVectorScale(right, -halfWidth)), XMVectorScale(up, -halfHeight));
            corners[3] = XMVectorAdd(XMVectorAdd(center, XMVectorScale(right, halfWidth)), XMVectorScale(up, -halfHeight));
        };
        for (int cascade = 0; cascade < 2; ++cascade) {
            std::array<XMVECTOR, 4> nearCorners{}, farCorners{};
            makeCorners(splitDistances[cascade], nearCorners);
            makeCorners(splitDistances[cascade + 1], farCorners);
            const float maxFloat = std::numeric_limits<float>::max();
            XMVECTOR minimum = XMVectorSet(maxFloat, maxFloat, maxFloat, 0);
            XMVECTOR maximum = XMVectorSet(-maxFloat, -maxFloat, -maxFloat, 0);
            for (int corner = 0; corner < 4; ++corner) {
                minimum = XMVectorMin(minimum, XMVector3TransformCoord(nearCorners[corner], lightView));
                maximum = XMVectorMax(maximum, XMVector3TransformCoord(nearCorners[corner], lightView));
                minimum = XMVectorMin(minimum, XMVector3TransformCoord(farCorners[corner], lightView));
                maximum = XMVectorMax(maximum, XMVector3TransformCoord(farCorners[corner], lightView));
            }
            const float nearZ = std::max(0.1f, -XMVectorGetZ(maximum) - 2.0f);
            const float farZ = std::max(nearZ + 1.0f, -XMVectorGetZ(minimum) + 2.0f);
            const XMMATRIX cascadeProjection = XMMatrixOrthographicOffCenterRH(
                XMVectorGetX(minimum) - 1.0f, XMVectorGetX(maximum) + 1.0f,
                XMVectorGetY(minimum) - 1.0f, XMVectorGetY(maximum) + 1.0f,
                nearZ, farZ);
            if (cascade == 0) XMStoreFloat4x4(&shadowData.lightViewProjection, lightView * cascadeProjection);
            else XMStoreFloat4x4(&shadowData.lightViewProjectionCascade1, lightView * cascadeProjection);
        }
    }
    state.context->UpdateSubresource(state.shadowConstants.Get(), 0, nullptr, &shadowData, 0, 0);
    RayConstants rays{};
    if (data.parameters.w > 0.5f || settings.shadowMethod == ShadowMethod::AnalyticRay) {
        for (const auto& actor : world.actors) {
            if (!actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            const auto* material = assets.findMaterial(actor.mesh.materialId);
            if (!mesh || !material) continue;
            // Analytic tracing has exact intersections for built-in spheres,
            // planes, and cubes. Other actors are represented by rayTriangles.
            if (mesh->primitive != assets::MeshPrimitive::Sphere &&
                mesh->primitive != assets::MeshPrimitive::Plane &&
                mesh->primitive != assets::MeshPrimitive::Cube) continue;
            if (rays.info.x >= 32.0f) break;
            auto& object = rays.objects[static_cast<int>(rays.info.x++)];
            const auto inverse = XMMatrixInverse(nullptr, actorMatrix(actor));
            XMStoreFloat4x4(&object.inverseWorld, inverse);
            XMStoreFloat4x4(&object.normalTransform, XMMatrixTranspose(inverse));
            object.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2], 1);
            const float primitive = mesh->primitive == assets::MeshPrimitive::Sphere ? 0.0f :
                mesh->primitive == assets::MeshPrimitive::Plane ? 1.0f : 2.0f;
            object.properties = XMFLOAT4(material->roughness, material->metallic, primitive, material->grid ? 1.0f : 0.0f);
        }
    }
    rays.info.y = wantsRayScene ? static_cast<float>(state.rayTriangleCount) : 0.0f;
    state.context->UpdateSubresource(state.rayConstants.Get(), 0, nullptr, &rays, 0, 0);
    ID3D11Buffer* constants[] = { state.constants.Get(), state.objectConstants.Get(), state.rayConstants.Get(), state.shadowConstants.Get() };
    state.context->VSSetConstantBuffers(0, 4, constants);
    state.context->PSSetConstantBuffers(0, 4, constants);
    ID3D11SamplerState* sampler = state.sampler.Get();
    state.context->PSSetSamplers(0, 1, &sampler);
    ID3D11SamplerState* comparisonSampler = state.shadowSampler.Get();
    state.context->PSSetSamplers(1, 1, &comparisonSampler);
    state.context->GSSetShader(nullptr, nullptr, 0);
    state.context->HSSetShader(nullptr, nullptr, 0);
    state.context->DSSetShader(nullptr, nullptr, 0);
    state.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    state.context->RSSetState(state.rasterizer.Get());
    const D3D11_VIEWPORT viewport{static_cast<float>(viewportX), static_cast<float>(viewportY), static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0, 1};
    state.context->RSSetViewports(1, &viewport);
    state.context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11ShaderResourceView* emptyViews[5]{};
    state.context->PSSetShaderResources(0, 5, emptyViews);
    if (settings.shadowMethod != ShadowMethod::AnalyticRay && settings.shadowMethod != ShadowMethod::Disabled) {
        // Render all visible geometry from the light into the depth-only map.
        // Clearing slot 3 first avoids a D3D11 SRV/DSV hazard when switching
        // between frames that use deferred, forward, and shadow-map passes.
        const D3D11_VIEWPORT shadowViewport{0, 0, static_cast<float>(Impl::shadowMapSize), static_cast<float>(Impl::shadowMapSize), 0, 1};
        state.context->RSSetViewports(1, &shadowViewport);
        SceneConstants shadowScene = data;
        const UINT cascadeCount = useCsm ? Impl::shadowCascadeCount : 1;
        for (UINT cascade = 0; cascade < cascadeCount; ++cascade) {
            ID3D11DepthStencilView* cascadeDepth = state.shadowDepthSlices[cascade].Get();
            state.context->OMSetRenderTargets(0, nullptr, cascadeDepth);
            state.context->ClearDepthStencilView(cascadeDepth, D3D11_CLEAR_DEPTH, 1.0f, 0);
            shadowScene.viewProjection = cascade == 0 ? shadowData.lightViewProjection : shadowData.lightViewProjectionCascade1;
            state.context->UpdateSubresource(state.constants.Get(), 0, nullptr, &shadowScene, 0, 0);
            if (!state.drawGeometry(nullptr, world, assets, error)) return false;
        }
        // Explicitly release the shadow DSV before binding the same resource as
        // a pixel shader SRV; relying on D3D11's automatic hazard resolution
        // would silently unbind the SRV on some drivers.
        state.context->OMSetRenderTargets(0, nullptr, nullptr);
        state.context->UpdateSubresource(state.constants.Get(), 0, nullptr, &data, 0, 0);
    }
    const D3D11_VIEWPORT sceneViewport{static_cast<float>(viewportX), static_cast<float>(viewportY), static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0, 1};
    state.context->RSSetViewports(1, &sceneViewport);
    ID3D11ShaderResourceView* shadowResources[] = { nullptr, nullptr, nullptr, state.shadowView.Get(), state.rayTriangleView.Get() };
    state.context->PSSetShaderResources(0, 5, shadowResources);
    state.context->ClearDepthStencilView(state.depth.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ID3D11RenderTargetView* backbuffer = settings.editorViewport ? state.sceneTarget.Get() : state.backbuffer.Get();
    const float backgroundClear[4] = {world.blackBackground ? 0.0f : 0.025f, world.blackBackground ? 0.0f : 0.035f, world.blackBackground ? 0.0f : 0.055f, 1.0f};
    state.context->ClearRenderTargetView(backbuffer, backgroundClear);
    if (settings.path == RenderPath::Deferred) {
        ID3D11RenderTargetView* targets[3]{};
        const float clear[4]{};
        for (std::size_t i = 0; i < 3; ++i) {
            targets[i] = state.gbufferTargets[i].Get();
            state.context->ClearRenderTargetView(targets[i], clear);
        }
        state.context->OMSetRenderTargets(3, targets, state.depth.Get());
        if (!state.drawGeometry(state.gbufferPS.Get(), world, assets, error)) return false;
        state.context->OMSetRenderTargets(1, &backbuffer, nullptr);
        ID3D11ShaderResourceView* views[] = {state.gbufferViews[0].Get(), state.gbufferViews[1].Get(), state.gbufferViews[2].Get(), state.shadowView.Get(), state.rayTriangleView.Get()};
        state.context->PSSetShaderResources(0, 5, views);
        state.drawFullscreen(state.deferredPS.Get());
        state.context->PSSetShaderResources(0, 5, emptyViews);
    } else {
        state.context->OMSetRenderTargets(1, &backbuffer, state.depth.Get());
        state.drawFullscreen(state.backgroundPS.Get());
        if (!state.drawGeometry(state.forwardPS.Get(), world, assets, error)) return false;
    }
    // Keep the backbuffer bound for the caller's Dear ImGui draw submission.
    if (settings.editorViewport) {
        backbuffer = state.backbuffer.Get();
        state.context->ClearRenderTargetView(backbuffer, editorClear);
    }
    state.context->OMSetRenderTargets(1, &backbuffer, nullptr);
    const HRESULT removed = state.device->GetDeviceRemovedReason();
    return !failed(removed, "Render/device status", error);
}

void SceneRenderer::setShaderRoot(std::filesystem::path root) { impl_->shaderRoot = std::filesystem::absolute(std::move(root)).lexically_normal(); }
void SceneRenderer::setMaterialShaderPath(std::filesystem::path path) {
    impl_->materialOverride = path.empty() ? std::filesystem::path{} : std::filesystem::absolute(std::move(path)).lexically_normal();
}
std::filesystem::path SceneRenderer::materialShaderPath() const { return impl_->materialPath(); }
std::string SceneRenderer::shaderSource() const { return impl_->materialSource; }
std::uint64_t SceneRenderer::shaderRevision() const { return impl_->revision; }

bool SceneRenderer::applyShaderSource(const std::string& source, std::string& diagnostics) {
    diagnostics.clear();
    if (!impl_->device) { diagnostics = "Initialize the renderer before applying shaders."; return false; }
    Impl candidate;
    candidate.device = impl_->device;
    candidate.shaderRoot = impl_->shaderRoot;
    candidate.materialOverride = impl_->materialOverride;
    candidate.materialSource = source;
    if (!readText(candidate.shaderRoot / "Private/Scene.hlsl", candidate.sceneSource, diagnostics) ||
        !candidate.createPipeline(diagnostics)) return false;
    // No live GPU state or source is changed until all six stages and contracts pass.
    impl_->geometryVS = std::move(candidate.geometryVS);
    impl_->fullscreenVS = std::move(candidate.fullscreenVS);
    impl_->forwardPS = std::move(candidate.forwardPS);
    impl_->gbufferPS = std::move(candidate.gbufferPS);
    impl_->deferredPS = std::move(candidate.deferredPS);
    impl_->backgroundPS = std::move(candidate.backgroundPS);
    impl_->inputLayout = std::move(candidate.inputLayout);
    impl_->materialSource = source;
    impl_->sceneSource = std::move(candidate.sceneSource);
    ++impl_->revision;
    diagnostics = "Applied all 6 shader stages. Revision " + std::to_string(impl_->revision) + ".";
    return true;
}

bool SceneRenderer::reloadShaders(std::string& diagnostics) {
    std::string source;
    if (!readText(impl_->materialPath(), source, diagnostics)) return false;
    return applyShaderSource(source, diagnostics);
}

bool SceneRenderer::saveShaderSource(std::string& error) {
    error.clear();
    if (!impl_->revision) { error = "There is no successfully compiled shader to save."; return false; }
    const auto target = impl_->materialPath();
    auto temporary = target;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) { error = "Cannot write shader: " + pathUtf8(temporary); return false; }
        file.write(impl_->materialSource.data(), static_cast<std::streamsize>(impl_->materialSource.size()));
        file.close();
        if (!file) { error = "Failed writing shader: " + pathUtf8(temporary); std::error_code ignored; std::filesystem::remove(temporary, ignored); return false; }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace shader file (Win32 " + std::to_string(GetLastError()) + "): " + pathUtf8(target);
        std::error_code ignored; std::filesystem::remove(temporary, ignored); return false;
    }
    return true;
}

std::string SceneRenderer::rayTracingStatus(const scene::World& world, const assets::AssetLibrary& assets) const {
    unsigned analytic = 0;
    unsigned actors = 0;
    std::uint64_t triangles = 0;
    for (const auto& actor : world.actors) {
        if (!actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh || !assets.findMaterial(actor.mesh.materialId)) continue;
        ++actors;
        const bool builtin = mesh->primitive == assets::MeshPrimitive::Sphere ||
            mesh->primitive == assets::MeshPrimitive::Plane ||
            mesh->primitive == assets::MeshPrimitive::Cube;
        if (builtin && analytic++ < 32) continue;
        if (mesh->indices.size() % 3 != 0 || mesh->indices.empty())
            return "Ray effects unavailable: mesh " + mesh->name + " is not triangulated.";
        triangles += mesh->indices.size() / 3;
    }
    if (!actors) return "Ray effects unavailable: no visible traceable actors.";
    return "Analytic GPU rays: sphere/plane/cube + generic mesh actors (" +
        std::to_string(actors) + " actor(s), " + std::to_string(triangles) +
        " mesh triangle(s)); no hardware DXR.";
}
bool SceneRenderer::rayTracingSupported(const scene::World& world, const assets::AssetLibrary& assets) const {
    return rayTracingStatus(world, assets).starts_with("Analytic");
}

bool SceneRenderer::present(bool vsync, std::string& error) {
    error.clear();
    if (!impl_->swapChain) { error = "Renderer is not initialized."; return false; }
    return !failed(impl_->swapChain->Present(vsync ? 1 : 0, 0), "Present", error);
}

void SceneRenderer::shutdown() {
    if (impl_ && impl_->context) { impl_->context->ClearState(); impl_->context->Flush(); }
    if (impl_) {
        const auto root = impl_->shaderRoot;
        const auto material = impl_->materialOverride;
        *impl_ = Impl{};
        impl_->shaderRoot = root;
        impl_->materialOverride = material;
    }
}

ID3D11Device* SceneRenderer::device() const { return impl_->device.Get(); }
ID3D11DeviceContext* SceneRenderer::context() const { return impl_->context.Get(); }
void* SceneRenderer::sceneTexture() const { return impl_->sceneView.Get(); }
std::uint32_t SceneRenderer::sceneTextureWidth() const { return impl_->width; }
std::uint32_t SceneRenderer::sceneTextureHeight() const { return impl_->height; }
std::string SceneRenderer::adapterName() const { return impl_->adapter; }
bool SceneRenderer::validationEnabled() const { return impl_->infoQueue != nullptr; }

std::uint32_t SceneRenderer::validationErrorCount() const {
    if (!impl_->infoQueue) return 0;
    std::uint32_t count = 0;
    const auto total = impl_->infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < total; ++i) {
        SIZE_T size = 0;
        if (FAILED(impl_->infoQueue->GetMessage(i, nullptr, &size))) continue;
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (SUCCEEDED(impl_->infoQueue->GetMessage(i, message, &size)) &&
            (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) ++count;
    }
    return count;
}

bool SceneRenderer::saveScreenshot(const wchar_t* path, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.swapChain) { error = "Renderer is not initialized."; return false; }
    ComPtr<ID3D11Texture2D> source, staging;
    if (failed(state.swapChain->GetBuffer(0, IID_PPV_ARGS(&source)), "Get screenshot backbuffer", error)) return false;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    if (failed(state.device->CreateTexture2D(&description, nullptr, &staging), "Create screenshot staging", error)) return false;
    state.context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (failed(state.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read screenshot", error)) return false;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(description.Width) * description.Height * 4);
    for (UINT y = 0; y < description.Height; ++y) {
        const auto* row = static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* output = pixels.data() + static_cast<std::size_t>(y) * description.Width * 4;
        for (UINT x = 0; x < description.Width; ++x) {
            output[x * 4] = row[x * 4 + 2];
            output[x * 4 + 1] = row[x * 4 + 1];
            output[x * 4 + 2] = row[x * 4];
            output[x * 4 + 3] = 255;
        }
    }
    state.context->Unmap(staging.Get(), 0);
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER info{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(info);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(description.Width);
    info.biHeight = -static_cast<LONG>(description.Height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"wb") != 0 || !file) { error = "Could not open screenshot output file."; return false; }
    const bool written = std::fwrite(&fileHeader, sizeof(fileHeader), 1, file) == 1 &&
        std::fwrite(&info, sizeof(info), 1, file) == 1 &&
        std::fwrite(pixels.data(), 1, pixels.size(), file) == pixels.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) { error = "Could not write complete screenshot."; return false; }
    return true;
}

} // namespace rae::renderer
