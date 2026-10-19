#include <RAEngine/Core/Engine.h>

namespace rae {

Engine::~Engine() { shutdown(); }

bool Engine::initialize(const EngineCreateInfo& info) {
    shutdown();
    renderer_ = renderer::createRenderBackend(info.backend);
    if (!renderer_ || !renderer_->initialize(info.renderer)) {
        renderer_.reset();
        return false;
    }
    initialized_ = true;
    return true;
}

void Engine::shutdown() noexcept {
    if (renderer_) {
        renderer_->shutdown();
        renderer_.reset();
    }
    initialized_ = false;
}

void Engine::beginFrame() noexcept {
    if (initialized_) renderer_->beginFrame();
}

void Engine::endFrame() noexcept {
    if (initialized_) {
        renderer_->endFrame();
        renderer_->present();
    }
}

} // namespace rae

