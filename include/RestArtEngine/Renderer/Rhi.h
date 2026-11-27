#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rae::renderer::rhi {

enum class BufferUsage { Vertex, Index, Constant };
enum class Format { RGBA8Unorm, RGBA16Float, Depth32Float };
enum class ShaderStage { Vertex, Pixel };
enum class CompareOperation { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class CullMode { None, Front, Back };
enum class LoadOp { Load, Clear, DontCare };
enum class StoreOp { Store, DontCare };

struct BufferDesc {
    std::uint32_t byteSize = 0;
    BufferUsage usage = BufferUsage::Vertex;
    bool immutable = false;
    std::string name;
};
struct TextureDesc {
    std::uint32_t width = 0, height = 0, arrayLayers = 1;
    Format format = Format::RGBA8Unorm;
    bool sampled = true;
    std::string name;
};
class Buffer {
public:
    virtual ~Buffer() = default;
    [[nodiscard]] virtual const BufferDesc& description() const noexcept = 0;
};
class Texture {
public:
    virtual ~Texture() = default;
    [[nodiscard]] virtual const TextureDesc& description() const noexcept = 0;
};
class Shader {
public:
    virtual ~Shader() = default;
    [[nodiscard]] virtual ShaderStage stage() const noexcept = 0;
};
using BufferHandle = std::shared_ptr<Buffer>;
using TextureHandle = std::shared_ptr<Texture>;
using ShaderHandle = std::shared_ptr<Shader>;

struct VertexAttribute {
    std::string semantic;
    std::uint32_t semanticIndex = 0, slot = 0, offset = 0, components = 3;
    bool perInstance = false;
};
struct GraphicsPipelineDesc {
    ShaderHandle vertexShader, pixelShader;
    std::vector<VertexAttribute> attributes;
    bool depthEnable = true, depthWrite = true;
    CompareOperation depthCompare = CompareOperation::Less;
    CullMode cullMode = CullMode::None;
    bool frontCounterClockwise = true;
    std::vector<Format> colorFormats;
    std::optional<Format> depthFormat;
    std::string name;
};
class GraphicsPipeline {
public:
    virtual ~GraphicsPipeline() = default;
    [[nodiscard]] virtual const GraphicsPipelineDesc& description() const noexcept = 0;
};
using GraphicsPipelineHandle = std::shared_ptr<GraphicsPipeline>;

struct ColorAttachment {
    TextureHandle texture;
    LoadOp load = LoadOp::Load;
    StoreOp store = StoreOp::Store;
    std::array<float, 4> clear{};
};
struct DepthAttachment {
    TextureHandle texture;
    std::uint32_t layer = 0;
    LoadOp load = LoadOp::Load;
    StoreOp store = StoreOp::Store;
    float clearDepth = 1.0f;
};
struct SampledTextureBinding {
    TextureHandle texture;
    std::uint32_t slot = 0;
};
struct RenderPassDesc {
    std::string name;
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    std::vector<SampledTextureBinding> reads;
};
struct Viewport {
    float x = 0, y = 0, width = 0, height = 0, minDepth = 0, maxDepth = 1;
};
struct VertexBufferBinding {
    BufferHandle buffer;
    std::uint32_t stride = 0, offset = 0;
};
struct Statistics {
    std::uint64_t buffersCreated = 0, texturesCreated = 0, shadersCreated = 0, pipelinesCreated = 0;
    std::uint64_t bufferUploads = 0, renderPasses = 0, drawCalls = 0, indexedDrawCalls = 0;
};

class CommandContext {
public:
    virtual ~CommandContext() = default;
    // Each write is ordered with surrounding commands. An earlier draw must
    // retain its own constants until GPU consumption; explicit backends need
    // upload versions/ring allocation plus fences, not a single overwritten GPU address.
    virtual void writeBuffer(const BufferHandle&, std::span<const std::byte>) = 0;
    // Null buffer handles explicitly unbind the requested slots.
    virtual void bindConstantBuffers(ShaderStage, std::uint32_t first, std::span<const BufferHandle>) = 0;
    // Attachments and sampled reads are executable declarations: validate,
    // resolve read/write hazards, bind and clear. DontCare allows discard or
    // retention; it never promises contents are initialized or preserved.
    virtual void beginRenderPass(const RenderPassDesc&) = 0;
    // Idempotent. Releases pass resources and draw bindings; each subsequent
    // pass must explicitly bind its own pipeline and vertex/index streams.
    virtual void endRenderPass() = 0;
    virtual void setViewport(const Viewport&) = 0;
    virtual void bindGraphicsPipeline(const GraphicsPipelineHandle&) = 0;
    // A null buffer unbinds its stream, with zero stride and offset.
    virtual void bindVertexBuffers(std::uint32_t first, std::span<const VertexBufferBinding>) = 0;
    // A null buffer with offset zero explicitly unbinds the index stream.
    virtual void bindIndexBuffer(const BufferHandle&, std::uint32_t offset = 0) = 0;
    virtual void drawIndexed(std::uint32_t count, std::uint32_t first = 0, std::int32_t base = 0) = 0;
    virtual void drawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount,
        std::uint32_t firstIndex = 0, std::int32_t baseVertex = 0, std::uint32_t firstInstance = 0) = 0;
    virtual void draw(std::uint32_t count, std::uint32_t first = 0) = 0;
};
class Device {
public:
    virtual ~Device() = default;
    [[nodiscard]] virtual BufferHandle createBuffer(const BufferDesc&, std::span<const std::byte> initial = {}) = 0;
    [[nodiscard]] virtual TextureHandle createTexture(const TextureDesc&) = 0;
    [[nodiscard]] virtual TextureHandle uploadRgba8(std::uint32_t width, std::uint32_t height,
        std::span<const std::byte> bytes, std::string_view name = {}) = 0;
    [[nodiscard]] virtual ShaderHandle createShader(ShaderStage, std::span<const std::byte>, std::string_view name = {}) = 0;
    [[nodiscard]] virtual GraphicsPipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
    [[nodiscard]] virtual CommandContext& commands() noexcept = 0;
    [[nodiscard]] virtual const Statistics& statistics() const noexcept = 0;
};

} // namespace rae::renderer::rhi
