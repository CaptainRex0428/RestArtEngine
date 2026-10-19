#pragma once

// The renderer API intentionally contains no graphics API headers.  Concrete
// backends can therefore be compiled on every host while their implementation
// is filled in incrementally.

#include <cstdint>
#include <memory>
#include <string_view>

namespace rae::renderer {

enum class RenderBackendType : std::uint8_t {
    Auto = 0,
    Null,
    OpenGL,
    DirectX11,
    DirectX12,
    Vulkan,
};

struct RenderBackendCreateInfo {
    // A backend may interpret this as HWND, X11 Window, wl_surface, etc.  The
    // generic renderer never dereferences it.
    void* nativeWindow = nullptr;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool enableValidation = false;
    bool vsync = true;
    const char* applicationName = "RAEngine";
};

struct RenderBackendCapabilities {
    // This is true for the initial backend scaffolds.  It lets callers expose
    // the selected API in tooling before the native device code lands.
    bool placeholder = true;
    bool hardwareAvailable = false;
    bool supportsValidation = false;
    bool supportsPresentation = false;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual RenderBackendType type() const noexcept = 0;
    virtual const char* name() const noexcept = 0;
    virtual const RenderBackendCapabilities& capabilities() const noexcept = 0;

    // initialize/shutdown are idempotent.  A native implementation can use the
    // create info to acquire a device and swap chain without changing callers.
    virtual bool initialize(const RenderBackendCreateInfo& info) = 0;
    virtual void shutdown() noexcept = 0;
    virtual bool isInitialized() const noexcept = 0;

    virtual void beginFrame() noexcept = 0;
    virtual void endFrame() noexcept = 0;
    virtual void present() noexcept = 0;
};

/// Create a backend object.  The returned object is not initialized.
/// Auto currently selects the platform's preferred API and falls back to Null.
[[nodiscard]] std::unique_ptr<IRenderBackend> createRenderBackend(
    RenderBackendType type = RenderBackendType::Auto);

/// Parse command-line/configuration values (case insensitive).
[[nodiscard]] RenderBackendType parseRenderBackendType(std::string_view value) noexcept;

[[nodiscard]] const char* toString(RenderBackendType type) noexcept;

} // namespace rae::renderer
