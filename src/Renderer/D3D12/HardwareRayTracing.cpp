#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3d12shader.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "HardwareRayTracing.h"
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Scene/World.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

namespace rae::renderer::d3d12 {
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX;

bool failed(HRESULT hr, const char* operation, std::string& error) {
    if (SUCCEEDED(hr)) return false;
    std::ostringstream stream;
    stream << operation << " failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ").";
    error = stream.str();
    return true;
}

struct Library {
    HMODULE handle = nullptr;
    ~Library() { if (handle) FreeLibrary(handle); }
};
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
bool readText(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot read DXR shader: " + utf8(path); return false; }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad()) { error = "Failed reading DXR shader: " + utf8(path); return false; }
    return true;
}
void appendImportDiagnostics(ID3D11Device* device, std::string& error) {
    ComPtr<ID3D11InfoQueue> queue;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = count > 8 ? count - 8 : 0; index < count; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &bytes))) continue;
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &bytes)) && message->pDescription &&
            message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            error += "\nDX11: ";
            error.append(message->pDescription, message->DescriptionByteLength &&
                message->pDescription[message->DescriptionByteLength - 1] == '\0' ?
                message->DescriptionByteLength - 1 : message->DescriptionByteLength);
        }
    }
}

// DXC's include callback has no parent argument. Rewrite only literal include
// paths in each source to absolute paths before giving that source to DXC.
// The compiler still evaluates conditionals/macros and include guards itself;
// inactive includes are never opened. Every nested file keeps its own parent.
class Includes final : public IDxcIncludeHandler {
    std::atomic<ULONG> references_{1};
    ComPtr<IDxcUtils> utils_;
    std::filesystem::path scenePath_, engineMaterialPath_, materialPath_;
    std::vector<std::filesystem::path> includeDirectories_;
    const std::string& sceneSource_;
    const std::string& materialSource_;
public:
    std::string lastError;
    Includes(IDxcUtils* utils, const std::filesystem::path& root,
             const std::filesystem::path& materialPath, const std::string& sceneSource,
             const std::string& materialSource)
        : utils_(utils), scenePath_(std::filesystem::absolute(root / "Private/Scene.hlsl").lexically_normal()),
          engineMaterialPath_(std::filesystem::absolute(root / "Materials/Standard.hlsli").lexically_normal()),
          materialPath_(std::filesystem::absolute(materialPath).lexically_normal()),
          sceneSource_(sceneSource), materialSource_(materialSource) {
        includeDirectories_.push_back(std::filesystem::absolute(root).lexically_normal());
        includeDirectories_.push_back(engineMaterialPath_.parent_path());
        includeDirectories_.push_back(scenePath_.parent_path());
        includeDirectories_.push_back(materialPath_.parent_path());
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDxcIncludeHandler)) {
            *result = static_cast<IDxcIncludeHandler*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    std::string rewrite(const std::string& source, const std::filesystem::path& parent) const {
        std::string result;
        result.reserve(source.size());
        std::size_t start = 0;
        while (start < source.size()) {
            const auto end = source.find('\n', start);
            std::string line = source.substr(start, end == std::string::npos ? std::string::npos : end - start);
            auto cursor = line.find_first_not_of(" \t\r");
            if (cursor != std::string::npos && line[cursor] == '#') {
                cursor = line.find_first_not_of(" \t", cursor + 1);
                if (cursor != std::string::npos && line.compare(cursor, 7, "include") == 0) {
                    cursor = line.find_first_not_of(" \t", cursor + 7);
                    if (cursor != std::string::npos && (line[cursor] == '"' || line[cursor] == '<')) {
                        const auto close = line.find(line[cursor] == '"' ? '"' : '>', cursor + 1);
                        if (close != std::string::npos) {
                            auto path = pathFromUtf8(std::string_view(line).substr(cursor + 1, close - cursor - 1));
                            path = std::filesystem::absolute(parent / path).lexically_normal();
                            if (path == engineMaterialPath_) path = materialPath_;
                            line.replace(cursor, close - cursor + 1, "\"" + utf8(path) + "\"");
                        }
                    }
                }
            }
            result += line;
            if (end == std::string::npos) break;
            result += '\n'; start = end + 1;
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR name, IDxcBlob** result) override {
        if (!name || !result) return E_POINTER;
        *result = nullptr;
        try {
            std::filesystem::path path(name);
            // Literal paths were made absolute above. Macro-expanded includes
            // remain the compiler's responsibility; DXC normally supplies its
            // resolved -I candidate. Also accept relative callback names, using
            // the actual directories of sources already opened in this compile.
            if (!path.is_absolute()) {
                bool found = false;
                for (auto directory = includeDirectories_.rbegin(); directory != includeDirectories_.rend(); ++directory) {
                    const auto candidate = std::filesystem::absolute(*directory / path).lexically_normal();
                    std::error_code code;
                    if (candidate == scenePath_ || candidate == materialPath_ || std::filesystem::is_regular_file(candidate, code)) {
                        path = candidate; found = true; break;
                    }
                }
                if (!found) path = std::filesystem::absolute(path).lexically_normal();
            } else path = path.lexically_normal();
            if (path == engineMaterialPath_) path = materialPath_;
            std::string source;
            if (path == scenePath_) source = sceneSource_;
            else if (path == materialPath_) source = materialSource_;
            else if (!readText(path, source, lastError)) return E_FAIL;
            includeDirectories_.push_back(path.parent_path());
            source = rewrite(source, path.parent_path());
            if (source.size() > std::numeric_limits<UINT32>::max()) return E_OUTOFMEMORY;
            ComPtr<IDxcBlobEncoding> blob;
            const HRESULT hr = utils_->CreateBlob(source.data(), static_cast<UINT32>(source.size()), DXC_CP_UTF8, &blob);
            if (SUCCEEDED(hr)) *result = blob.Detach();
            return hr;
        } catch (const std::exception& exception) {
            lastError = std::string("Cannot resolve DXR include: ") + exception.what();
            return E_FAIL;
        }
    }
};

struct RayTriangle {
    XMFLOAT3 position0, position1, position2, normal;
    XMFLOAT4 color, properties;
};
struct HardwareConstants {
    std::array<float, 16> inverseViewProjection;
    std::uint32_t outputInfo[4];
    float traceOptions[4];
    std::uint32_t accumulationInfo[4]; // sample offset, batch, bounces, GI method
};
static_assert(sizeof(RayTriangle) == 80 && sizeof(HardwareConstants) == 112);

// Compare the full frame contract rather than one combined frame hash. Geometry
// hashes are the same immutable-content identities used by the upload cache.
// Exposure is a resolve operation and is deliberately removed from this key.
struct HistoryKey {
    std::array<float, 36> sceneConstants;
    std::array<float, 16> inverseViewProjection;
    std::uint64_t positions = 0, triangles = 0, normals = 0;
    std::uint64_t pipelineRevision = 0, epoch = 0;
    std::uint32_t width = 0, height = 0, maxBounces = 0, method = 0;
    bool reflections = false, shadows = false;
    bool operator==(const HistoryKey&) const = default;
};

std::uint64_t hashBytes(const void* data, std::size_t bytes) {
    auto hash = UINT64_C(1469598103934665603);
    const auto* input = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= input[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
bool validTransform(const scene::Actor& actor) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(actor.transform.position[i]) || !std::isfinite(actor.transform.rotation[i]) ||
            !std::isfinite(actor.transform.scale[i]) || std::abs(actor.transform.scale[i]) < 0.0001f) return false;
    return true;
}
bool finite3(FXMVECTOR value) {
    XMFLOAT3 stored; XMStoreFloat3(&stored, value);
    return std::isfinite(stored.x) && std::isfinite(stored.y) && std::isfinite(stored.z);
}

bool gather(const scene::World& world, const assets::AssetLibrary& assets,
            const assets::MaterialAsset* preview, std::vector<RayTriangle>& triangles,
            std::vector<XMFLOAT3>& positions, std::vector<XMFLOAT3>& normals, std::string& error) {
    for (const auto& actor : world.actors) {
        if (!actor.visible || !validTransform(actor)) continue;
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        const auto* material = preview && preview->id == actor.mesh.materialId ? preview : assets.findMaterial(actor.mesh.materialId);
        if (!mesh || !material) continue;
        if (mesh->indices.size() % 3 != 0) { error = "DXR mesh is not triangulated: " + mesh->name; return false; }
        const auto& t = actor.transform;
        const XMMATRIX matrix = XMMatrixScaling(t.scale[0], t.scale[1], t.scale[2]) *
            XMMatrixRotationRollPitchYaw(XMConvertToRadians(t.rotation[0]), XMConvertToRadians(t.rotation[1]), XMConvertToRadians(t.rotation[2])) *
            XMMatrixTranslation(t.position[0], t.position[1], t.position[2]);
        const XMMATRIX normalMatrix = XMMatrixTranspose(XMMatrixInverse(nullptr, matrix));
        const bool mirrored = std::signbit(t.scale[0]) ^ std::signbit(t.scale[1]) ^ std::signbit(t.scale[2]);
        for (std::size_t index = 0; index < mesh->indices.size(); index += 3) {
            XMVECTOR points[3], vertexNormals[3];
            for (unsigned vertex = 0; vertex < 3; ++vertex) {
                const auto offset = mesh->indices[index + vertex];
                if (offset >= mesh->vertices.size()) { error = "DXR mesh index out of range: " + mesh->name; return false; }
                const auto& input = mesh->vertices[offset];
                points[vertex] = XMVector3TransformCoord(XMVectorSet(input.position[0], input.position[1], input.position[2], 1), matrix);
                vertexNormals[vertex] = XMVector3TransformNormal(XMVectorSet(input.normal[0], input.normal[1], input.normal[2], 0), normalMatrix);
                if (!finite3(points[vertex])) { error = "DXR mesh has a non-finite world position: " + mesh->name; return false; }
            }
            const XMVECTOR cross = XMVector3Cross(XMVectorSubtract(points[1], points[0]), XMVectorSubtract(points[2], points[0]));
            const float area = XMVectorGetX(XMVector3LengthSq(cross));
            if (!std::isfinite(area)) { error = "DXR mesh has an overflowing triangle: " + mesh->name; return false; }
            if (area < 1e-12f) continue;
            if (triangles.size() >= std::numeric_limits<UINT>::max() / 3u) {
                error = "DXR scene exceeds the addressable geometry size."; return false;
            }
            const XMVECTOR faceNormal = XMVectorScale(XMVector3Normalize(cross), mirrored ? -1.0f : 1.0f);
            RayTriangle triangle{};
            XMStoreFloat3(&triangle.position0, points[0]); XMStoreFloat3(&triangle.position1, points[1]);
            XMStoreFloat3(&triangle.position2, points[2]); XMStoreFloat3(&triangle.normal, faceNormal);
            triangle.color = {material->baseColor[0], material->baseColor[1], material->baseColor[2], 1};
            triangle.properties = {material->roughness, material->metallic, 3,
                static_cast<float>((material->grid ? 1u : 0u) | (material->twoSided ? 2u : 0u))};
            triangles.push_back(triangle);
            for (unsigned vertex = 0; vertex < 3; ++vertex) {
                XMFLOAT3 position, normal;
                XMStoreFloat3(&position, points[vertex]); positions.push_back(position);
                XMVECTOR n = vertexNormals[vertex];
                if (!finite3(n) || XMVectorGetX(XMVector3LengthSq(n)) < 1e-12f) n = faceNormal;
                XMStoreFloat3(&normal, XMVector3Normalize(n)); normals.push_back(normal);
            }
        }
    }
    return true;
}

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result{}; result.Type = type;
    result.CreationNodeMask = result.VisibleNodeMask = 1; return result;
}
D3D12_RESOURCE_DESC bufferDescription(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; result.Width = std::max<std::uint64_t>(bytes, 256);
    result.Height = result.DepthOrArraySize = result.MipLevels = 1;
    result.SampleDesc.Count = 1; result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; result.Flags = flags;
    return result;
}
void uavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource; list->ResourceBarrier(1, &barrier);
}

bool validateShader(IDxcUtils* utils, IDxcResult* result, std::string& error) {
    ComPtr<IDxcBlob> reflectionBlob;
    if (failed(result->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflectionBlob), nullptr), "Get DXR shader reflection", error)) return false;
    const DxcBuffer buffer{reflectionBlob->GetBufferPointer(), reflectionBlob->GetBufferSize(), DXC_CP_ACP};
    ComPtr<ID3D12ShaderReflection> reflection;
    if (failed(utils->CreateReflection(&buffer, IID_PPV_ARGS(&reflection)), "Reflect DXR shader", error)) return false;
    D3D12_SHADER_DESC description{};
    if (failed(reflection->GetDesc(&description), "Read DXR shader reflection", error)) return false;
    bool scene = false, hardware = false, acceleration = false, triangles = false, normals = false;
    bool output = false, accumulation = false;
    for (UINT index = 0; index < description.BoundResources; ++index) {
        D3D12_SHADER_INPUT_BIND_DESC binding{};
        if (failed(reflection->GetResourceBindingDesc(index, &binding), "Read DXR binding", error)) return false;
        if (binding.Space != 0 || binding.BindCount != 1) { error = "DXR shader requires single resources in register space 0."; return false; }
        if (binding.Type == D3D_SIT_CBUFFER && (binding.BindPoint == 0 || binding.BindPoint == 5)) {
            const bool isScene = binding.BindPoint == 0;
            auto* constants = reflection->GetConstantBufferByName(binding.Name);
            D3D12_SHADER_BUFFER_DESC constantDescription{};
            if (failed(constants->GetDesc(&constantDescription), "Read DXR constant buffer", error)) return false;
            if (constantDescription.Size != (isScene ? 144u : 112u)) {
                error = std::string(binding.Name) + ": DXR constant buffer size does not match the CPU contract."; return false;
            }
            struct Field { const char* name; UINT offset, size; };
            static constexpr Field sceneFields[] = {{"viewProjection",0,64},{"cameraPosition",64,16},{"baseColor",80,16},
                {"lightDirectionIntensity",96,16},{"parameters",112,16},{"viewportDebug",128,16}};
            static constexpr Field hardwareFields[] = {{"inverseViewProjection",0,64},{"outputInfo",64,16},
                {"traceOptions",80,16},{"accumulationInfo",96,16}};
            const Field* fields = isScene ? sceneFields : hardwareFields;
            const auto count = isScene ? std::size(sceneFields) : std::size(hardwareFields);
            for (std::size_t fieldIndex = 0; fieldIndex < count; ++fieldIndex) {
                const auto& field = fields[fieldIndex]; D3D12_SHADER_VARIABLE_DESC variable{};
                auto* reflectedVariable = constants->GetVariableByName(field.name);
                if (FAILED(reflectedVariable->GetDesc(&variable)) || variable.StartOffset != field.offset || variable.Size != field.size) {
                    error = std::string("DXR constant field contract mismatch: ") + field.name; return false;
                }
                D3D12_SHADER_TYPE_DESC type{};
                if (FAILED(reflectedVariable->GetType()->GetDesc(&type)) ||
                    type.Type != ((std::strcmp(field.name, "outputInfo") == 0 ||
                        std::strcmp(field.name, "accumulationInfo") == 0) ? D3D_SVT_UINT : D3D_SVT_FLOAT) ||
                    (field.size == 64 && (type.Class != D3D_SVC_MATRIX_ROWS || type.Rows != 4 || type.Columns != 4)) ||
                    (field.size == 16 && (type.Class != D3D_SVC_VECTOR || type.Columns != 4))) {
                    error = std::string("DXR constant field type mismatch: ") + field.name; return false;
                }
            }
            if (isScene) scene = true; else hardware = true;
        } else if (binding.Type == D3D_SIT_STRUCTURED && (binding.BindPoint == 4 || binding.BindPoint == 8)) {
            // DXC exposes a structured resource as RESOURCE_BIND_INFO with a
            // single $Element variable. Validate the element itself, since a
            // valid register binding alone cannot protect an 80-byte CPU upload.
            auto* resource = reflection->GetConstantBufferByName(binding.Name);
            D3D12_SHADER_BUFFER_DESC resourceDescription{};
            if (FAILED(resource->GetDesc(&resourceDescription)) || resourceDescription.Variables != 1) {
                error = std::string("Cannot reflect DXR structured element: ") + binding.Name; return false;
            }
            auto* element = resource->GetVariableByIndex(0);
            D3D12_SHADER_VARIABLE_DESC variable{};
            D3D12_SHADER_TYPE_DESC elementType{};
            if (FAILED(element->GetDesc(&variable)) || FAILED(element->GetType()->GetDesc(&elementType))) {
                error = std::string("Cannot read DXR structured element: ") + binding.Name; return false;
            }
            if (binding.BindPoint == 4) {
                if (variable.Size != sizeof(RayTriangle) || elementType.Class != D3D_SVC_STRUCT || elementType.Members != 6) {
                    error = "DXR rayTriangles requires the renderer's 80-byte RayTriangle structure."; return false;
                }
                const char* names[] = {"position0", "position1", "position2", "normal", "color", "properties"};
                const UINT offsets[] = {0, 12, 24, 36, 48, 64};
                for (UINT memberIndex = 0; memberIndex < std::size(names); ++memberIndex) {
                    D3D12_SHADER_TYPE_DESC member{};
                    if (FAILED(element->GetType()->GetMemberTypeByName(names[memberIndex])->GetDesc(&member)) ||
                        member.Offset != offsets[memberIndex] || member.Type != D3D_SVT_FLOAT ||
                        member.Class != D3D_SVC_VECTOR || member.Rows != 1 || member.Columns != (memberIndex < 4 ? 3u : 4u)) {
                        error = std::string("DXR RayTriangle layout mismatch: ") + names[memberIndex]; return false;
                    }
                }
                triangles = true;
            } else {
                if (variable.Size != sizeof(XMFLOAT3) || elementType.Type != D3D_SVT_FLOAT ||
                    elementType.Class != D3D_SVC_VECTOR || elementType.Rows != 1 || elementType.Columns != 3) {
                    error = "DXR vertex normals require tightly packed float3 elements."; return false;
                }
                normals = true;
            }
        }
        else if (binding.Type == D3D_SIT_RTACCELERATIONSTRUCTURE && binding.BindPoint == 7) acceleration = true;
        else if (binding.Type == D3D_SIT_UAV_RWTYPED && binding.BindPoint <= 1 &&
                 binding.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && binding.ReturnType == D3D_RETURN_TYPE_FLOAT) {
            if (binding.BindPoint == 0) output = true; else accumulation = true;
        }
        else { error = std::string("Unexpected DXR shader resource: ") + binding.Name; return false; }
    }
    UINT x = 0, y = 0, z = 0; reflection->GetThreadGroupSize(&x, &y, &z);
    if (!(scene && hardware && acceleration && triangles && normals && output && accumulation) || x != 8 || y != 8 || z != 1) {
        error = "DXR shader must preserve b0/b5, t4/t7/t8, u0/u1 and the 8x8x1 thread-group contract."; return false;
    }
    return true;
}
} // namespace

struct HardwareRayTracing::Impl {
    // The DLL outlives every compiler object created through it.
    Library compilerLibrary;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    // Fence completion registrations can outlive a timed-out wait. Keep their
    // event alive until both native devices and all shared fences are released.
    Handle event;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> infoQueue;
    UINT64 nextFence = 0, submitted = 0;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Resource> outputTexture, accumulationTexture;
    ComPtr<ID3D11Texture2D> sharedOutput;
    std::uint32_t width = 0, height = 0;
    ComPtr<ID3D12Resource> constants, triangleData, normalData;
    ComPtr<ID3D12Resource> vertexData, instanceData, blas, tlas, scratch;
    std::uint64_t positionHash = 0, triangleHash = 0, normalHash = 0;
    std::uint32_t triangleCount = 0;
    bool geometryReady = false, capable = false;
    std::uint64_t dispatches = 0;
    HistoryKey historyKey{};
    bool historyValid = false;
    std::uint32_t accumulatedSamples = 0;
    std::uint64_t pipelineRevision = 0;
    GlobalIlluminationStatus giStatus;
    std::string message = "Hardware ray tracing has not been initialized.";

    ~Impl() {
        // A module owns all its DX12 submissions. Drain DX11 copies as well,
        // before shared resources/fences and the compiler library are released.
        if (queue && fence && context11 && fence11 && event.value) {
            std::string ignored;
            if (SUCCEEDED(context11->Signal(fence11.Get(), ++nextFence))) {
                context11->Flush(); wait(nextFence, ignored);
            }
            wait(submitted, ignored);
        }
    }
    bool wait(UINT64 value, std::string& error) const {
        if (!value || !fence || fence->GetCompletedValue() >= value) {
            if (fence && fence->GetCompletedValue() == UINT64_MAX) {
                error = "DXR device was removed."; return false;
            }
            return true;
        }
        if (!event.value) { error = "The DXR completion event is unavailable."; return false; }
        if (failed(fence->SetEventOnCompletion(value, event.value), "Set DXR completion event", error)) return false;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        for (;;) {
            const UINT64 completed = fence->GetCompletedValue();
            if (completed == UINT64_MAX) { error = "DXR device was removed."; return false; }
            if (completed >= value) return true;
            if (device && failed(device->GetDeviceRemovedReason(), "DXR device removed", error)) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                error = "Timed out waiting for hardware ray tracing (10 seconds)."; return false;
            }
            // A previous timeout leaves a valid event registration behind. Its
            // later signal is only a wake-up: success always requires this
            // call's actual fence value, never merely WAIT_OBJECT_0.
            const DWORD result = WaitForSingleObject(event.value,
                static_cast<DWORD>(std::min<ULONGLONG>(250, deadline - now)));
            if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) {
                error = "Cannot wait for hardware ray tracing (Win32 " + std::to_string(GetLastError()) + ").";
                return false;
            }
        }
    }
    bool createBuffer(std::uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                      D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& resource, std::string& error) {
        const auto properties = heap(type); const auto description = bufferDescription(bytes, flags);
        return !failed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            state, nullptr, IID_PPV_ARGS(&resource)), "Create DXR buffer", error);
    }
    bool upload(const void* bytes, std::size_t size, ComPtr<ID3D12Resource>& resource, std::string& error) {
        if (!resource || resource->GetDesc().Width < size) {
            ComPtr<ID3D12Resource> candidate;
            if (!createBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                              D3D12_RESOURCE_FLAG_NONE, candidate, error)) return false;
            resource = std::move(candidate);
        }
        void* mapped = nullptr; const D3D12_RANGE readRange{0, 0};
        if (failed(resource->Map(0, &readRange, &mapped), "Map DXR upload", error)) return false;
        if (size) std::memcpy(mapped, bytes, size);
        const D3D12_RANGE writtenRange{0, size}; resource->Unmap(0, &writtenRange); return true;
    }
    bool prepareOutput(std::uint32_t w, std::uint32_t h, UINT64 dx11Done, std::string& error) {
        if (outputTexture && accumulationTexture && width == w && height == h) return true;
        // Previous DX11 reads must finish before replacing a shared allocation
        // or its descriptor. Normal frame reuse remains GPU ordered.
        if (!wait(dx11Done, error)) return false;
        const auto properties = heap(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = w; description.Height = h;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM; description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // DX11's shared-texture import contract requires a render-target-capable
        // 2D allocation even though this module writes it through a DX12 UAV.
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        ComPtr<ID3D12Resource> candidate;
        if (failed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&candidate)), "Create shared DXR image", error)) return false;
        Handle sharedHandle;
        if (failed(device->CreateSharedHandle(candidate.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle.value),
                   "Create DXR image shared handle", error)) return false;
        ComPtr<ID3D11Texture2D> candidate11;
        if (failed(device11->OpenSharedResource1(sharedHandle.value, IID_PPV_ARGS(&candidate11)), "Open DXR image in DX11", error)) {
            appendImportDiagnostics(device11.Get(), error); return false;
        }
        // Keep the estimator in full-precision linear HDR. This texture is not
        // shared with DX11 and never passes through the display transfer curve.
        D3D12_RESOURCE_DESC accumulationDescription = description;
        accumulationDescription.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        accumulationDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> accumulationCandidate;
        if (failed(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &accumulationDescription,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&accumulationCandidate)),
            "Create HDR path accumulation image", error)) return false;
        // Both resources exist before either live descriptor is replaced.
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = description.Format; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto descriptor = descriptors->GetCPUDescriptorHandleForHeapStart();
        device->CreateUnorderedAccessView(candidate.Get(), nullptr, &view, descriptor);
        descriptor.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        view.Format = accumulationDescription.Format;
        device->CreateUnorderedAccessView(accumulationCandidate.Get(), nullptr, &view, descriptor);
        outputTexture = std::move(candidate); sharedOutput = std::move(candidate11); width = w; height = h;
        accumulationTexture = std::move(accumulationCandidate);
        historyValid = false; accumulatedSamples = 0;
        return true;
    }

    bool buildGeometry(const std::vector<XMFLOAT3>& positions, std::string& error) {
        const bool hasTriangles = !positions.empty();
        if (hasTriangles && !upload(positions.data(), positions.size() * sizeof(XMFLOAT3), vertexData, error)) return false;
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        // Non-opaque candidates preserve per-material Two-sided culling in the
        // query, including opposite face rules for receiver->light shadows.
        geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometry.Triangles.VertexCount = static_cast<UINT>(positions.size());
        geometry.Triangles.VertexBuffer.StartAddress = hasTriangles ? vertexData->GetGPUVirtualAddress() : 0;
        geometry.Triangles.VertexBuffer.StrideInBytes = sizeof(XMFLOAT3);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};
        bottom.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; bottom.NumDescs = 1;
        bottom.pGeometryDescs = &geometry; bottom.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomInfo{};
        if (hasTriangles) device->GetRaytracingAccelerationStructurePrebuildInfo(&bottom, &bottomInfo);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY; top.NumDescs = hasTriangles ? 1u : 0u;
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&top, &topInfo);
        if (!topInfo.ResultDataMaxSizeInBytes || (hasTriangles && !bottomInfo.ResultDataMaxSizeInBytes)) {
            error = "The DXR driver did not provide acceleration-structure allocation sizes."; return false;
        }
        ComPtr<ID3D12Resource> newBlas, newTlas, newScratch;
        if ((hasTriangles && !createBuffer(bottomInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, newBlas, error)) ||
            !createBuffer(topInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, newTlas, error) ||
            !createBuffer(std::max(bottomInfo.ScratchDataSizeInBytes, topInfo.ScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, newScratch, error)) return false;
        if (hasTriangles) {
            D3D12_RAYTRACING_INSTANCE_DESC instance{};
            instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
            instance.InstanceMask = 0xff; instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
            instance.AccelerationStructure = newBlas->GetGPUVirtualAddress();
            if (!upload(&instance, sizeof(instance), instanceData, error)) return false;
            top.InstanceDescs = instanceData->GetGPUVirtualAddress();
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
            build.Inputs = bottom; build.DestAccelerationStructureData = newBlas->GetGPUVirtualAddress();
            build.ScratchAccelerationStructureData = newScratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
            uavBarrier(list.Get(), newBlas.Get()); uavBarrier(list.Get(), newScratch.Get());
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildTop{};
        buildTop.Inputs = top; buildTop.DestAccelerationStructureData = newTlas->GetGPUVirtualAddress();
        buildTop.ScratchAccelerationStructureData = newScratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&buildTop, 0, nullptr); uavBarrier(list.Get(), newTlas.Get());
        blas = std::move(newBlas); tlas = std::move(newTlas); scratch = std::move(newScratch);
        return true;
    }
};

HardwareRayTracing::HardwareRayTracing() : impl_(std::make_unique<Impl>()) {}
HardwareRayTracing::~HardwareRayTracing() = default;

bool HardwareRayTracing::initialize(ID3D11Device* device11, ID3D11DeviceContext* context11, std::string& error) {
    error.clear();
    if (impl_->capable) return true;
    auto candidate = std::make_unique<Impl>();
    const auto fail = [&]() { impl_->message = error; return false; };
    if (!device11 || !context11) { error = "Hardware ray tracing requires an initialized DX11 device."; return fail(); }
    if (failed(device11->QueryInterface(IID_PPV_ARGS(&candidate->device11)), "Query DX11 shared-fence device", error) ||
        failed(context11->QueryInterface(IID_PPV_ARGS(&candidate->context11)), "Query DX11 shared-fence context", error)) return fail();
    ComPtr<ID3D11InfoQueue> validation11;
    if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&validation11)))) {
        static std::once_flag validationOnce;
        std::call_once(validationOnce, [] {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        });
    }
    ComPtr<IDXGIDevice> dxgiDevice; ComPtr<IDXGIAdapter> adapter;
    if (failed(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)), "Query hardware RT adapter", error) ||
        failed(dxgiDevice->GetAdapter(&adapter), "Get hardware RT adapter", error) ||
        failed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate->device)), "Create same-adapter DX12 device", error)) return fail();
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};
    if (failed(candidate->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)), "Query DXR tier", error)) return fail();
    if (options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) { error = "This adapter does not support DXR 1.1 hardware ray queries."; return fail(); }
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_5};
    if (failed(candidate->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)), "Query DXR Shader Model", error)) return fail();
    if (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5) { error = "Hardware ray tracing requires Shader Model 6.5."; return fail(); }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R8G8B8A8_UNORM};
    if (failed(candidate->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)), "Query DXR output format", error)) return fail();
    if (!(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)) { error = "The adapter cannot write the shared DXR image format."; return fail(); }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT accumulationFormat{DXGI_FORMAT_R32G32B32A32_FLOAT};
    if (failed(candidate->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &accumulationFormat,
               sizeof(accumulationFormat)), "Query HDR path accumulation format", error)) return fail();
    constexpr auto accumulationSupport = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
    if ((accumulationFormat.Support2 & accumulationSupport) != accumulationSupport) {
        error = "The adapter cannot read and write full-precision HDR path accumulation."; return fail();
    }
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) { error = "Cannot locate the DXR shader compiler beside the executable."; return fail(); }
    executable.resize(length);
    const auto compilerPath = std::filesystem::path(executable).parent_path() / "dxcompiler.dll";
    candidate->compilerLibrary.handle = LoadLibraryExW(compilerPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!candidate->compilerLibrary.handle) { error = "Hardware ray tracing requires dxcompiler.dll and dxil.dll beside RestArtEngine.exe."; return fail(); }
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(candidate->compilerLibrary.handle, "DxcCreateInstance"));
    if (!create) { error = "The installed DXC library has no DxcCreateInstance entry point."; return fail(); }
    if (failed(create(CLSID_DxcUtils, IID_PPV_ARGS(&candidate->utils)), "Create DXC utilities", error) ||
        failed(create(CLSID_DxcCompiler, IID_PPV_ARGS(&candidate->compiler)), "Create DXC compiler", error)) return fail();
    D3D12_COMMAND_QUEUE_DESC queueDescription{}; queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (failed(candidate->device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(&candidate->queue)), "Create DXR queue", error) ||
        failed(candidate->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&candidate->allocator)), "Create DXR command allocator", error) ||
        failed(candidate->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, candidate->allocator.Get(), nullptr,
            IID_PPV_ARGS(&candidate->list)), "Create DXR command list", error) ||
        failed(candidate->list->Close(), "Close initial DXR command list", error) ||
        failed(candidate->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&candidate->fence)), "Create DXR shared fence", error)) return fail();
    Handle sharedFence;
    if (failed(candidate->device->CreateSharedHandle(candidate->fence.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence.value),
               "Share DXR fence", error) ||
        failed(candidate->device11->OpenSharedFence(sharedFence.value, IID_PPV_ARGS(&candidate->fence11)), "Open DXR fence in DX11", error)) return fail();
    candidate->event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!candidate->event.value) { error = "Cannot create DXR completion event."; return fail(); }
    D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
    descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptors.NumDescriptors = 2; descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (failed(candidate->device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&candidate->descriptors)), "Create DXR descriptors", error)) return fail();
    candidate->device.As(&candidate->infoQueue);
    candidate->capable = true;
    candidate->message = "D3D12 DXR 1.1 supported; hardware pipeline has not been compiled.";
    impl_ = std::move(candidate); return true;
}

bool HardwareRayTracing::supported() const { return impl_->capable; }

bool HardwareRayTracing::preparePipeline(const std::filesystem::path& shaderRoot,
                                        const std::filesystem::path& materialPath,
                                        const std::string& sceneSource,
                                        const std::string& materialSource, std::string& error) {
    error.clear(); auto& state = *impl_;
    if (!state.capable) { error = state.message; return false; }
    const auto sourcePath = std::filesystem::absolute(shaderRoot / "Private/HardwareRayTracing.hlsl").lexically_normal();
    std::string source;
    if (!readText(sourcePath, source, error)) return false;
    ComPtr<Includes> includes;
    includes.Attach(new Includes(state.utils.Get(), shaderRoot, materialPath, sceneSource, materialSource));
    source = includes->rewrite(source, sourcePath.parent_path());
    const std::wstring sourceName = sourcePath.wstring();
    const auto projectInclude = std::filesystem::absolute(materialPath).parent_path().wstring();
    const auto pipelineInclude = sourcePath.parent_path().wstring();
    const auto shaderInclude = std::filesystem::absolute(shaderRoot).wstring();
    const auto engineMaterialInclude = std::filesystem::absolute(shaderRoot / "Materials").wstring();
    const wchar_t* arguments[] = {sourceName.c_str(), L"-E", L"CSMain", L"-T", L"cs_6_5", L"-HV", L"2021",
        L"-Ges", L"-WX", L"-O3", L"-Zpr", L"-I", projectInclude.c_str(), L"-I", pipelineInclude.c_str(),
        L"-I", shaderInclude.c_str(), L"-I", engineMaterialInclude.c_str()};
    const DxcBuffer buffer{source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> result;
    if (failed(state.compiler->Compile(&buffer, arguments, static_cast<UINT32>(std::size(arguments)), includes.Get(),
                                      IID_PPV_ARGS(&result)), "Compile hardware ray tracing shader", error)) return false;
    HRESULT status = E_FAIL;
    if (failed(result->GetStatus(&status), "Read DXR compiler status", error)) return false;
    if (FAILED(status)) {
        ComPtr<IDxcBlobUtf8> diagnostics;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr);
        error = diagnostics && diagnostics->GetStringLength() ? std::string(diagnostics->GetStringPointer(), diagnostics->GetStringLength()) : "Hardware ray tracing shader compilation failed.";
        if (!includes->lastError.empty()) error += "\n" + includes->lastError;
        return false;
    }
    if (!validateShader(state.utils.Get(), result.Get(), error)) return false;
    ComPtr<IDxcBlob> shader;
    if (failed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr), "Get DXR shader object", error)) return false;
    D3D12_ROOT_PARAMETER parameters[6]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor.ShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[1].Descriptor.ShaderRegister = 5;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[2].Descriptor.ShaderRegister = 4;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[3].Descriptor.ShaderRegister = 7;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[4].Descriptor.ShaderRegister = 8;
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 2; range.BaseShaderRegister = 0;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable.NumDescriptorRanges = 1; parameters[5].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = static_cast<UINT>(std::size(parameters)); rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> rootBlob, rootErrors;
    if (failed(D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootErrors), "Serialize DXR root signature", error)) {
        if (rootErrors) error += std::string(static_cast<const char*>(rootErrors->GetBufferPointer()), rootErrors->GetBufferSize());
        return false;
    }
    ComPtr<ID3D12RootSignature> rootCandidate;
    if (failed(state.device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                IID_PPV_ARGS(&rootCandidate)), "Create DXR root signature", error)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = rootCandidate.Get();
    pipelineDescription.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ComPtr<ID3D12PipelineState> candidate;
    if (failed(state.device->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&candidate)), "Create hardware ray tracing pipeline", error) ||
        !state.wait(state.submitted, error)) return false;
    state.pipeline = std::move(candidate); state.rootSignature = std::move(rootCandidate);
    ++state.pipelineRevision;
    state.historyValid = false; state.accumulatedSamples = 0;
    state.giStatus.accumulatedSamples = 0;
    state.giStatus.effective = GlobalIlluminationMethod::Disabled;
    state.giStatus.reason = "Shader pipeline updated; accumulation restarts on the next lit frame.";
    state.message = "D3D12 DXR 1.1 hardware ray tracing ready.";
    return true;
}

bool HardwareRayTracing::render(const Frame& frame, const scene::World& world,
                               const assets::AssetLibrary& assets, const assets::MaterialAsset* materialOverride,
                               std::string& error) {
    error.clear(); auto& state = *impl_;
    const auto targetSamples = std::clamp(frame.globalIllumination.targetSamples, 1u, kGiMaxTargetSamples);
    state.giStatus.requested = frame.globalIllumination.method;
    state.giStatus.targetSamples = targetSamples;
    const auto fail = [&] {
        // A submitted batch may have partially written history even if its
        // synchronization fails. The next safe render must start from scratch.
        state.historyValid = false; state.accumulatedSamples = 0;
        state.giStatus.accumulatedSamples = 0;
        state.giStatus.effective = GlobalIlluminationMethod::Disabled;
        state.message = "Hardware ray tracing failed: " + error;
        state.giStatus.reason = state.message;
        return false;
    };
    if (!state.capable || !state.pipeline) { error = state.message; return fail(); }
    if (frame.globalIllumination.method != GlobalIlluminationMethod::Disabled &&
        frame.globalIllumination.method != GlobalIlluminationMethod::PathTracing) {
        error = "The requested global illumination method is unavailable in this backend."; return fail();
    }
    if (!frame.width || !frame.height || frame.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || frame.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        error = "Invalid hardware ray tracing output dimensions."; return fail();
    }
    bool recording = false;
    try {
        if (!state.wait(state.submitted, error)) return fail();
        // Release the prior DX11 copy/read before the DX12 queue writes again.
        const UINT64 dx11Done = ++state.nextFence;
        if (failed(state.context11->Signal(state.fence11.Get(), dx11Done), "Signal DX11-to-DXR fence", error)) return fail();
        state.context11->Flush();
        if (failed(state.queue->Wait(state.fence.Get(), dx11Done), "Wait for DX11 DXR image consumer", error) ||
            !state.prepareOutput(frame.width, frame.height, dx11Done, error)) return fail();
        std::vector<RayTriangle> triangles;
        std::vector<XMFLOAT3> positions, normals;
        if (!gather(world, assets, materialOverride, triangles, positions, normals, error)) return fail();
        const auto positionHash = hashBytes(positions.data(), positions.size() * sizeof(XMFLOAT3));
        const auto triangleHash = hashBytes(triangles.data(), triangles.size() * sizeof(RayTriangle));
        const auto normalHash = hashBytes(normals.data(), normals.size() * sizeof(XMFLOAT3));
        const bool pathRequested = frame.globalIllumination.method == GlobalIlluminationMethod::PathTracing;
        const bool pathLit = pathRequested && frame.sceneConstants[34] == 0.0f;
        const auto maxBounces = std::clamp(frame.globalIllumination.maxBounces, 1u, kGiMaxBounces);
        HistoryKey nextHistory{};
        nextHistory.sceneConstants = frame.sceneConstants;
        nextHistory.sceneConstants[30] = 0.0f; // exposure belongs to display resolve
        nextHistory.sceneConstants[34] = 0.0f; // debug views pause without poisoning HDR history
        nextHistory.inverseViewProjection = frame.inverseViewProjection;
        nextHistory.positions = positionHash; nextHistory.triangles = triangleHash; nextHistory.normals = normalHash;
        nextHistory.pipelineRevision = state.pipelineRevision; nextHistory.epoch = frame.historyEpoch;
        nextHistory.width = frame.width; nextHistory.height = frame.height; nextHistory.maxBounces = maxBounces;
        nextHistory.method = static_cast<std::uint32_t>(frame.globalIllumination.method);
        nextHistory.reflections = frame.reflections; nextHistory.shadows = frame.shadows;
        const bool restart = pathLit && (!state.historyValid || !(nextHistory == state.historyKey) ||
            targetSamples < state.accumulatedSamples);
        const std::uint32_t sampleOffset = restart ? 0u : state.accumulatedSamples;
        const std::uint32_t batch = pathLit ? std::min(
            std::clamp(frame.globalIllumination.samplesPerFrame, 1u, kGiMaxSamplesPerFrame),
            targetSamples - sampleOffset) : 0u;
        // Null/empty scenes still bind valid upload buffers; the shader guards
        // traversal using outputInfo.z and renders the normal background.
        if ((!state.triangleData || triangleHash != state.triangleHash) &&
            !state.upload(triangles.data(), triangles.size() * sizeof(RayTriangle), state.triangleData, error)) return fail();
        if ((!state.normalData || normalHash != state.normalHash) &&
            !state.upload(normals.data(), normals.size() * sizeof(XMFLOAT3), state.normalData, error)) return fail();
        std::array<unsigned char, 512> constants{};
        std::memcpy(constants.data(), frame.sceneConstants.data(), sizeof(frame.sceneConstants));
        const HardwareConstants hardware{frame.inverseViewProjection,
            {frame.width, frame.height, static_cast<std::uint32_t>(triangles.size()), frame.shadows ? 1u : 0u},
            {frame.reflections ? 1.0f : 0.0f, 0, 0, 0},
            {sampleOffset, batch, maxBounces, pathRequested ? 1u : 0u}};
        std::memcpy(constants.data() + 256, &hardware, sizeof(hardware));
        if (!state.upload(constants.data(), constants.size(), state.constants, error)) return fail();
        if (failed(state.allocator->Reset(), "Reset DXR allocator", error) ||
            failed(state.list->Reset(state.allocator.Get(), state.pipeline.Get()), "Reset DXR command list", error)) return fail();
        recording = true;
        const bool rebuild = !state.geometryReady || positionHash != state.positionHash || triangles.size() != state.triangleCount;
        if (rebuild && !state.buildGeometry(positions, error)) {
            state.list->Close(); recording = false; state.geometryReady = false; return fail();
        }
        D3D12_RESOURCE_BARRIER transition{};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = state.outputTexture.Get();
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        state.list->ResourceBarrier(1, &transition);
        ID3D12DescriptorHeap* heaps[] = {state.descriptors.Get()}; state.list->SetDescriptorHeaps(1, heaps);
        state.list->SetComputeRootSignature(state.rootSignature.Get());
        state.list->SetComputeRootConstantBufferView(0, state.constants->GetGPUVirtualAddress());
        state.list->SetComputeRootConstantBufferView(1, state.constants->GetGPUVirtualAddress() + 256);
        state.list->SetComputeRootShaderResourceView(2, state.triangleData->GetGPUVirtualAddress());
        state.list->SetComputeRootShaderResourceView(3, state.tlas->GetGPUVirtualAddress());
        state.list->SetComputeRootShaderResourceView(4, state.normalData->GetGPUVirtualAddress());
        state.list->SetComputeRootDescriptorTable(5, state.descriptors->GetGPUDescriptorHandleForHeapStart());
        state.list->Dispatch((frame.width + 7) / 8, (frame.height + 7) / 8, 1);
        // The running mean remains in UAV state across frames. Explicitly order
        // its read/write dependencies before the next dispatch can access it.
        uavBarrier(state.list.Get(), state.accumulationTexture.Get());
        std::swap(transition.Transition.StateBefore, transition.Transition.StateAfter);
        state.list->ResourceBarrier(1, &transition);
        const HRESULT closeResult = state.list->Close(); recording = false;
        if (failed(closeResult, "Close DXR command list", error)) { state.geometryReady = false; return fail(); }
        ID3D12CommandList* lists[] = {state.list.Get()}; state.queue->ExecuteCommandLists(1, lists);
        const UINT64 completed = ++state.nextFence;
        // Even a failed signal must prevent allocator/upload reuse on a retry.
        // Waiting for the unsignaled value will fail safely or time out.
        state.submitted = completed;
        if (failed(state.queue->Signal(state.fence.Get(), completed), "Signal DXR-to-DX11 fence", error)) return fail();
        if (failed(state.context11->Wait(state.fence11.Get(), completed), "Wait for hardware ray tracing in DX11", error)) return fail();
        if (failed(state.device->GetDeviceRemovedReason(), "Hardware ray tracing device status", error)) return fail();
        state.positionHash = positionHash; state.triangleHash = triangleHash; state.normalHash = normalHash;
        state.triangleCount = static_cast<std::uint32_t>(triangles.size()); state.geometryReady = true;
        if (pathLit) {
            if (restart) ++state.giStatus.historyRevision;
            state.historyKey = nextHistory; state.historyValid = true;
            state.accumulatedSamples = sampleOffset + batch;
            state.giStatus.effective = GlobalIlluminationMethod::PathTracing;
            state.giStatus.reason = state.accumulatedSamples >= targetSamples ? "Target samples reached." : "Accumulating linear HDR samples.";
        } else if (pathRequested) {
            state.giStatus.effective = GlobalIlluminationMethod::Disabled;
            state.giStatus.reason = "Path tracing is paused in this viewport debug mode.";
        } else {
            state.historyValid = false; state.accumulatedSamples = 0;
            state.giStatus.effective = GlobalIlluminationMethod::Disabled;
            state.giStatus.reason = "Global illumination is disabled; using the hardware preview.";
        }
        state.giStatus.accumulatedSamples = state.accumulatedSamples;
        ++state.dispatches;
        state.message = "D3D12 DXR 1.1 active: " + std::to_string(state.triangleCount) + " triangles, hardware primary rays, " +
            (frame.reflections ? "reflections on, " : "reflections off, ") + (frame.shadows ? "shadows on." : "shadows off.");
        if (pathRequested) state.message += " Path tracing " + std::to_string(state.accumulatedSamples) + "/" +
            std::to_string(targetSamples) + " samples. " + state.giStatus.reason;
        return true;
    } catch (const std::exception& exception) {
        if (recording) { state.list->Close(); state.geometryReady = false; }
        error = std::string("Unable to prepare hardware ray tracing: ") + exception.what();
        return fail();
    }
}

ID3D11Texture2D* HardwareRayTracing::output() const { return impl_->sharedOutput.Get(); }
std::string HardwareRayTracing::status() const { return impl_->message; }
std::uint64_t HardwareRayTracing::dispatchCount() const { return impl_->dispatches; }
GlobalIlluminationStatus HardwareRayTracing::globalIlluminationStatus() const { return impl_->giStatus; }
std::uint32_t HardwareRayTracing::validationErrorCount() const {
    const auto& queue = impl_->infoQueue;
    if (!queue) return 0;
    std::uint32_t errors = 0;
    const auto count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T bytes = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &bytes))) continue;
        std::vector<unsigned char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(index, message, &bytes)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION || message->Severity == D3D12_MESSAGE_SEVERITY_ERROR)) ++errors;
    }
    return errors;
}

} // namespace rae::renderer::d3d12
