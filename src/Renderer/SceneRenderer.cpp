#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "RestArtEngine/Renderer/SceneRenderer.h"
#include "RestArtEngine/Renderer/SelectionOutline.h"
#include "RestArtEngine/Renderer/Lighting.h"
#include "RestArtEngine/Renderer/RenderFeatures.h"
#include "RestArtEngine/Renderer/RenderPipeline.h"
#include "D3D12/HardwareRayTracing.h"
#include "D3D11/D3D11Rhi.h"
#include "D3D12/D3D12Rhi.h"
#include "MeshAttributes.h"
#include "Passes/ShadowPass.h"
#include "RestArtEngine/Assets/AssetLibrary.h"
#include "RestArtEngine/Scene/World.h"
#include "RestArtEngine/Core/TraceProfiler.h"
#include "RestArtEngine/Core/RuntimeDiagnostics.h"
#include "RestArtEngine/Renderer/FrameCaptureArchive.h"
#include "RestArtEngine/Renderer/FrameCaptureMetadata.h"
#include "RhiInspection.h"
#include <d3d11shader.h>
#include <DirectXPackedVector.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include <new>
#include <map>
#include <tuple>

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

bool writeBitmap(std::FILE* file, std::uint32_t width, std::uint32_t height,
                 std::span<const unsigned char> pixels, std::string& error) {
    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    if (std::fwrite(&header, sizeof(header), 1, file) != 1 ||
        std::fwrite(&info, sizeof(info), 1, file) != 1 ||
        std::fwrite(pixels.data(), 1, pixels.size(), file) != pixels.size()) {
        error = "Could not write complete screenshot."; return false;
    }
    return true;
}

std::span<const std::byte> gpuBytes(const void* data, std::size_t size) {
    return {static_cast<const std::byte*>(data), size};
}

struct Vertex { XMFLOAT3 position; XMFLOAT3 normal; };
struct alignas(16) SceneConstants {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 cameraPosition;
    XMFLOAT4 baseColor;
    XMFLOAT4 lightDirectionIntensity;
    XMFLOAT4 parameters;
    XMFLOAT4 viewportDebug;
    XMFLOAT4 cameraForward; // xyz forward, w orthographic
    XMFLOAT4 cameraLensParams; // near, far, ortho height, tan(vertical FOV / 2)
};
static_assert(sizeof(SceneConstants) == 176);
struct alignas(16) ObjectConstants {
    XMFLOAT4X4 worldMatrix, normalMatrix;
    XMFLOAT4 color, parameters;
};
struct alignas(16) RayObject {
    XMFLOAT4X4 inverseWorld, normalTransform;
    XMFLOAT4 color, properties;
};
struct alignas(16) RayConstants { XMFLOAT4 info; RayObject objects[32]; };
struct alignas(16) GlobalIlluminationConstants { XMFLOAT4 viewport; };
static_assert(sizeof(GlobalIlluminationConstants) == 16);
struct alignas(16) DepthOfFieldConstants { XMFLOAT4 viewport, parameters; };
static_assert(sizeof(DepthOfFieldConstants) == 32);
struct alignas(16) AntiAliasingConstants {
    XMFLOAT4X4 inverseViewProjection, previousViewProjection;
    XMFLOAT4 currentEye, currentForward, previousEye, previousForward;
    XMFLOAT4 viewport, parameters;
};
static_assert(sizeof(AntiAliasingConstants) == 224);
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
// Editor-only outline constants, kept out of SceneConstants so the runtime scene
// contract is unchanged when no editor selection exists.
struct alignas(16) OutlineConstants {
    XMFLOAT4 color;  // rgb + alpha, authored by the editor theme
    XMFLOAT4 params; // ring radius in pixels, 1/width, 1/height, reserved
};
static_assert(sizeof(OutlineConstants) == 32);
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
        {"OutlineConstants", D3D_SIT_CBUFFER, 4, sizeof(OutlineConstants)},
        {"GlobalIlluminationConstants", D3D_SIT_CBUFFER, 6, sizeof(GlobalIlluminationConstants)},
        {"LightingConstants", D3D_SIT_CBUFFER, 7, sizeof(LightingConstants)},
        {"DepthOfFieldConstants", D3D_SIT_CBUFFER, 8, sizeof(DepthOfFieldConstants)},
        {"AntiAliasingConstants", D3D_SIT_CBUFFER, 5, sizeof(AntiAliasingConstants)},
        {"albedoBuffer", D3D_SIT_TEXTURE, 0, 0}, {"normalBuffer", D3D_SIT_TEXTURE, 1, 0}, {"positionBuffer", D3D_SIT_TEXTURE, 2, 0},
        {"shadowMap", D3D_SIT_TEXTURE, 3, 0}, {"gbufferSampler", D3D_SIT_SAMPLER, 0, 0},
        {"shadowSampler", D3D_SIT_SAMPLER, 1, 0},
        {"rayTriangles", D3D_SIT_STRUCTURED, 4, 0}, {"outlineMask", D3D_SIT_TEXTURE, 5, 0},
        {"depthBuffer", D3D_SIT_TEXTURE, 6, 0},
        {"indirectLightingBuffer", D3D_SIT_TEXTURE, 9, 0},
        {"dofColor", D3D_SIT_TEXTURE, 12, 0},
        {"aaSource", D3D_SIT_TEXTURE, 7, 0}, {"aaHistory", D3D_SIT_TEXTURE, 8, 0},
        {"rayUvVertices", D3D_SIT_STRUCTURED, 10, 0}, {"rayUvTriangles", D3D_SIT_STRUCTURED, 11, 0}
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
        if (expected->type == D3D_SIT_STRUCTURED && (binding.BindPoint == 10 || binding.BindPoint == 11)) {
            auto* resource = reflection->GetConstantBufferByName(binding.Name);
            auto* element = resource->GetVariableByIndex(0);
            D3D11_SHADER_VARIABLE_DESC variable{}; D3D11_SHADER_TYPE_DESC type{};
            if (FAILED(element->GetDesc(&variable)) || FAILED(element->GetType()->GetDesc(&type)) ||
                variable.Size != 16 || type.Class != D3D_SVC_VECTOR || type.Rows != 1 || type.Columns != 4 ||
                type.Type != (binding.BindPoint == 10 ? D3D_SVT_FLOAT : D3D_SVT_UINT)) {
                error = std::string(entry) + ": UV buffers require float4 / uint4 elements."; return false;
            }
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
        {"SceneConstants","cameraForward",144,16}, {"SceneConstants","cameraLensParams",160,16},
        {"ObjectConstants","worldMatrix",0,64}, {"ObjectConstants","normalMatrix",64,64},
        {"ObjectConstants","objectColor",128,16}, {"ObjectConstants","objectParameters",144,16},
        {"RayConstants","rayInfo",0,16}, {"RayConstants","rayObjects",16,5120},
        {"ShadowConstants","lightViewProjection",0,64},
        {"ShadowConstants","lightViewProjectionCascade1",64,64},
        {"ShadowConstants","shadowParameters",128,16},
        {"ShadowConstants","cascadeParameters",144,16},
        {"OutlineConstants","outlineColor",0,16},
        {"OutlineConstants","outlineParams",16,16},
        {"GlobalIlluminationConstants","giViewport",0,16},
        {"DepthOfFieldConstants","dofViewport",0,16},
        {"DepthOfFieldConstants","dofParameters",16,16},
        {"AntiAliasingConstants","aaInverseViewProjection",0,64},
        {"AntiAliasingConstants","aaPreviousViewProjection",64,64},
        {"AntiAliasingConstants","aaCurrentEye",128,16},
        {"AntiAliasingConstants","aaCurrentForward",144,16},
        {"AntiAliasingConstants","aaPreviousEye",160,16},
        {"AntiAliasingConstants","aaPreviousForward",176,16},
        {"AntiAliasingConstants","aaViewport",192,16},
        {"AntiAliasingConstants","aaParameters",208,16},
        {"LightingConstants","lightInfo",0,16},
        {"LightingConstants","skyColor",16,16},
        {"LightingConstants","sceneLights",32,sizeof(GpuLight) * maxSceneLights}
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
        if (std::strcmp(variable.name, "sceneLights") == 0) {
            if (type.Class != D3D_SVC_STRUCT || type.Elements != maxSceneLights || type.Members != 6) {
                error = std::string(entry) + ": sceneLights requires 32 six-vector light structures."; return false;
            }
            const char* members[] = {"positionType", "directionRange", "colorIntensity", "axisUWidth", "axisVHeight", "spotShadow"};
            for (UINT i = 0; i < std::size(members); ++i) {
                D3D11_SHADER_TYPE_DESC member{};
                if (FAILED(variableType->GetMemberTypeByName(members[i])->GetDesc(&member)) ||
                    member.Offset != i * 16 || member.Type != D3D_SVT_FLOAT || member.Class != D3D_SVC_VECTOR ||
                    member.Rows != 1 || member.Columns != 4 || member.Elements != 0) {
                    error = std::string(entry) + ": incompatible sceneLights member: " + members[i]; return false;
                }
            }
        } else if (variable.size <= 64) {
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
        if (desc.InputParameters != 4) { error = "VSMain requires POSITION/NORMAL and two packed UV inputs."; return false; }
        unsigned seen = 0;
        for (UINT i = 0; i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC input{}; reflection->GetInputParameterDesc(i, &input);
            unsigned bit = 0;
            if (std::strcmp(input.SemanticName, "POSITION") == 0 && input.SemanticIndex == 0 && input.Mask == 7) bit = 1;
            if (std::strcmp(input.SemanticName, "NORMAL") == 0 && input.SemanticIndex == 0 && input.Mask == 7) bit = 2;
            if (std::strcmp(input.SemanticName, "TEXCOORD") == 0 && input.SemanticIndex < 2 && input.Mask == 15) bit = 4u << input.SemanticIndex;
            if (!bit || (seen & bit) || input.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32) {
                error = "VSMain input contract changed (POSITION/NORMAL float3 and TEXCOORD0/1 float4 required)."; return false;
            }
            seen |= bit;
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
const assets::MaterialAsset* resolveMaterial(const assets::AssetLibrary& assets, assets::AssetId id,
                                            const assets::MaterialAsset* materialOverride) {
    const auto* material = assets.findMaterial(id);
    return material && materialOverride && materialOverride->id == id ? materialOverride : material;
}
float materialFlags(const assets::MaterialAsset& material) {
    // Numeric integer flags shared by raster and ray material properties.
    return static_cast<float>((material.grid ? 1u : 0u) | (material.twoSided ? 2u : 0u) |
        (material.effectiveCullMode() == assets::MaterialCullMode::Front ? 4u : 0u));
}
bool mirroredTransform(const scene::Actor& actor) {
    return std::signbit(actor.transform.scale[0]) ^ std::signbit(actor.transform.scale[1]) ^
        std::signbit(actor.transform.scale[2]);
}
// Geometry data is immutable between explicit MeshAsset revisions. Compare the
// small scene inputs before expanding triangles, not their multi-megabyte output.
struct RaySceneInput {
    scene::ActorId actor = 0;
    assets::AssetId mesh = 0, material = 0;
    assets::MeshGeometryKey geometry{};
    assets::MeshPrimitive primitive{};
    std::array<float, 14> values{}; // transform, base color, roughness, metallic
    bool grid = false, twoSided = false, cullFront = false;
    assets::MaterialDepthTest depthTest = assets::MaterialDepthTest::Less;
    bool depthWrite = true;
    std::uint32_t slot = 0;
    bool operator==(const RaySceneInput&) const = default;
};
std::vector<RaySceneInput> makeRaySceneInputs(const scene::World& world, const assets::AssetLibrary& assets,
                                            const assets::MaterialAsset* preview) {
    std::vector<RaySceneInput> result;
    result.reserve(world.actors.size());
    for (const auto& actor : world.actors) {
        if (actor.light || !actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh) continue;
        const auto& t = actor.transform;
        for (std::size_t slot = 0; slot < mesh->materialSlotCount(); ++slot) {
            const auto* material = resolveMaterial(assets, actor.mesh.materialForSlot(*mesh, slot), preview);
            if (!material) continue;
            result.push_back({actor.id, mesh->id, material->id, mesh->geometryKey(), mesh->primitive,
                {t.position[0], t.position[1], t.position[2], t.rotation[0], t.rotation[1], t.rotation[2],
                 t.scale[0], t.scale[1], t.scale[2], material->baseColor[0], material->baseColor[1],
                 material->baseColor[2], material->roughness, material->metallic}, material->grid, material->twoSided,
                 material->effectiveCullMode() == assets::MaterialCullMode::Front,
                 material->depthTest, material->depthWrite, static_cast<std::uint32_t>(slot)});
        }
    }
    return result;
}
} // namespace

struct SceneRenderer::Impl {
    using Clock = std::chrono::steady_clock;
    MainBackendSelection backendSelection;
    bool backendReady = false;
    bool d3d12InitializationAttempted = false;
    std::string backendInitializationFailure;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::shared_ptr<rhi::Device> rhiDevice;
    std::unique_ptr<RhiInspection> inspection;
    rhi::CommandContext& sceneCommands() {
        if (!inspection) inspection = std::make_unique<RhiInspection>(rhiDevice->commands());
        return *inspection;
    }
    std::string diagnosticVS, diagnosticPS;
    std::shared_ptr<d3d12::D3D12RhiDevice> native12;
    rhi::TextureHandle swapchainImage, sceneColorImage, sceneDepthImage, shadowImage;
    // The native DXR module owns and transitions its HDR indirect image; the
    // raster lighting samples an RHI-owned copy so both state machines stay
    // independent.
    rhi::TextureHandle nativeIndirectCopy;
    std::array<rhi::TextureHandle, 3> gbufferImages;
    rhi::ShaderHandle geometryModule, fullscreenModule;
    // Index 7 remains the depth-only shadow pipeline without a pixel stage.
    std::array<rhi::ShaderHandle, 11> pixelModules;
    std::map<std::tuple<unsigned, unsigned, unsigned, bool, bool>, rhi::GraphicsPipelineHandle> graphicsPipelines;
    rhi::BufferHandle sceneBuffer, objectBuffer, rayBuffer, shadowBuffer, giBuffer, lightingBuffer, dofBuffer, aaBuffer, zeroUv;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11InfoQueue> infoQueue;
    std::unique_ptr<d3d12::HardwareRayTracing> hardwareRayTracing;
    bool hardwarePipelineReady = false, hardwareActive = false;
    bool hardwareRuntimeFailed = false, hardwareRequested = false;
    bool hardwareDepthWasCompatible = true;
    std::string hardwareStatus = "Hardware ray tracing has not been initialized.";
    // Latest main-scene composition and feature policy. Tool previews render
    // their own frames but never replace this snapshot.
    RenderPipeline pipeline;
    RenderFeatureMatrix features;
    GlobalIlluminationStatus giStatus;
    std::uint64_t giHistoryEpoch = 0;
    ComPtr<ID3D11RenderTargetView> backbuffer;
    ComPtr<ID3D11RenderTargetView> sceneTarget;
    ComPtr<ID3D11Texture2D> sceneTexture;
    struct PreviewImage {
        rhi::TextureHandle image;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width = 0, height = 0;
    };
    std::array<PreviewImage, 8> previews;
    std::vector<PreviewImage> retiredPreviews;
    bool previewSceneReady = false;
    scene::World previewWorld;
    assets::AssetLibrary previewAssets;
    ComPtr<ID3D11ShaderResourceView> sceneView;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11ShaderResourceView> depthView;
    ComPtr<ID3D11Texture2D> shadowTexture;
    ComPtr<ID3D11DepthStencilView> shadowDepth;
    std::array<ComPtr<ID3D11DepthStencilView>, 2> shadowDepthSlices;
    ComPtr<ID3D11ShaderResourceView> shadowView;
    std::array<ComPtr<ID3D11RenderTargetView>, 3> gbufferTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>, 3> gbufferViews;
    ComPtr<ID3D11VertexShader> geometryVS, fullscreenVS;
    ComPtr<ID3D11PixelShader> forwardPS, gbufferPS, deferredPS, backgroundPS;
    ComPtr<ID3D11PixelShader> outlineMaskPS, outlineCompositePS, dofPS;
    // Created on the first editor frame that has a selection; runtime frames
    // never allocate the mask.
    std::unique_ptr<SelectionOutlinePass> outline;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> constants, objectConstants, rayConstants, shadowConstants, giConstants, lightingConstants, dofConstants;
    ComPtr<ID3D11Buffer> rayTriangleBuffer;
    ComPtr<ID3D11ShaderResourceView> rayTriangleView;
    ComPtr<ID3D11Buffer> rayUvBuffer, rayUvIndexBuffer, zeroUvBuffer;
    ComPtr<ID3D11ShaderResourceView> rayUvView, rayUvIndexView;
    std::uint64_t rayUvBytes = 0;
    std::uint64_t rayTriangleHash = 0;
    UINT rayTriangleCount = 0;
    UINT raySceneActorCount = 0;
    std::vector<RaySceneInput> raySceneInputs;
    bool raySceneInputsValid = false;
    Clock::time_point rayLastUsed{};
    bool raySceneActive = false;
    std::uint64_t usageEpoch = 0;
    struct GpuMesh {
        rhi::BufferHandle vertexBuffer, indexBuffer, uvBuffer;
        ComPtr<ID3D11Buffer> vertices, indices, uv;
        UINT count = 0;
        assets::MeshGeometryKey geometry{};
        std::uint64_t bytes = 0, lastUsedEpoch = 0;
        Clock::time_point lastUsed{};
    };
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
    ComPtr<ID3D11DepthStencilState> depthDisabled;
    ComPtr<ID3D11SamplerState> sampler, shadowSampler;
    // Doubled from the original 2048 so a texel covers half the world distance;
    // combined with the 4x4 PCF option this removes the hard stair-step edges
    // that survived the old 2x2 kernel. Two cascades cost ~128 MiB of depth.
    static constexpr UINT shadowMapSize = 4096;
    static constexpr UINT shadowCascadeCount = 2;
    std::uint32_t width = 0, height = 0;
    std::uint32_t sceneWidth = 0, sceneHeight = 0;
    struct SceneTargets {
        rhi::TextureHandle colorImage, depthImage;
        std::array<rhi::TextureHandle, 3> images;
        ComPtr<ID3D11RenderTargetView> colorTarget;
        ComPtr<ID3D11Texture2D> color;
        ComPtr<ID3D11ShaderResourceView> colorView;
        ComPtr<ID3D11DepthStencilView> depthTarget;
        ComPtr<ID3D11ShaderResourceView> depthResource;
        std::array<ComPtr<ID3D11RenderTargetView>, 3> gbufferTargets;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> gbufferViews;
    };
    // UI draw lists borrow raw SRVs. A target replacement during UI building
    // must keep every old generation until all platform windows have submitted.
    std::vector<SceneTargets> retiredSceneTargets;
    std::string adapter;

    // Private DX11 diagnostic resources; public snapshots contain no native handles.
#include "RenderDiagnostics.inl"
#include "GeometryStatistics.inl"
#include "FrustumCulling.inl"
#include "DepthOfField.inl"
#include "AntiAliasing.inl"

    bool createSwapChain(HWND__* window, std::uint32_t targetWidth, std::uint32_t targetHeight, std::string& error) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = targetWidth;
        description.Height = targetHeight;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        ComPtr<IDXGISwapChain1> created;
        if (failed(factory->CreateSwapChainForHwnd(device.Get(), window, &description, nullptr, nullptr, &created),
                   "Create swap chain", error)) return false;
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        return !failed(created.As(&swapChain), "Query swap chain", error);
    }

    void retireSceneTargets() {
        if (sceneColorImage) {
            auto& retired = retiredSceneTargets.emplace_back();
            retired.colorImage = std::move(sceneColorImage);
            retired.depthImage = std::move(sceneDepthImage);
            retired.images = std::move(gbufferImages);
            retired.colorTarget = std::move(sceneTarget);
            retired.color = std::move(sceneTexture);
            retired.colorView = std::move(sceneView);
            retired.depthTarget = std::move(depth);
            retired.depthResource = std::move(depthView);
            retired.gbufferTargets = std::move(gbufferTargets);
            retired.gbufferViews = std::move(gbufferViews);
        }
        sceneWidth = sceneHeight = 0;
    }

#include "RhiSceneResources.inl"

    bool createPipeline(std::string& error, const LoadProgress& progress = {}) {
        try {
        ComPtr<ID3DBlob> shader;
        progress.report(0.0f, "Compiling geometry shader", "VSMain (1/12)");
        if (!compile(shaderRoot, materialPath(), sceneSource, materialSource, "VSMain", "vs_5_0", shader, error)) return false;
        geometryModule = rhiDevice->createShader(rhi::ShaderStage::Vertex,
            gpuBytes(shader->GetBufferPointer(), shader->GetBufferSize()), "Scene geometry VS");
        if (device) geometryVS = d3d11::nativeVertexShader(geometryModule);
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (device && failed(device->CreateInputLayout(layout, static_cast<UINT>(std::size(layout)), shader->GetBufferPointer(), shader->GetBufferSize(), &inputLayout), "Create input layout", error)) return false;
        progress.report(0.07f, "Geometry shader ready", "VSMain (1/12)");
        shader.Reset();
        progress.report(0.07f, "Compiling fullscreen shader", "VSFullscreen (2/12)");
        if (!compile(shaderRoot, materialPath(), sceneSource, materialSource, "VSFullscreen", "vs_5_0", shader, error)) return false;
        fullscreenModule = rhiDevice->createShader(rhi::ShaderStage::Vertex,
            gpuBytes(shader->GetBufferPointer(), shader->GetBufferSize()), "Scene fullscreen VS");
        if (device) fullscreenVS = d3d11::nativeVertexShader(fullscreenModule);
        progress.report(0.14f, "Fullscreen shader ready", "VSFullscreen (2/12)");
        unsigned moduleIndex = 0;
        unsigned compiledPixels = 0;
        const auto pixelShader = [&](const char* entry, ComPtr<ID3D11PixelShader>& result) {
            shader.Reset();
            const auto detail = std::string(entry) + " (" + std::to_string(compiledPixels + 3) + "/12)";
            progress.report(0.14f + 0.056f * static_cast<float>(compiledPixels), "Compiling scene shader", detail);
            if (!compile(shaderRoot, materialPath(), sceneSource, materialSource, entry, "ps_5_0", shader, error)) return false;
            auto module = rhiDevice->createShader(rhi::ShaderStage::Pixel,
                gpuBytes(shader->GetBufferPointer(), shader->GetBufferSize()), entry);
            if (device) result = d3d11::nativePixelShader(module);
            pixelModules[moduleIndex++] = std::move(module);
            ++compiledPixels;
            progress.report(0.14f + 0.056f * static_cast<float>(compiledPixels), "Scene shader ready", detail);
            return true;
        };
        if (!pixelShader("PSForward", forwardPS) || !pixelShader("PSGBuffer", gbufferPS) ||
            !pixelShader("PSDeferred", deferredPS) || !pixelShader("PSBackground", backgroundPS) ||
            !pixelShader("PSOutlineMask", outlineMaskPS) || !pixelShader("PSOutlineComposite", outlineCompositePS) ||
            !pixelShader("PSDepthOfField", dofPS))
            return false;
        // Index 7 is reserved for the depth-only shadow PSO.
        ++moduleIndex;
        ComPtr<ID3D11PixelShader> aaPixel;
        if (!pixelShader("PSFXAA",aaPixel) || !pixelShader("PSTAA",aaPixel) ||
            !pixelShader("PSAntiAliasingPresent",aaPixel)) return false;
        if (device) {
            progress.report(0.70f, "Preparing selection outline");
            if (!outline) outline = std::make_unique<SelectionOutlinePass>();
            if (!outline->initialize(device.Get(), error)) return false;
        }

        progress.report(0.73f, "Creating scene constant buffers");
        const auto constant = [&](rhi::BufferHandle& handle, ComPtr<ID3D11Buffer>& native,
            std::uint32_t size, const char* name) {
            handle = rhiDevice->createBuffer({size, rhi::BufferUsage::Constant, false, name});
            if (device) native = d3d11::nativeBuffer(handle);
        };
        constant(sceneBuffer, constants, sizeof(SceneConstants), "Scene constants");
        constant(objectBuffer, objectConstants, sizeof(ObjectConstants), "Actor constants");
        constant(rayBuffer, rayConstants, sizeof(RayConstants), "Analytic ray constants");
        constant(shadowBuffer, shadowConstants, sizeof(ShadowConstants), "Shadow constants");
        constant(giBuffer, giConstants, sizeof(GlobalIlluminationConstants), "GI constants");
        constant(dofBuffer, dofConstants, sizeof(DepthOfFieldConstants), "DOF constants");
        constant(lightingBuffer, lightingConstants, sizeof(LightingConstants), "Lighting constants");
        aaBuffer = rhiDevice->createBuffer({sizeof(AntiAliasingConstants),rhi::BufferUsage::Constant,false,"Anti-aliasing constants"});
        meshAttributes::PackedUv missingUvData{};
        this->zeroUv = rhiDevice->createBuffer({sizeof(missingUvData), rhi::BufferUsage::Vertex, true, "Absent UV stream"},
            gpuBytes(&missingUvData, sizeof(missingUvData)));
        progress.report(0.77f, "Scene constant buffers ready");
        if (device) {
        progress.report(0.77f, "Creating raster, depth and sampler states");
        zeroUvBuffer = d3d11::nativeBuffer(this->zeroUv);
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        if (failed(device->CreateRasterizerState(&raster, &rasterizer), "Create rasterizer", error)) return false;
        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = FALSE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depthDescription.DepthFunc = D3D11_COMPARISON_LESS;
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
        }
        // Validate every supported material-state variant before a hot-reload
        // candidate can replace the live pipeline set.
        for (unsigned rasterVariant = 0; rasterVariant != 6; ++rasterVariant) {
            const auto detail = "Material and shadow variants " + std::to_string(rasterVariant * 37) + "/222";
            progress.report(0.80f + 0.03f * static_cast<float>(rasterVariant), "Preparing graphics pipelines", detail);
            (void)graphicsPipeline(7, rasterVariant, 3, false);
            for (unsigned depthIndex = 0; depthIndex != 18; ++depthIndex) {
                (void)graphicsPipeline(0, rasterVariant, depthIndex, false);
                (void)graphicsPipeline(1, rasterVariant, depthIndex, false);
            }
            progress.report(0.80f + 0.03f * static_cast<float>(rasterVariant + 1), "Graphics pipeline group ready",
                "Material and shadow variants " + std::to_string((rasterVariant + 1) * 37) + "/222");
        }
        progress.report(0.98f, "Preparing fullscreen pipelines", "Lighting, background and anti-aliasing (0/5)");
        (void)graphicsPipeline(2, 0, 16, true, false);
        (void)graphicsPipeline(3, 0, 16, true, true);
        for (unsigned index=8;index<=10;++index) (void)graphicsPipeline(index,0,16,true,false);
        progress.report(1.0f, "Raster pipelines ready", "227 graphics pipelines");
        return true;
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }

    bool prepareMesh(const assets::MeshAsset& mesh, std::string& error) {
        try {
        if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0 ||
            mesh.vertices.size() > std::numeric_limits<UINT>::max() / sizeof(assets::MeshVertex) ||
            mesh.indices.size() > std::numeric_limits<UINT>::max() / sizeof(std::uint32_t)) {
            error = "Invalid or oversized mesh: " + mesh.name; return false;
        }
        const auto geometry = mesh.geometryKey();
        if (const auto it = meshes.find(mesh.id); it != meshes.end() && it->second.geometry == geometry) {
            it->second.lastUsed = Clock::now();
            it->second.lastUsedEpoch = usageEpoch;
            return true;
        }
        rae::diagnostics::TraceScope uploadTrace("Mesh geometry upload", "Renderer CPU");
        try { meshAttributes::validateSections(mesh); }
        catch (const std::exception& failure) { error = failure.what(); return false; }
        for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) { error = "Mesh index out of bounds: " + mesh.name; return false; }
        GpuMesh gpu; gpu.geometry = geometry; gpu.count = static_cast<UINT>(mesh.indices.size());
        const auto vertexBytes = static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(assets::MeshVertex));
        const auto indexBytes = static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));
        gpu.vertexBuffer = rhiDevice->createBuffer({vertexBytes, rhi::BufferUsage::Vertex, true, mesh.name + " vertices"},
            gpuBytes(mesh.vertices.data(), vertexBytes));
        gpu.indexBuffer = rhiDevice->createBuffer({indexBytes, rhi::BufferUsage::Index, true, mesh.name + " indices"},
            gpuBytes(mesh.indices.data(), indexBytes));
        if (device) {
            gpu.vertices = d3d11::nativeBuffer(gpu.vertexBuffer);
            gpu.indices = d3d11::nativeBuffer(gpu.indexBuffer);
        }
        gpu.bytes = mesh.vertices.size() * sizeof(assets::MeshVertex) + mesh.indices.size() * sizeof(std::uint32_t);
        if (mesh.uvChannelMask()) {
            if (mesh.vertices.size() > std::numeric_limits<UINT>::max() / sizeof(meshAttributes::PackedUv)) {
                error = "Mesh UV buffer exceeds addressable size."; return false;
            }
            std::vector<meshAttributes::PackedUv> uv; uv.reserve(mesh.vertices.size());
            try { for (std::size_t i = 0; i < mesh.vertices.size(); ++i) uv.push_back(meshAttributes::vertexUv(mesh, i)); }
            catch (const std::exception& failure) { error = failure.what(); return false; }
            const auto uvBytes = static_cast<std::uint32_t>(uv.size() * sizeof(meshAttributes::PackedUv));
            gpu.uvBuffer = rhiDevice->createBuffer({uvBytes, rhi::BufferUsage::Vertex, true, mesh.name + " UVs"},
                gpuBytes(uv.data(), uvBytes));
            if (device) gpu.uv = d3d11::nativeBuffer(gpu.uvBuffer);
            gpu.bytes += uvBytes;
        }
        gpu.lastUsed = Clock::now();
        gpu.lastUsedEpoch = usageEpoch;
        meshes[mesh.id] = std::move(gpu); return true;
        } catch (const std::exception& failure) {
            error = std::string("Unable to prepare mesh geometry/UV data: ") + failure.what();
            return false;
        }
    }

    bool prepareRayTriangles(const scene::World& world, const assets::AssetLibrary& assets,
                             std::string& error, const assets::MaterialAsset* materialPreview) {
        try {
        std::vector<RaySceneInput> nextInputs;
        try { nextInputs = makeRaySceneInputs(world, assets, materialPreview); }
        catch (const std::bad_alloc&) { error = "Unable to allocate ray scene input keys."; return false; }
        if (raySceneInputsValid && nextInputs == raySceneInputs) return true;
        rae::diagnostics::TraceScope gatherTrace("Software ray scene rebuild", "Renderer CPU");
        std::vector<RayTriangle> triangles;
        meshAttributes::RayUvData uvData;
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
            if (actor.light || !actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            if (!mesh) continue;
            std::vector<const assets::MaterialAsset*> materials;
            for (std::size_t slot = 0; slot < mesh->materialSlotCount(); ++slot)
                materials.push_back(resolveMaterial(assets, actor.mesh.materialForSlot(*mesh, slot), materialPreview));
            if (std::none_of(materials.begin(), materials.end(), [](const auto* value) { return value != nullptr; })) continue;
            ++raySceneActorCount;
            // Built-in primitives retain their exact analytic intersections
            // for the first 32 actors. Additional built-in actors fall back
            // to the same triangle path as imported and future mesh types, so
            // every visible actor remains traceable.
            const bool builtin = meshAttributes::analytic(*mesh);
            if (builtin && analyticOrdinal++ < 32) continue;
            if (mesh->indices.size() % 3 != 0) {
                error = "Ray scene mesh has a non-triangulated index buffer: " + mesh->name;
                return false;
            }
            const XMMATRIX matrix = actorMatrix(actor);
            std::uint32_t uvOffset = 0;
            try { meshAttributes::validateSections(*mesh); uvOffset = uvData.appendMesh(*mesh); }
            catch (const std::exception& failure) { error = failure.what(); return false; }
            for (std::size_t index = 0; index < mesh->indices.size(); index += 3) {
                const auto slot = mesh->materialSlotForIndex(static_cast<std::uint32_t>(index));
                if (slot >= materials.size()) { error = "Mesh material section index is invalid."; return false; }
                const auto* material = materials[slot];
                if (!material) continue;
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
                XMStoreFloat3(&triangle.normal, XMVectorScale(XMVector3Normalize(crossProduct),
                    mirroredTransform(actor) ? -1.0f : 1.0f));
                triangle.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2],
                    uvData.appendTriangle(*mesh, index, uvOffset));
                triangle.properties = XMFLOAT4(material->roughness, material->metallic, 3.0f, materialFlags(*material));
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
        const auto hashArray = [&](const auto& values) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(values.data());
            for (std::size_t i = 0; i < values.size() * sizeof(values[0]); ++i) { hash ^= bytes[i]; hash *= 1099511628211ull; }
        };
        hashArray(uvData.vertices); hashArray(uvData.triangles);
        if (hash == rayTriangleHash && rayTriangleCount == triangles.size()) {
            raySceneInputs = std::move(nextInputs); raySceneInputsValid = true;
            return true;
        }
        if (triangles.empty()) {
            rayTriangleHash = hash;
            rayTriangleCount = 0;
            rayTriangleBuffer.Reset(); rayTriangleView.Reset();
            rayUvBuffer.Reset(); rayUvIndexBuffer.Reset(); rayUvView.Reset(); rayUvIndexView.Reset(); rayUvBytes = 0;
            raySceneInputs = std::move(nextInputs); raySceneInputsValid = true;
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
        ComPtr<ID3D11Buffer> newUvBuffer, newUvIndexBuffer;
        ComPtr<ID3D11ShaderResourceView> newUvView, newUvIndexView;
        const auto createUv = [&](const void* data, std::size_t size, ComPtr<ID3D11Buffer>& buffer,
                                  ComPtr<ID3D11ShaderResourceView>& srv) {
            const std::array<std::uint32_t, 4> zero{};
            if (size > std::numeric_limits<UINT>::max()) { error = "Ray UV buffer exceeds addressable size."; return false; }
            D3D11_BUFFER_DESC desc{}; desc.ByteWidth = static_cast<UINT>(std::max<std::size_t>(16, size));
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; desc.StructureByteStride = 16;
            D3D11_SUBRESOURCE_DATA upload{}; upload.pSysMem = size ? data : zero.data();
            if (failed(device->CreateBuffer(&desc, &upload, &buffer), "Create ray UV buffer", error)) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; view.Buffer.NumElements = desc.ByteWidth / 16;
            return !failed(device->CreateShaderResourceView(buffer.Get(), &view, &srv), "Create ray UV view", error);
        };
        const auto uvBytes = uvData.vertices.size() * sizeof(meshAttributes::PackedUv);
        const auto indexBytes = uvData.triangles.size() * sizeof(meshAttributes::UvTriangle);
        if (!createUv(uvData.vertices.data(), uvBytes, newUvBuffer, newUvView) ||
            !createUv(uvData.triangles.data(), indexBytes, newUvIndexBuffer, newUvIndexView)) return false;
        rayUvBuffer = std::move(newUvBuffer); rayUvView = std::move(newUvView);
        rayUvIndexBuffer = std::move(newUvIndexBuffer); rayUvIndexView = std::move(newUvIndexView);
        rayUvBytes = uvBytes + indexBytes;
        rayTriangleHash = hash;
        rayTriangleCount = static_cast<UINT>(triangles.size());
        rayTriangleBuffer = std::move(newRayTriangleBuffer);
        rayTriangleView = std::move(newRayTriangleView);
        raySceneInputs = std::move(nextInputs); raySceneInputsValid = true;
        return true;
        } catch (const std::exception& failure) {
            error = std::string("Unable to prepare ray geometry/UV data: ") + failure.what();
            return false;
        }
    }

#include "RhiSceneExecution.inl"

    bool drawGeometry(unsigned pixelShader, const scene::World& world,
                      const assets::AssetLibrary& assets, std::string& error,
                      const assets::MaterialAsset* materialPreview,
                      const XMFLOAT4X4* clipMatrix = nullptr) {

        // A revision/allocation key detects reloads and explicit geometry edits.
        // Shadow, raster and outline passes share the already prepared buffers.
        std::unordered_set<assets::AssetId> preparedMeshes;
        preparedMeshes.reserve(std::min(world.actors.size(), assets.meshes.size()));
        for (const auto& actor : world.actors) {
            if (actor.light || !actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            if (!mesh) continue;
            if (clipMatrix && !intersectsView(actor, *mesh, *clipMatrix)) continue;
            if (preparedMeshes.insert(mesh->id).second && !prepareMesh(*mesh, error)) return false;
            auto& gpu = meshes.at(mesh->id);
            ObjectConstants data{};
            const auto matrix = actorMatrix(actor);
            XMStoreFloat4x4(&data.worldMatrix, matrix);
            XMStoreFloat4x4(&data.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, matrix)));
            bindRhiMesh(gpu);
            meshAttributes::sections(*mesh, [&](std::uint32_t first, std::uint32_t count, std::uint32_t slot) {
                const auto* material = resolveMaterial(assets, actor.mesh.materialForSlot(*mesh, slot), materialPreview);
                if (!material) return;
                bindRhiMaterialPipeline(*material, mirroredTransform(actor), pixelShader);
                data.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2],
                    static_cast<float>(mesh->uvChannelMask()));
                data.parameters = XMFLOAT4(material->roughness, material->metallic, 0, materialFlags(*material));
                uploadConstants(objectBuffer, data);
                sceneCommands().drawIndexed(count, first);
                if (auto* event = recordDraw(actor.id, mesh->id, material->id, count,
                    static_cast<std::uint32_t>(mesh->vertices.size()))) {
                    event->actorName = actor.name; event->meshName = mesh->name; event->materialName = material->name;
                    event->materialSlot = slot;
                }
            });
        }
        return true;
    }

    // Draws only the editor's selected actors into the outline mask. The mask
    // pass owns depth/blend state; this routine reuses the mesh cache and the
    // per-actor constant update of the scene draw so both stay in sync.
    bool drawSelectedGeometry(const std::uint64_t* actors, std::uint32_t count, const scene::World& world,
                              const assets::AssetLibrary& assets, std::string& error) {
        if (!actors || count == 0) return true;
        const UINT stride = sizeof(assets::MeshVertex), offset = 0;
        std::unordered_set<assets::AssetId> preparedMeshes;
        for (const auto& actor : world.actors) {
            if (actor.light || !actor.visible || !validTransform(actor)) continue;
            bool selected = false;
            for (std::uint32_t index = 0; index < count && !selected; ++index) selected = actors[index] == actor.id;
            if (!selected) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            if (!mesh) continue;
            if (preparedMeshes.insert(mesh->id).second && !prepareMesh(*mesh, error)) return false;
            auto& gpu = meshes.at(mesh->id);
            ObjectConstants data{};
            const auto matrix = actorMatrix(actor);
            XMStoreFloat4x4(&data.worldMatrix, matrix);
            XMStoreFloat4x4(&data.normalMatrix, XMMatrixTranspose(XMMatrixInverse(nullptr, matrix)));
            uploadConstants(objectBuffer, data);
            ID3D11Buffer* vertexBuffers[] = {gpu.vertices.Get(), gpu.uv ? gpu.uv.Get() : zeroUvBuffer.Get()};
            const UINT strides[] = {stride, gpu.uv ? static_cast<UINT>(sizeof(meshAttributes::PackedUv)) : 0u};
            const UINT offsets[] = {offset, 0};
            context->IASetVertexBuffers(0, 2, vertexBuffers, strides, offsets);
            context->IASetIndexBuffer(gpu.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            context->DrawIndexed(gpu.count, 0, 0);
            if (auto* event = recordDraw(actor.id, mesh->id, actor.mesh.materialId, gpu.count,
                static_cast<std::uint32_t>(mesh->vertices.size()))) {
                event->actorName = actor.name; event->meshName = mesh->name;
                if (const auto* material = assets.findMaterial(actor.mesh.materialId)) event->materialName = material->name;
            }
        }
        return true;
    }

    void drawFullscreen(ID3D11PixelShader* pixelShader) {
        // A preceding geometry/shadow draw may have enabled per-material culling.
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(nullptr);
        context->VSSetShader(fullscreenVS.Get(), nullptr, 0);
        context->PSSetShader(pixelShader, nullptr, 0);
        context->OMSetDepthStencilState(depthDisabled.Get(), 0);
        context->Draw(3, 0);
        recordDraw(0, 0, 0, 0, 3);
    }
};

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}
SceneRenderer::~SceneRenderer() { shutdown(); }

bool SceneRenderer::initializeDevice(HWND__* window, std::uint32_t width,
                                     std::uint32_t height, std::string& error,
                                     const MainBackendRequest& backend) {
    // The caller may pass a request borrowed from this renderer. Preserve it
    // before shutdown resets the previous device and its selection snapshot.
    const auto request = backend;
    shutdown();
    error.clear();
    auto& state = *impl_;
    state.backendSelection = selectMainBackend(request);
    if (!state.backendSelection.startupAllowed) {
        error = state.backendSelection.reason;
        return false;
    }
    if (!window || width == 0 || height == 0) {
        error = "A valid window and nonzero dimensions are required.";
        return false;
    }
    const auto failStartup = [&]() {
        const auto selection = state.backendSelection;
        const bool attempted = state.d3d12InitializationAttempted;
        const auto failure = state.backendInitializationFailure;
        shutdown();
        impl_->backendSelection = selection;
        impl_->d3d12InitializationAttempted = attempted;
        impl_->backendInitializationFailure = failure;
        return false;
    };
    state.width = width;
    state.height = height;
    // CPU scene construction is independent of the backend and must not cause
    // a native initialization failure to be mistaken for an API fallback.
    try { state.defaultAssets.createDefaults(); state.defaultWorld.createDefaultScene(); }
    catch (const std::exception& failure) { error = failure.what(); return failStartup(); }
    if (state.backendSelection.effective == MainRenderBackend::D3D12) {
        state.d3d12InitializationAttempted = true;
        try {
            state.native12 = d3d12::createD3D12RhiDevice(window, width, height,
#if defined(_DEBUG)
                true
#else
                false
#endif
            );
            state.rhiDevice = state.native12;
            state.adapter = state.native12->adapterName();
            if (!state.createTargets(error)) throw std::runtime_error(error);
            state.backendReady = true;
            return true;
        } catch (const std::exception& failure) {
            const std::string nativeError = failure.what();
            state.backendInitializationFailure = nativeError;
            if (!request.allowFallbackToD3D11) {
                error = "Native D3D12 initialization failed: " + nativeError;
                return failStartup();
            }
            // The attempt owns no D3D11 resources. Drain and release the failed
            // device/targets before starting the explicitly permitted fallback.
            shutdown();
            auto& fallback = *impl_;
            fallback.backendSelection = selectD3D11Fallback(request);
            fallback.backendInitializationFailure = nativeError;
            fallback.d3d12InitializationAttempted = true;
            fallback.width = width; fallback.height = height;
            // shutdown preserves the Impl object but releases its CPU defaults.
            try { fallback.defaultAssets.createDefaults(); fallback.defaultWorld.createDefaultScene(); }
            catch (const std::exception& failure) { error = failure.what(); return failStartup(); }
        }
    }
    if (failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&state.factory)), "Create DXGI factory", error)) return failStartup();
    struct Adapter { ComPtr<IDXGIAdapter1> object; DXGI_ADAPTER_DESC1 description; };
    std::vector<Adapter> adapters;
    for (UINT index = 0;; ++index) {
        Adapter adapter;
        if (FAILED(state.factory->EnumAdapters1(index, &adapter.object))) break;
        if (!adapter.object || FAILED(adapter.object->GetDesc1(&adapter.description))) continue;
        if (!(adapter.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) adapters.push_back(std::move(adapter));
    }
    std::stable_sort(adapters.begin(), adapters.end(), [](const Adapter& a, const Adapter& b) {
        return a.description.DedicatedVideoMemory > b.description.DedicatedVideoMemory;
    });
    const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0};
    // The device is created on its own; every window owns its swap chain. This
    // keeps boot free to move from the borderless loading window to the editor
    // window without recreating shaders or GPU caches.
    const auto create = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, featureLevels, 1,
            D3D11_SDK_VERSION, &state.device, nullptr, &state.context);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            state.device.Reset(); state.context.Reset();
            hr = D3D11CreateDevice(adapter, type, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG,
                featureLevels, 1, D3D11_SDK_VERSION, &state.device, nullptr, &state.context);
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
    if (failed(result, "Create D3D11 device", error)) return failStartup();
    state.rhiDevice = d3d11::createRhiDevice(state.device.Get(), state.context.Get());
    state.device.As(&state.infoQueue);
    if (!state.createSwapChain(window, width, height, error)) return failStartup();
    if (!state.createTargets(error)) return failStartup();
    state.backendReady = true;
    return true;
}

const MainBackendSelection& SceneRenderer::mainBackendSelection() const { return impl_->backendSelection; }
bool SceneRenderer::mainBackendReady() const { return impl_->backendReady && impl_->rhiDevice != nullptr; }
bool SceneRenderer::d3d12InitializationAttempted() const { return impl_->d3d12InitializationAttempted; }
const std::string& SceneRenderer::backendInitializationError() const { return impl_->backendInitializationFailure; }

void SceneRenderer::detachWindow() {
    // Release the window-bound surface while its HWND is still alive: releasing a
    // swap chain after its window has been destroyed can block inside DXGI.
    auto& state = *impl_;
    if (!state.rhiDevice) return;
    state.resetGeometryViewport();
    if (state.context) { state.context->ClearState(); state.context->Flush(); }
    if (state.rhiDevice) state.sceneCommands().endRenderPass();
    state.swapchainImage.reset();
    state.backbuffer.Reset();
    state.retireSceneTargets();
    state.swapChain.Reset();
    if (state.native12) state.native12->detachWindow();
}

bool SceneRenderer::attachWindow(HWND__* window, std::uint32_t width,
                                 std::uint32_t height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!window || width == 0 || height == 0) {
        error = "A valid window and nonzero dimensions are required.";
        return false;
    }
    if (!state.rhiDevice) { error = "Renderer is not initialized."; return false; }
    detachWindow();
    state.width = width;
    state.height = height;
    if (state.native12) {
        try { state.native12->attachWindow(window, width, height); }
        catch (const std::exception& failure) { error = failure.what(); return false; }
    } else if (!state.createSwapChain(window, width, height, error)) return false;
    const bool targetsReady = state.createTargets(error);
    return targetsReady;
}
bool SceneRenderer::initializePipeline(std::string& error, const LoadProgress& progress) {
    error.clear();
    auto& state = *impl_;
    if (!state.rhiDevice) {
        error = "Create the render device before compiling the shader pipeline.";
        return false;
    }
    progress.report(0.0f, "Reading engine shaders", "Private/Scene.hlsl");
    if (!readText(state.shaderRoot / "Private/Scene.hlsl", state.sceneSource, error)) return false;
    progress.report(0.025f, "Reading material shader", pathUtf8(state.materialPath()));
    if (!readText(state.materialPath(), state.materialSource, error)) return false;
    const float rasterEnd = state.backendSelection.allowD3D12Bridge ? 0.75f : 0.98f;
    if (!state.createPipeline(error, progress.range(0.05f, rasterEnd))) return false;
    state.revision = 1;
    if (state.native12) {
        // Native D3D12 owns the raster device/queue; the DXR transport reuses
        // them and exposes D3D12 images instead of the DX11 shared bridge.
        state.hardwareRayTracing = std::make_unique<d3d12::HardwareRayTracing>();
        state.d3d12InitializationAttempted = true;
        std::string hardwareError;
        progress.report(0.75f, "Initializing native D3D12 ray tracing", "DXR 1.1 device and command list");
        if (state.hardwareRayTracing->initializeNative(state.native12->device(), state.native12->queue(), hardwareError)) {
            progress.report(0.82f, "Compiling native DXR pipeline", "HardwareRayTracing.hlsl::CSMain");
            state.hardwarePipelineReady = state.hardwareRayTracing->preparePipeline(state.shaderRoot,
                state.materialPath(), state.sceneSource, state.materialSource, hardwareError);
            state.hardwareStatus = state.hardwarePipelineReady
                ? "Native D3D12 DXR 1.1 ready; disabled by project settings."
                : "Native DXR pipeline unavailable: " + hardwareError;
        } else {
            state.hardwareStatus = "Native D3D12 ray tracing unavailable: " + hardwareError;
        }
        progress.report(1.0f, "Native D3D12 pipeline ready", state.hardwareStatus);
        return true;
    }
    if (!state.backendSelection.allowD3D12Bridge) {
        state.hardwareStatus = "Hardware ray tracing disabled by the strict D3D11 backend; "
            "D3D12 is not initialized. The project ray tracing and GI requests are retained.";
        progress.report(1.0f, "D3D11 pipeline ready", "Hardware ray tracing disabled by backend selection");
        return true;
    }
    state.hardwareRayTracing = std::make_unique<d3d12::HardwareRayTracing>();
    std::string hardwareError;
    state.d3d12InitializationAttempted = true;
    progress.report(0.75f, "Initializing hardware ray tracing", "Same-adapter D3D12 device and shared fence");
    if (state.hardwareRayTracing->initialize(state.device.Get(), state.context.Get(), hardwareError)) {
        progress.report(0.82f, "Compiling hardware ray tracing", "HardwareRayTracing.hlsl::CSMain");
        state.hardwarePipelineReady = state.hardwareRayTracing->preparePipeline(state.shaderRoot,
            state.materialPath(), state.sceneSource, state.materialSource, hardwareError);
        state.hardwareStatus = state.hardwarePipelineReady ? "DXR 1.1 ready; disabled by project settings."
            : "Hardware pipeline unavailable: " + hardwareError;
    } else {
        state.hardwareStatus = "Hardware ray tracing unavailable: " + hardwareError;
    }
    progress.report(1.0f, "D3D11 pipeline ready", state.hardwareStatus);
    return true;
}

bool SceneRenderer::initialize(HWND__* window, std::uint32_t width,
                               std::uint32_t height, std::string& error,
                               const MainBackendRequest& backend) {
    if (!initializeDevice(window, width, height, error, backend)) return false;
    if (!initializePipeline(error)) {
        const auto selection = impl_->backendSelection;
        const bool attemptedD3D12 = impl_->d3d12InitializationAttempted;
        shutdown();
        impl_->backendSelection = selection;
        impl_->d3d12InitializationAttempted = attemptedD3D12;
        return false;
    }
    return true;
}

bool SceneRenderer::resize(std::uint32_t width, std::uint32_t height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (state.native12) {
        if (!width || !height) return true;
        try {
            state.sceneCommands().endRenderPass();
            state.swapchainImage.reset();
            state.native12->releaseBorrowedUiTextures();
            state.native12->resize(width, height);
            state.width = width; state.height = height;
            state.resetGeometryViewport();
            return state.createTargets(error);
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }
    if (!state.swapChain) { error = "Renderer is not initialized."; return false; }
    if (width == 0 || height == 0) return true;
    if (width == state.width && height == state.height) {
        // A previous resize/target allocation may have failed after releasing
        // the old surface. Permit retrying even when dimensions did not change.
        if (!state.backbuffer) return state.createTargets(error);
        return ensureSceneTargetSize(width, height, error);
    }
    state.resetGeometryViewport();
    state.context->ClearState();
    state.context->Flush();
    if (state.rhiDevice) state.sceneCommands().endRenderPass();
    state.swapchainImage.reset();
    state.backbuffer.Reset();
    if (failed(state.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize swap chain", error)) {
        // DXGI can retain its previous buffer on failure. Reacquire it without
        // hiding the original failure; device removal may also prevent recovery.
        std::string recoveryError;
        if (!state.createTargets(recoveryError)) error += " Target recovery: " + recoveryError;
        return false;
    }
    state.width = width; state.height = height;
    return state.createTargets(error);
}

bool SceneRenderer::ensureSceneTargetSize(std::uint32_t width, std::uint32_t height, std::string& error) {
    error.clear();
    auto& state = *impl_;
    if (!state.rhiDevice || !state.swapchainImage) { error = "Renderer is not initialized."; return false; }
    if (width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        error = "Scene target dimensions must be between 1 and 16384 pixels.";
        return false;
    }
    if (state.sceneColorImage && width == state.sceneWidth && height == state.sceneHeight) return true;
    if (!state.createSceneTargets(width, height, error)) return false;
    state.resetGeometryViewport();
    ++state.giHistoryEpoch;
    return true;
}

void SceneRenderer::releaseRetiredSceneTargets() {
    if (impl_->native12 && (!impl_->retiredSceneTargets.empty() || !impl_->retiredPreviews.empty()))
        impl_->native12->waitIdle();
    impl_->retiredSceneTargets.clear();
    impl_->retiredPreviews.clear();
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
    ++impl_->usageEpoch;
    impl_->raySceneActive = false;
    if (impl_->native12) {
        try {
            impl_->swapchainImage = impl_->native12->backbuffer();
            rhi::RenderPassDesc pass;
            pass.name = "Window clear";
            pass.colors = {{impl_->swapchainImage, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                {color[0], color[1], color[2], color[3]}}};
            impl_->sceneCommands().beginRenderPass(pass);
            impl_->sceneCommands().endRenderPass();
            return true;
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }
    if (!impl_->context || !impl_->backbuffer) {
        error = "Renderer is not initialized.";
        return false;
    }
    ID3D11RenderTargetView* target = impl_->backbuffer.Get();
    impl_->context->OMSetRenderTargets(1, &target, nullptr);
    impl_->context->ClearRenderTargetView(target, color);
    return true;
}

bool SceneRenderer::renderPreviewFrame(const SceneSettings& settings, const scene::World& world,
                                       const assets::AssetLibrary& assets,
                                       const assets::MaterialAsset* materialOverride,
                                       std::uint32_t size, std::string& error, PreviewSlot slot, std::uint32_t height) {
    error.clear();
    auto& state = *impl_;
    if (!state.rhiDevice || !state.sceneColorImage || !state.swapchainImage) {
        error = "Renderer is not initialized.";
        return false;
    }
    const auto slotIndex = static_cast<std::size_t>(slot);
    if (slotIndex >= state.previews.size()) { error = "Invalid preview slot."; return false; }
    if (std::min(state.sceneWidth, state.sceneHeight) < 16) { error = "Scene target is too small for a preview."; return false; }
    size = std::max(16u, size);
    height = height ? std::max(16u, height) : size;
    const double scale = std::min({1.0, static_cast<double>(state.sceneWidth)/size,
        static_cast<double>(state.sceneHeight)/height});
    size = std::max(1u, static_cast<std::uint32_t>(size*scale));
    height = std::max(1u, static_cast<std::uint32_t>(height*scale));
    auto& image = state.previews[slotIndex];
    Impl::PreviewImage candidate;
    const bool replaceImage = (!image.texture && !image.image) || image.width != size || image.height != height;
    if (replaceImage) {
        if (state.native12) {
            try { candidate.image = state.rhiDevice->createTexture({size, height, 1,
                rhi::Format::RGBA8Unorm, true, "Tool preview"}); }
            catch (const std::exception& failure) { error = failure.what(); return false; }
        } else {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = size;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (failed(state.device->CreateTexture2D(&description, nullptr, &candidate.texture),
                   "Create preview texture", error)) return false;
        if (failed(state.device->CreateShaderResourceView(candidate.texture.Get(), nullptr, &candidate.view),
                   "Create preview view", error)) return false;
        }
        candidate.width = size; candidate.height = height;
    }
    auto previewSettings = settings;
    previewSettings.viewportWidth = static_cast<int>(size);
    previewSettings.viewportHeight = static_cast<int>(height);
    if (!render(previewSettings, world, assets, error, materialOverride)) return false;
    if (state.native12) {
        try { state.native12->copyTextureRegion(replaceImage ? candidate.image : image.image,
            state.sceneColorImage, 0, 0, size, height); }
        catch (const std::exception& failure) { error = failure.what(); return false; }
    } else {
        const D3D11_BOX region{0, 0, 0, size, height, 1};
        state.context->CopySubresourceRegion(replaceImage ? candidate.texture.Get() : image.texture.Get(), 0, 0, 0, 0,
            state.sceneTexture.Get(), 0, &region);
    }
    if (replaceImage) {
        if (image.view || image.image) state.retiredPreviews.push_back(std::move(image));
        image = std::move(candidate);
    }
    return true;
}

bool SceneRenderer::renderCameraPreview(const scene::World& world, const assets::AssetLibrary& assets,
    const camera::Camera& camera, std::uint32_t width, std::uint32_t height, std::string& error,
    float exposure, PreviewSlot slot) {
    if (!camera::valid(camera)) { error = "Invalid camera pose or lens."; return false; }
    SceneSettings settings;
    settings.offscreenTarget = true;
    settings.antiAliasing.method = AntiAliasingMethod::FXAA;
    settings.cameraPose = camera.pose;
    settings.cameraLens = camera.lens;
    settings.exposure = exposure;
    settings.viewId = 0x100u + static_cast<std::uint64_t>(slot);
    return renderPreviewFrame(settings, world, assets, nullptr, width, error, slot, height);
}

bool SceneRenderer::renderViewportPreview(const SceneSettings& requested, const scene::World& world,
    const assets::AssetLibrary& assets, const assets::MaterialAsset* materialOverride,
    std::uint32_t width, std::uint32_t height, std::string& error, PreviewSlot slot) {
    if (slot < PreviewSlot::Viewport0 || slot > PreviewSlot::Viewport3) {
        error = "Viewport preview requires its own viewport slot."; return false;
    }
    auto settings = requested;
    settings.offscreenTarget = true;
    settings.editorViewport = true;
    settings.viewportVisible = true;
    settings.viewportX = settings.viewportY = 0;
    settings.hardwareRayTracing = false;
    settings.globalIllumination.method = GlobalIlluminationMethod::Disabled;
    return renderPreviewFrame(settings, world, assets, materialOverride, width, error, slot, height);
}

bool SceneRenderer::renderMaterialPreview(const assets::MaterialAsset& material, std::uint32_t size,
                                          std::string& error, PreviewSlot slot) {
    return renderMaterialPreview(material, size, 0.65f, 0.18f, 3.0f, error, slot);
}

bool SceneRenderer::renderMaterialPreview(const assets::MaterialAsset& material, std::uint32_t size,
                                          float yaw, float pitch, float distance, std::string& error, PreviewSlot slot) {
    auto& state = *impl_;
    if (!state.previewSceneReady) {
        state.previewAssets = state.defaultAssets;
        state.previewWorld.createActor("Preview", 1, 1);
        state.previewSceneReady = true;
    }
    // The tool library is isolated from the user's committed assets. Both the
    // actor reference and library entry must match the requested material ID:
    // resolveMaterial deliberately requires an existing matching asset.
    state.previewWorld.actors.front().mesh.materialId = material.id;
    state.previewAssets.materials.assign(1, material);
    SceneSettings settings;
    settings.editorViewport = false;
    settings.offscreenTarget = true;
    settings.antiAliasing.method = AntiAliasingMethod::FXAA;
    settings.viewportVisible = true;
    settings.viewportX = 0;
    settings.viewportY = 0;
    settings.viewportWidth = static_cast<int>(size);
    settings.viewportHeight = static_cast<int>(size);
    settings.path = RenderPath::Forward;
    settings.rayTracing = false;
    settings.shadowMethod = ShadowMethod::Disabled;
    settings.cameraYaw = yaw;
    settings.cameraPitch = pitch;
    settings.cameraDistance = distance;
    settings.cameraLens.farClip = std::max(100.0f, distance*2.0f);
    settings.cameraTarget[0] = 0.0f;
    settings.cameraTarget[1] = 0.0f;
    settings.cameraTarget[2] = 0.0f;
    return renderPreviewFrame(settings, state.previewWorld, state.previewAssets, &material, size, error, slot);
}

bool SceneRenderer::renderScenePreview(const scene::World& world, const assets::AssetLibrary& assets,
                                       std::uint32_t size, std::string& error, PreviewSlot slot,
                                       float yaw, float pitch, float zoom) {
    error.clear();
    float minimum[3] = {1e30f, 1e30f, 1e30f};
    float maximum[3] = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (const auto& actor : world.actors) {
        if (actor.light || !actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh || mesh->vertices.empty()) continue;
        const auto& bounds = mesh->localBounds();
        if (!bounds.valid) continue;
        const XMMATRIX matrix = actorMatrix(actor);
        // An affine transform of the cached local AABB conservatively bounds
        // every vertex, without traversing a large mesh for each preview frame.
        for (unsigned corner = 0; corner < 8; ++corner) {
            const XMVECTOR position = XMVector3TransformCoord(
                XMVectorSet((corner & 1) ? bounds.maximum[0] : bounds.minimum[0],
                    (corner & 2) ? bounds.maximum[1] : bounds.minimum[1],
                    (corner & 4) ? bounds.maximum[2] : bounds.minimum[2], 1.0f), matrix);
            minimum[0] = std::min(minimum[0], XMVectorGetX(position));
            minimum[1] = std::min(minimum[1], XMVectorGetY(position));
            minimum[2] = std::min(minimum[2], XMVectorGetZ(position));
            maximum[0] = std::max(maximum[0], XMVectorGetX(position));
            maximum[1] = std::max(maximum[1], XMVectorGetY(position));
            maximum[2] = std::max(maximum[2], XMVectorGetZ(position));
            any = true;
        }
    }
    if (!any) { error = "The scene has no visible geometry to preview."; return false; }
    const float extent[3] = {maximum[0] - minimum[0], maximum[1] - minimum[1], maximum[2] - minimum[2]};
    const float radius = std::max(0.5f, 0.5f * std::sqrt(extent[0] * extent[0] + extent[1] * extent[1] + extent[2] * extent[2]));
    SceneSettings settings;
    settings.editorViewport = false;
    settings.offscreenTarget = true;
    settings.antiAliasing.method = AntiAliasingMethod::FXAA;
    settings.viewportVisible = true;
    settings.viewportX = 0;
    settings.viewportY = 0;
    settings.viewportWidth = static_cast<int>(size);
    settings.viewportHeight = static_cast<int>(size);
    settings.path = RenderPath::Forward;
    settings.rayTracing = false;
    settings.shadowMethod = ShadowMethod::Disabled;
    settings.cameraTarget[0] = (minimum[0] + maximum[0]) * 0.5f;
    settings.cameraTarget[1] = (minimum[1] + maximum[1]) * 0.5f;
    settings.cameraTarget[2] = (minimum[2] + maximum[2]) * 0.5f;
    settings.cameraYaw = yaw;
    settings.cameraPitch = std::clamp(pitch, -1.45f, 1.45f);
    settings.cameraDistance = std::clamp(radius * 2.6f * std::clamp(zoom, 0.35f, 4.0f), 1.5f, 80.0f);
    settings.cameraLens.farClip = std::max(100.0f, settings.cameraDistance*2.0f);
    return renderPreviewFrame(settings, world, assets, nullptr, size, error, slot);
}

void* SceneRenderer::previewTexture(PreviewSlot slot) const {
    const auto slotIndex = static_cast<std::size_t>(slot);
    if (impl_->native12 && slotIndex < impl_->previews.size() && impl_->previews[slotIndex].image)
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(impl_->native12->uiTextureId(impl_->previews[slotIndex].image)));
    return slotIndex < impl_->previews.size() ? impl_->previews[slotIndex].view.Get() : nullptr;
}

void* SceneRenderer::materialPreviewTexture() const {
    return previewTexture(PreviewSlot::MaterialEditor);
}

bool SceneRenderer::readMaterialPreviewPixels(std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                                              std::uint32_t& height, std::string& error) const {
    return readPreviewPixels(PreviewSlot::MaterialEditor, pixels, width, height, error);
}

bool SceneRenderer::readPreviewPixels(PreviewSlot slot, std::vector<std::uint8_t>& pixels,
                                      std::uint32_t& width, std::uint32_t& height, std::string& error) const {
    error.clear();
    pixels.clear();
    width = height = 0;
    auto& state = *impl_;
    const auto slotIndex = static_cast<std::size_t>(slot);
    if (slotIndex >= state.previews.size()) { error = "Invalid preview slot."; return false; }
    const auto& image = state.previews[slotIndex];
    if (state.native12 && image.image) {
        try {
            const auto readback = state.native12->readTexture(image.image);
            width = readback.width; height = readback.height;
            pixels.resize(static_cast<std::size_t>(width)*height*4);
            for (std::uint32_t row = 0; row < height; ++row)
                std::memcpy(pixels.data() + static_cast<std::size_t>(row)*width*4,
                    readback.bytes.data() + static_cast<std::size_t>(row)*readback.rowPitch, width*4);
            return true;
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }
    if (!state.device || !state.context || !image.texture) {
        error = "No image has been rendered in this preview slot yet.";
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    image.texture->GetDesc(&description);
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (failed(state.device->CreateTexture2D(&stagingDescription, nullptr, &staging),
               "Create preview staging texture", error)) return false;
    state.context->CopyResource(staging.Get(), image.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (failed(state.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
               "Map preview staging texture", error)) return false;
    width = description.Width;
    height = description.Height;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * width * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(width) * 4);
    }
    state.context->Unmap(staging.Get(), 0);
    return true;
}

bool SceneRenderer::render(const SceneSettings& settings, const scene::World& world,
                           const assets::AssetLibrary& assets, std::string& error,
                           const assets::MaterialAsset* materialOverride) {
    try {
    error.clear();
    auto& state = *impl_;
    ++state.usageEpoch;
    if (state.native12) state.swapchainImage = state.native12->backbuffer();
    state.raySceneActive = false;
    const bool diagnosticMainScene = !settings.offscreenTarget;
    if (diagnosticMainScene) state.beginGeometryFrame(world, assets);
    if (!state.rhiDevice || !state.swapchainImage) {
        if (diagnosticMainScene) state.failGeometryFrame();
        error = "Renderer has no render target."; return false;
    }
    // Runtime/fullscreen rendering targets the main swap chain. The editor's
    // detached scene target may have a different size from the previous frame.
    if (!settings.editorViewport && !settings.offscreenTarget &&
        !ensureSceneTargetSize(state.width, state.height, error)) {
        state.failGeometryFrame();
        return false;
    }
    const float editorClear[4] = {0.025f, 0.035f, 0.055f, 1.0f};
    if (settings.editorViewport && (!settings.viewportVisible || settings.viewportWidth < 1 || settings.viewportHeight < 1)) {
        if (diagnosticMainScene) state.resetGeometryViewport();
        return clear(editorClear, error);
    }
    struct GeometryFrameScope {
        Impl& state;
        bool mainScene, completed = false;
        ~GeometryFrameScope() { if (mainScene && !completed) state.failGeometryFrame(); }
    } geometryFrameScope{state, diagnosticMainScene};
    ID3D11ShaderResourceView* noIndirect = nullptr;
    if (state.context) state.context->PSSetShaderResources(9, 1, &noIndirect);
    if (diagnosticMainScene) {
        state.hardwareActive = false;
        if (settings.hardwareRayTracing && !state.hardwareRequested) {
            state.hardwareRuntimeFailed = false;
            ++state.giHistoryEpoch;
        }
        state.hardwareRequested = settings.hardwareRayTracing;
        state.giStatus.requested = settings.globalIllumination.method;
        state.giStatus.effective = GlobalIlluminationMethod::Disabled;
        state.giStatus.requestedMode = settings.globalIllumination.mode;
        state.giStatus.effectiveMode = GlobalIlluminationMode::Progressive;
        state.giStatus.accumulatedSamples = 0;
        state.giStatus.targetSamples = settings.globalIllumination.targetSamples;
        state.giStatus.reason = settings.globalIllumination.method == GlobalIlluminationMethod::Disabled
            ? "Global illumination disabled."
            : !settings.hardwareRayTracing ? "Path tracing requires hardware ray tracing."
            : state.hardwareStatus;
    }
    rae::diagnostics::TraceScope submitTrace(diagnosticMainScene ? "Main scene submit" : "Tool preview submit", "Renderer CPU");
    state.beginDiagnostics(diagnosticMainScene);
    struct DiagnosticScope {
        Impl& state;
        bool mainScene;
        ~DiagnosticScope() { if (mainScene) state.finishDiagnostics(); }
    } diagnosticScope{state, diagnosticMainScene};
    const bool sceneTarget = settings.editorViewport || settings.offscreenTarget;
    const auto targetWidth = sceneTarget ? state.sceneWidth : state.width;
    const auto targetHeight = sceneTarget ? state.sceneHeight : state.height;
    int viewportX = settings.viewportWidth > 0 ? std::clamp(settings.viewportX, 0, static_cast<int>(targetWidth) - 1) : 0;
    int viewportY = settings.viewportHeight > 0 ? std::clamp(settings.viewportY, 0, static_cast<int>(targetHeight) - 1) : 0;
    int viewportWidth = settings.viewportWidth > 0 ? std::clamp(settings.viewportWidth, 1, static_cast<int>(targetWidth) - viewportX) : static_cast<int>(targetWidth);
    int viewportHeight = settings.viewportHeight > 0 ? std::clamp(settings.viewportHeight, 1, static_cast<int>(targetHeight) - viewportY) : static_cast<int>(targetHeight);
    // Runtime callers omit a rectangle. Honour authored framing there as well
    // as in the editor, whose explicit image rectangle already includes bars.
    const bool fitAuthoredAspect = settings.viewportWidth <= 0 && settings.viewportHeight <= 0
        && camera::valid(settings.cameraLens) && settings.cameraLens.aspectRatio > 0;
    if (fitAuthoredAspect) {
        const double aspect = settings.cameraLens.aspectRatio;
        if (static_cast<double>(viewportWidth)/viewportHeight > aspect) {
            viewportWidth = std::clamp(static_cast<int>(std::lround(viewportHeight*aspect)),1,viewportWidth);
            viewportX = (static_cast<int>(targetWidth)-viewportWidth)/2;
        } else {
            viewportHeight = std::clamp(static_cast<int>(std::lround(viewportWidth/aspect)),1,viewportHeight);
            viewportY = (static_cast<int>(targetHeight)-viewportHeight)/2;
        }
    }
    const auto cameraView = camera::evaluate(resolveCamera(settings),
        static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight));
    if (diagnosticMainScene) state.beginFrameCapture(settings, cameraView.lens);
    XMFLOAT4X4 viewValues, projectionValues;
    std::memcpy(&viewValues, cameraView.view.data(), sizeof(viewValues));
    std::memcpy(&projectionValues, cameraView.projection.data(), sizeof(projectionValues));
    const XMMATRIX view = XMLoadFloat4x4(&viewValues);
    XMMATRIX projection = XMLoadFloat4x4(&projectionValues);
    const XMVECTOR eye = XMVectorSet(cameraView.position[0], cameraView.position[1], cameraView.position[2], 1);
    const bool wantsRayScene = !state.native12 && (settings.rayTracing || settings.shadowMethod == ShadowMethod::AnalyticRay);
    const auto lighting = gatherLighting(world, settings.lightIntensity);
    const bool hardwareImageExpected = diagnosticMainScene && settings.hardwareRayTracing &&
        state.hardwarePipelineReady && state.hardwareRayTracing && state.hardwareRayTracing->supported() &&
        !state.hardwareRuntimeFailed && std::all_of(world.actors.begin(),world.actors.end(),[&](const scene::Actor& actor) {
            if (!actor.visible || actor.light || actor.camera || !validTransform(actor)) return true;
            const auto* mesh=assets.findMesh(actor.mesh.meshId);
            if (!mesh) return true;
            bool compatible=true;
            meshAttributes::sections(*mesh,[&](std::uint32_t,std::uint32_t count,std::uint32_t slot) {
                const auto* material=resolveMaterial(assets,actor.mesh.materialForSlot(*mesh,slot),materialOverride);
                if(count && material && !material->hardwareDepthCompatible()) compatible=false;
            });
            return compatible;
        });
    const auto aaPolicy = state.resolveAntiAliasing(settings,hardwareImageExpected);
    const bool antiAliasing = aaPolicy.effective != AntiAliasingMethod::Disabled;
    const bool temporalAntiAliasing = aaPolicy.effective == AntiAliasingMethod::TAA;
    const XMFLOAT4 aaViewport(static_cast<float>(viewportX),static_cast<float>(viewportY),
        static_cast<float>(viewportWidth),static_cast<float>(viewportHeight));
    if (antiAliasing) state.ensureAntiAliasingTargets(targetWidth,targetHeight,temporalAntiAliasing);
    if (temporalAntiAliasing)
        state.prepareTemporalAntiAliasing(settings,world,assets,materialOverride,cameraView,lighting,aaViewport,projection);
    else if (diagnosticMainScene && state.aaHistoryValid) state.invalidateAntiAliasing();
    if (diagnosticMainScene) {
        state.aaStatus = aaPolicy;
        state.aaStatus.historyRevision = state.aaHistoryRevision;
    }
    const auto aaConstants = state.antiAliasingConstants(settings,cameraView,view*projection,aaViewport);
    state.raySceneActive = wantsRayScene;
    if (wantsRayScene) state.rayLastUsed = Impl::Clock::now();
    if (wantsRayScene && !state.prepareRayTriangles(world, assets, error, materialOverride)) return false;
    SceneConstants data{};
    XMStoreFloat4x4(&data.viewProjection, view * projection);
    XMStoreFloat4(&data.cameraPosition, eye);
    // cameraPosition.w is unused by the shaders; reuse it as the "solid black
    // backdrop" flag so the scene format can switch the sky gradient off
    // without changing the locked SceneConstants layout.
    data.cameraPosition.w = world.blackBackground ? 1.0f : 0.0f;
    data.cameraForward = XMFLOAT4(cameraView.forward[0], cameraView.forward[1], cameraView.forward[2],
        cameraView.lens.projection == camera::Projection::Orthographic ? 1.0f : 0.0f);
    data.cameraLensParams = XMFLOAT4(cameraView.lens.nearClip, cameraView.lens.farClip,
        cameraView.lens.orthographicHeight, std::tan(XMConvertToRadians(cameraView.lens.verticalFovDegrees)*.5f));
    data.baseColor = XMFLOAT4(settings.baseColor[0], settings.baseColor[1], settings.baseColor[2], 1);
    XMStoreFloat4(&data.lightDirectionIntensity, XMVector3Normalize(XMVectorSet(-3, 6, -2, 0)));
    data.lightDirectionIntensity.w = world.environment.overrideLighting
        ? world.environment.directionalIntensity : settings.lightIntensity;
    data.parameters = XMFLOAT4(std::clamp(settings.roughness, 0.04f, 1.0f),
        std::clamp(settings.metallic, 0.0f, 1.0f), settings.exposure,
        (!state.native12 && settings.rayTracing && rayTracingSupported(world, assets)) ? 1.0f : 0.0f);
    data.viewportDebug = XMFLOAT4(static_cast<float>(targetWidth), static_cast<float>(targetHeight),
        static_cast<float>(settings.debugView), static_cast<float>((settings.shadowMethod == ShadowMethod::Disabled ||
            (state.native12 && settings.shadowMethod == ShadowMethod::AnalyticRay)) ? 2 :
            settings.shadowMethod == ShadowMethod::AnalyticRay ? 1 : 0));
    state.uploadConstants(state.sceneBuffer, data);
    XMVECTOR lightDirection = XMVector3Normalize(XMVectorSet(-3, 6, -2, 0));
    if (lighting.info[1] > .5f && lighting.info[2] >= 0) {
        const auto& sun = lighting.lights[static_cast<std::size_t>(lighting.info[2])];
        lightDirection = XMVectorSet(-sun.directionRange[0], -sun.directionRange[1], -sun.directionRange[2], 0);
    }
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
        settings.shadowMethod == ShadowMethod::ShadowMapPCF16 ? 5.0f :
        useCsm ? 4.0f : 0.0f;
    shadowData.parameters = XMFLOAT4(1.0f / static_cast<float>(Impl::shadowMapSize), 0.0015f, 0.008f, shadowFilter);
    const float shadowFeature = settings.shadowMethod == ShadowMethod::GTAO ? 1.0f :
        settings.shadowMethod == ShadowMethod::ContactShadow ? 2.0f : 0.0f;
    const float cascadeSplit = cameraView.lens.nearClip + (cameraView.lens.farClip-cameraView.lens.nearClip)*.1f;
    shadowData.cascadeParameters = XMFLOAT4(cascadeSplit, useCsm ? 1.0f : 0.0f, shadowFeature, 0);
    XMStoreFloat4x4(&shadowData.lightViewProjectionCascade1, lightView * lightProjection);
    if (useCsm) {
        const XMVECTOR forward = XMVectorSet(cameraView.forward[0],cameraView.forward[1],cameraView.forward[2],0);
        const XMVECTOR right = XMVectorSet(cameraView.right[0],cameraView.right[1],cameraView.right[2],0);
        const XMVECTOR up = XMVectorSet(cameraView.up[0],cameraView.up[1],cameraView.up[2],0);
        const float aspect = cameraView.aspectRatio;
        const float tanHalfFov = data.cameraLensParams.w;
        const float splitDistances[3] = {cameraView.lens.nearClip, cascadeSplit, cameraView.lens.farClip};
        const auto makeCorners = [&](float distance, std::array<XMVECTOR, 4>& corners) {
            const XMVECTOR center = XMVectorAdd(eye, XMVectorScale(forward, distance));
            const float halfHeight = cameraView.lens.projection == camera::Projection::Orthographic
                ? cameraView.lens.orthographicHeight*.5f : tanHalfFov * distance;
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
    state.uploadConstants(state.shadowBuffer, shadowData);
    RayConstants rays{};
    if (data.parameters.w > 0.5f || settings.shadowMethod == ShadowMethod::AnalyticRay) {
        for (const auto& actor : world.actors) {
            if (actor.light || !actor.visible || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            const auto* material = mesh ? resolveMaterial(assets, actor.mesh.materialForSlot(*mesh, 0), materialOverride) : nullptr;
            if (!mesh || !material) continue;
            // Analytic tracing has exact intersections for built-in spheres,
            // planes, and cubes. Other actors are represented by rayTriangles.
            if (!meshAttributes::analytic(*mesh)) continue;
            if (rays.info.x >= 32.0f) break;
            auto& object = rays.objects[static_cast<int>(rays.info.x++)];
            const auto inverse = XMMatrixInverse(nullptr, actorMatrix(actor));
            XMStoreFloat4x4(&object.inverseWorld, inverse);
            XMStoreFloat4x4(&object.normalTransform, XMMatrixTranspose(inverse));
            object.color = XMFLOAT4(material->baseColor[0], material->baseColor[1], material->baseColor[2], 1);
            const float primitive = mesh->primitive == assets::MeshPrimitive::Sphere ? 0.0f :
                mesh->primitive == assets::MeshPrimitive::Plane ? 1.0f : 2.0f;
            object.properties = XMFLOAT4(material->roughness, material->metallic, primitive, materialFlags(*material));
        }
    }
    rays.info.y = wantsRayScene ? static_cast<float>(state.rayTriangleCount) : 0.0f;
    state.uploadConstants(state.rayBuffer, rays);
    const rhi::BufferHandle constants[] = {state.sceneBuffer, state.objectBuffer, state.rayBuffer, state.shadowBuffer};
    auto& commands = state.sceneCommands();
    commands.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, constants);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, constants);
    state.uploadConstants(state.lightingBuffer, lighting);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 7, std::span(&state.lightingBuffer, 1));
    if (state.context) {
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
    }
    ID3D11ShaderResourceView* emptyViews[5]{};
    // A visible material with a raster-only depth state keeps hardware primary
    // visibility and GI inconsistent with the raster receiver. The request is
    // retained and the reason is reported; the raster path still renders.
    bool hardwareDepthCompatible = true;
    const assets::MaterialAsset* incompatibleDepthMaterial = nullptr;
    for (const auto& actor : world.actors) {
        if (actor.light || !actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh || mesh->indices.empty()) continue;
        meshAttributes::sections(*mesh, [&](std::uint32_t, std::uint32_t count, std::uint32_t slot) {
            const auto* material = resolveMaterial(assets, actor.mesh.materialForSlot(*mesh, slot), materialOverride);
            if (count && material && !material->hardwareDepthCompatible()) {
                hardwareDepthCompatible = false; incompatibleDepthMaterial = material;
            }
        });
        if (!hardwareDepthCompatible) break;
    }
    const bool hardwareBackendAllowed = state.native12
        ? (state.hardwareRayTracing && state.hardwareRayTracing->nativeMode())
        : state.backendSelection.allowD3D12Bridge;
    if (diagnosticMainScene) {
        if (hardwareDepthCompatible != state.hardwareDepthWasCompatible) ++state.giHistoryEpoch;
        state.hardwareDepthWasCompatible = hardwareDepthCompatible;
        if (settings.hardwareRayTracing && hardwareBackendAllowed && !hardwareDepthCompatible) {
            state.hardwareStatus = "Hardware ray tracing inactive: material '" + incompatibleDepthMaterial->name +
                "' uses a raster-only depth state. Use ZTest Less/LessEqual and ZWrite On for DXR/GI.";
            if (settings.globalIllumination.method != GlobalIlluminationMethod::Disabled)
                state.giStatus.reason = state.hardwareStatus;
        }
    }
    // Resolve per-frame capabilities, then compose the pass list declaratively.
    // Everything below executes what the declaration contains; no pass is
    // reached through a hidden branch outside this list.
    const bool realtimeGiRequested = settings.globalIllumination.method == GlobalIlluminationMethod::PathTracing &&
        settings.globalIllumination.mode == GlobalIlluminationMode::Realtime;
    const bool rasterShadowMap = settings.shadowMethod != ShadowMethod::AnalyticRay &&
        settings.shadowMethod != ShadowMethod::Disabled &&
        (lighting.info[1] < .5f || lighting.info[2] >= 0);
    const bool hardwareTransport = diagnosticMainScene && hardwareDepthCompatible && hardwareBackendAllowed &&
        settings.hardwareRayTracing && state.hardwarePipelineReady && state.hardwareRayTracing &&
        state.hardwareRayTracing->supported() && !state.hardwareRuntimeFailed;
    const bool realtimeIndirect = hardwareTransport && realtimeGiRequested && settings.debugView == 0;
    const bool depthOfField = !state.native12 && depthOfFieldEligible(cameraView.lens) && settings.debugView <= 1;
    const bool selectionOutline = settings.editorViewport && settings.outlineActors &&
        settings.outlineActorCount > 0;
    PipelineBuildInputs pipelineInputs;
    pipelineInputs.path = settings.path;
    pipelineInputs.rasterShadowMap = rasterShadowMap;
    pipelineInputs.rayTriangleScene = wantsRayScene;
    pipelineInputs.hardwareTransport = hardwareTransport;
    pipelineInputs.realtimeIndirect = realtimeIndirect;
    pipelineInputs.depthOfField = depthOfField;
    pipelineInputs.antiAliasing = antiAliasing;
    pipelineInputs.temporalAntiAliasing = temporalAntiAliasing;
    pipelineInputs.selectionOutline = selectionOutline && !state.native12;
    pipelineInputs.frameCapture = diagnosticMainScene && state.captureRequested;
    auto pipeline = buildScenePipeline(pipelineInputs);
    const auto pipelineValidation = validatePipeline(pipeline);
    if (!pipelineValidation.valid) {
        error = "Render pipeline declaration is inconsistent: " + pipelineValidation.error;
        return false;
    }
    if (diagnosticMainScene) state.pipeline = pipeline;

    const D3D11_VIEWPORT sceneViewport{static_cast<float>(viewportX), static_cast<float>(viewportY),
        static_cast<float>(viewportWidth), static_cast<float>(viewportHeight), 0, 1};
    ID3D11RenderTargetView* backbuffer = (settings.editorViewport || settings.offscreenTarget)
        ? state.sceneTarget.Get() : state.backbuffer.Get();
    ID3D11RenderTargetView* finalBackbuffer = backbuffer;
    if (antiAliasing && state.device) backbuffer = d3d11::nativeColorTarget(state.aaSourceImage);
    const bool blackClear = world.blackBackground || fitAuthoredAspect;
    const float backgroundClear[4] = {blackClear ? 0.0f : 0.025f, blackClear ? 0.0f : 0.035f,
        blackClear ? 0.0f : 0.055f, 1.0f};
    // Realtime GI publishes a resolved indirect image consumed by either raster
    // path. The transport pass overwrites this binding when it produced one.
    GlobalIlluminationConstants giData{};
    giData.viewport = XMFLOAT4(0.0f, static_cast<float>(viewportWidth),
        static_cast<float>(viewportHeight), 0.0f);
    state.uploadConstants(state.giBuffer, giData);
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel, 6, std::span(&state.giBuffer, 1));
    bool hardwareFrameReady = false;
    bool thinLensImage = false;
    ID3D11ShaderResourceView* indirectView = nullptr;
    const auto drawMainGeometry = [&](unsigned shader) {
        if (diagnosticMainScene) state.beginGeometrySample();
        struct SampleScope {
            Impl& state;
            bool mainScene;
            ~SampleScope() { if (mainScene) state.endGeometrySample(); }
        } sampleScope{state, diagnosticMainScene};
        return state.drawGeometry(shader, world, assets, error, materialOverride,
            settings.frustumCulling ? &data.viewProjection : nullptr);
    };
    // Raster passes own their input binding: the shadow map must not be bound as
    // an SRV while the shadow pass still writes it as a depth target.
    const auto beginRasterPasses = [&] {
        if (state.context) {
        ID3D11ShaderResourceView* shadowResources[] = {nullptr, nullptr, nullptr,
            nullptr, state.rayTriangleView.Get()};
        state.context->PSSetShaderResources(0, 5, shadowResources);
        ID3D11ShaderResourceView* uvResources[] = {state.rayUvView.Get(), state.rayUvIndexView.Get()};
        state.context->PSSetShaderResources(10, 2, uvResources);
        }
        if (diagnosticMainScene) state.prepareGeometryStatistics(hardwareFrameReady && !realtimeGiRequested);
        commands.setViewport({sceneViewport.TopLeftX, sceneViewport.TopLeftY, sceneViewport.Width, sceneViewport.Height});
    };
    const auto releaseRasterInputs = [&] {
        if (!state.context) return;
        ID3D11ShaderResourceView* noIndirectLighting = nullptr;
        state.context->PSSetShaderResources(9, 1, &noIndirectLighting);
        ID3D11ShaderResourceView* noUv[] = {nullptr, nullptr};
        state.context->PSSetShaderResources(10, 2, noUv);
    };

    const auto destinationImage = (settings.editorViewport || settings.offscreenTarget)
        ? state.sceneColorImage : state.swapchainImage;
    const auto rasterDestinationImage = antiAliasing ? state.aaSourceImage : destinationImage;
    const std::array<float, 4> clearColor{backgroundClear[0], backgroundClear[1], backgroundClear[2], backgroundClear[3]};
    struct RhiPassScope {
        Impl& state;
        bool complete = false;
        ~RhiPassScope() {
            state.sceneCommands().endRenderPass();
            if (!complete && state.context) {
                // Native postprocess/outline bridges can fail with no RHI pass
                // active. Release their temporary outputs/reads on that path.
                state.context->OMSetRenderTargets(0, nullptr, nullptr);
                ID3D11ShaderResourceView* empty[13]{};
                state.context->PSSetShaderResources(0, 13, empty);
                state.context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            }
        }
    } rhiPassScope{state};
    for (const auto& pass : pipeline.passes) {
        commands.endRenderPass();
        const auto diagnosticPassCount = state.diagnosticFrame.passes.size();
        if (diagnosticMainScene && pass.kind != RenderPassKind::FrameCapture)
            state.capturePassBoundary(pass, true, destinationImage, useCsm ? Impl::shadowCascadeCount : 1);
        switch (pass.kind) {
        case RenderPassKind::ShadowMap: {
            state.beginDiagnosticPass(pass.name);
            passes::ShadowPassContext shadowPass;
            shadowPass.commands = &commands;
            shadowPass.shadowConstants = state.shadowBuffer;
            const std::uint32_t cascadeCount = useCsm ? Impl::shadowCascadeCount : 1;
            shadowPass.depthArray = state.shadowImage;
            shadowPass.cascadeCount = cascadeCount;
            shadowPass.mapSize = Impl::shadowMapSize;
            shadowPass.beginCascade = [&](std::uint32_t cascade) {
                SceneConstants shadowScene = data;
                shadowScene.viewProjection = cascade == 0
                    ? shadowData.lightViewProjection : shadowData.lightViewProjectionCascade1;
                state.uploadConstants(state.sceneBuffer, shadowScene);
            };
            shadowPass.draw = [&](std::uint32_t cascade, std::string& drawError) {
                const XMFLOAT4X4& clip = cascade == 0
                    ? shadowData.lightViewProjection : shadowData.lightViewProjectionCascade1;
                return state.drawGeometry(7, world, assets, drawError, materialOverride,
                    settings.frustumCulling ? &clip : nullptr);
            };
            shadowPass.restoreSceneConstants = [&] {
                state.uploadConstants(state.sceneBuffer, data);
            };
            if (!passes::executeShadowPass(shadowPass, error)) return false;
            break;
        }
        case RenderPassKind::HardwareTransport: {
            state.beginDiagnosticPass(realtimeGiRequested
                ? "DXR realtime GI transport / resolve" : "DXR hardware scene");
            d3d12::Frame frame;
            frame.lighting = lighting;
            std::memcpy(frame.sceneConstants.data(), &data, sizeof(data));
            frame.sceneConstants[31] = settings.hardwareRayTracingReflections ? 1.0f : 0.0f;
            // Material footprint calculations need the actual viewport extent.
            frame.sceneConstants[32] = static_cast<float>(viewportWidth);
            frame.sceneConstants[33] = static_cast<float>(viewportHeight);
            XMFLOAT4X4 inverseViewProjection;
            XMStoreFloat4x4(&inverseViewProjection, XMMatrixInverse(nullptr, view * projection));
            std::memcpy(frame.inverseViewProjection.data(), &inverseViewProjection, sizeof(inverseViewProjection));
            frame.width = static_cast<std::uint32_t>(viewportWidth);
            frame.height = static_cast<std::uint32_t>(viewportHeight);
            frame.reflections = settings.hardwareRayTracingReflections;
            frame.shadows = settings.hardwareRayTracingShadows && settings.shadowMethod != ShadowMethod::Disabled;
            frame.globalIllumination = settings.globalIllumination;
            if (depthOfFieldEligible(cameraView.lens) && settings.debugView == 0 &&
                settings.globalIllumination.method == GlobalIlluminationMethod::PathTracing &&
                settings.globalIllumination.mode == GlobalIlluminationMode::Progressive) {
                const auto& dof = cameraView.lens.depthOfField;
                const float focalLength = dof.sensorHeightMm*.001f/(2*data.cameraLensParams.w);
                frame.thinLens = {dof.focusDistance,focalLength/(2*dof.fStop),1,0};
            }
            const auto& environment = world.environment;
            // Authored light Actors own lighting when present, even while hidden.
            // Do not also upload the legacy scene emitter in that mode.
            const bool usesLightActors = std::any_of(world.actors.begin(), world.actors.end(),
                [](const scene::Actor& actor) { return actor.light.has_value(); });
            if (!usesLightActors && environment.overrideLighting) {
                const auto& light = environment.areaLight;
                frame.areaLight = {light.center[0], light.center[1], light.center[2], light.enabled ? 1.0f : 0.0f,
                    light.halfAxisU[0], light.halfAxisU[1], light.halfAxisU[2], environment.environmentIntensity,
                    light.halfAxisV[0], light.halfAxisV[1], light.halfAxisV[2], 0,
                    light.radiance[0], light.radiance[1], light.radiance[2], 0};
            }
            frame.historyEpoch = state.giHistoryEpoch;
            frame.viewId = settings.viewId;
            if (state.context) {
                state.context->OMSetRenderTargets(0, nullptr, nullptr);
                state.context->PSSetShaderResources(0, 5, emptyViews);
            }
            std::string hardwareError;
            if (state.hardwareRayTracing->render(frame, world, assets, materialOverride, hardwareError)) {
                hardwareFrameReady = true;
                thinLensImage = hardwareFrameReady && settings.debugView == 0 &&
                    settings.globalIllumination.method == GlobalIlluminationMethod::PathTracing &&
                    !realtimeGiRequested;
                if (realtimeIndirect) {
                    if (state.native12) {
                        if (auto* indirect = state.hardwareRayTracing->nativeIndirect()) {
                            const auto dimensions = state.hardwareRayTracing->nativeOutputSize();
                            if (!state.nativeIndirectCopy ||
                                state.nativeIndirectCopy->description().width != dimensions.first ||
                                state.nativeIndirectCopy->description().height != dimensions.second)
                                state.nativeIndirectCopy = state.rhiDevice->createTexture(
                                    rhi::TextureDesc{dimensions.first, dimensions.second, 1,
                                        rhi::Format::RGBA16Float, true, "Native DXR indirect copy"});
                            const auto imported = state.native12->importTexture(indirect,
                                rhi::TextureDesc{dimensions.first, dimensions.second, 1,
                                    rhi::Format::RGBA16Float, false, "Native DXR indirect source"},
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                            state.native12->copyTextureRegion(state.nativeIndirectCopy, imported, 0, 0,
                                dimensions.first, dimensions.second);
                        }
                    } else {
                        indirectView = state.hardwareRayTracing->indirectLighting();
                    }
                }
                state.hardwareActive = true;
                state.hardwareStatus = state.hardwareRayTracing->status();
                state.giStatus = state.hardwareRayTracing->globalIlluminationStatus();
                if (!state.native12 && indirectView) {
                    giData.viewport = XMFLOAT4(1.0f, static_cast<float>(viewportWidth),
                        static_cast<float>(viewportHeight), 0.0f);
                    state.uploadConstants(state.giBuffer, giData);
                    state.context->PSSetShaderResources(9, 1, &indirectView);
                }
            } else {
                state.hardwareRuntimeFailed = true;
                state.hardwareStatus = "Hardware ray tracing failed; raster retained. " + hardwareError;
                state.giStatus.reason = state.hardwareStatus;
            }
            break;
        }
        case RenderPassKind::GBuffer: {
            state.beginDiagnosticPass(pass.name);
            beginRasterPasses();
            rhi::RenderPassDesc rasterPass;
            rasterPass.name = pass.name;
            for (const auto& image : state.gbufferImages)
                rasterPass.colors.push_back({image, rhi::LoadOp::Clear, rhi::StoreOp::Store, {}});
            rasterPass.depth = rhi::DepthAttachment{state.sceneDepthImage, 0, rhi::LoadOp::Clear};
            if (contains(pass.reads, PipelineResource::ShadowMap))
                rasterPass.reads.push_back({state.shadowImage, 3});
            commands.beginRenderPass(rasterPass);
            if (!drawMainGeometry(1)) return false;
            break;
        }
        case RenderPassKind::DeferredLighting: {
            state.beginDiagnosticPass(pass.name);
            rhi::RenderPassDesc rasterPass;
            rasterPass.name = pass.name;
            rasterPass.colors = {{rasterDestinationImage, rhi::LoadOp::Clear, rhi::StoreOp::Store, clearColor}};
            rasterPass.reads = {{state.gbufferImages[0], 0}, {state.gbufferImages[1], 1},
                {state.gbufferImages[2], 2}};
            if (contains(pass.reads, PipelineResource::ShadowMap))
                rasterPass.reads.push_back({state.shadowImage, 3});
            if (contains(pass.reads, PipelineResource::IndirectLighting) && state.nativeIndirectCopy)
                rasterPass.reads.push_back({state.nativeIndirectCopy, 9});
            commands.beginRenderPass(rasterPass);
            state.drawRhiFullscreen(2, false);
            commands.endRenderPass();
            releaseRasterInputs();
            break;
        }
        case RenderPassKind::ForwardBackground: {
            state.beginDiagnosticPass(pass.name);
            beginRasterPasses();
            rhi::RenderPassDesc rasterPass;
            rasterPass.name = pass.name;
            rasterPass.colors = {{rasterDestinationImage, rhi::LoadOp::Clear, rhi::StoreOp::Store, clearColor}};
            rasterPass.depth = rhi::DepthAttachment{state.sceneDepthImage, 0, rhi::LoadOp::Clear};
            if (contains(pass.reads, PipelineResource::ShadowMap))
                rasterPass.reads.push_back({state.shadowImage, 3});
            if (contains(pass.reads, PipelineResource::IndirectLighting) && state.nativeIndirectCopy)
                rasterPass.reads.push_back({state.nativeIndirectCopy, 9});
            commands.beginRenderPass(rasterPass);
            state.drawRhiFullscreen(3, true);
            break;
        }
        case RenderPassKind::ForwardGeometry: {
            state.beginDiagnosticPass(pass.name);
            rhi::RenderPassDesc rasterPass;
            rasterPass.name = pass.name;
            rasterPass.colors = {{rasterDestinationImage}};
            rasterPass.depth = rhi::DepthAttachment{state.sceneDepthImage};
            if (contains(pass.reads, PipelineResource::ShadowMap))
                rasterPass.reads.push_back({state.shadowImage, 3});
            if (contains(pass.reads, PipelineResource::IndirectLighting) && state.nativeIndirectCopy)
                rasterPass.reads.push_back({state.nativeIndirectCopy, 9});
            commands.beginRenderPass(rasterPass);
            if (!drawMainGeometry(0)) return false;
            commands.endRenderPass();
            releaseRasterInputs();
            break;
        }        case RenderPassKind::HardwareResolve: {
            // The pass is declared from the transport attempt; a failed attempt
            // keeps the raster image and reports the reason separately.
            if (!hardwareFrameReady || realtimeGiRequested) break;
            state.beginDiagnosticPass(pass.name);
            if (state.native12) {
                if (auto* output = state.hardwareRayTracing->nativeOutput()) {
                    const auto dimensions = state.hardwareRayTracing->nativeOutputSize();
                    const auto image = state.native12->importTexture(output,
                        rhi::TextureDesc{dimensions.first, dimensions.second, 1,
                            rhi::Format::RGBA8Unorm, false, "Native DXR output"},
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
                    state.native12->copyTextureRegionTo(rasterDestinationImage,
                        static_cast<std::uint32_t>(viewportX), static_cast<std::uint32_t>(viewportY),
                        image, 0, 0, static_cast<std::uint32_t>(viewportWidth),
                        static_cast<std::uint32_t>(viewportHeight));
                }
                break;
            }
            state.context->OMSetRenderTargets(0, nullptr, nullptr);
            ComPtr<ID3D11Resource> destination;
            backbuffer->GetResource(&destination);
            state.context->CopySubresourceRegion(destination.Get(), 0,
                static_cast<UINT>(viewportX), static_cast<UINT>(viewportY), 0,
                state.hardwareRayTracing->output(), 0, nullptr);
            state.context->OMSetRenderTargets(1, &backbuffer, nullptr);
            break;
        }
        case RenderPassKind::DepthOfField: {
            // Progressive hardware frames already resolved thin-lens depth of
            // field inside the path; the declared pass short-circuits.
            if (thinLensImage) break;
            state.beginDiagnosticPass(pass.name);
            if (!state.renderDepthOfField(backbuffer, sceneViewport, cameraView.lens, error)) return false;
            break;
        }
        case RenderPassKind::SelectionMask: {
            state.beginDiagnosticPass(pass.name);
            if (!state.outline) { error = "Selection outline is not initialized."; return false; }
            if (!state.outline->prepare(targetWidth, targetHeight, error)) return false;
            const auto drawSelected = [&] {
                return state.drawSelectedGeometry(settings.outlineActors, settings.outlineActorCount,
                    world, assets, error);
            };
            if (!state.outline->drawMasks(state.context.Get(), state.depth.Get(), sceneViewport,
                    state.geometryVS.Get(), state.inputLayout.Get(), state.outlineMaskPS.Get(),
                    state.constants.Get(), state.objectConstants.Get(), drawSelected, error)) return false;
            break;
        }
        case RenderPassKind::AntiAliasing: {
            state.beginDiagnosticPass(pass.name);
            state.executeAntiAliasing(destinationImage,aaConstants,temporalAntiAliasing);
            backbuffer = finalBackbuffer;
            break;
        }
        case RenderPassKind::AntiAliasingPresent: {
            state.beginDiagnosticPass(pass.name);
            state.presentTemporalAntiAliasing(destinationImage,aaConstants,view*XMLoadFloat4x4(&projectionValues));
            break;
        }
        case RenderPassKind::SelectionComposite: {
            state.beginDiagnosticPass(pass.name);
            if (!state.outline) { error = "Selection outline is not initialized."; return false; }
            state.outline->composite(state.context.Get(), backbuffer, state.depthView.Get(), sceneViewport,
                state.fullscreenVS.Get(), state.outlineCompositePS.Get(),
                settings.outlineColor, settings.outlineThickness, settings.outlineFrontObjects);
            state.recordDraw(0, 0, 0, 0, 3);
            break;
        }
        case RenderPassKind::FrameCapture: {
            // Close the timed pass list before the snapshot so the capture keeps
            // the CPU submit total of the rendered passes. Failure retains the
            // previous immutable snapshot and does not abort rendering.
            if (diagnosticMainScene) state.finishDiagnostics();
            if (state.context) state.context->OMSetRenderTargets(0, nullptr, nullptr);
            std::string captureError;
            if (!state.copyFrameCapture(settings, captureError)) {
                state.capture.status = "Capture failed: " + captureError + (state.capture.valid ? " Previous capture retained." : "");
                rae::diagnostics::runtimeDiagnostics().log(rae::diagnostics::MessageSeverity::Error, "Frame Inspector", state.capture.status);
            }
            state.captureRequested = false;
            if (state.context) state.context->OMSetRenderTargets(1, &backbuffer, nullptr);
            break;
        }
        }
        if (diagnosticMainScene && pass.kind != RenderPassKind::FrameCapture &&
            state.diagnosticFrame.passes.size() > diagnosticPassCount) {
            commands.endRenderPass();
            state.endDiagnosticPass();
            state.capturePassBoundary(pass, false, destinationImage, useCsm ? Impl::shadowCascadeCount : 1);
            state.captureProduced |= pass.writes;
        }
    }
    if (diagnosticMainScene) {
        state.finishDiagnostics();
        // Record the three-layer feature policy for this frame. Requests are
        // copied from settings; capabilities come from the device, the material
        // scan and the transport attempt above. Neither layer rewrites the other.
        FeatureRequests featureRequests;
        featureRequests.path = settings.path;
        featureRequests.shadowMethod = settings.shadowMethod;
        featureRequests.rayTracing = settings.rayTracing;
        featureRequests.hardwareRayTracing = settings.hardwareRayTracing;
        featureRequests.hardwareRayTracingReflections = settings.hardwareRayTracingReflections;
        featureRequests.hardwareRayTracingShadows = settings.hardwareRayTracingShadows;
        featureRequests.globalIllumination = settings.globalIllumination.method;
        featureRequests.globalIlluminationMode = settings.globalIllumination.mode;
        featureRequests.depthOfField = depthOfFieldEligible(cameraView.lens) && settings.debugView <= 1;
        featureRequests.selectionOutline = selectionOutline;
        FeatureCapabilities featureCapabilities;
        featureCapabilities.nativeD3D12 = state.native12 != nullptr;
        featureCapabilities.analyticRayTracingAvailable = !state.native12;
        featureCapabilities.depthOfFieldAvailable = !state.native12;
        featureCapabilities.selectionOutlineAvailable = !state.native12;
        featureCapabilities.shaderPipelineReady = state.revision != 0;
        // The module is linked even when strict D3D11 intentionally never creates it.
        featureCapabilities.hardwareRayTracingBuilt = true;
        featureCapabilities.hardwareRayTracingBackendAllowed = hardwareBackendAllowed;
        featureCapabilities.hardwareRayTracingDevice =
            state.hardwareRayTracing && state.hardwareRayTracing->supported();
        featureCapabilities.hardwarePipelineReady = state.hardwarePipelineReady;
        featureCapabilities.hardwareDepthCompatible = hardwareDepthCompatible;
        featureCapabilities.hardwareFrameFailed = state.hardwareRuntimeFailed;
        featureCapabilities.editorSelection = selectionOutline;
        featureCapabilities.thinLensDepthOfField = thinLensImage;
        state.features = evaluateRenderFeatures(featureRequests, featureCapabilities);
    }
    commands.endRenderPass();
    // Keep the backbuffer bound for the caller's Dear ImGui draw submission.
    if (state.native12) {
        if (settings.editorViewport) {
            rhi::RenderPassDesc windowPass;
            windowPass.name = "Editor background";
            windowPass.colors = {{state.swapchainImage, rhi::LoadOp::Clear, rhi::StoreOp::Store,
                {editorClear[0], editorClear[1], editorClear[2], editorClear[3]}}};
            commands.beginRenderPass(windowPass);
            commands.endRenderPass();
        }
        const bool complete = !failed(state.native12->device()->GetDeviceRemovedReason(), "D3D12 render/device status", error);
        geometryFrameScope.completed = complete;
        rhiPassScope.complete = complete;
        return complete;
    }
    if (settings.editorViewport) {
        backbuffer = state.backbuffer.Get();
        state.context->ClearRenderTargetView(backbuffer, editorClear);
    }
    state.context->OMSetRenderTargets(1, &backbuffer, nullptr);
    const HRESULT removed = state.device->GetDeviceRemovedReason();
    const bool complete = !failed(removed, "Render/device status", error);
    geometryFrameScope.completed = complete;
    rhiPassScope.complete = complete;
    return complete;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

void SceneRenderer::setShaderRoot(std::filesystem::path root) { impl_->shaderRoot = std::filesystem::absolute(std::move(root)).lexically_normal(); }
void SceneRenderer::setMaterialShaderPath(std::filesystem::path path) {
    impl_->materialOverride = path.empty() ? std::filesystem::path{} : std::filesystem::absolute(std::move(path)).lexically_normal();
}
std::filesystem::path SceneRenderer::materialShaderPath() const { return impl_->materialPath(); }
std::string SceneRenderer::shaderSource() const { return impl_->materialSource; }
std::uint64_t SceneRenderer::shaderRevision() const { return impl_->revision; }
const SceneGeometryStatistics& SceneRenderer::geometryStatistics() const { return impl_->geometryStatistics; }
const RenderPipeline& SceneRenderer::activePipeline() const { return impl_->pipeline; }
const RenderFeatureMatrix& SceneRenderer::renderFeatures() const { return impl_->features; }

bool SceneRenderer::applyShaderSource(const std::string& source, std::string& diagnostics) {
    rae::diagnostics::TraceScope trace("Shader Compile / Pipeline Build", "Shader");
    diagnostics.clear();
    struct CompileMessage {
        std::string& text;
        bool success = false;
        ~CompileMessage() { rae::diagnostics::runtimeDiagnostics().log(success ? rae::diagnostics::MessageSeverity::Info
            : rae::diagnostics::MessageSeverity::Error, "Shader", text); }
    } compileMessage{diagnostics};
    if (!impl_->rhiDevice) { diagnostics = "Initialize the renderer before applying shaders."; return false; }
    Impl candidate;
    candidate.device = impl_->device;
    candidate.rhiDevice = impl_->rhiDevice;
    candidate.native12 = impl_->native12;
    candidate.shaderRoot = impl_->shaderRoot;
    candidate.materialOverride = impl_->materialOverride;
    candidate.materialSource = source;
    if (!readText(candidate.shaderRoot / "Private/Scene.hlsl", candidate.sceneSource, diagnostics) ||
        !candidate.createPipeline(diagnostics)) return false;
    // The DX12 pipeline is also a candidate. It publishes only after validation;
    // from this point the DX11 swaps below cannot fail, so both APIs advance once.
    std::string hardwareWarning;
    if (impl_->hardwareRayTracing && impl_->hardwareRayTracing->supported()) {
        if (!impl_->hardwareRayTracing->preparePipeline(candidate.shaderRoot, candidate.materialPath(),
            candidate.sceneSource, source, diagnostics)) {
            if (impl_->hardwareRequested) return false;
            // Pixel-derivative materials remain valid for raster-only projects.
            // Never run the previous hardware pipeline against new raster source.
            hardwareWarning = diagnostics;
            impl_->hardwarePipelineReady = false;
            impl_->hardwareStatus = "Hardware pipeline unavailable for this material: " + diagnostics;
        } else {
            impl_->hardwarePipelineReady = true;
            impl_->hardwareRuntimeFailed = false;
        }
    }
    // No live GPU state or source is changed until all twelve stages and contracts pass.
    impl_->geometryModule = std::move(candidate.geometryModule);
    impl_->fullscreenModule = std::move(candidate.fullscreenModule);
    impl_->pixelModules = std::move(candidate.pixelModules);
    impl_->graphicsPipelines = std::move(candidate.graphicsPipelines);
    impl_->geometryVS = std::move(candidate.geometryVS);
    impl_->fullscreenVS = std::move(candidate.fullscreenVS);
    impl_->forwardPS = std::move(candidate.forwardPS);
    impl_->gbufferPS = std::move(candidate.gbufferPS);
    impl_->deferredPS = std::move(candidate.deferredPS);
    impl_->backgroundPS = std::move(candidate.backgroundPS);
    impl_->outlineMaskPS = std::move(candidate.outlineMaskPS);
    impl_->outlineCompositePS = std::move(candidate.outlineCompositePS);
    impl_->dofPS = std::move(candidate.dofPS);
    impl_->inputLayout = std::move(candidate.inputLayout);
    impl_->materialSource = source;
    impl_->sceneSource = std::move(candidate.sceneSource);
    ++impl_->revision;
    impl_->invalidateAntiAliasing();
    diagnostics = "Applied all 12 raster shader stages";
    if (impl_->hardwarePipelineReady) diagnostics += " and the DXR hardware pipeline";
    diagnostics += ". Revision " + std::to_string(impl_->revision) + ".";
    if (!hardwareWarning.empty()) diagnostics += " Hardware mode unavailable: " + hardwareWarning;
    compileMessage.success = true;
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
    if (impl_->native12) return "Ray effects unavailable: software ray structured buffers have not been migrated to native D3D12.";
    unsigned analytic = 0;
    unsigned actors = 0;
    std::uint64_t triangles = 0;
    for (const auto& actor : world.actors) {
        if (actor.light || !actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh) continue;
        bool hasMaterial = false;
        for (std::size_t slot = 0; slot < mesh->materialSlotCount(); ++slot)
            hasMaterial |= assets.findMaterial(actor.mesh.materialForSlot(*mesh, slot)) != nullptr;
        if (!hasMaterial) continue;
        ++actors;
        const bool builtin = meshAttributes::analytic(*mesh);
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

bool SceneRenderer::hardwareRayTracingSupported() const {
    return impl_->hardwareRayTracing && impl_->hardwareRayTracing->supported();
}
bool SceneRenderer::hardwareRayTracingActive() const { return impl_->hardwareActive; }
std::string SceneRenderer::hardwareRayTracingStatus() const {
    if (impl_->hardwareActive) return impl_->hardwareStatus;
    if (!impl_->hardwareRequested && impl_->hardwarePipelineReady)
        return "DXR 1.1 ready; disabled by project settings.";
    return impl_->hardwareStatus;
}
std::uint64_t SceneRenderer::hardwareRayTracingDispatchCount() const {
    return impl_->hardwareRayTracing ? impl_->hardwareRayTracing->dispatchCount() : 0;
}
GlobalIlluminationStatus SceneRenderer::globalIlluminationStatus() const { return impl_->giStatus; }
AntiAliasingStatus SceneRenderer::antiAliasingStatus() const { return impl_->aaStatus; }
bool SceneRenderer::readScenePixels(std::vector<std::uint8_t>& pixels, std::uint32_t& width,
    std::uint32_t& height, std::string& error) const {
    error.clear(); pixels.clear(); width=height=0;
    auto& state=*impl_;
    if (!state.sceneColorImage) { error="Scene target is unavailable."; return false; }
    try {
        if (state.native12) {
            const auto result=state.native12->readTexture(state.sceneColorImage);
            width=result.width; height=result.height;
            pixels.resize(static_cast<std::size_t>(width)*height*4);
            for(unsigned y=0;y<height;++y) std::memcpy(pixels.data()+static_cast<std::size_t>(y)*width*4,
                result.bytes.data()+static_cast<std::size_t>(y)*result.rowPitch,width*4);
            return true;
        }
        D3D11_TEXTURE2D_DESC desc{}; state.sceneTexture->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if(failed(state.device->CreateTexture2D(&desc,nullptr,&staging),"Create scene readback",error)) return false;
        state.context->OMSetRenderTargets(0,nullptr,nullptr);
        state.context->CopyResource(staging.Get(),state.sceneTexture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(failed(state.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map scene readback",error)) return false;
        width=desc.Width; height=desc.Height; pixels.resize(static_cast<std::size_t>(width)*height*4);
        for(unsigned y=0;y<height;++y) std::memcpy(pixels.data()+static_cast<std::size_t>(y)*width*4,
            static_cast<const std::uint8_t*>(mapped.pData)+static_cast<std::size_t>(y)*mapped.RowPitch,width*4);
        state.context->Unmap(staging.Get(),0);
        return true;
    } catch(const std::exception& failure) { error=failure.what(); return false; }
}
void SceneRenderer::resetGlobalIllumination() {
    ++impl_->giHistoryEpoch;
    impl_->giStatus.accumulatedSamples = 0;
    impl_->giStatus.reason = "Progressive history reset; waiting for the next main scene frame.";
}

bool SceneRenderer::present(bool vsync, std::string& error) {
    error.clear();
    if (impl_->native12) {
        try {
            impl_->native12->present(vsync);
            impl_->swapchainImage = impl_->native12->backbuffer();
            releaseRetiredSceneTargets();
            return true;
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }
    releaseRetiredSceneTargets();
    if (!impl_->swapChain) { error = "Renderer is not initialized."; return false; }
    const bool result = !failed(impl_->swapChain->Present(vsync ? 1 : 0, 0), "Present", error);
    // UI submission has consumed any retired capture SRVs. D3D11 owns pending
    // GPU references, so clearing here needs no idle wait.
    impl_->retiredCapturedTextures.clear();
    return result;
}

rhi::Statistics SceneRenderer::rhiStatistics() const {
    return impl_->rhiDevice ? impl_->rhiDevice->statistics() : rhi::Statistics{};
}

RendererCacheStats SceneRenderer::cacheStats() const {
    RendererCacheStats result;
    result.meshCount = impl_->meshes.size();
    for (const auto& [id, mesh] : impl_->meshes) result.meshGpuBytesEstimate += mesh.bytes;
    if (impl_->rayTriangleBuffer) {
        result.rayTriangleCount = impl_->rayTriangleCount;
        result.rayGpuBytesEstimate = static_cast<std::uint64_t>(impl_->rayTriangleCount) * sizeof(RayTriangle) + impl_->rayUvBytes;
    }
    return result;
}

rae::GarbageCollectionMemoryStats SceneRenderer::collectGarbage(double maxIdleSeconds) {
    rae::GarbageCollectionMemoryStats result;
    if (!impl_->rhiDevice || !std::isfinite(maxIdleSeconds) || maxIdleSeconds < 0) return result;
    auto& state = *impl_;
    const auto now = Impl::Clock::now();
    const auto expired = [&](const auto& lastUsed) {
        return std::chrono::duration<double>(now - lastUsed).count() >= maxIdleSeconds;
    };
    if (state.native12) {
        state.native12->waitIdle();
        for (auto it = state.meshes.begin(); it != state.meshes.end();) {
            if (it->second.lastUsedEpoch == state.usageEpoch || !expired(it->second.lastUsed)) { ++it; continue; }
            result.gpuBytesReleasedEstimate += it->second.bytes;
            result.objectsReleased += it->second.uvBuffer ? 3 : 2;
            it = state.meshes.erase(it);
        }
        return result;
    }
    // The immediate context owns binding references too. Remove only bindings
    // to evicted resources; keep all live render targets/pipeline/UI resources.
    ComPtr<ID3D11Buffer> boundVertex, boundIndex;
    UINT stride = 0, offset = 0, indexOffset = 0;
    DXGI_FORMAT indexFormat{};
    state.context->IAGetVertexBuffers(0, 1, &boundVertex, &stride, &offset);
    state.context->IAGetIndexBuffer(&boundIndex, &indexFormat, &indexOffset);
    for (auto it = state.meshes.begin(); it != state.meshes.end();) {
        const auto& mesh = it->second;
        if (mesh.lastUsedEpoch == state.usageEpoch || !expired(mesh.lastUsed)) { ++it; continue; }
        if (boundVertex.Get() == mesh.vertices.Get()) {
            const rhi::VertexBufferBinding emptyBindings[2]{};
            state.sceneCommands().bindVertexBuffers(0, emptyBindings);
            boundVertex.Reset();
        }
        if (boundIndex.Get() == mesh.indices.Get()) {
            state.sceneCommands().bindIndexBuffer(nullptr);
            boundIndex.Reset();
        }
        result.gpuBytesReleasedEstimate += mesh.bytes;
        result.objectsReleased += mesh.uv ? 3 : 2;
        it = state.meshes.erase(it);
    }
    if (!state.raySceneActive && state.rayTriangleBuffer && expired(state.rayLastUsed)) {
        ComPtr<ID3D11ShaderResourceView> boundRay;
        state.context->PSGetShaderResources(4, 1, &boundRay);
        if (boundRay.Get() == state.rayTriangleView.Get()) {
            ID3D11ShaderResourceView* empty = nullptr;
            state.context->PSSetShaderResources(4, 1, &empty);
        }
        boundRay.Reset();
        result.gpuBytesReleasedEstimate += static_cast<std::uint64_t>(state.rayTriangleCount) * sizeof(RayTriangle) + state.rayUvBytes;
        result.objectsReleased += 6;
        state.rayTriangleView.Reset();
        state.rayTriangleBuffer.Reset();
        state.rayUvView.Reset(); state.rayUvIndexView.Reset();
        state.rayUvBuffer.Reset(); state.rayUvIndexBuffer.Reset(); state.rayUvBytes = 0;
        // Invalidate the hash fast path as well, so identical scene contents
        // rebuild the buffer/SRV when ray effects are enabled again.
        state.rayTriangleHash = 0;
        state.rayTriangleCount = 0;
        state.raySceneActorCount = 0;
        state.raySceneInputsValid = false;
        state.raySceneInputs.clear();
        state.rayLastUsed = {};
    }
    // D3D11 owns pending GPU work. No Flush/idle wait and no residency claim.
    return result;
}

void SceneRenderer::shutdown() {
    if (impl_ && impl_->native12) {
        try { impl_->native12->waitIdle(); } catch (...) {}
    }
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
std::shared_ptr<rhi::Device> SceneRenderer::graphicsDevice() const { return impl_->rhiDevice; }
void* SceneRenderer::sceneTexture() const {
    if (impl_->native12 && impl_->sceneColorImage)
        return reinterpret_cast<void*>(static_cast<std::uintptr_t>(impl_->native12->uiTextureId(impl_->sceneColorImage)));
    return impl_->sceneView.Get();
}
std::uint32_t SceneRenderer::sceneTextureWidth() const { return impl_->sceneWidth; }
std::uint32_t SceneRenderer::sceneTextureHeight() const { return impl_->sceneHeight; }
std::string SceneRenderer::adapterName() const { return impl_->adapter; }
bool SceneRenderer::validationEnabled() const { return impl_->native12 ? impl_->native12->validationEnabled() : impl_->infoQueue != nullptr; }

std::uint32_t SceneRenderer::validationErrorCount() const {
    if (impl_->native12) return impl_->native12->validationErrorCount();
    std::uint32_t count = impl_->hardwareRayTracing ? impl_->hardwareRayTracing->validationErrorCount() : 0;
    if (!impl_->infoQueue) return count;
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
    std::FILE* file = nullptr;
    if (!path || _wfopen_s(&file, path, L"wb") != 0 || !file) {
        error = "Could not open screenshot output file.";
        return false;
    }
    // Close on every exit, including allocation exceptions during readback.
    std::unique_ptr<std::FILE, decltype(&std::fclose)> owned(file, &std::fclose);
    const bool written = saveScreenshot(owned.get(), error);
    const bool closed = std::fclose(owned.release()) == 0;
    if (!closed && written) error = "Could not close screenshot output file.";
    return written && closed;
}

bool SceneRenderer::saveScreenshot(std::FILE* file, std::string& error) {
    error.clear();
    if (!file) { error = "Screenshot output stream is null."; return false; }
    auto& state = *impl_;
    if (state.native12) {
        try {
            const auto image = state.native12->readTexture(state.native12->backbuffer());
            std::vector<unsigned char> pixels(static_cast<std::size_t>(image.width)*image.height*4);
            for (std::uint32_t y = 0; y < image.height; ++y) {
                const auto* row = reinterpret_cast<const unsigned char*>(image.bytes.data()) +
                    static_cast<std::size_t>(y)*image.rowPitch;
                auto* output = pixels.data() + static_cast<std::size_t>(y)*image.width*4;
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    output[x*4] = row[x*4+2]; output[x*4+1] = row[x*4+1];
                    output[x*4+2] = row[x*4]; output[x*4+3] = 255;
                }
            }
            return writeBitmap(file, image.width, image.height, pixels, error);
        } catch (const std::exception& failure) { error = failure.what(); return false; }
    }
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
    // Allocate before Map so a failed allocation cannot leave the staging
    // texture mapped. The conversion below performs no further allocations.
    std::vector<unsigned char> pixels(static_cast<std::size_t>(description.Width) * description.Height * 4);
    state.context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (failed(state.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read screenshot", error)) return false;
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
    return writeBitmap(file, description.Width, description.Height, pixels, error);
}

#include "RenderDiagnosticsApi.inl"

} // namespace rae::renderer
