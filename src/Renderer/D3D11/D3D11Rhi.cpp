#include "D3D11Rhi.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace rae::renderer::d3d11 {
namespace {
using Microsoft::WRL::ComPtr;
using namespace rhi;

[[noreturn]] void reject(const char* message) { throw std::runtime_error(std::string("D3D11 RHI: ") + message); }
void require(bool condition, const char* message) { if (!condition) reject(message); }
void check(HRESULT result, const char* operation) {
    if (SUCCEEDED(result)) return;
    std::ostringstream error;
    error << "D3D11 RHI: " << operation << " failed (HRESULT 0x" << std::hex
          << static_cast<unsigned long>(result) << ").";
    throw std::runtime_error(error.str());
}
void debugName(ID3D11DeviceChild* object, std::string_view name) {
    if (object && !name.empty()) {
        require(name.size() <= std::numeric_limits<UINT>::max(), "Debug name is too long.");
        check(object->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(name.size()), name.data()),
            "Set resource debug name");
    }
}
// Debug labels are optional. Invalid UTF-8 or allocation failure must never
// change whether the render pass itself can execute.
std::wstring annotationLabel(std::string_view name) noexcept {
    if (name.empty() || name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        name.find('\0') != std::string_view::npos) return {};
    const auto length = static_cast<int>(name.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), length, nullptr, 0);
    if (count <= 0) return {};
    try {
        std::wstring label(static_cast<std::size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), length, label.data(), count) != count)
            return {};
        return label;
    } catch (...) { return {}; }
}
struct AnnotationScope {
    ID3DUserDefinedAnnotation* annotation = nullptr;
    bool active = false;
    ~AnnotationScope() { if (active) annotation->EndEvent(); }
};
DXGI_FORMAT colorFormat(Format format) {
    switch (format) {
    case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::Depth32Float: return DXGI_FORMAT_D32_FLOAT;
    }
    reject("Unknown texture format.");
}
D3D11_COMPARISON_FUNC comparison(CompareOperation operation) {
    switch (operation) {
    case CompareOperation::Never: return D3D11_COMPARISON_NEVER;
    case CompareOperation::Less: return D3D11_COMPARISON_LESS;
    case CompareOperation::Equal: return D3D11_COMPARISON_EQUAL;
    case CompareOperation::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOperation::Greater: return D3D11_COMPARISON_GREATER;
    case CompareOperation::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
    case CompareOperation::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOperation::Always: return D3D11_COMPARISON_ALWAYS;
    }
    reject("Unknown depth comparison operation.");
}
void validate(LoadOp operation) {
    require(operation == LoadOp::Load || operation == LoadOp::Clear || operation == LoadOp::DontCare,
        "Unknown attachment load operation.");
}
void validate(StoreOp operation) {
    require(operation == StoreOp::Store || operation == StoreOp::DontCare,
        "Unknown attachment store operation.");
}
struct DeviceIdentity {};
using Identity = std::shared_ptr<DeviceIdentity>;

struct D3D11Buffer final : Buffer {
    BufferDesc desc;
    Identity owner;
    ComPtr<ID3D11Buffer> object;
    const BufferDesc& description() const noexcept override { return desc; }
};
struct D3D11Texture final : Texture {
    TextureDesc desc;
    Identity owner;
    ComPtr<ID3D11Texture2D> object;
    ComPtr<ID3D11RenderTargetView> color;
    std::vector<ComPtr<ID3D11DepthStencilView>> depths;
    ComPtr<ID3D11ShaderResourceView> sampled;
    const TextureDesc& description() const noexcept override { return desc; }
};
struct D3D11Shader final : Shader {
    ShaderStage shaderStage = ShaderStage::Vertex;
    Identity owner;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    std::vector<std::byte> bytecode;
    ShaderStage stage() const noexcept override { return shaderStage; }
};
struct D3D11Pipeline final : GraphicsPipeline {
    GraphicsPipelineDesc desc;
    Identity owner;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth;
    const GraphicsPipelineDesc& description() const noexcept override { return desc; }
};
template<class Native, class Public>
Native& cast(const std::shared_ptr<Public>& handle) {
    require(handle != nullptr, "A required resource handle is null.");
    auto* result = dynamic_cast<Native*>(handle.get());
    require(result != nullptr, "Resource handle belongs to a different backend.");
    return *result;
}
template<class Native, class Public>
Native& owned(const std::shared_ptr<Public>& handle, const Identity& owner) {
    auto& result = cast<Native>(handle);
    require(result.owner == owner, "Resource handle belongs to a different RHI device.");
    return result;
}

class D3D11Device final : public Device, public CommandContext {
public:
    D3D11Device(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context) {
        require(device && context, "Device and immediate context are required.");
        ComPtr<ID3D11Device> contextDevice;
        context->GetDevice(&contextDevice);
        require(contextDevice.Get() == device, "The context belongs to a different device.");
        require(context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE, "An immediate context is required.");
        // Older runtimes can omit this optional debugging interface.
        (void)context_.As(&annotation_);
    }
    ~D3D11Device() override { if (annotationOpen_) annotation_->EndEvent(); }
    CommandContext& commands() noexcept override { return *this; }
    const Statistics& statistics() const noexcept override { return statistics_; }

    BufferHandle createBuffer(const BufferDesc& desc, std::span<const std::byte> initial) override {
        require(desc.byteSize > 0, "Buffer size must be nonzero.");
        require(initial.empty() || initial.size() == desc.byteSize, "Initial buffer data must cover the complete buffer.");
        require(!desc.immutable || initial.size() == desc.byteSize, "Immutable buffers require complete initial data.");
        D3D11_BUFFER_DESC native{};
        native.ByteWidth = desc.byteSize;
        native.Usage = desc.immutable ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        switch (desc.usage) {
        case BufferUsage::Vertex: native.BindFlags = D3D11_BIND_VERTEX_BUFFER; break;
        case BufferUsage::Index:
            require(desc.byteSize % sizeof(std::uint32_t) == 0, "Index buffer size must be a multiple of four.");
            native.BindFlags = D3D11_BIND_INDEX_BUFFER;
            break;
        case BufferUsage::Constant:
            require(desc.byteSize % 16 == 0 && desc.byteSize <= D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16u,
                "Constant buffer size must be aligned to 16 bytes and no larger than 64 KiB.");
            native.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            break;
        default: reject("Unknown buffer usage.");
        }
        auto result = std::make_shared<D3D11Buffer>();
        result->desc = desc;
        result->owner = identity_;
        const D3D11_SUBRESOURCE_DATA data{initial.data(), 0, 0};
        check(device_->CreateBuffer(&native, initial.empty() ? nullptr : &data, &result->object), "Create buffer");
        debugName(result->object.Get(), desc.name);
        ++statistics_.buffersCreated;
        if (!initial.empty()) ++statistics_.bufferUploads;
        return result;
    }
    TextureHandle createTexture(const TextureDesc& desc) override {
        require(desc.width && desc.height && desc.width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
            desc.height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION, "Invalid texture dimensions.");
        require(desc.arrayLayers && desc.arrayLayers <= D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION,
            "Invalid texture array layer count.");
        D3D11_TEXTURE2D_DESC native{};
        native.Width = desc.width;
        native.Height = desc.height;
        native.MipLevels = 1;
        native.ArraySize = desc.arrayLayers;
        native.Format = desc.format == Format::Depth32Float ? DXGI_FORMAT_R32_TYPELESS : colorFormat(desc.format);
        native.SampleDesc.Count = 1;
        native.Usage = D3D11_USAGE_DEFAULT;
        native.BindFlags = desc.format == Format::Depth32Float ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
        if (desc.sampled) native.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        auto result = std::make_shared<D3D11Texture>();
        result->desc = desc;
        result->owner = identity_;
        check(device_->CreateTexture2D(&native, nullptr, &result->object), "Create texture");
        finishTexture(*result, native.BindFlags);
        ++statistics_.texturesCreated;
        return result;
    }
    ShaderHandle createShader(ShaderStage stage, std::span<const std::byte> bytecode, std::string_view name) override {
        require(!bytecode.empty(), "Shader bytecode is empty.");
        auto result = std::make_shared<D3D11Shader>();
        result->shaderStage = stage;
        result->owner = identity_;
        if (stage == ShaderStage::Vertex) {
            result->bytecode.assign(bytecode.begin(), bytecode.end());
            check(device_->CreateVertexShader(bytecode.data(), bytecode.size(), nullptr, &result->vertex), "Create vertex shader");
            debugName(result->vertex.Get(), name);
        } else if (stage == ShaderStage::Pixel) {
            check(device_->CreatePixelShader(bytecode.data(), bytecode.size(), nullptr, &result->pixel), "Create pixel shader");
            debugName(result->pixel.Get(), name);
        } else reject("Unknown shader stage.");
        ++statistics_.shadersCreated;
        return result;
    }
    GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
        auto& vertex = owned<D3D11Shader>(desc.vertexShader, identity_);
        require(vertex.shaderStage == ShaderStage::Vertex, "Graphics pipeline needs a vertex shader.");
        if (desc.pixelShader) require(owned<D3D11Shader>(desc.pixelShader, identity_).shaderStage == ShaderStage::Pixel,
            "A non-pixel shader was provided as the pixel stage.");
        require(desc.colorFormats.size() <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, "Too many color attachments in pipeline.");
        for (const auto format : desc.colorFormats) {
            (void)colorFormat(format);
            require(format != Format::Depth32Float, "A depth format cannot be a color attachment.");
        }
        require(!desc.depthFormat || *desc.depthFormat == Format::Depth32Float, "Unsupported depth attachment format.");
        require(!desc.depthEnable || desc.depthFormat.has_value(), "Depth testing requires a pipeline depth format.");
        auto result = std::make_shared<D3D11Pipeline>();
        result->desc = desc;
        result->owner = identity_;
        require(desc.attributes.size() <= D3D11_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT, "Too many vertex attributes.");
        std::vector<D3D11_INPUT_ELEMENT_DESC> attributes;
        for (const auto& attribute : desc.attributes) {
            require(!attribute.semantic.empty() && attribute.slot < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
                "Invalid vertex attribute semantic or stream slot.");
            require(attribute.components >= 2 && attribute.components <= 4, "Vertex attribute must have two to four components.");
            require(attribute.offset <= D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES - attribute.components * 4,
                "Vertex attribute exceeds maximum stream stride.");
            const DXGI_FORMAT format = attribute.components == 2 ? DXGI_FORMAT_R32G32_FLOAT :
                attribute.components == 3 ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
            attributes.push_back({attribute.semantic.c_str(), attribute.semanticIndex, format, attribute.slot,
                attribute.offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
        }
        if (!attributes.empty()) check(device_->CreateInputLayout(attributes.data(), static_cast<UINT>(attributes.size()),
            vertex.bytecode.data(), vertex.bytecode.size(), &result->layout), "Create input layout");
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        switch (desc.cullMode) {
        case CullMode::None: raster.CullMode = D3D11_CULL_NONE; break;
        case CullMode::Front: raster.CullMode = D3D11_CULL_FRONT; break;
        case CullMode::Back: raster.CullMode = D3D11_CULL_BACK; break;
        default: reject("Unknown culling mode.");
        }
        raster.FrontCounterClockwise = desc.frontCounterClockwise ? TRUE : FALSE;
        raster.DepthClipEnable = TRUE;
        check(device_->CreateRasterizerState(&raster, &result->rasterizer), "Create pipeline rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = desc.depthEnable ? TRUE : FALSE;
        depth.DepthWriteMask = desc.depthWrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = comparison(desc.depthCompare);
        check(device_->CreateDepthStencilState(&depth, &result->depth), "Create pipeline depth state");
        debugName(result->layout.Get(), desc.name);
        debugName(result->rasterizer.Get(), desc.name);
        debugName(result->depth.Get(), desc.name);
        ++statistics_.pipelinesCreated;
        return result;
    }

    void writeBuffer(const BufferHandle& handle, std::span<const std::byte> data) override {
        auto& buffer = owned<D3D11Buffer>(handle, identity_);
        require(!buffer.desc.immutable, "An immutable buffer cannot be updated.");
        require(data.size() == buffer.desc.byteSize, "Buffer update must cover its complete byte range.");
        // UpdateSubresource copies the CPU bytes into the ordered D3D11 stream.
        // D3D11 preserves earlier draw versions; an explicit backend must supply
        // its own versioned upload storage and completion tracking.
        context_->UpdateSubresource(buffer.object.Get(), 0, nullptr, data.data(), 0, 0);
        ++statistics_.bufferUploads;
    }
    void bindConstantBuffers(ShaderStage stage, std::uint32_t first, std::span<const BufferHandle> handles) override {
        require(first <= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT &&
            handles.size() <= D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - first,
            "Constant buffer binding exceeds stage slots.");
        require(stage == ShaderStage::Vertex || stage == ShaderStage::Pixel, "Unknown constant buffer shader stage.");
        std::vector<ID3D11Buffer*> buffers;
        for (const auto& handle : handles) {
            if (!handle) { buffers.push_back(nullptr); continue; }
            auto& buffer = owned<D3D11Buffer>(handle, identity_);
            require(buffer.desc.usage == BufferUsage::Constant, "Non-constant buffer bound as constants.");
            buffers.push_back(buffer.object.Get());
        }
        if (buffers.empty()) return;
        if (stage == ShaderStage::Vertex) context_->VSSetConstantBuffers(first, static_cast<UINT>(buffers.size()), buffers.data());
        else context_->PSSetConstantBuffers(first, static_cast<UINT>(buffers.size()), buffers.data());
    }
    void beginRenderPass(const RenderPassDesc& desc) override {
        require(!activePass_, "The preceding render pass has not ended.");
        require(desc.colors.size() <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, "Too many render pass color attachments.");
        require(!desc.colors.empty() || desc.depth.has_value(), "A graphics render pass needs an attachment.");
        std::uint32_t width = 0, height = 0;
        std::vector<ID3D11Resource*> writes;
        std::vector<ID3D11RenderTargetView*> targets;
        const auto attachment = [&](D3D11Texture& texture) {
            if (width) require(width == texture.desc.width && height == texture.desc.height,
                "Render pass attachments have different dimensions.");
            else { width = texture.desc.width; height = texture.desc.height; }
            require(std::find(writes.begin(), writes.end(), texture.object.Get()) == writes.end(),
                "A resource is used by more than one render pass attachment.");
            writes.push_back(texture.object.Get());
        };
        for (const auto& color : desc.colors) {
            validate(color.load); validate(color.store);
            auto& texture = owned<D3D11Texture>(color.texture, identity_);
            require(texture.color != nullptr && texture.desc.format != Format::Depth32Float, "Color attachment has no color target view.");
            if (color.load == LoadOp::Clear) for (const auto channel : color.clear)
                require(std::isfinite(channel), "Color clear values must be finite.");
            attachment(texture);
            targets.push_back(texture.color.Get());
        }
        ID3D11DepthStencilView* depth = nullptr;
        if (desc.depth) {
            validate(desc.depth->load); validate(desc.depth->store);
            auto& texture = owned<D3D11Texture>(desc.depth->texture, identity_);
            require(texture.desc.format == Format::Depth32Float && desc.depth->layer < texture.depths.size(),
                "Depth attachment has no requested depth layer.");
            require(std::isfinite(desc.depth->clearDepth) && desc.depth->clearDepth >= 0 && desc.depth->clearDepth <= 1,
                "Depth clear value must be between zero and one.");
            attachment(texture);
            depth = texture.depths[desc.depth->layer].Get();
        }
        std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> usedSlots{};
        for (const auto& read : desc.reads) {
            require(read.slot < usedSlots.size() && !usedSlots[read.slot], "Invalid or duplicate sampled texture slot.");
            usedSlots[read.slot] = true;
            auto& texture = owned<D3D11Texture>(read.texture, identity_);
            require(texture.sampled != nullptr, "Sampled texture has no shader resource view.");
            require(std::find(writes.begin(), writes.end(), texture.object.Get()) == writes.end(),
                "Render pass reads and writes the same texture.");
        }
        // Copy the declaration before mutating native state: allocation failure
        // cannot leave a partially published pass or half-cleared attachments.
        RenderPassDesc candidate = desc;
        const auto label = annotation_ ? annotationLabel(candidate.name) : std::wstring{};
        AnnotationScope event{annotation_.Get(), false};
        if (!label.empty()) event.active = annotation_->BeginEvent(label.c_str()) >= 0;
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        unbindShaderAliases(writes);
        context_->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.empty() ? nullptr : targets.data(), depth);
        for (const auto& color : candidate.colors) if (color.load == LoadOp::Clear)
            context_->ClearRenderTargetView(cast<D3D11Texture>(color.texture).color.Get(), color.clear.data());
        if (candidate.depth && candidate.depth->load == LoadOp::Clear)
            context_->ClearDepthStencilView(depth, D3D11_CLEAR_DEPTH, candidate.depth->clearDepth, 0);
        for (const auto& read : candidate.reads) {
            auto* view = cast<D3D11Texture>(read.texture).sampled.Get();
            context_->PSSetShaderResources(read.slot, 1, &view);
        }
        pass_ = std::move(candidate);
        activePass_ = true;
        annotationOpen_ = std::exchange(event.active, false);
        ++statistics_.renderPasses;
    }
    void endRenderPass() override {
        if (!activePass_) return;
        for (const auto& read : pass_.reads) {
            ComPtr<ID3D11ShaderResourceView> current;
            context_->PSGetShaderResources(read.slot, 1, &current);
            if (current.Get() == cast<D3D11Texture>(read.texture).sampled.Get()) {
                ID3D11ShaderResourceView* empty = nullptr;
                context_->PSSetShaderResources(read.slot, 1, &empty);
            }
        }
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        activePass_ = false;
        pass_ = {};
        // A new pass must explicitly establish its draw state. Do not retain
        // the last mesh through command bookkeeping after its pass has ended.
        pipeline_.reset();
        for (UINT slot = 0; slot < vertices_.size(); ++slot) {
            if (!vertices_[slot].buffer) continue;
            ID3D11Buffer* empty = nullptr;
            const UINT zero = 0;
            context_->IASetVertexBuffers(slot, 1, &empty, &zero, &zero);
            vertices_[slot] = {};
        }
        if (indices_) context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        indices_.reset();
        indexOffset_ = 0;
        if (annotationOpen_) {
            annotation_->EndEvent();
            annotationOpen_ = false;
        }
    }
    void setViewport(const Viewport& viewport) override {
        require(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) &&
            std::isfinite(viewport.height) && std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth),
            "Viewport values must be finite.");
        require(viewport.width > 0 && viewport.height > 0 && viewport.x >= D3D11_VIEWPORT_BOUNDS_MIN &&
            viewport.y >= D3D11_VIEWPORT_BOUNDS_MIN && viewport.x + viewport.width <= D3D11_VIEWPORT_BOUNDS_MAX &&
            viewport.y + viewport.height <= D3D11_VIEWPORT_BOUNDS_MAX, "Viewport dimensions exceed D3D11 bounds.");
        require(viewport.minDepth >= 0 && viewport.maxDepth <= 1 && viewport.minDepth <= viewport.maxDepth,
            "Viewport depth range must be within zero and one.");
        const D3D11_VIEWPORT native{viewport.x, viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth};
        context_->RSSetViewports(1, &native);
    }
    void bindGraphicsPipeline(const GraphicsPipelineHandle& handle) override {
        auto& pipeline = owned<D3D11Pipeline>(handle, identity_);
        if (activePass_) validatePipeline(pipeline);
        const auto& vertex = owned<D3D11Shader>(pipeline.desc.vertexShader, identity_);
        ID3D11PixelShader* pixel = pipeline.desc.pixelShader ? owned<D3D11Shader>(pipeline.desc.pixelShader, identity_).pixel.Get() : nullptr;
        context_->IASetInputLayout(pipeline.layout.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertex.vertex.Get(), nullptr, 0);
        context_->PSSetShader(pixel, nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        context_->RSSetState(pipeline.rasterizer.Get());
        context_->OMSetDepthStencilState(pipeline.depth.Get(), 0);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        pipeline_ = handle;
    }
    void bindVertexBuffers(std::uint32_t first, std::span<const VertexBufferBinding> bindings) override {
        require(first <= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT &&
            bindings.size() <= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - first, "Vertex streams exceed available slots.");
        std::vector<ID3D11Buffer*> buffers;
        std::vector<UINT> strides, offsets;
        for (const auto& binding : bindings) {
            if (!binding.buffer) {
                require(binding.offset == 0 && binding.stride == 0,
                    "An unbound vertex stream must have zero offset and stride.");
                buffers.push_back(nullptr); strides.push_back(0); offsets.push_back(0);
                continue;
            }
            auto& buffer = owned<D3D11Buffer>(binding.buffer, identity_);
            require(buffer.desc.usage == BufferUsage::Vertex, "Non-vertex buffer bound as a vertex stream.");
            require(binding.offset <= buffer.desc.byteSize && binding.stride <= D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES,
                "Vertex stream offset or stride exceeds the buffer contract.");
            buffers.push_back(buffer.object.Get()); strides.push_back(binding.stride); offsets.push_back(binding.offset);
        }
        if (buffers.empty()) return;
        context_->IASetVertexBuffers(first, static_cast<UINT>(buffers.size()), buffers.data(), strides.data(), offsets.data());
        for (std::size_t index = 0; index < bindings.size(); ++index) vertices_[first + index] = bindings[index];
    }
    void bindIndexBuffer(const BufferHandle& handle, std::uint32_t offset) override {
        if (!handle) {
            require(offset == 0, "An unbound index stream must have zero offset.");
            context_->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
            indices_.reset();
            indexOffset_ = 0;
            return;
        }
        auto& buffer = owned<D3D11Buffer>(handle, identity_);
        require(buffer.desc.usage == BufferUsage::Index, "Non-index buffer bound as an index stream.");
        require(offset % sizeof(std::uint32_t) == 0 && offset <= buffer.desc.byteSize, "Invalid index buffer offset.");
        context_->IASetIndexBuffer(buffer.object.Get(), DXGI_FORMAT_R32_UINT, offset);
        indices_ = handle;
        indexOffset_ = offset;
    }
    void drawIndexed(std::uint32_t count, std::uint32_t first, std::int32_t base) override {
        validateDraw();
        auto& indices = owned<D3D11Buffer>(indices_, identity_);
        const auto last = static_cast<std::uint64_t>(first) + count;
        require(static_cast<std::uint64_t>(indexOffset_) + last * sizeof(std::uint32_t) <= indices.desc.byteSize,
            "Indexed draw exceeds the index buffer byte range.");
        validateVertices(0, 1);
        context_->DrawIndexed(count, first, base);
        ++statistics_.drawCalls;
        ++statistics_.indexedDrawCalls;
    }
    void draw(std::uint32_t count, std::uint32_t first) override {
        validateDraw();
        validateVertices(first, count);
        context_->Draw(count, first);
        ++statistics_.drawCalls;
    }

    TextureHandle import(ID3D11Texture2D* object, std::string name) {
        require(object != nullptr, "Cannot import a null texture.");
        ComPtr<ID3D11Device> owner;
        object->GetDevice(&owner);
        require(owner.Get() == device_.Get(), "Imported texture belongs to another native device.");
        D3D11_TEXTURE2D_DESC native{};
        object->GetDesc(&native);
        require(native.MipLevels == 1 && native.SampleDesc.Count == 1, "Imported texture must be single-mip and single-sampled.");
        auto result = std::make_shared<D3D11Texture>();
        result->desc.width = native.Width;
        result->desc.height = native.Height;
        result->desc.arrayLayers = native.ArraySize;
        result->desc.sampled = (native.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
        result->desc.name = std::move(name);
        switch (native.Format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: result->desc.format = Format::RGBA8Unorm; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: result->desc.format = Format::RGBA16Float; break;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT: result->desc.format = Format::Depth32Float; break;
        default: reject("Imported texture format is not represented by this RHI slice.");
        }
        result->owner = identity_;
        result->object = object;
        finishTexture(*result, native.BindFlags);
        return result;
    }

private:
    void finishTexture(D3D11Texture& texture, UINT flags) {
        const auto& desc = texture.desc;
        debugName(texture.object.Get(), desc.name);
        if (desc.format == Format::Depth32Float && (flags & D3D11_BIND_DEPTH_STENCIL)) {
            texture.depths.resize(desc.arrayLayers);
            for (std::uint32_t layer = 0; layer < desc.arrayLayers; ++layer) {
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_D32_FLOAT;
                view.ViewDimension = desc.arrayLayers > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
                if (desc.arrayLayers > 1) { view.Texture2DArray.FirstArraySlice = layer; view.Texture2DArray.ArraySize = 1; }
                check(device_->CreateDepthStencilView(texture.object.Get(), &view, &texture.depths[layer]), "Create depth attachment view");
                debugName(texture.depths[layer].Get(), desc.name);
            }
        } else if (flags & D3D11_BIND_RENDER_TARGET) {
            check(device_->CreateRenderTargetView(texture.object.Get(), nullptr, &texture.color), "Create color attachment view");
            debugName(texture.color.Get(), desc.name);
        }
        if (desc.sampled) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = desc.format == Format::Depth32Float ? DXGI_FORMAT_R32_FLOAT : colorFormat(desc.format);
            view.ViewDimension = desc.arrayLayers > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
            if (desc.arrayLayers > 1) { view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = desc.arrayLayers; }
            else view.Texture2D.MipLevels = 1;
            check(device_->CreateShaderResourceView(texture.object.Get(), &view, &texture.sampled), "Create sampled texture view");
            debugName(texture.sampled.Get(), desc.name);
        }
    }
    void unbindShaderAliases(const std::vector<ID3D11Resource*>& writes) {
        std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views{};
        const auto clearStage = [&](bool vertex) {
            if (vertex) context_->VSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
            else context_->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
            for (UINT slot = 0; slot < views.size(); ++slot) {
                auto* view = views[slot];
                if (!view) continue;
                ComPtr<ID3D11Resource> resource;
                view->GetResource(&resource);
                if (std::find(writes.begin(), writes.end(), resource.Get()) != writes.end()) {
                    ID3D11ShaderResourceView* empty = nullptr;
                    if (vertex) context_->VSSetShaderResources(slot, 1, &empty);
                    else context_->PSSetShaderResources(slot, 1, &empty);
                }
                view->Release();
                views[slot] = nullptr;
            }
        };
        clearStage(true);
        clearStage(false);
    }
    void validateDraw() const {
        require(activePass_, "Draw issued outside a render pass.");
        const auto& pipeline = owned<D3D11Pipeline>(pipeline_, identity_);
        validatePipeline(pipeline);
    }
    void validatePipeline(const D3D11Pipeline& pipeline) const {
        require(pipeline.desc.colorFormats.size() == pass_.colors.size(), "Pipeline and pass color attachment counts differ.");
        for (std::size_t index = 0; index < pass_.colors.size(); ++index)
            require(pipeline.desc.colorFormats[index] == pass_.colors[index].texture->description().format,
                "Pipeline and pass color formats differ.");
        require(pipeline.desc.depthFormat.has_value() == pass_.depth.has_value(), "Pipeline and pass depth attachments differ.");
        if (pass_.depth) require(*pipeline.desc.depthFormat == pass_.depth->texture->description().format,
            "Pipeline and pass depth formats differ.");
    }
    void validateVertices(std::uint32_t first, std::uint32_t count) const {
        if (!count) return;
        const auto& pipeline = owned<D3D11Pipeline>(pipeline_, identity_);
        for (const auto& attribute : pipeline.desc.attributes) {
            const auto& binding = vertices_[attribute.slot];
            const auto& buffer = owned<D3D11Buffer>(binding.buffer, identity_);
            const auto vertex = static_cast<std::uint64_t>(first) + count - 1;
            const auto end = static_cast<std::uint64_t>(binding.offset) + vertex * binding.stride + attribute.offset + attribute.components * 4u;
            require(end <= buffer.desc.byteSize, "Vertex attribute fetch exceeds its buffer byte range.");
        }
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3DUserDefinedAnnotation> annotation_;
    bool annotationOpen_ = false;
    Identity identity_ = std::make_shared<DeviceIdentity>();
    Statistics statistics_;
    RenderPassDesc pass_;
    bool activePass_ = false;
    GraphicsPipelineHandle pipeline_;
    std::array<VertexBufferBinding, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices_{};
    BufferHandle indices_;
    std::uint32_t indexOffset_ = 0;
};
} // namespace

std::shared_ptr<rhi::Device> createRhiDevice(ID3D11Device* device, ID3D11DeviceContext* context) {
    return std::make_shared<D3D11Device>(device, context);
}
rhi::TextureHandle importTexture(rhi::Device& device, ID3D11Texture2D* texture, std::string name) {
    auto* native = dynamic_cast<D3D11Device*>(&device);
    require(native != nullptr, "Cannot import a D3D11 texture into another backend.");
    return native->import(texture, std::move(name));
}
ID3D11Buffer* nativeBuffer(const rhi::BufferHandle& handle) { return handle ? cast<D3D11Buffer>(handle).object.Get() : nullptr; }
ID3D11Texture2D* nativeTexture(const rhi::TextureHandle& handle) { return handle ? cast<D3D11Texture>(handle).object.Get() : nullptr; }
ID3D11RenderTargetView* nativeColorTarget(const rhi::TextureHandle& handle) { return handle ? cast<D3D11Texture>(handle).color.Get() : nullptr; }
ID3D11DepthStencilView* nativeDepthTarget(const rhi::TextureHandle& handle, std::uint32_t layer) {
    if (!handle) return nullptr;
    auto& texture = cast<D3D11Texture>(handle);
    require(layer < texture.depths.size(), "Native depth view layer is out of range.");
    return texture.depths[layer].Get();
}
ID3D11ShaderResourceView* nativeShaderResource(const rhi::TextureHandle& handle) { return handle ? cast<D3D11Texture>(handle).sampled.Get() : nullptr; }
ID3D11VertexShader* nativeVertexShader(const rhi::ShaderHandle& handle) {
    if (!handle) return nullptr;
    auto& shader = cast<D3D11Shader>(handle);
    require(shader.shaderStage == ShaderStage::Vertex, "Native vertex shader requested from another shader stage.");
    return shader.vertex.Get();
}
ID3D11PixelShader* nativePixelShader(const rhi::ShaderHandle& handle) {
    if (!handle) return nullptr;
    auto& shader = cast<D3D11Shader>(handle);
    require(shader.shaderStage == ShaderStage::Pixel, "Native pixel shader requested from another shader stage.");
    return shader.pixel.Get();
}
ID3D11InputLayout* nativeInputLayout(const rhi::GraphicsPipelineHandle& handle) { return handle ? cast<D3D11Pipeline>(handle).layout.Get() : nullptr; }

} // namespace rae::renderer::d3d11
