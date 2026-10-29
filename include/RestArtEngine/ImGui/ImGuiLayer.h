#pragma once

#include <RestArtEngine/Core/GarbageCollector.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ImGuiViewport;

namespace rae::imgui {

class WindowChrome;

struct ImGuiMemoryStats {
    std::uint64_t fontAtlasCpuBytes = 0;
    std::uint64_t windowTransientCpuBytes = 0;
    std::uint64_t tableTransientCpuBytes = 0;
};

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
    // Main thread, between frames after draw submission. Preserves live fonts,
    // window/settings state and every GPU object owned by the UI backend.
    rae::GarbageCollectionMemoryStats collectGarbage(double maxIdleSeconds);
    [[nodiscard]] ImGuiMemoryStats memoryStats() const;
    // Enables persistent window positions/sizes for interactive editor sessions.
    // Automated validation keeps the default null filename for deterministic frames.
    void setIniFilename(const char* filename) noexcept;
    void setWindowChrome(WindowChrome* chrome) noexcept { chrome_ = chrome; }
    // Receives OS file drops on the UI thread. The handler should only queue
    // work; importing belongs to the editor frame, after Content hit testing.
    // x/y match ImGui coordinates (desktop coordinates with platform windows).
    // sourceViewport identifies the native receiver; 0 means unknown. Retain it
    // so a separate OS window cannot import into Content behind that window.
    using FileDropHandler = std::function<void(std::vector<std::filesystem::path>, float x, float y,
        unsigned sourceViewport)>;
    void setFileDropHandler(FileDropHandler handler);

    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint64_t frameCount() const noexcept { return frameCount_; }

    static bool handleMessage(void* window, unsigned message,
                              std::uintptr_t wparam, std::intptr_t lparam);

private:
    static void createPlatformWindow(ImGuiViewport* viewport);
    void dispatchFileDrop(void* window, std::uintptr_t drop);
    void (*createPlatformWindow_)(ImGuiViewport*) = nullptr;
    void* context_ = nullptr;
    void* previousFrameContext_ = nullptr;
    void* window_ = nullptr;
    WindowChrome* chrome_ = nullptr;
    FileDropHandler fileDropHandler_;
    ID3D11ShaderResourceView* logo_ = nullptr;
    bool platformInitialized_ = false;
    bool rendererInitialized_ = false;
    bool frameActive_ = false;
    bool initialized_ = false;
    std::uint64_t frameCount_ = 0;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> windowLastUsed_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> tableLastUsed_;
    std::unordered_map<int, std::chrono::steady_clock::time_point> tableTempLastUsed_;
};

} // namespace rae::imgui

