#include "D3D12Rhi.h"

#include <d3d12sdklayers.h>
#include <d3d12shader.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace rae::renderer::d3d12 {
namespace {
using Microsoft::WRL::ComPtr;
using namespace rhi;
constexpr UINT sampledSlots = 24, constantSlots = 11, vertexSlots = 32;

bool initializeDebugRuntime(bool requested) {
    // Debug-layer selection is process-wide. Re-enabling it after another
    // device exists can remove that device; honor the first native-device
    // request and report the resulting capability through validationEnabled().
    static std::once_flag initialized;
    static bool enabled = false;
    std::call_once(initialized, [requested] {
        if (!requested) return;
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            enabled = true;
        }
    });
    return enabled;
}

[[noreturn]] void reject(const char* message) { throw std::runtime_error(std::string("D3D12 RHI: ") + message); }
void require(bool condition, const char* message) { if (!condition) reject(message); }
void checked(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream error;
    error << "D3D12 RHI: " << operation << " failed (HRESULT 0x" << std::hex
          << static_cast<unsigned long>(result) << ").";
    throw std::runtime_error(error.str());
}
std::wstring wide(std::string_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto size = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count) != count) return {};
    return result;
}
void nameObject(ID3D12Object* object, std::string_view name) {
    if (!object || name.empty()) return;
    const auto label = wide(name);
    if (!label.empty()) (void)object->SetName(label.c_str());
}
DXGI_FORMAT format(Format value) {
    switch (value) {
    case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::Depth32Float: return DXGI_FORMAT_D32_FLOAT;
    }
    reject("Unknown texture format.");
}
UINT pixelBytes(Format value) { return value == Format::RGBA16Float ? 8u : 4u; }
D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES result{};
    result.Type = type;
    result.CreationNodeMask = result.VisibleNodeMask = 1;
    return result;
}
D3D12_RESOURCE_DESC bufferDescription(UINT64 size) {
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}
ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    const auto heap = heapProperties(type);
    const auto desc = bufferDescription(size);
    ComPtr<ID3D12Resource> result;
    checked(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&result)), "Create buffer allocation");
    return result;
}
void fillUpload(ID3D12Resource* object, std::span<const std::byte> bytes, UINT64 allocatedSize) {
    void* mapped = nullptr;
    const D3D12_RANGE noReads{0, 0};
    checked(object->Map(0, &noReads, &mapped), "Map upload allocation");
    std::memset(mapped, 0, static_cast<std::size_t>(allocatedSize));
    if (!bytes.empty()) std::memcpy(mapped, bytes.data(), bytes.size());
    object->Unmap(0, nullptr);
}

// A range is returned only when its RAII owner (including recorded-command
// retention) is released. UI-owned descriptors use a separate deferred queue.
struct DescriptorPool {
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride = 0, capacity = 0;
    bool visible = false;
    std::vector<bool> used;
    std::mutex mutex;
    DescriptorPool(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool shaderVisible)
        : capacity(count), visible(shaderVisible), used(count, false) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type; desc.NumDescriptors = count;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        checked(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "Create descriptor heap");
        stride = device->GetDescriptorHandleIncrementSize(type);
    }
    UINT allocate(UINT count) {
        std::lock_guard lock(mutex);
        require(count > 0 && count <= capacity, "Invalid descriptor allocation size.");
        UINT run = 0;
        for (UINT index = 0; index < capacity; ++index) {
            run = used[index] ? 0 : run + 1;
            if (run != count) continue;
            const UINT first = index + 1 - count;
            for (UINT slot = first; slot <= index; ++slot) used[slot] = true;
            return first;
        }
        reject("Descriptor heap is exhausted.");
    }
    void release(UINT first, UINT count) noexcept {
        std::lock_guard lock(mutex);
        for (UINT index = first; index < first + count && index < capacity; ++index) used[index] = false;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) const {
        auto handle = heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) const {
        auto handle = heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * stride;
        return handle;
    }
};
struct DescriptorRange {
    std::shared_ptr<DescriptorPool> pool;
    UINT first = 0, count = 0;
    DescriptorRange(std::shared_ptr<DescriptorPool> source, UINT size)
        : pool(std::move(source)), first(pool->allocate(size)), count(size) {}
    DescriptorRange(const DescriptorRange&) = delete;
    DescriptorRange& operator=(const DescriptorRange&) = delete;
    ~DescriptorRange() { pool->release(first, count); }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT offset = 0) const { return pool->cpu(first + offset); }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT offset = 0) const { return pool->gpu(first + offset); }
};
struct DeviceIdentity {};
using Identity = std::shared_ptr<DeviceIdentity>;
struct Buffer12 final : Buffer {
    BufferDesc desc;
    Identity owner;
    ComPtr<ID3D12Resource> allocation;
    const BufferDesc& description() const noexcept override { return desc; }
};
struct Texture12 final : Texture {
    TextureDesc desc;
    Identity owner;
    ComPtr<ID3D12Resource> allocation;
    std::shared_ptr<DescriptorRange> sampled, sampledSource, colors, depths;
    std::vector<D3D12_RESOURCE_STATES> states;
    const TextureDesc& description() const noexcept override { return desc; }
};
struct Shader12 final : Shader {
    ShaderStage shaderStage = ShaderStage::Vertex;
    Identity owner;
    std::vector<std::byte> bytecode;
    ShaderStage stage() const noexcept override { return shaderStage; }
};
struct Pipeline12 final : GraphicsPipeline {
    GraphicsPipelineDesc desc;
    Identity owner;
    ComPtr<ID3D12PipelineState> object;
    const GraphicsPipelineDesc& description() const noexcept override { return desc; }
};
template<class Native, class Public>
Native& owned(const std::shared_ptr<Public>& handle, const Identity& identity) {
    require(handle != nullptr, "A required resource handle is null.");
    auto* object = dynamic_cast<Native*>(handle.get());
    require(object != nullptr && object->owner == identity, "Resource belongs to another backend or device.");
    return *object;
}
void validate(LoadOp op) {
    require(op == LoadOp::Load || op == LoadOp::Clear || op == LoadOp::DontCare, "Unknown attachment load operation.");
}
void validate(StoreOp op) { require(op == StoreOp::Store || op == StoreOp::DontCare, "Unknown attachment store operation."); }

class Device12 final : public D3D12RhiDevice, public CommandContext {
public:
    explicit Device12(bool validation);
    ~Device12() override;
    CommandContext& commands() noexcept override { return *this; }
    const Statistics& statistics() const noexcept override { return statistics_; }
    BufferHandle createBuffer(const BufferDesc&, std::span<const std::byte>) override;
    TextureHandle createTexture(const TextureDesc&) override;
    ShaderHandle createShader(ShaderStage, std::span<const std::byte>, std::string_view) override;
    GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) override;
    void writeBuffer(const BufferHandle&, std::span<const std::byte>) override;
    void bindConstantBuffers(ShaderStage, std::uint32_t, std::span<const BufferHandle>) override;
    void beginRenderPass(const RenderPassDesc&) override;
    void endRenderPass() override;
    void setViewport(const Viewport&) override;
    void bindGraphicsPipeline(const GraphicsPipelineHandle&) override;
    void bindVertexBuffers(std::uint32_t, std::span<const VertexBufferBinding>) override;
    void bindIndexBuffer(const BufferHandle&, std::uint32_t) override;
    void drawIndexed(std::uint32_t, std::uint32_t, std::int32_t) override;
    void drawIndexedInstanced(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::uint32_t) override;
    void draw(std::uint32_t, std::uint32_t) override;
    void attachWindow(void*, std::uint32_t, std::uint32_t) override;
    void detachWindow() override;
    void resize(std::uint32_t, std::uint32_t) override;
    TextureHandle backbuffer() const override;
    TextureHandle importTexture(ID3D12Resource*, const TextureDesc&, D3D12_RESOURCE_STATES) override;
    void beginFrame() override;
    void present(bool) override;
    void waitIdle() override;
    void submitAndWait() override;
    void prepareUi() override;
    void finishUiFrame() override;
    void releaseBorrowedUiTextures() override;
    ID3D12Device* device() const noexcept override { return device_.Get(); }
    ID3D12CommandQueue* queue() const noexcept override { return queue_.Get(); }
    ID3D12GraphicsCommandList* commandList() override { beginFrame(); return list_.Get(); }
    ID3D12DescriptorHeap* srvHeap() const noexcept override { return sampledPool_->heap.Get(); }
    std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE> allocateUiDescriptor() override;
    void freeUiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE) override;
    TextureHandle uploadRgba8(std::uint32_t, std::uint32_t, std::span<const std::byte>, std::string_view) override;
    std::uint64_t uiTextureId(const TextureHandle&) override;
    void copyTextureRegion(const TextureHandle&, const TextureHandle&, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t) override;
    void copyTextureRegionTo(const TextureHandle&, std::uint32_t, std::uint32_t, const TextureHandle&,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override;
    TextureReadback readTexture(const TextureHandle&, std::uint32_t) override;
    void beginDiagnosticFrame(std::uint64_t) override;
    void beginDiagnosticPass(std::string_view) override;
    void endDiagnosticPass() override;
    void endDiagnosticFrame() override;
    std::optional<DiagnosticTiming> pollDiagnosticTiming() override;
    std::vector<std::string> debugMessages() const override;
    std::string adapterName() const override { return adapterName_; }
    bool validationEnabled() const noexcept override { return infoQueue_ != nullptr; }
    std::uint32_t validationErrorCount() const noexcept override;
private:
    void createRootSignature();
    void acquireBackbuffers();
    std::shared_ptr<Texture12> wrapTexture(const TextureDesc&, ComPtr<ID3D12Resource>, D3D12_RESOURCE_STATES);
    std::shared_ptr<Texture12> wrapImportedTexture(const TextureDesc&, ComPtr<ID3D12Resource>, D3D12_RESOURCE_STATES);
    void transition(Texture12&, D3D12_RESOURCE_STATES, UINT layer = UINT_MAX);
    void waitFence(UINT64);
    void signalAndWait();
    void retain(const std::shared_ptr<void>& object) { pendingObjects_.push_back(object); }
    void validatePipeline(const Pipeline12&) const;
    void validateVertices(std::uint32_t, std::uint32_t) const;
    void bindDrawState();
    void bindAttachments();
    void releaseUiDescriptors();
    Identity identity_ = std::make_shared<DeviceIdentity>();
    ComPtr<IDXGIFactory4> factory_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> infoQueue_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12Resource> zeroConstants_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 nextFence_ = 0;
    bool open_ = false, activePass_ = false;
    std::uint32_t width_ = 0, height_ = 0;
    std::string adapterName_;
    std::shared_ptr<DescriptorPool> sampledPool_, sourcePool_, colorPool_, depthPool_;
    std::shared_ptr<DescriptorRange> nullSampled_, passTable_;
    std::vector<std::shared_ptr<Texture12>> backbuffers_;
    std::vector<std::weak_ptr<Texture12>> sampledTextures_;
    std::vector<std::shared_ptr<void>> pendingObjects_;
    // UI draw lists can borrow IDs before an intermediate readback flush. Their
    // owners survive until all UI queues complete, including no-present frames.
    std::vector<TextureHandle> uiBorrowedTextures_;
    std::vector<ComPtr<ID3D12Resource>> pendingAllocations_;
    std::vector<UINT> uiDescriptors_, retiredUiDescriptors_;
    std::array<std::array<BufferHandle, constantSlots>, 2> constants_{};
    std::array<VertexBufferBinding, vertexSlots> vertices_{};
    BufferHandle index_;
    UINT indexOffset_ = 0;
    GraphicsPipelineHandle pipeline_;
    RenderPassDesc pass_;
    Viewport viewport_{};
    Statistics statistics_;
    static constexpr UINT maxDiagnosticPasses = 64;
    static constexpr UINT diagnosticTickCount = 2 + maxDiagnosticPasses * 2;
    struct TimingSlot {
        ComPtr<ID3D12QueryHeap> heap;
        ComPtr<ID3D12Resource> readback;
        std::vector<std::string> passes;
        UINT64 frameId = 0, serial = 0, submissionFence = 0;
        bool inUse = false, resolved = false, passOpen = false;
    };
    std::array<TimingSlot, 4> diagnosticTimings_;
    TimingSlot* activeDiagnosticTiming_ = nullptr;
    UINT64 diagnosticFrequency_ = 0, diagnosticSerial_ = 0;
};

Device12::Device12(bool validation) {
    const bool debugEnabled = initializeDebugRuntime(validation);
    checked(CreateDXGIFactory2(debugEnabled ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)), "Create DXGI factory");
    ComPtr<IDXGIAdapter1> selected;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT result = factory_->EnumAdapters1(index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        checked(result, "Enumerate adapters");
        DXGI_ADAPTER_DESC1 desc{}; checked(adapter->GetDesc1(&desc), "Read adapter description");
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
            selected = std::move(adapter); break;
        }
    }
    if (!device_) {
        checked(factory_->EnumWarpAdapter(IID_PPV_ARGS(&selected)), "Get WARP adapter");
        checked(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), "Create D3D12 WARP device");
    }
    DXGI_ADAPTER_DESC1 desc{}; checked(selected->GetDesc1(&desc), "Read selected adapter");
    const int required = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    if (required > 1) {
        adapterName_.resize(static_cast<std::size_t>(required));
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName_.data(), required, nullptr, nullptr);
        adapterName_.pop_back();
    }
    if (debugEnabled) (void)device_.As(&infoQueue_);
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    checked(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), "Create direct queue");
    checked(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_)), "Create command allocator");
    checked(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
        IID_PPV_ARGS(&list_)), "Create graphics command list");
    checked(list_->Close(), "Close initial command list");
    checked(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "Create completion fence");
    sampledPool_ = std::make_shared<DescriptorPool>(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, true);
    sourcePool_ = std::make_shared<DescriptorPool>(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, false);
    colorPool_ = std::make_shared<DescriptorPool>(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8192, false);
    depthPool_ = std::make_shared<DescriptorPool>(device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4096, false);
    createRootSignature();
    zeroConstants_ = makeBuffer(device_.Get(), 65536, D3D12_HEAP_TYPE_UPLOAD);
    fillUpload(zeroConstants_.Get(), {}, 65536);
    nullSampled_ = std::make_shared<DescriptorRange>(sourcePool_, sampledSlots);
    for (UINT slot = 0; slot < sampledSlots; ++slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (slot == 4 || slot == 10 || slot == 11) {
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.Buffer.NumElements = 1;
            view.Buffer.StructureByteStride = slot == 4 ? 80 : 16;
        } else if (slot == 3) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.Texture2DArray.MipLevels = view.Texture2DArray.ArraySize = 1;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Format = slot == 6 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            view.Texture2D.MipLevels = 1;
        }
        device_->CreateShaderResourceView(nullptr, &view, nullSampled_->cpu(slot));
    }
    // Last construction step: no raw event handle can leak on earlier failures.
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(fenceEvent_ != nullptr, "Cannot create completion event.");
}
Device12::~Device12() {
    try { endRenderPass(); waitIdle(); } catch (...) {}
    if (fenceEvent_) CloseHandle(fenceEvent_);
}
void Device12::createRootSignature() {
    std::array<D3D12_ROOT_PARAMETER, constantSlots + 1> parameters{};
    for (UINT slot = 0; slot < constantSlots; ++slot) {
        parameters[slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[slot].Descriptor.ShaderRegister = slot;
        parameters[slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = sampledSlots;
    parameters[constantSlots].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[constantSlots].DescriptorTable.NumDescriptorRanges = 1;
    parameters[constantSlots].DescriptorTable.pDescriptorRanges = &range;
    parameters[constantSlots].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    for (UINT slot = 0; slot < samplers.size(); ++slot) {
        auto& sampler = samplers[slot];
        sampler.Filter = slot ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = slot ? D3D12_TEXTURE_ADDRESS_MODE_BORDER : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = slot ? D3D12_COMPARISON_FUNC_LESS_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        sampler.MaxAnisotropy = 1;
        sampler.MaxLOD = slot ? 0.0f : D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = slot;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size()); description.pParameters = parameters.data();
    description.NumStaticSamplers = static_cast<UINT>(samplers.size()); description.pStaticSamplers = samplers.data();
    description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, diagnostics;
    checked(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &diagnostics), "Serialize raster root signature");
    checked(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)), "Create raster root signature");
}

BufferHandle Device12::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial) {
    require(desc.byteSize > 0, "Buffer size must be nonzero.");
    require(initial.empty() || initial.size() == desc.byteSize, "Initial bytes must cover the complete buffer.");
    require(!desc.immutable || initial.size() == desc.byteSize, "Immutable buffers require complete initial data.");
    switch (desc.usage) {
    case BufferUsage::Vertex: break;
    case BufferUsage::Index: require(desc.byteSize % 4 == 0, "Index buffer size must be aligned to four bytes."); break;
    case BufferUsage::Constant:
        require(desc.byteSize % 16 == 0 && desc.byteSize <= 65536, "Constant buffers require 16-byte alignment and a 64 KiB maximum."); break;
    default: reject("Unknown buffer usage.");
    }
    auto result = std::make_shared<Buffer12>(); result->desc = desc; result->owner = identity_;
    const UINT64 allocated = desc.usage == BufferUsage::Constant ? (static_cast<UINT64>(desc.byteSize) + 255) & ~UINT64{255} : desc.byteSize;
    result->allocation = makeBuffer(device_.Get(), allocated, D3D12_HEAP_TYPE_UPLOAD);
    fillUpload(result->allocation.Get(), initial, allocated);
    nameObject(result->allocation.Get(), desc.name);
    ++statistics_.buffersCreated;
    if (!initial.empty()) ++statistics_.bufferUploads;
    return result;
}
void Device12::writeBuffer(const BufferHandle& handle, std::span<const std::byte> bytes) {
    auto& buffer = owned<Buffer12>(handle, identity_);
    require(!buffer.desc.immutable, "An immutable buffer cannot be updated.");
    require(bytes.size() == buffer.desc.byteSize, "Buffer writes must cover the complete buffer.");
    const UINT64 allocated = buffer.desc.usage == BufferUsage::Constant ?
        (static_cast<UINT64>(buffer.desc.byteSize) + 255) & ~UINT64{255} : buffer.desc.byteSize;
    auto version = makeBuffer(device_.Get(), allocated, D3D12_HEAP_TYPE_UPLOAD);
    fillUpload(version.Get(), bytes, allocated); nameObject(version.Get(), buffer.desc.name);
    // Both recorded earlier draws and future draws keep their own immutable
    // upload versions. The completion fence, never CPU scope, retires them.
    pendingAllocations_.push_back(buffer.allocation);
    buffer.allocation = std::move(version);
    ++statistics_.bufferUploads;
}

std::shared_ptr<Texture12> Device12::wrapTexture(const TextureDesc& desc,
    ComPtr<ID3D12Resource> allocation, D3D12_RESOURCE_STATES initial) {
    auto result = std::make_shared<Texture12>();
    result->desc = desc; result->owner = identity_; result->allocation = std::move(allocation);
    result->states.assign(desc.arrayLayers, initial); nameObject(result->allocation.Get(), desc.name);
    if (desc.format == Format::Depth32Float) {
        result->depths = std::make_shared<DescriptorRange>(depthPool_, desc.arrayLayers);
        for (UINT layer = 0; layer < desc.arrayLayers; ++layer) {
            D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format = DXGI_FORMAT_D32_FLOAT;
            view.ViewDimension = desc.arrayLayers > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY : D3D12_DSV_DIMENSION_TEXTURE2D;
            if (desc.arrayLayers > 1) { view.Texture2DArray.FirstArraySlice = layer; view.Texture2DArray.ArraySize = 1; }
            device_->CreateDepthStencilView(result->allocation.Get(), &view, result->depths->cpu(layer));
        }
    } else {
        result->colors = std::make_shared<DescriptorRange>(colorPool_, 1);
        device_->CreateRenderTargetView(result->allocation.Get(), nullptr, result->colors->cpu());
    }
    if (desc.sampled) {
        result->sampled = std::make_shared<DescriptorRange>(sampledPool_, 1);
        result->sampledSource = std::make_shared<DescriptorRange>(sourcePool_, 1);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = desc.format == Format::Depth32Float ? DXGI_FORMAT_R32_FLOAT : format(desc.format);
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = desc.arrayLayers > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (desc.arrayLayers > 1) { view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = desc.arrayLayers; }
        else view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(result->allocation.Get(), &view, result->sampledSource->cpu());
        device_->CopyDescriptorsSimple(1, result->sampled->cpu(), result->sampledSource->cpu(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sampledTextures_.push_back(result);
    }
    return result;
}
std::shared_ptr<Texture12> Device12::wrapImportedTexture(const TextureDesc& desc,
    ComPtr<ID3D12Resource> allocation, D3D12_RESOURCE_STATES initial) {
    auto result = std::make_shared<Texture12>();
    result->desc = desc; result->owner = identity_; result->allocation = std::move(allocation);
    result->states.assign(desc.arrayLayers, initial);
    nameObject(result->allocation.Get(), desc.name);
    if (desc.sampled) {
        result->sampled = std::make_shared<DescriptorRange>(sampledPool_, 1);
        result->sampledSource = std::make_shared<DescriptorRange>(sourcePool_, 1);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format(desc.format);
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = desc.arrayLayers > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (desc.arrayLayers > 1) { view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = desc.arrayLayers; }
        else view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(result->allocation.Get(), &view, result->sampledSource->cpu());
        device_->CopyDescriptorsSimple(1, result->sampled->cpu(), result->sampledSource->cpu(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sampledTextures_.push_back(result);
    }
    return result;
}

TextureHandle Device12::createTexture(const TextureDesc& desc) {
    require(desc.width && desc.height && desc.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        desc.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid texture dimensions.");
    require(desc.arrayLayers && desc.arrayLayers <= D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION, "Invalid texture array size.");
    D3D12_RESOURCE_DESC native{}; native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    native.Width = desc.width; native.Height = desc.height;
    native.DepthOrArraySize = static_cast<UINT16>(desc.arrayLayers); native.MipLevels = 1;
    native.Format = desc.format == Format::Depth32Float ? DXGI_FORMAT_R32_TYPELESS : format(desc.format);
    native.SampleDesc.Count = 1;
    native.Flags = desc.format == Format::Depth32Float ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{}; clear.Format = format(desc.format);
    if (desc.format == Format::Depth32Float) clear.DepthStencil.Depth = 1.0f;
    const auto heap = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> allocation;
    checked(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &native, D3D12_RESOURCE_STATE_COMMON,
        &clear, IID_PPV_ARGS(&allocation)), "Create texture");
    auto result = wrapTexture(desc, std::move(allocation), D3D12_RESOURCE_STATE_COMMON);
    ++statistics_.texturesCreated;
    return result;
}

ShaderHandle Device12::createShader(ShaderStage stage, std::span<const std::byte> bytecode, std::string_view) {
    require(stage == ShaderStage::Vertex || stage == ShaderStage::Pixel, "Unknown shader stage.");
    require(!bytecode.empty(), "Shader bytecode is empty.");
    ComPtr<ID3D12ShaderReflection> reflection;
    checked(D3DReflect(bytecode.data(), bytecode.size(), IID_PPV_ARGS(&reflection)), "Reflect raster shader bytecode");
    D3D12_SHADER_DESC desc{}; checked(reflection->GetDesc(&desc), "Read raster shader reflection");
    const auto type = D3D12_SHVER_GET_TYPE(desc.Version);
    require(type == static_cast<UINT>(stage == ShaderStage::Vertex ? D3D12_SHVER_VERTEX_SHADER : D3D12_SHVER_PIXEL_SHADER), "Shader bytecode stage does not match its declaration.");
    auto result = std::make_shared<Shader12>(); result->owner = identity_; result->shaderStage = stage;
    result->bytecode.assign(bytecode.begin(), bytecode.end()); ++statistics_.shadersCreated;
    return result;
}
GraphicsPipelineHandle Device12::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    auto& vertex = owned<Shader12>(desc.vertexShader, identity_);
    require(vertex.stage() == ShaderStage::Vertex, "Pipeline vertex stage is not a vertex shader.");
    Shader12* pixel = desc.pixelShader ? &owned<Shader12>(desc.pixelShader, identity_) : nullptr;
    require(!pixel || pixel->stage() == ShaderStage::Pixel, "Pipeline pixel stage is not a pixel shader.");
    require(desc.colorFormats.size() <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, "Too many pipeline color targets.");
    require(!desc.depthFormat || *desc.depthFormat == Format::Depth32Float, "Invalid pipeline depth format.");
    require(!desc.depthEnable || desc.depthFormat.has_value(), "Depth testing requires a depth format.");
    require(desc.attributes.size() <= D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT, "Too many input attributes.");
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
    for (const auto& attribute : desc.attributes) {
        require(!attribute.semantic.empty() && attribute.slot < vertexSlots && attribute.components >= 2 &&
            attribute.components <= 4 && attribute.offset <= 2048 - attribute.components * 4, "Invalid vertex attribute.");
        inputs.push_back({attribute.semantic.c_str(), attribute.semanticIndex,
            attribute.components == 2 ? DXGI_FORMAT_R32G32_FLOAT : attribute.components == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT,
            attribute.slot, attribute.offset,
            attribute.perInstance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
            attribute.perInstance ? 1u : 0u});
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC native{};
    native.pRootSignature = root_.Get();
    native.VS = {vertex.bytecode.data(), vertex.bytecode.size()};
    if (pixel) native.PS = {pixel->bytecode.data(), pixel->bytecode.size()};
    native.InputLayout = {inputs.data(), static_cast<UINT>(inputs.size())};
    native.SampleMask = UINT_MAX;
    native.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    native.SampleDesc.Count = 1;
    native.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    switch (desc.cullMode) {
    case CullMode::None: native.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
    case CullMode::Front: native.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
    case CullMode::Back: native.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
    default: reject("Unknown culling mode.");
    }
    native.RasterizerState.FrontCounterClockwise = desc.frontCounterClockwise;
    native.RasterizerState.DepthClipEnable = TRUE;
    native.DepthStencilState.DepthEnable = desc.depthEnable;
    native.DepthStencilState.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    const auto comparison = static_cast<UINT>(desc.depthCompare);
    require(comparison <= static_cast<UINT>(CompareOperation::Always), "Unknown depth comparison.");
    native.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(comparison + 1);
    native.DepthStencilState.StencilReadMask = native.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    native.DepthStencilState.FrontFace = native.DepthStencilState.BackFace = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    native.NumRenderTargets = static_cast<UINT>(desc.colorFormats.size());
    for (UINT index = 0; index < native.NumRenderTargets; ++index) {
        require(desc.colorFormats[index] != Format::Depth32Float, "Color attachment has a depth format.");
        native.RTVFormats[index] = format(desc.colorFormats[index]);
    }
    if (desc.depthFormat) native.DSVFormat = format(*desc.depthFormat);
    for (auto& blend : native.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    auto result = std::make_shared<Pipeline12>(); result->owner = identity_; result->desc = desc;
    checked(device_->CreateGraphicsPipelineState(&native, IID_PPV_ARGS(&result->object)), "Create graphics pipeline");
    nameObject(result->object.Get(), desc.name); ++statistics_.pipelinesCreated;
    return result;
}

void Device12::beginFrame() {
    if (open_) return;
    // A previous submit may have reported a timeout. Never reset its allocator
    // merely because the failed wait unwound back to the application.
    if (nextFence_) waitFence(nextFence_);
    checked(allocator_->Reset(), "Reset completed command allocator");
    checked(list_->Reset(allocator_.Get(), nullptr), "Open graphics command list");
    open_ = true;
    if (activePass_) bindAttachments();
}
void Device12::transition(Texture12& texture, D3D12_RESOURCE_STATES after, UINT layer) {
    beginFrame();
    if (layer != UINT_MAX) require(layer < texture.states.size(), "Texture transition layer is out of range.");
    for (UINT index = 0; index < texture.states.size(); ++index) {
        if (layer != UINT_MAX && layer != index) continue;
        auto& before = texture.states[index];
        if (before == after) continue;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.allocation.Get();
        barrier.Transition.Subresource = index;
        barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
        list_->ResourceBarrier(1, &barrier);
        before = after;
    }
}
void Device12::bindConstantBuffers(ShaderStage stage, std::uint32_t first, std::span<const BufferHandle> buffers) {
    require(stage == ShaderStage::Vertex || stage == ShaderStage::Pixel, "Unknown constant buffer shader stage.");
    require(first <= constantSlots && buffers.size() <= constantSlots - first, "Constant bindings exceed the scene root signature.");
    for (const auto& handle : buffers) if (handle)
        require(owned<Buffer12>(handle, identity_).desc.usage == BufferUsage::Constant, "Non-constant buffer bound as constants.");
    auto& destination = constants_[stage == ShaderStage::Vertex ? 0 : 1];
    for (std::size_t index = 0; index < buffers.size(); ++index) destination[first + index] = buffers[index];
    // Addresses are emitted again at every draw: writing a bound logical
    // constant buffer must select its new version without modifying old draws.
}
void Device12::beginRenderPass(const RenderPassDesc& desc) {
    require(!activePass_, "The preceding render pass has not ended.");
    require(desc.colors.size() <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT &&
        (!desc.colors.empty() || desc.depth), "Invalid render pass attachment count.");
    UINT width = 0, height = 0;
    std::vector<ID3D12Resource*> writes;
    const auto attachment = [&](Texture12& texture) {
        if (width) require(width == texture.desc.width && height == texture.desc.height, "Pass attachment dimensions differ.");
        else { width = texture.desc.width; height = texture.desc.height; }
        require(std::find(writes.begin(), writes.end(), texture.allocation.Get()) == writes.end(), "Pass writes a texture more than once.");
        writes.push_back(texture.allocation.Get());
    };
    for (const auto& color : desc.colors) {
        validate(color.load); validate(color.store);
        auto& texture = owned<Texture12>(color.texture, identity_);
        require(texture.colors != nullptr && texture.desc.format != Format::Depth32Float, "Color attachment has no RTV.");
        for (const auto channel : color.clear) if (color.load == LoadOp::Clear)
            require(std::isfinite(channel), "Color clear must be finite.");
        attachment(texture);
    }
    if (desc.depth) {
        validate(desc.depth->load); validate(desc.depth->store);
        auto& texture = owned<Texture12>(desc.depth->texture, identity_);
        require(texture.depths && desc.depth->layer < texture.desc.arrayLayers, "Depth attachment layer is invalid.");
        require(std::isfinite(desc.depth->clearDepth) && desc.depth->clearDepth >= 0 && desc.depth->clearDepth <= 1,
            "Depth clear must be between zero and one.");
        attachment(texture);
    }
    std::array<bool, sampledSlots> usedSlots{};
    for (const auto& read : desc.reads) {
        require(read.slot < sampledSlots && !usedSlots[read.slot], "Invalid or duplicate sampled read slot.");
        usedSlots[read.slot] = true;
        auto& texture = owned<Texture12>(read.texture, identity_);
        require(texture.sampled != nullptr, "Pass sampled texture has no SRV.");
        require(std::find(writes.begin(), writes.end(), texture.allocation.Get()) == writes.end(), "Pass reads and writes the same texture.");
        require(read.slot != 4 && read.slot != 10 && read.slot != 11, "Structured-buffer slots cannot bind a texture.");
        if (read.slot == 3)
            require(texture.desc.format == Format::Depth32Float, "The shadow slot requires a sampled depth array.");
        else require(texture.desc.arrayLayers == 1, "Sampled texture dimension does not match the fixed scene shader slot.");
    }
    RenderPassDesc candidate = desc;
    auto table = std::make_shared<DescriptorRange>(sampledPool_, sampledSlots);
    device_->CopyDescriptorsSimple(sampledSlots, table->cpu(), nullSampled_->cpu(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (const auto& read : candidate.reads) {
        const auto& texture = owned<Texture12>(read.texture, identity_);
        if (read.slot == 3) {
            // Texture2DArray is also a valid view of a one-layer texture. The
            // fixed shadow shader must keep that dimension for one cascade.
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = texture.desc.arrayLayers;
            device_->CreateShaderResourceView(texture.allocation.Get(), &view, table->cpu(read.slot));
        } else device_->CopyDescriptorsSimple(1, table->cpu(read.slot), texture.sampledSource->cpu(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    // Complete validation and descriptor allocation before publishing a pass.
    beginFrame();
    for (const auto& read : candidate.reads) {
        retain(read.texture);
        transition(owned<Texture12>(read.texture, identity_),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    for (const auto& color : candidate.colors) {
        retain(color.texture); transition(owned<Texture12>(color.texture, identity_), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (candidate.depth) {
        retain(candidate.depth->texture);
        transition(owned<Texture12>(candidate.depth->texture, identity_), D3D12_RESOURCE_STATE_DEPTH_WRITE, candidate.depth->layer);
    }
    retain(table);
    pass_ = std::move(candidate); passTable_ = std::move(table); activePass_ = true;
    bindAttachments();
    for (const auto& color : pass_.colors) if (color.load == LoadOp::Clear)
        list_->ClearRenderTargetView(owned<Texture12>(color.texture, identity_).colors->cpu(), color.clear.data(), 0, nullptr);
    if (pass_.depth && pass_.depth->load == LoadOp::Clear)
        list_->ClearDepthStencilView(owned<Texture12>(pass_.depth->texture, identity_).depths->cpu(pass_.depth->layer),
            D3D12_CLEAR_FLAG_DEPTH, pass_.depth->clearDepth, 0, 0, nullptr);
    // DontCare permits retained data. It does not promise initialization.
    ++statistics_.renderPasses;
}
void Device12::bindAttachments() {
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> colors{};
    for (UINT index = 0; index < pass_.colors.size(); ++index)
        colors[index] = owned<Texture12>(pass_.colors[index].texture, identity_).colors->cpu();
    D3D12_CPU_DESCRIPTOR_HANDLE depth{};
    if (pass_.depth) depth = owned<Texture12>(pass_.depth->texture, identity_).depths->cpu(pass_.depth->layer);
    list_->OMSetRenderTargets(static_cast<UINT>(pass_.colors.size()), colors.data(), FALSE, pass_.depth ? &depth : nullptr);
}
void Device12::endRenderPass() {
    if (!activePass_) return;
    if (open_) {
        list_->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        list_->IASetIndexBuffer(nullptr);
        const std::array<D3D12_VERTEX_BUFFER_VIEW, vertexSlots> empty{};
        list_->IASetVertexBuffers(0, vertexSlots, empty.data());
    }
    activePass_ = false; pass_ = {}; passTable_.reset(); pipeline_.reset();
    vertices_ = {}; index_.reset(); indexOffset_ = 0;
}
void Device12::setViewport(const Viewport& viewport) {
    require(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) &&
        std::isfinite(viewport.height) && std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth), "Viewport values must be finite.");
    require(viewport.width > 0 && viewport.height > 0 && viewport.width <= D3D12_VIEWPORT_BOUNDS_MAX &&
        viewport.height <= D3D12_VIEWPORT_BOUNDS_MAX && viewport.x >= D3D12_VIEWPORT_BOUNDS_MIN && viewport.y >= D3D12_VIEWPORT_BOUNDS_MIN &&
        viewport.x + viewport.width <= D3D12_VIEWPORT_BOUNDS_MAX && viewport.y + viewport.height <= D3D12_VIEWPORT_BOUNDS_MAX,
        "Viewport exceeds D3D12 bounds.");
    require(viewport.minDepth >= 0 && viewport.maxDepth <= 1 && viewport.minDepth <= viewport.maxDepth, "Invalid viewport depth range.");
    viewport_ = viewport; beginFrame();
    const D3D12_VIEWPORT native{viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth};
    list_->RSSetViewports(1, &native);
    const D3D12_RECT scissor{static_cast<LONG>(std::max(0.0f, std::floor(viewport.x))),
        static_cast<LONG>(std::max(0.0f, std::floor(viewport.y))),
        static_cast<LONG>(std::max(0.0f, std::ceil(viewport.x + viewport.width))),
        static_cast<LONG>(std::max(0.0f, std::ceil(viewport.y + viewport.height)))};
    list_->RSSetScissorRects(1, &scissor);
}
void Device12::validatePipeline(const Pipeline12& pipeline) const {
    require(pipeline.desc.colorFormats.size() == pass_.colors.size(), "Pipeline and pass color counts differ.");
    for (std::size_t index = 0; index < pass_.colors.size(); ++index)
        require(pipeline.desc.colorFormats[index] == pass_.colors[index].texture->description().format, "Pipeline and pass color formats differ.");
    require(pipeline.desc.depthFormat.has_value() == pass_.depth.has_value(), "Pipeline and pass depth attachments differ.");
    if (pass_.depth) require(*pipeline.desc.depthFormat == pass_.depth->texture->description().format, "Pipeline and pass depth formats differ.");
}
void Device12::bindGraphicsPipeline(const GraphicsPipelineHandle& handle) {
    auto& pipeline = owned<Pipeline12>(handle, identity_);
    if (activePass_) validatePipeline(pipeline);
    beginFrame(); list_->SetPipelineState(pipeline.object.Get());
    list_->SetGraphicsRootSignature(root_.Get());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pipeline_ = handle; retain(handle);
}
void Device12::bindVertexBuffers(std::uint32_t first, std::span<const VertexBufferBinding> bindings) {
    require(first <= vertexSlots && bindings.size() <= vertexSlots - first, "Vertex binding exceeds available slots.");
    std::vector<D3D12_VERTEX_BUFFER_VIEW> views; views.reserve(bindings.size());
    for (const auto& binding : bindings) {
        if (!binding.buffer) {
            require(binding.stride == 0 && binding.offset == 0, "Unbound vertex stream must have zero stride and offset.");
            views.push_back({}); continue;
        }
        const auto& buffer = owned<Buffer12>(binding.buffer, identity_);
        require(buffer.desc.usage == BufferUsage::Vertex && binding.offset <= buffer.desc.byteSize && binding.stride <= 2048,
            "Invalid vertex buffer usage, offset or stride.");
        views.push_back({buffer.allocation->GetGPUVirtualAddress() + binding.offset,
            buffer.desc.byteSize - binding.offset, binding.stride});
    }
    if (views.empty()) return;
    beginFrame(); list_->IASetVertexBuffers(first, static_cast<UINT>(views.size()), views.data());
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        vertices_[first + index] = bindings[index];
        if (bindings[index].buffer) { retain(bindings[index].buffer); pendingAllocations_.push_back(owned<Buffer12>(bindings[index].buffer, identity_).allocation); }
    }
}
void Device12::bindIndexBuffer(const BufferHandle& handle, std::uint32_t offset) {
    if (!handle) {
        require(offset == 0, "Unbound index buffer requires a zero offset.");
        beginFrame(); list_->IASetIndexBuffer(nullptr); index_.reset(); indexOffset_ = 0; return;
    }
    const auto& buffer = owned<Buffer12>(handle, identity_);
    require(buffer.desc.usage == BufferUsage::Index && offset % 4 == 0 && offset <= buffer.desc.byteSize, "Invalid index buffer usage or offset.");
    beginFrame();
    const D3D12_INDEX_BUFFER_VIEW view{buffer.allocation->GetGPUVirtualAddress() + offset, buffer.desc.byteSize - offset, DXGI_FORMAT_R32_UINT};
    list_->IASetIndexBuffer(&view); index_ = handle; indexOffset_ = offset;
    retain(handle); pendingAllocations_.push_back(buffer.allocation);
}
void Device12::validateVertices(std::uint32_t first, std::uint32_t count) const {
    if (!count) return;
    const auto& pipeline = owned<Pipeline12>(pipeline_, identity_);
    for (const auto& attribute : pipeline.desc.attributes) {
        if (attribute.perInstance) continue;
        const auto& binding = vertices_[attribute.slot];
        const auto& buffer = owned<Buffer12>(binding.buffer, identity_);
        const UINT64 last = static_cast<UINT64>(first) + count - 1;
        const UINT64 end = binding.offset + last * binding.stride + attribute.offset + attribute.components * 4u;
        require(end <= buffer.desc.byteSize, "Vertex attribute fetch exceeds its buffer.");
    }
}
void Device12::bindDrawState() {
    require(activePass_, "Draw issued outside a render pass.");
    const auto& pipeline = owned<Pipeline12>(pipeline_, identity_); validatePipeline(pipeline);
    require(viewport_.width > 0 && viewport_.height > 0, "Draw requires a viewport.");
    beginFrame(); bindAttachments(); setViewport(viewport_);
    list_->SetPipelineState(pipeline.object.Get()); list_->SetGraphicsRootSignature(root_.Get());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12DescriptorHeap* heaps[] = {sampledPool_->heap.Get()}; list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootDescriptorTable(constantSlots, passTable_->gpu());
    for (UINT slot = 0; slot < constantSlots; ++slot) {
        const auto& vertex = constants_[0][slot]; const auto& pixel = constants_[1][slot];
        require(!vertex || !pixel || vertex == pixel, "The scene root signature requires matching vertex/pixel constant buffers at shared slots.");
        const auto& handle = pixel ? pixel : vertex;
        auto* allocation = zeroConstants_.Get();
        if (handle) {
            const auto& buffer = owned<Buffer12>(handle, identity_);
            allocation = buffer.allocation.Get(); retain(handle); pendingAllocations_.push_back(buffer.allocation);
        }
        list_->SetGraphicsRootConstantBufferView(slot, allocation->GetGPUVirtualAddress());
    }
    // Never assume native UI or a preceding list kept our IA state intact.
    std::array<D3D12_VERTEX_BUFFER_VIEW, vertexSlots> views{};
    for (UINT slot = 0; slot < vertexSlots; ++slot) if (vertices_[slot].buffer) {
        const auto& binding = vertices_[slot]; const auto& buffer = owned<Buffer12>(binding.buffer, identity_);
        views[slot] = {buffer.allocation->GetGPUVirtualAddress() + binding.offset, buffer.desc.byteSize - binding.offset, binding.stride};
        retain(binding.buffer); pendingAllocations_.push_back(buffer.allocation);
    }
    list_->IASetVertexBuffers(0, vertexSlots, views.data());
    if (index_) {
        const auto& buffer = owned<Buffer12>(index_, identity_);
        const D3D12_INDEX_BUFFER_VIEW view{buffer.allocation->GetGPUVirtualAddress() + indexOffset_, buffer.desc.byteSize - indexOffset_, DXGI_FORMAT_R32_UINT};
        list_->IASetIndexBuffer(&view); retain(index_); pendingAllocations_.push_back(buffer.allocation);
    } else list_->IASetIndexBuffer(nullptr);
    retain(pipeline_);
}
void Device12::drawIndexed(std::uint32_t count, std::uint32_t first, std::int32_t base) {
    require(activePass_, "Indexed draw issued outside a render pass.");
    const auto& indices = owned<Buffer12>(index_, identity_);
    require(static_cast<UINT64>(indexOffset_) + (static_cast<UINT64>(first) + count) * 4 <= indices.desc.byteSize,
        "Indexed draw exceeds its index buffer.");
    validateVertices(0, count ? 1u : 0u); bindDrawState();
    list_->DrawIndexedInstanced(count, 1, first, base, 0); ++statistics_.drawCalls; ++statistics_.indexedDrawCalls;
}
void Device12::draw(std::uint32_t count, std::uint32_t first) {
    require(activePass_, "Draw issued outside a render pass.");
    validateVertices(first, count); bindDrawState();
    list_->DrawInstanced(count, 1, first, 0); ++statistics_.drawCalls;
}
void Device12::drawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount,
    std::uint32_t firstIndex, std::int32_t baseVertex, std::uint32_t firstInstance) {
    if (!indexCount || !instanceCount) return;
    require(activePass_, "Instanced indexed draw issued outside a render pass.");
    const auto& indices = owned<Buffer12>(index_, identity_);
    require(static_cast<UINT64>(indexOffset_) + (static_cast<UINT64>(firstIndex) + indexCount) * 4 <= indices.desc.byteSize,
        "Instanced indexed draw exceeds its index buffer.");
    validateVertices(0, 1); bindDrawState();
    list_->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
    ++statistics_.drawCalls; ++statistics_.indexedDrawCalls;
}

void Device12::waitFence(UINT64 value) {
    if (fence_->GetCompletedValue() == UINT64_MAX) reject("Device removed while waiting for GPU completion.");
    if (fence_->GetCompletedValue() >= value) return;
    checked(fence_->SetEventOnCompletion(value, fenceEvent_), "Set completion event");
    const ULONGLONG deadline = GetTickCount64() + 10000;
    for (;;) {
        const UINT64 completed = fence_->GetCompletedValue();
        require(completed != UINT64_MAX, "Device removed during GPU completion wait.");
        if (completed >= value) return;
        checked(device_->GetDeviceRemovedReason(), "GPU completion/device status");
        const ULONGLONG now = GetTickCount64();
        require(now < deadline, "GPU completion wait timed out after ten seconds.");
        const DWORD result = WaitForSingleObject(fenceEvent_, static_cast<DWORD>(std::min<ULONGLONG>(250, deadline - now)));
        require(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, "Cannot wait for GPU completion event.");
    }
}
void Device12::signalAndWait() {
    const UINT64 value = ++nextFence_;
    checked(queue_->Signal(fence_.Get(), value), "Signal queue completion");
    // Only frames whose ResolveQueryData has been recorded in the submitted
    // list belong to this signal. A readback flush during an open diagnostic
    // frame must not mark that still-incomplete sample as GPU-complete.
    for (auto& slot : diagnosticTimings_)
        if (slot.inUse && slot.resolved && !slot.submissionFence) slot.submissionFence = value;
    waitFence(value);
}
void Device12::releaseUiDescriptors() {
    for (const UINT index : retiredUiDescriptors_) sampledPool_->release(index, 1);
    retiredUiDescriptors_.clear();
}
void Device12::submitAndWait() {
    if (open_) {
        checked(list_->Close(), "Close graphics command list");
        open_ = false;
        ID3D12CommandList* lists[] = {list_.Get()}; queue_->ExecuteCommandLists(1, lists);
    }
    // Only this main queue is completed here. The vendored platform-window UI
    // queues complete at their SwapBuffers; finishUiFrame owns that boundary.
    // One allocator and synchronous retirement favor correctness over overlap.
    signalAndWait();
    pendingObjects_.clear(); pendingAllocations_.clear();
}
void Device12::waitIdle() { submitAndWait(); }

void Device12::acquireBackbuffers() {
    DXGI_SWAP_CHAIN_DESC1 swapDesc{}; checked(swapchain_->GetDesc1(&swapDesc), "Read swapchain dimensions");
    std::vector<std::shared_ptr<Texture12>> candidate; candidate.reserve(swapDesc.BufferCount);
    for (UINT index = 0; index < swapDesc.BufferCount; ++index) {
        ComPtr<ID3D12Resource> allocation;
        checked(swapchain_->GetBuffer(index, IID_PPV_ARGS(&allocation)), "Get swapchain image");
        candidate.push_back(wrapTexture({swapDesc.Width, swapDesc.Height, 1, Format::RGBA8Unorm, false,
            "Swapchain image " + std::to_string(index)}, std::move(allocation), D3D12_RESOURCE_STATE_PRESENT));
    }
    backbuffers_ = std::move(candidate); width_ = swapDesc.Width; height_ = swapDesc.Height;
}
void Device12::attachWindow(void* window, std::uint32_t width, std::uint32_t height) {
    require(window && width && height, "A valid window and nonzero dimensions are required.");
    require(width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        "Window dimensions exceed D3D12 texture limits.");
    detachWindow();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width; desc.Height = height; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    ComPtr<IDXGISwapChain1> swapchain;
    checked(factory_->CreateSwapChainForHwnd(queue_.Get(), static_cast<HWND>(window), &desc,
        nullptr, nullptr, &swapchain), "Create native D3D12 swapchain");
    checked(swapchain.As(&swapchain_), "Query D3D12 swapchain");
    checked(factory_->MakeWindowAssociation(static_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER), "Disable automatic Alt-Enter");
    acquireBackbuffers();
}
void Device12::detachWindow() {
    endRenderPass();
    if (queue_) waitIdle();
    uiBorrowedTextures_.clear();
    backbuffers_.clear(); swapchain_.Reset(); width_ = height_ = 0;
}
void Device12::resize(std::uint32_t width, std::uint32_t height) {
    require(swapchain_ != nullptr, "Resize requires an attached window.");
    if (!width || !height) return;
    require(width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        "Resize dimensions exceed D3D12 texture limits.");
    if (width == width_ && height == height_ && !backbuffers_.empty()) return;
    endRenderPass();
    for (const auto& image : backbuffers_) transition(*image, D3D12_RESOURCE_STATE_PRESENT);
    waitIdle();
    for (const auto& image : backbuffers_)
        require(image.use_count() == 1, "Release borrowed swapchain images before resizing the window.");
    backbuffers_.clear();
    const HRESULT result = swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result)) {
        // DXGI retains the old surface on failure. Preserve a usable old target
        // so requesting its unchanged dimensions can recover and be retried.
        try { acquireBackbuffers(); } catch (...) {}
        checked(result, "Resize native D3D12 swapchain");
    }
    acquireBackbuffers();
}
TextureHandle Device12::backbuffer() const {
    require(swapchain_ != nullptr && !backbuffers_.empty(), "No attached swapchain backbuffer.");
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    require(index < backbuffers_.size(), "Swapchain returned an invalid image index.");
    return backbuffers_[index];
}
TextureHandle Device12::importTexture(ID3D12Resource* resource, const TextureDesc& desc,
    D3D12_RESOURCE_STATES currentState) {
    require(resource != nullptr, "Cannot import a null D3D12 texture.");
    require(desc.width && desc.height && desc.arrayLayers, "Imported texture description is invalid.");
    ComPtr<ID3D12Resource> held(resource);
    return wrapImportedTexture(desc, std::move(held), currentState);
}
void Device12::present(bool vsync) {
    require(swapchain_ != nullptr, "Present requires an attached window.");
    endRenderPass();
    auto target = backbuffer(); transition(owned<Texture12>(target, identity_), D3D12_RESOURCE_STATE_PRESENT);
    retain(target); submitAndWait();
    checked(swapchain_->Present(vsync ? 1 : 0, 0), "Present native D3D12 swapchain");
    // Complete presentation before the next frame reuses its allocator. UI
    // borrowing retires independently in finishUiFrame, after all UI queues.
    signalAndWait();
}
void Device12::prepareUi() {
    endRenderPass(); beginFrame();
    for (auto it = sampledTextures_.begin(); it != sampledTextures_.end();) {
        auto texture = it->lock();
        if (!texture) { it = sampledTextures_.erase(it); continue; }
        retain(texture);
        transition(*texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ++it;
    }
    auto target = backbuffer(); retain(target);
    auto& texture = owned<Texture12>(target, identity_); transition(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtv = texture.colors->cpu(); list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = {sampledPool_->heap.Get()}; list_->SetDescriptorHeaps(1, heaps);
}
void Device12::finishUiFrame() {
    require(!activePass_, "Finish UI frame requires the graphics pass to have ended.");
    // The caller has returned from RenderPlatformWindowsDefault, including the
    // vendored secondary queues' fence waits in Renderer_SwapBuffers. Finish
    // any main work even when the main HWND was minimized and never presented.
    submitAndWait();
    uiBorrowedTextures_.clear();
    releaseUiDescriptors();
}
void Device12::releaseBorrowedUiTextures() {
    // Platform UI may still hold descriptors from the previous surface; the
    // caller waits for the queue before resizing. Dropping the shared owners is
    // safe here and prevents a stale borrow from rejecting ResizeBuffers.
    endRenderPass();
    pass_ = {}; passTable_.reset(); pipeline_.reset();
    waitIdle();
    uiBorrowedTextures_.clear();
    releaseUiDescriptors();
    pendingObjects_.clear();
    pendingAllocations_.clear();
}
std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE> Device12::allocateUiDescriptor() {
    const UINT index = sampledPool_->allocate(1);
    try { uiDescriptors_.push_back(index); }
    catch (...) { sampledPool_->release(index, 1); throw; }
    return {sampledPool_->cpu(index), sampledPool_->gpu(index)};
}
void Device12::freeUiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr) return;
    const SIZE_T start = sampledPool_->cpu(0).ptr;
    require(handle.ptr >= start && (handle.ptr - start) % sampledPool_->stride == 0, "UI descriptor does not belong to the global SRV heap.");
    const SIZE_T position = (handle.ptr - start) / sampledPool_->stride;
    require(position < sampledPool_->capacity, "UI descriptor is outside the global SRV heap.");
    const UINT index = static_cast<UINT>(position);
    const auto found = std::find(uiDescriptors_.begin(), uiDescriptors_.end(), index);
    require(found != uiDescriptors_.end(), "UI descriptor was not allocated here or was already freed.");
    retiredUiDescriptors_.push_back(index); uiDescriptors_.erase(found);
    // Recycling waits for finishUiFrame, not an intermediate main-queue flush:
    // platform UI lists can still borrow the old descriptor on other queues.
}
std::uint64_t Device12::uiTextureId(const TextureHandle& handle) {
    if (!handle) return 0;
    auto& texture = owned<Texture12>(handle, identity_);
    require(texture.sampled != nullptr && texture.desc.arrayLayers == 1 && texture.desc.format != Format::Depth32Float,
        "UI texture IDs require a sampled single-layer color texture.");
    // UI draw lists borrow this descriptor before they record GPU commands.
    // Retain the texture immediately, even if its owner replaces it meanwhile.
    if (std::find(uiBorrowedTextures_.begin(), uiBorrowedTextures_.end(), handle) == uiBorrowedTextures_.end())
        uiBorrowedTextures_.push_back(handle);
    return texture.sampled->gpu().ptr;
}
TextureHandle Device12::uploadRgba8(std::uint32_t width, std::uint32_t height,
    std::span<const std::byte> bytes, std::string_view name) {
    require(static_cast<UINT64>(width) * height * 4 == bytes.size(), "RGBA8 upload byte count does not match dimensions.");
    auto handle = createTexture({width, height, 1, Format::RGBA8Unorm, true, std::string(name)});
    auto& texture = owned<Texture12>(handle, identity_);
    const auto desc = texture.allocation->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
    auto upload = makeBuffer(device_.Get(), total, D3D12_HEAP_TYPE_UPLOAD);
    void* mapped = nullptr; const D3D12_RANGE noReads{0, 0};
    checked(upload->Map(0, &noReads, &mapped), "Map RGBA8 upload");
    std::memset(mapped, 0, static_cast<std::size_t>(total));
    for (UINT row = 0; row < height; ++row)
        std::memcpy(static_cast<std::byte*>(mapped) + footprint.Offset + static_cast<SIZE_T>(row) * footprint.Footprint.RowPitch,
            bytes.data() + static_cast<SIZE_T>(row) * width * 4, static_cast<SIZE_T>(width) * 4);
    upload->Unmap(0, nullptr);
    retain(handle); pendingAllocations_.push_back(upload); transition(texture, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = texture.allocation.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return handle;
}
void Device12::copyTextureRegion(const TextureHandle& destinationHandle, const TextureHandle& sourceHandle,
    std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t sourceLayer) {
    copyTextureRegionTo(destinationHandle, 0, 0, sourceHandle, x, y, width, height, sourceLayer);
}
void Device12::copyTextureRegionTo(const TextureHandle& destinationHandle, std::uint32_t destinationX,
    std::uint32_t destinationY, const TextureHandle& sourceHandle, std::uint32_t sourceX,
    std::uint32_t sourceY, std::uint32_t width, std::uint32_t height, std::uint32_t sourceLayer) {
    auto& destination = owned<Texture12>(destinationHandle, identity_); auto& source = owned<Texture12>(sourceHandle, identity_);
    require(destination.allocation.Get() != source.allocation.Get(), "Texture copy cannot alias its source.");
    require(sourceLayer < source.desc.arrayLayers, "Texture copy source layer is out of range.");
    require(destination.desc.format == source.desc.format && width && height &&
        static_cast<UINT64>(sourceX) + width <= source.desc.width &&
        static_cast<UINT64>(sourceY) + height <= source.desc.height &&
        static_cast<UINT64>(destinationX) + width <= destination.desc.width &&
        static_cast<UINT64>(destinationY) + height <= destination.desc.height,
        "Texture copy region or format is invalid.");
    const bool depth = source.desc.format == Format::Depth32Float;
    if (depth) require(sourceX == 0 && sourceY == 0 && destinationX == 0 && destinationY == 0 &&
        width == source.desc.width && height == source.desc.height &&
        destination.desc.width == source.desc.width && destination.desc.height == source.desc.height,
        "Depth copies require complete, identically sized subresources.");
    require(!activePass_, "Texture copy requires the graphics pass to have ended.");
    const auto oldSource = source.states[sourceLayer], oldDestination = destination.states[0];
    retain(sourceHandle); retain(destinationHandle);
    transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, sourceLayer); transition(destination, D3D12_RESOURCE_STATE_COPY_DEST, 0);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = source.allocation.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = sourceLayer;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = destination.allocation.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{sourceX, sourceY, 0, sourceX + width, sourceY + height, 1};
    list_->CopyTextureRegion(&dst, destinationX, destinationY, 0, &src, depth ? nullptr : &box);
    transition(source, oldSource, sourceLayer); transition(destination, oldDestination, 0);
}
TextureReadback Device12::readTexture(const TextureHandle& handle, std::uint32_t layer) {
    auto& texture = owned<Texture12>(handle, identity_);
    require(!activePass_, "Texture readback requires the graphics pass to have ended.");
    require(layer < texture.desc.arrayLayers, "Texture readback layer is out of range.");
    const auto desc = texture.allocation->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&desc, layer, 1, 0, &footprint, &rows, &rowSize, &total);
    auto staging = makeBuffer(device_.Get(), total, D3D12_HEAP_TYPE_READBACK);
    TextureReadback result; result.width = texture.desc.width; result.height = texture.desc.height;
    result.format = texture.desc.format; result.rowPitch = texture.desc.width * pixelBytes(texture.desc.format);
    result.bytes.resize(static_cast<SIZE_T>(result.rowPitch) * result.height);
    const auto oldState = texture.states[layer]; retain(handle); pendingAllocations_.push_back(staging);
    transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, layer);
    D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = texture.allocation.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = layer;
    D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = staging.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
    list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr); transition(texture, oldState, layer);
    submitAndWait();
    void* mapped = nullptr; const D3D12_RANGE reads{0, static_cast<SIZE_T>(total)};
    checked(staging->Map(0, &reads, &mapped), "Map texture readback");
    for (UINT row = 0; row < result.height; ++row)
        std::memcpy(result.bytes.data() + static_cast<SIZE_T>(row) * result.rowPitch,
            static_cast<const std::byte*>(mapped) + footprint.Offset + static_cast<SIZE_T>(row) * footprint.Footprint.RowPitch, result.rowPitch);
    const D3D12_RANGE noWrites{0, 0}; staging->Unmap(0, &noWrites);
    return result;
}
void Device12::beginDiagnosticFrame(std::uint64_t frameId) {
    require(activeDiagnosticTiming_ == nullptr, "The preceding diagnostic frame has not ended.");
    require(frameId != 0, "Diagnostic timing requires a nonzero frame ID.");
    TimingSlot* available = nullptr;
    for (auto& slot : diagnosticTimings_) if (!slot.inUse) { available = &slot; break; }
    if (!available) return; // Bounded collection; never block to free a slot.
    if (!diagnosticFrequency_) {
        UINT64 frequency = 0;
        checked(queue_->GetTimestampFrequency(&frequency), "Query timestamp frequency");
        require(frequency != 0, "The command queue returned an invalid timestamp frequency.");
        diagnosticFrequency_ = frequency;
    }
    auto& slot = *available;
    if (!slot.heap) {
        D3D12_QUERY_HEAP_DESC description{};
        description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        description.Count = diagnosticTickCount;
        ComPtr<ID3D12QueryHeap> heap;
        checked(device_->CreateQueryHeap(&description, IID_PPV_ARGS(&heap)), "Create diagnostic timestamp heap");
        auto readback = makeBuffer(device_.Get(), diagnosticTickCount * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
        nameObject(heap.Get(), "Diagnostic GPU timestamps");
        nameObject(readback.Get(), "Diagnostic GPU timestamp readback");
        slot.passes.reserve(maxDiagnosticPasses);
        slot.heap = std::move(heap); slot.readback = std::move(readback);
    }
    beginFrame();
    slot.passes.clear();
    slot.frameId = frameId; slot.serial = ++diagnosticSerial_;
    slot.submissionFence = 0; slot.resolved = slot.passOpen = false; slot.inUse = true;
    activeDiagnosticTiming_ = &slot;
    list_->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
}
void Device12::beginDiagnosticPass(std::string_view name) {
    if (!activeDiagnosticTiming_) return;
    endDiagnosticPass();
    auto& slot = *activeDiagnosticTiming_;
    if (slot.passes.size() == maxDiagnosticPasses) return;
    // Bound per-sample metadata as well as the GPU query count.
    auto label = std::string(name.substr(0, 256));
    beginFrame();
    const UINT index = 2 + static_cast<UINT>(slot.passes.size()) * 2;
    slot.passes.push_back(std::move(label));
    list_->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    slot.passOpen = true;
}
void Device12::endDiagnosticPass() {
    if (!activeDiagnosticTiming_ || !activeDiagnosticTiming_->passOpen) return;
    auto& slot = *activeDiagnosticTiming_;
    beginFrame();
    const UINT index = 3 + static_cast<UINT>(slot.passes.size() - 1) * 2;
    list_->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    slot.passOpen = false;
}
void Device12::endDiagnosticFrame() {
    if (!activeDiagnosticTiming_) return;
    endDiagnosticPass();
    auto& slot = *activeDiagnosticTiming_;
    beginFrame();
    list_->EndQuery(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    // Only initialized start/end pairs are resolved. The query heap survives
    // intermediate command-list flushes, so one diagnostic sample may span
    // multiple submissions without reusing another frame's query indices.
    const UINT count = 2 + static_cast<UINT>(slot.passes.size()) * 2;
    list_->ResolveQueryData(slot.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, count, slot.readback.Get(), 0);
    slot.resolved = true;
    activeDiagnosticTiming_ = nullptr;
}
std::optional<DiagnosticTiming> Device12::pollDiagnosticTiming() {
    const UINT64 completed = fence_->GetCompletedValue();
    require(completed != UINT64_MAX, "Device removed while polling diagnostic timestamps.");
    TimingSlot* available = nullptr;
    for (auto& slot : diagnosticTimings_) {
        if (!slot.inUse || !slot.resolved || !slot.submissionFence || slot.submissionFence > completed) continue;
        if (!available || slot.serial < available->serial) available = &slot;
    }
    if (!available) return std::nullopt;
    auto& slot = *available;
    const UINT count = 2 + static_cast<UINT>(slot.passes.size()) * 2;
    std::array<UINT64, diagnosticTickCount> ticks{};
    const D3D12_RANGE range{0, static_cast<SIZE_T>(count) * sizeof(UINT64)};
    void* mapped = nullptr;
    checked(slot.readback->Map(0, &range, &mapped), "Map completed diagnostic timestamps");
    std::memcpy(ticks.data(), mapped, range.End);
    const D3D12_RANGE noWrites{0, 0}; slot.readback->Unmap(0, &noWrites);
    // A consumed malformed sample must not permanently occupy the bounded ring.
    slot.inUse = false;
    require(diagnosticFrequency_ != 0 && ticks[1] >= ticks[0], "Invalid GPU frame timestamps.");
    const double milliseconds = 1000.0 / static_cast<double>(diagnosticFrequency_);
    DiagnosticTiming result;
    result.frameId = slot.frameId;
    result.totalMs = static_cast<double>(ticks[1] - ticks[0]) * milliseconds;
    result.passes.reserve(slot.passes.size());
    for (std::size_t index = 0; index < slot.passes.size(); ++index) {
        const auto begin = ticks[2 + index * 2], end = ticks[3 + index * 2];
        require(begin >= ticks[0] && end >= begin && end <= ticks[1], "Invalid GPU pass timestamps.");
        result.passes.emplace_back(slot.passes[index], static_cast<double>(end - begin) * milliseconds);
    }
    return result;
}
std::vector<std::string> Device12::debugMessages() const {
    std::vector<std::string> result;
    if (!infoQueue_) return result;
    const UINT64 count = infoQueue_->GetNumStoredMessagesAllowedByRetrievalFilter();
    const UINT64 begin = count > 128 ? count - 128 : 0;
    result.reserve(static_cast<std::size_t>(count - begin));
    for (UINT64 index = begin; index < count; ++index) {
        SIZE_T size = 0;
        if (FAILED(infoQueue_->GetMessage(index, nullptr, &size))) continue;
        if (size > 1024 * 1024) {
            result.push_back("D3D12 [unavailable] debug message exceeds 1 MiB retrieval limit");
            continue;
        }
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue_->GetMessage(index, message, &size))) continue;
        const char* severity = "unknown";
        switch (message->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: severity = "corruption"; break;
        case D3D12_MESSAGE_SEVERITY_ERROR: severity = "error"; break;
        case D3D12_MESSAGE_SEVERITY_WARNING: severity = "warning"; break;
        case D3D12_MESSAGE_SEVERITY_INFO: severity = "info"; break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE: severity = "message"; break;
        }
        std::string text;
        if (message->pDescription && message->DescriptionByteLength) {
            const auto length = message->DescriptionByteLength -
                (message->pDescription[message->DescriptionByteLength - 1] == '\0' ? 1 : 0);
            text.assign(message->pDescription, length);
        }
        result.push_back("D3D12 [" + std::string(severity) + "] ID=" +
            std::to_string(static_cast<unsigned>(message->ID)) + " " + text);
    }
    return result;
}
std::uint32_t Device12::validationErrorCount() const noexcept {
    if (!infoQueue_) return 0;
    std::uint32_t count = 0;
    try {
        const UINT64 total = infoQueue_->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < total; ++index) {
            SIZE_T size = 0;
            if (FAILED(infoQueue_->GetMessage(index, nullptr, &size))) continue;
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue_->GetMessage(index, message, &size)) &&
                (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) ++count;
        }
    } catch (...) { return count; }
    return count;
}
} // namespace

std::shared_ptr<D3D12RhiDevice> createD3D12RhiDevice(void* hwnd,
    std::uint32_t width, std::uint32_t height, bool validation) {
    auto result = std::make_shared<Device12>(validation);
    if (hwnd) result->attachWindow(hwnd, width, height);
    return result;
}
} // namespace rae::renderer::d3d12
