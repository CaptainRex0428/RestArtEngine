#pragma once

#include <RestArtEngine/Renderer/RenderBackend.h>

#include <memory>
#include <string_view>

namespace rae {

struct EngineCreateInfo {
    renderer::RenderBackendType backend = renderer::RenderBackendType::Auto;
    renderer::RenderBackendCreateInfo renderer{};
};

class Engine final {
public:
    Engine() = default;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool initialize(const EngineCreateInfo& info = {});
    void shutdown() noexcept;

    void beginFrame() noexcept;
    void endFrame() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept { return initialized_; }
    [[nodiscard]] renderer::IRenderBackend* renderer() noexcept { return renderer_.get(); }
    [[nodiscard]] const renderer::IRenderBackend* renderer() const noexcept { return renderer_.get(); }

private:
    std::unique_ptr<renderer::IRenderBackend> renderer_;
    bool initialized_ = false;
};

} // namespace rae

