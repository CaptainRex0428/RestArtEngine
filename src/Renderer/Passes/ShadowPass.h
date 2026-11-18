#pragma once

// Raster shadow-map pass.
//
// One stage of the declared scene pipeline: clear each cascade slice of the
// persistent depth array, draw every visible actor from the light's point of
// view, then restore the camera constants. Ownership of the depth array, the
// constant buffer and the mesh cache stays with SceneRenderer in this
// iteration; the pass receives bindings and two callbacks so it never reaches
// into renderer internals. Attachments and commands use the shared RHI contract;
// this module does not depend on a native graphics API.

#include <RestArtEngine/Renderer/Rhi.h>

#include <cstdint>
#include <functional>
#include <string>

namespace rae::renderer::passes {

struct ShadowPassContext {
    rhi::CommandContext* commands = nullptr;
    // b3 ShadowConstants, already uploaded for this frame by the caller.
    rhi::BufferHandle shadowConstants;
    // Persistent sampled depth array; each cascade writes one layer.
    rhi::TextureHandle depthArray;
    std::uint32_t cascadeCount = 0;
    std::uint32_t mapSize = 0;
    // Writes the light-space constants for one cascade before its draw.
    std::function<void(std::uint32_t cascade)> beginCascade;
    // Draws visible geometry for one cascade. Returns false with `error` set
    // when a mesh upload fails.
    std::function<bool(std::uint32_t cascade, std::string& error)> draw;
    // Restores the camera scene constants after the last cascade.
    std::function<void()> restoreSceneConstants;
};

// Executes the pass. Restores the scene constants and releases the depth
// attachment on both success and failure paths.
[[nodiscard]] bool executeShadowPass(const ShadowPassContext& pass, std::string& error);

} // namespace rae::renderer::passes
