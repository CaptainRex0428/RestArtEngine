#include "../src/Renderer/RhiInspection.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace rae::renderer;
namespace rhi = rae::renderer::rhi;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct Buffer final : rhi::Buffer {
    rhi::BufferDesc desc;
    Buffer(rhi::BufferUsage usage, std::uint32_t size, std::string name) : desc{size, usage, false, std::move(name)} {}
    const rhi::BufferDesc& description() const noexcept override { return desc; }
};
struct Texture final : rhi::Texture {
    rhi::TextureDesc desc{32, 16, 1, rhi::Format::RGBA8Unorm, true, "Output color"};
    const rhi::TextureDesc& description() const noexcept override { return desc; }
};
struct Pipeline final : rhi::GraphicsPipeline {
    rhi::GraphicsPipelineDesc desc;
    const rhi::GraphicsPipelineDesc& description() const noexcept override { return desc; }
};
// Deliberately holds no resource handles: these tests isolate the observer's
// forwarding/state/lifetime contract, not a particular GPU API's validation.
struct ForwardingContext final : rhi::CommandContext {
    std::string rejectNext;
    unsigned calls = 0;
    void accept(const char* operation) {
        ++calls;
        if (rejectNext == operation) { rejectNext.clear(); throw std::runtime_error("Rejected by backend."); }
    }
    void writeBuffer(const rhi::BufferHandle&, std::span<const std::byte>) override { accept("write"); }
    void bindConstantBuffers(rhi::ShaderStage, std::uint32_t, std::span<const rhi::BufferHandle>) override { accept("constants"); }
    void beginRenderPass(const rhi::RenderPassDesc&) override { accept("begin"); }
    void endRenderPass() override { accept("end"); }
    void setViewport(const rhi::Viewport&) override { accept("viewport"); }
    void bindGraphicsPipeline(const rhi::GraphicsPipelineHandle&) override { accept("pipeline"); }
    void bindVertexBuffers(std::uint32_t, std::span<const rhi::VertexBufferBinding>) override { accept("vertices"); }
    void bindIndexBuffer(const rhi::BufferHandle&, std::uint32_t) override { accept("index"); }
    void drawIndexed(std::uint32_t, std::uint32_t, std::int32_t) override { accept("indexed"); }
    void draw(std::uint32_t, std::uint32_t) override { accept("draw"); }
};
template<class Operation>
void rejected(ForwardingContext& target, const char* name, Operation operation) {
    target.rejectNext = name;
    const auto before = target.calls;
    bool failed = false;
    try { operation(); } catch (const std::runtime_error&) { failed = true; }
    require(failed && target.calls == before + 1, "A failed command must reach the backend first and propagate its failure.");
}
struct Setup {
    rhi::BufferHandle constants = std::make_shared<Buffer>(rhi::BufferUsage::Constant, 16, "Constants");
    rhi::BufferHandle vertices = std::make_shared<Buffer>(rhi::BufferUsage::Vertex, 512, "Vertices");
    rhi::BufferHandle indices = std::make_shared<Buffer>(rhi::BufferUsage::Index, 128, "Indices");
    rhi::TextureHandle color = std::make_shared<Texture>();
    rhi::GraphicsPipelineHandle pipeline = std::make_shared<Pipeline>();
    void bind(RhiInspection& inspection) const {
        rhi::RenderPassDesc pass;
        pass.name = "Test pass"; pass.colors = {{color}};
        inspection.beginRenderPass(pass);
        inspection.setViewport({1, 2, 32, 16, .1f, .9f});
        inspection.bindGraphicsPipeline(pipeline);
        const std::array streams{rhi::VertexBufferBinding{vertices, 12, 8}};
        inspection.bindVertexBuffers(31, streams);
        inspection.bindIndexBuffer(indices, 4);
        const std::array buffers{constants};
        inspection.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, buffers);
        inspection.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, buffers);
    }
};
std::span<const std::byte> byteView(const std::array<std::uint8_t, 16>& value) {
    return std::as_bytes(std::span(value));
}
const RenderResourceBinding& binding(const RenderDrawEvent& event, const char* kind) {
    const auto found = std::find_if(event.bindings.begin(), event.bindings.end(),
        [kind](const auto& value) { return value.kind == kind; });
    require(found != event.bindings.end(), "The expected binding must be present in the snapshot.");
    return *found;
}

void versionsAndForwarding() {
    ForwardingContext target;
    RhiInspection inspection(target);
    Setup resources;
    inspection.start(true); resources.bind(inspection);
    std::array<std::uint8_t, 16> first{}, second{}, rejectedBytes{};
    first.fill(17); second.fill(34); rejectedBytes.fill(51);
    inspection.writeBuffer(resources.constants, byteView(first));
    inspection.drawIndexed(6, 7, -3);
    // The effective draw contents are fixed before another write or bind.
    inspection.writeBuffer(resources.constants, byteView(second));
    inspection.setViewport({8, 9, 64, 48, 0, 1});
    RenderDrawEvent before;
    inspection.describe(before);
    require(before.detailedState && before.command == "DrawIndexed" && before.firstIndex == 7 && before.baseVertex == -3,
        "Successful indexed parameters must be captured at the draw.");
    require(before.constants.size() == 2 && before.constants[0].version == 1 && before.constants[0].bytes[0] == 17 &&
        before.constants[1].bytes[0] == 17 && before.viewport[0] == 1,
        "Later writes and viewport changes must not mutate the previous draw snapshot.");
    inspection.draw(3, 2);
    RenderDrawEvent after;
    inspection.describe(after);
    require(after.command == "Draw" && after.firstIndex == 2 && after.constants[0].version == 2 &&
        after.constants[0].bytes[0] == 34 && after.viewport[0] == 8,
        "A later draw must use its own effective constants and viewport.");
    require(binding(after, "Vertex buffer").slot == 31 && binding(after, "Vertex buffer").stride == 12,
        "Non-indexed draws must retain high-slot vertex stream bindings too.");
    rejected(target, "write", [&] { inspection.writeBuffer(resources.constants, byteView(rejectedBytes)); });
    const std::array<rhi::BufferHandle, 1> emptyConstants{};
    rejected(target, "constants", [&] { inspection.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, emptyConstants); });
    rejected(target, "viewport", [&] { inspection.setViewport({0, 0, 99, 99}); });
    rejected(target, "pipeline", [&] { inspection.bindGraphicsPipeline(nullptr); });
    const std::array<rhi::VertexBufferBinding, 1> emptyVertices{};
    rejected(target, "vertices", [&] { inspection.bindVertexBuffers(31, emptyVertices); });
    rejected(target, "index", [&] { inspection.bindIndexBuffer(nullptr); });
    rhi::RenderPassDesc other; other.name = "Rejected other pass";
    rejected(target, "begin", [&] { inspection.beginRenderPass(other); });
    rejected(target, "end", [&] { inspection.endRenderPass(); });
    inspection.drawIndexed(6, 3, 1);
    RenderDrawEvent stillValid;
    inspection.describe(stillValid);
    require(stillValid.detailedState && stillValid.constants.size() == 2 && stillValid.constants[0].version == 2 &&
        stillValid.constants[0].bytes[0] == 34 && stillValid.viewport[0] == 8 &&
        binding(stillValid, "Color attachment").name == "Output color" &&
        binding(stillValid, "Index buffer").offset == 4 && binding(stillValid, "Vertex buffer").slot == 31,
        "Backend-rejected changes must not publish into subsequent inspection state.");
    rejected(target, "draw", [&] { inspection.draw(99, 99); });
    rejected(target, "indexed", [&] { inspection.drawIndexed(99, 99, 99); });
    RenderDrawEvent retained;
    inspection.describe(retained);
    require(retained.command == "DrawIndexed" && retained.firstIndex == 3 && retained.baseVertex == 1,
        "A failed draw cannot replace the last successful draw snapshot.");
    inspection.endRenderPass();
    require(before.constants[0].version == 1 && before.constants[0].bytes[0] == 17,
        "Finished event values must remain immutable after the active pass ends.");
    inspection.describe(retained);
    require(!retained.detailedState && retained.bindings.empty() && retained.constants.empty(),
        "Native commands after a finished RHI pass cannot inherit stale detailed bindings.");
    inspection.start(false);
    resources.bind(inspection);
    inspection.writeBuffer(resources.constants, byteView(first)); inspection.draw(3);
    inspection.describe(retained);
    require(!retained.detailedState && retained.constants.empty(), "Disabled inspection still forwards but retains no draw payloads.");
    inspection.endRenderPass();
}

void lifetime() {
    ForwardingContext target;
    RhiInspection inspection(target);
    inspection.start(true);
    auto resources = std::make_unique<Setup>();
    const std::weak_ptr<rhi::Buffer> constants = resources->constants, vertices = resources->vertices, indices = resources->indices;
    const std::weak_ptr<rhi::Texture> color = resources->color;
    const std::weak_ptr<rhi::GraphicsPipeline> pipeline = resources->pipeline;
    resources->bind(inspection);
    std::array<std::uint8_t, 16> bytes{};
    inspection.writeBuffer(resources->constants, byteView(bytes)); inspection.drawIndexed(3);
    RenderDrawEvent snapshot; inspection.describe(snapshot);
    resources.reset();
    require(constants.expired(), "Observed constant slots and upload metadata must not retain a buffer beyond backend ownership.");
    require(!vertices.expired() && !indices.expired() && !color.expired() && !pipeline.expired(),
        "Active pass observation must retain valid current resource descriptions.");
    inspection.endRenderPass();
    require(vertices.expired() && indices.expired() && color.expired() && pipeline.expired(),
        "Ending a successful pass must release every borrowed attachment, stream and pipeline.");
    require(snapshot.constants.size() == 2 && snapshot.bindings.size() == 3,
        "A finished metadata event owns only values after all backend resources expire.");
}

void budgets() {
    ForwardingContext target;
    RhiInspection inspection(target);
    Setup resources;
    // 65,520 is a legal aligned constant size. Two stages at draw 65 cross
    // the remaining budget even though the previous draw is still below it.
    resources.constants = std::make_shared<Buffer>(rhi::BufferUsage::Constant, 65520, "Near-budget constants");
    inspection.start(true); resources.bind(inspection);
    std::vector<std::byte> bytes(65520);
    inspection.writeBuffer(resources.constants, bytes);
    std::size_t total = 0;
    for (unsigned draw = 0; draw < 65; ++draw) {
        inspection.draw(3);
        RenderDrawEvent event; inspection.describe(event);
        for (const auto& constant : event.constants) total += constant.bytes.size();
        require(total <= 8ull * 1024 * 1024, "No single draw may cross the exact aggregate 8 MiB constant payload limit.");
        if (draw < 64) require(event.detailedState, "Draws whose full constant state fits must remain inspectable.");
        else require(!event.detailedState && event.constants[0].bytes.empty() &&
            event.pipelineState.find("budget reached") != std::string::npos,
            "An oversized next draw must retain binding identities and explicitly omit its constant bytes.");
    }
    inspection.endRenderPass(); inspection.start(true);
    resources.bind(inspection);
    std::vector<rhi::BufferHandle> live;
    bytes.resize(65536);
    for (unsigned index = 0; index < 129; ++index) {
        live.push_back(std::make_shared<Buffer>(rhi::BufferUsage::Constant, 65536, "Upload cache pressure"));
        inspection.writeBuffer(live.back(), bytes);
    }
    const std::array last{live.back()};
    inspection.bindConstantBuffers(rhi::ShaderStage::Vertex, 0, last);
    inspection.bindConstantBuffers(rhi::ShaderStage::Pixel, 0, last);
    inspection.draw(3);
    RenderDrawEvent unavailable; inspection.describe(unavailable);
    require(!unavailable.detailedState && unavailable.constants[0].bytes.empty() &&
        unavailable.pipelineState.find("upload-cache budget") != std::string::npos,
        "Upload-cache exhaustion must report unavailable contents rather than retain unbounded or stale bytes.");
    inspection.endRenderPass();
}
} // namespace

int main() {
    try {
        versionsAndForwarding(); lifetime(); budgets();
        std::cout << "RHI inspection forwarding tests passed (" << checks << " checks).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI inspection forwarding tests failed: " << error.what() << '\n';
        return 1;
    }
}
