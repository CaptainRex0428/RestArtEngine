#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ImGuiViewport;

namespace rae::imgui {

class ImGuiLayer final {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    // Owns the UI context and backend state; the window and device are borrowed.
    // Secondary platform windows are opt-in; simple surfaces and render-only
    // validation retain the original single-window coordinate space by default.
    bool initialize(void* window, ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                    bool enablePlatformWindows = false);
    void shutdown() noexcept;
    void beginFrame(float deltaSeconds) noexcept;
    // A minimized main surface can still submit and present secondary windows.
    void endFrame(bool renderMain = true) noexcept;
    [[nodiscard]] bool hasVisiblePlatformWindows() const noexcept;
    // Native requests can arrive while every window is minimized. Service one
    // UI frame to acknowledge them without keeping idle minimized surfaces live.
    [[nodiscard]] bool hasPendingPlatformWindowRequests() const noexcept;
    // Enables persistent window positions/sizes for interactive editor sessions.
    // Automated validation keeps the default null filename for deterministic frames.
    void setIniFilename(const char* filename) noexcept;

    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }

    static bool handleMessage(void* window, unsigned message,
                              std::uintptr_t wparam, std::intptr_t lparam);

private:
    static void createPlatformWindow(ImGuiViewport* viewport);
    void (*createPlatformWindow_)(ImGuiViewport*) = nullptr;
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

