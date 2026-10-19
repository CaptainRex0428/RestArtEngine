#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace rae::imgui {

class ImGuiLayer final {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Owns the UI context and backend state; the window and device are borrowed.
    bool initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext);
    void shutdown() noexcept;
    void beginFrame(float deltaSeconds) noexcept;
    void endFrame() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }

    static bool handleMessage(void* window, unsigned message,
                              std::uintptr_t wparam, std::intptr_t lparam);

private:
    void* context_ = nullptr;
    void* previousFrameContext_ = nullptr;
    void* window_ = nullptr;
    bool platformInitialized_ = false;
    bool rendererInitialized_ = false;
    bool frameActive_ = false;
    bool initialized_ = false;
    std::uint64_t frameCount_ = 0;
};

} // namespace rae::imgui

