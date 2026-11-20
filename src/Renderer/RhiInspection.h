#pragma once

#include <RestArtEngine/Renderer/Rhi.h>
#include <RestArtEngine/Renderer/RenderDiagnostics.h>

#include <cstring>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace rae::renderer {

// Records successful forwarded commands. Draw metadata is fixed at the draw,
// so later writes/binds cannot change its effective state. Finished metadata
// owns values, never backend resources or borrowed native pointers.
class RhiInspection final : public rhi::CommandContext {
public:
    explicit RhiInspection(rhi::CommandContext& target) : target_(target) {}
    void start(bool enabled) {
        enabled_ = enabled;
        bytes_ = uploadBytes_ = 0;
        uploads_.clear(); constants_ = {};
        pass_ = {}; pipeline_.reset(); vertices_ = {}; index_.reset();
        viewport_ = {}; indexOffset_ = 0; lastDraw_ = {}; haveDraw_ = false;
    }
    void writeBuffer(const rhi::BufferHandle& buffer, std::span<const std::byte> bytes) override {
        target_.writeBuffer(buffer, bytes);
        if (!enabled_ || !buffer || buffer->description().usage != rhi::BufferUsage::Constant) return;
        pruneUploads();
        auto found = uploads_.find(buffer.get());
        if (found == uploads_.end()) {
            if (uploads_.size() == 4096) return;
            found = uploads_.emplace(buffer.get(), Upload{}).first;
            found->second.owner = buffer;
        }
        auto& upload = found->second;
        ++upload.version;
        uploadBytes_ -= upload.bytes.size();
        std::vector<std::uint8_t>().swap(upload.bytes); upload.available = false;
        // A successful write invalidates old bytes even if the new contents
        // exceed the cache budget; do not report stale data as a new version.
        if (bytes.size() > constantBudget - uploadBytes_) return;
        upload.bytes.resize(bytes.size());
        if (!bytes.empty()) std::memcpy(upload.bytes.data(), bytes.data(), bytes.size());
        uploadBytes_ += bytes.size(); upload.available = true;
    }
    void bindConstantBuffers(rhi::ShaderStage stage, std::uint32_t first,
        std::span<const rhi::BufferHandle> buffers) override {
        target_.bindConstantBuffers(stage, first, buffers);
        if (enabled_) for (std::size_t index = 0; index < buffers.size() && first + index < 16; ++index)
            constants_[stage == rhi::ShaderStage::Vertex ? 0 : 1][first + index] = buffers[index];
    }
    void beginRenderPass(const rhi::RenderPassDesc& pass) override {
        target_.beginRenderPass(pass);
        if (enabled_) { pass_ = pass; lastDraw_ = {}; haveDraw_ = false; }
    }
    void endRenderPass() override {
        target_.endRenderPass();
        pass_ = {}; pipeline_.reset(); vertices_ = {}; index_.reset();
        lastDraw_ = {}; haveDraw_ = false;
        // Backends preserve constant slots across passes. Weak observer slots
        // preserve logical state without extending any buffer's lifetime.
        pruneUploads();
    }
    void setViewport(const rhi::Viewport& viewport) override {
        target_.setViewport(viewport);
        if (enabled_) viewport_ = viewport;
    }
    void bindGraphicsPipeline(const rhi::GraphicsPipelineHandle& pipeline) override {
        target_.bindGraphicsPipeline(pipeline);
        if (enabled_) pipeline_ = pipeline;
    }
    void bindVertexBuffers(std::uint32_t first, std::span<const rhi::VertexBufferBinding> streams) override {
        target_.bindVertexBuffers(first, streams);
        if (enabled_) for (std::size_t index = 0; index < streams.size() && first + index < vertices_.size(); ++index)
            vertices_[first + index] = streams[index];
    }
    void bindIndexBuffer(const rhi::BufferHandle& buffer, std::uint32_t offset = 0) override {
        target_.bindIndexBuffer(buffer, offset);
        if (enabled_) { index_ = buffer; indexOffset_ = offset; }
    }
    void drawIndexed(std::uint32_t count, std::uint32_t first = 0, std::int32_t base = 0) override {
        target_.drawIndexed(count, first, base);
        if (enabled_) snapshot(true, first, base);
    }
    void drawIndexedInstanced(std::uint32_t indexCount, std::uint32_t instanceCount,
        std::uint32_t firstIndex = 0, std::int32_t baseVertex = 0, std::uint32_t firstInstance = 0) override {
        target_.drawIndexedInstanced(indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
        if (enabled_) snapshot(true, firstIndex, baseVertex);
    }
    void draw(std::uint32_t count, std::uint32_t first = 0) override {
        target_.draw(count, first);
        if (enabled_) snapshot(false, first, 0);
    }
    void describe(RenderDrawEvent& event) const {
        event.detailedState = false;
        event.bindings.clear(); event.constants.clear();
        if (!enabled_ || !haveDraw_) {
            event.viewport = {}; event.firstIndex = 0; event.baseVertex = 0;
            event.pipelineState = enabled_ ? "Native command: detailed RHI bindings unavailable."
                : "Detailed state was not recorded; capture a scene frame.";
            return;
        }
        event.detailedState = lastDraw_.detailedState;
        event.command = lastDraw_.command;
        event.firstIndex = lastDraw_.firstIndex; event.baseVertex = lastDraw_.baseVertex;
        event.viewport = lastDraw_.viewport;
        event.pipelineState = lastDraw_.pipelineState;
        event.bindings = lastDraw_.bindings; event.constants = lastDraw_.constants;
    }

private:
    static constexpr std::size_t constantBudget = 8ull * 1024 * 1024;
    struct Upload {
        std::weak_ptr<rhi::Buffer> owner;
        std::uint64_t version = 0;
        std::vector<std::uint8_t> bytes;
        bool available = false;
    };
    void pruneUploads() {
        for (auto item = uploads_.begin(); item != uploads_.end();) {
            if (!item->second.owner.expired()) { ++item; continue; }
            uploadBytes_ -= item->second.bytes.size();
            item = uploads_.erase(item);
        }
    }
    void snapshot(bool indexed, std::uint32_t first, std::int32_t base) {
        RenderDrawEvent event;
        if (!pipeline_ || pass_.name.empty()) {
            event.pipelineState = "Native command: detailed RHI bindings unavailable.";
            lastDraw_ = std::move(event); haveDraw_ = true;
            return;
        }
        event.detailedState = true;
        event.command = indexed ? "DrawIndexed" : "Draw";
        event.firstIndex = first; event.baseVertex = base;
        event.viewport = {viewport_.x, viewport_.y, viewport_.width, viewport_.height, viewport_.minDepth, viewport_.maxDepth};
        const auto& pipeline = pipeline_->description();
        static constexpr const char* comparisons[] = {"Never", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always"};
        const auto comparison = static_cast<unsigned>(pipeline.depthCompare);
        std::ostringstream state;
        state << "TriangleList | Cull " << (pipeline.cullMode == rhi::CullMode::Back ? "Back" : pipeline.cullMode == rhi::CullMode::Front ? "Front" : "None")
            << " | Front " << (pipeline.frontCounterClockwise ? "CCW" : "CW")
            << " | Depth " << (pipeline.depthEnable ? (comparison < 8 ? comparisons[comparison] : "Unknown") : "Disabled")
            << " | ZWrite " << (pipeline.depthWrite ? "On" : "Off") << " | Blend disabled | Samples 1";
        event.pipelineState = state.str();
        const auto texture = [&](const rhi::TextureHandle& image, std::string kind, std::uint32_t slot, std::uint32_t layer) {
            if (!image) return;
            const auto& description = image->description();
            RenderResourceBinding binding;
            binding.stage = kind == "Sampled texture" ? "PS" : "Output";
            binding.kind = std::move(kind); binding.slot = slot; binding.name = description.name;
            binding.width = description.width; binding.height = description.height; binding.layer = layer;
            binding.format = description.format == rhi::Format::RGBA8Unorm ? "RGBA8_UNORM"
                : description.format == rhi::Format::RGBA16Float ? "RGBA16F" : "D32_FLOAT";
            binding.bytes = std::uint64_t(description.width) * description.height * description.arrayLayers *
                (description.format == rhi::Format::RGBA16Float ? 8 : 4);
            event.bindings.push_back(std::move(binding));
        };
        for (std::uint32_t index = 0; index < pass_.colors.size(); ++index)
            texture(pass_.colors[index].texture, "Color attachment", index, 0);
        if (pass_.depth) texture(pass_.depth->texture, "Depth attachment", 0, pass_.depth->layer);
        for (const auto& read : pass_.reads) texture(read.texture, "Sampled texture", read.slot, 0);
        const auto buffer = [&](const rhi::BufferHandle& handle, const char* kind,
            std::uint32_t slot, std::uint32_t stride, std::uint32_t offset) {
            if (!handle) return;
            RenderResourceBinding binding;
            binding.stage = "IA"; binding.kind = kind; binding.name = handle->description().name;
            binding.bytes = handle->description().byteSize; binding.slot = slot; binding.stride = stride; binding.offset = offset;
            if (handle->description().usage == rhi::BufferUsage::Index) binding.format = "R32_UINT";
            event.bindings.push_back(std::move(binding));
        };
        for (std::uint32_t index = 0; index < vertices_.size(); ++index)
            buffer(vertices_[index].buffer, "Vertex buffer", index, vertices_[index].stride, vertices_[index].offset);
        if (indexed) buffer(index_, "Index buffer", 0, 4, indexOffset_);

        std::size_t required = 0;
        bool available = true;
        for (std::size_t stage = 0; stage < 2; ++stage) for (std::uint32_t slot = 0; slot < 16; ++slot) {
            const auto handle = constants_[stage][slot].lock();
            if (!handle) continue;
            const auto upload = uploads_.find(handle.get());
            if (upload == uploads_.end() || !upload->second.available) available = false;
            else required += upload->second.bytes.size();
        }
        const bool fits = required <= constantBudget - bytes_;
        for (std::size_t stage = 0; stage < 2; ++stage) for (std::uint32_t slot = 0; slot < 16; ++slot) {
            const auto handle = constants_[stage][slot].lock();
            if (!handle) continue;
            RenderConstantBinding binding;
            binding.stage = stage ? "PS" : "VS"; binding.slot = slot; binding.name = handle->description().name;
            const auto upload = uploads_.find(handle.get());
            if (upload != uploads_.end()) {
                binding.version = upload->second.version;
                if (fits && upload->second.available) binding.bytes = upload->second.bytes;
            }
            event.constants.push_back(std::move(binding));
        }
        if (fits) bytes_ += required;
        else { event.detailedState = false; event.pipelineState += " | Constant bytes omitted: 8 MiB capture budget reached."; }
        if (!available) { event.detailedState = false; event.pipelineState += " | Constant contents not observed or upload-cache budget reached."; }
        lastDraw_ = std::move(event); haveDraw_ = true;
    }

    rhi::CommandContext& target_;
    bool enabled_ = false, haveDraw_ = false;
    std::uint32_t indexOffset_ = 0;
    std::size_t bytes_ = 0, uploadBytes_ = 0;
    rhi::RenderPassDesc pass_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::Viewport viewport_{};
    std::array<rhi::VertexBufferBinding, 32> vertices_{};
    rhi::BufferHandle index_;
    std::array<std::array<std::weak_ptr<rhi::Buffer>, 16>, 2> constants_{};
    std::unordered_map<const rhi::Buffer*, Upload> uploads_;
    RenderDrawEvent lastDraw_;
};
} // namespace rae::renderer
