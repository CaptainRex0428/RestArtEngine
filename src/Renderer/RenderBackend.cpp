#include <RestArtEngine/Renderer/RenderBackend.h>

#include <cctype>

namespace rae::renderer {
namespace {

class BackendBase : public IRenderBackend {
public:
    BackendBase(RenderBackendType backendType, const char* backendName,
                bool hardwareAvailable, bool supportsValidation,
                bool supportsPresentation)
        : type_(backendType), name_(backendName) {
        capabilities_.hardwareAvailable = hardwareAvailable;
        capabilities_.supportsValidation = supportsValidation;
        capabilities_.supportsPresentation = supportsPresentation;
    }

    RenderBackendType type() const noexcept override { return type_; }
    const char* name() const noexcept override { return name_; }
    const RenderBackendCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    bool initialize(const RenderBackendCreateInfo& info) override {
        // Keep the scaffold usable without a window system or graphics SDK.
        // Native device creation can replace this method while preserving the
        // interface and lifecycle contract.
        width_ = info.width;
        height_ = info.height;
        vsync_ = info.vsync;
        initialized_ = true;
        return true;
    }

    void shutdown() noexcept override {
        initialized_ = false;
        frameActive_ = false;
    }
    bool isInitialized() const noexcept override { return initialized_; }

    void beginFrame() noexcept override { frameActive_ = initialized_; }
    void endFrame() noexcept override { frameActive_ = false; }
    void present() noexcept override {
        // Present is deliberately a no-op in the scaffold.  A native backend
        // owns its queue/swap-chain synchronization here.
    }

protected:
    RenderBackendType type_;
    const char* name_;
    RenderBackendCapabilities capabilities_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool vsync_ = true;
    bool initialized_ = false;
    bool frameActive_ = false;
};

class NullBackend final : public BackendBase {
public:
    NullBackend() : BackendBase(RenderBackendType::Null, "Null", false, false, false) {}
};

class OpenGLBackend final : public BackendBase {
public:
    OpenGLBackend()
        : BackendBase(RenderBackendType::OpenGL, "OpenGL", false, false, false) {}
};

class DirectX11Backend final : public BackendBase {
public:
    DirectX11Backend()
        : BackendBase(RenderBackendType::DirectX11, "DirectX 11", false, false, false) {}
};

class DirectX12Backend final : public BackendBase {
public:
    DirectX12Backend()
        : BackendBase(RenderBackendType::DirectX12, "DirectX 12", false, false, false) {}
};

class VulkanBackend final : public BackendBase {
public:
    VulkanBackend()
        : BackendBase(RenderBackendType::Vulkan, "Vulkan", false, false, false) {}
};

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto a = static_cast<unsigned char>(lhs[i]);
        const auto b = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

std::unique_ptr<IRenderBackend> createRenderBackend(RenderBackendType type) {
    if (type == RenderBackendType::Auto) {
#if defined(_WIN32)
        // D3D12 is the preferred Windows path once native device code is enabled.
        type = RenderBackendType::DirectX12;
#else
        type = RenderBackendType::Vulkan;
#endif
    }

    switch (type) {
    case RenderBackendType::Null:
        return std::make_unique<NullBackend>();
    case RenderBackendType::OpenGL:
        return std::make_unique<OpenGLBackend>();
    case RenderBackendType::DirectX11:
        return std::make_unique<DirectX11Backend>();
    case RenderBackendType::DirectX12:
        return std::make_unique<DirectX12Backend>();
    case RenderBackendType::Vulkan:
        return std::make_unique<VulkanBackend>();
    case RenderBackendType::Auto:
        break;
    }
    return std::make_unique<NullBackend>();
}

RenderBackendType parseRenderBackendType(std::string_view value) noexcept {
    if (equalsIgnoreCase(value, "auto")) return RenderBackendType::Auto;
    if (equalsIgnoreCase(value, "null")) return RenderBackendType::Null;
    if (equalsIgnoreCase(value, "opengl") || equalsIgnoreCase(value, "gl")) {
        return RenderBackendType::OpenGL;
    }
    if (equalsIgnoreCase(value, "directx11") || equalsIgnoreCase(value, "dx11")) {
        return RenderBackendType::DirectX11;
    }
    if (equalsIgnoreCase(value, "directx12") || equalsIgnoreCase(value, "dx12")) {
        return RenderBackendType::DirectX12;
    }
    if (equalsIgnoreCase(value, "vulkan") || equalsIgnoreCase(value, "vk")) {
        return RenderBackendType::Vulkan;
    }
    return RenderBackendType::Auto;
}

const char* toString(RenderBackendType type) noexcept {
    switch (type) {
    case RenderBackendType::Auto: return "Auto";
    case RenderBackendType::Null: return "Null";
    case RenderBackendType::OpenGL: return "OpenGL";
    case RenderBackendType::DirectX11: return "DirectX 11";
    case RenderBackendType::DirectX12: return "DirectX 12";
    case RenderBackendType::Vulkan: return "Vulkan";
    }
    return "Unknown";
}

} // namespace rae::renderer
