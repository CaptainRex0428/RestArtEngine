#pragma once

#include <RestArtEngine/Renderer/Rhi.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>

#include <utility>

namespace rae::renderer::d3d12 {

struct TextureReadback {
    std::uint32_t width = 0, height = 0, rowPitch = 0;
    rhi::Format format = rhi::Format::RGBA8Unorm;
    // Packed rows of the requested array layer; rowPitch is the returned row stride.
    std::vector<std::byte> bytes;
};

struct DiagnosticTiming {
    std::uint64_t frameId = 0;
    double totalMs = 0.0;
    std::vector<std::pair<std::string, double>> passes;
};

// Private native backend/ImGui interoperation. Public rendering remains RHI.
// Operations report failures by throwing std::runtime_error.
class D3D12RhiDevice : public rhi::Device {
public:
    virtual void attachWindow(void* hwnd, std::uint32_t width, std::uint32_t height) = 0;
    virtual void detachWindow() = 0;
    // Release borrowed backbuffer handles before resizing the window. A resize
    // with external surface owners is rejected without discarding the old set.
    virtual void resize(std::uint32_t width, std::uint32_t height) = 0;
    [[nodiscard]] virtual rhi::TextureHandle backbuffer() const = 0;
    // Wrap a D3D12 resource owned by another module (for example the native DXR
    // output image) for sampling/copying through the RHI. The wrapper keeps its
    // own resource-state bookkeeping, so the owner must pass the current state.
    [[nodiscard]] virtual rhi::TextureHandle importTexture(ID3D12Resource* resource,
        const rhi::TextureDesc& description, D3D12_RESOURCE_STATES currentState) = 0;
    virtual void beginFrame() = 0;
    virtual void present(bool vsync) = 0;
    virtual void waitIdle() = 0;
    // Completes the current command list and opens a fresh one on demand.
    virtual void submitAndWait() = 0;
    virtual void prepareUi() = 0;
    // Call after all main/platform UI rendering AND platform SwapBuffers have
    // completed, including minimized main-window frames without present().
    // The vendored DX12 platform backend waits for each secondary queue there;
    // this method completes the main queue and releases borrowed UI resources.
    virtual void finishUiFrame() = 0;
    // Droppable borrows kept for UI descriptors; a resize may release them once
    // the GPU is idle instead of failing on borrowed swapchain images.
    virtual void releaseBorrowedUiTextures() = 0;
    [[nodiscard]] virtual ID3D12Device* device() const noexcept = 0;
    [[nodiscard]] virtual ID3D12CommandQueue* queue() const noexcept = 0;
    [[nodiscard]] virtual ID3D12GraphicsCommandList* commandList() = 0;
    [[nodiscard]] virtual ID3D12DescriptorHeap* srvHeap() const noexcept = 0;
    [[nodiscard]] virtual std::pair<D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE>
        allocateUiDescriptor() = 0;
    virtual void freeUiDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE) = 0;
    [[nodiscard]] virtual rhi::TextureHandle uploadRgba8(std::uint32_t width, std::uint32_t height,
        std::span<const std::byte> bytes, std::string_view name = {}) = 0;
    [[nodiscard]] virtual std::uint64_t uiTextureId(const rhi::TextureHandle&) = 0;
    // Copies the selected source layer's rectangle into destination layer zero.
    virtual void copyTextureRegion(const rhi::TextureHandle& destination, const rhi::TextureHandle& source,
        std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
        std::uint32_t sourceLayer = 0) = 0;
    [[nodiscard]] virtual TextureReadback readTexture(const rhi::TextureHandle&, std::uint32_t layer = 0) = 0;
    // Main-queue GPU timestamps only, bounded to four in-flight samples and
    // 64 passes per sample. Calls are optional; no query allocations while off.
    // A full ring skips the new sample; poll never submits or waits. Creation,
    // device and invalid-timestamp failures throw instead of fabricating zero.
    virtual void beginDiagnosticFrame(std::uint64_t frameId) = 0;
    virtual void beginDiagnosticPass(std::string_view name) = 0;
    virtual void endDiagnosticPass() = 0;
    virtual void endDiagnosticFrame() = 0;
    [[nodiscard]] virtual std::optional<DiagnosticTiming> pollDiagnosticTiming() = 0;
    // Most recent 128 native debug-layer messages, preserving severity and ID.
    [[nodiscard]] virtual std::vector<std::string> debugMessages() const = 0;
    [[nodiscard]] virtual std::string adapterName() const = 0;
    [[nodiscard]] virtual bool validationEnabled() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t validationErrorCount() const noexcept = 0;
};

[[nodiscard]] std::shared_ptr<D3D12RhiDevice> createD3D12RhiDevice(void* hwnd,
    std::uint32_t width, std::uint32_t height, bool validation);

} // namespace rae::renderer::d3d12
