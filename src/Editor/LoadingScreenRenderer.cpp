#include <RestArtEngine/Editor/LoadingScreen.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>

namespace rae::editor {

// Keep the SceneRenderer adapter separate from WIC decoding and native upload,
// so standalone loading-artwork tests do not link the full scene renderer.
bool LoadingScreen::initialize(renderer::SceneRenderer& renderer,
    const std::filesystem::path& image, std::string& error) {
    if (renderer.device()) return initialize(renderer.device(), image, error);
    return initializeGraphicsDevice(renderer.graphicsDevice(), image, error);
}

} // namespace rae::editor
