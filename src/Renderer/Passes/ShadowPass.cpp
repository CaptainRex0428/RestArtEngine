#include "ShadowPass.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include <utility>

namespace rae::renderer::passes {

bool executeShadowPass(const ShadowPassContext& pass, std::string& error) {
    error.clear();
    if (!pass.context) {
        error = "Shadow pass has no device context.";
        return false;
    }
    if (!pass.shadowConstants) {
        error = "Shadow pass has no ShadowConstants buffer.";
        return false;
    }
    if (!pass.cascadeTargets || pass.cascadeCount == 0) {
        error = "Shadow pass has no cascade target.";
        return false;
    }
    if (pass.mapSize == 0) {
        error = "Shadow pass has a zero map size.";
        return false;
    }
    // The shared array is allocated with two slices; refusing more keeps the
    // depth views and the CPU cascade matrices from silently diverging.
    constexpr std::uint32_t maxCascades = 2;
    if (pass.cascadeCount > maxCascades) {
        error = "Shadow pass received more cascades than the depth array owns.";
        return false;
    }

    auto* context = pass.context;
    const auto restore = [&pass, context] {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        if (pass.restoreSceneConstants) pass.restoreSceneConstants();
    };
    const D3D11_VIEWPORT viewport{0.0f, 0.0f,
        static_cast<float>(pass.mapSize), static_cast<float>(pass.mapSize), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    for (std::uint32_t cascade = 0; cascade < pass.cascadeCount; ++cascade) {
        auto* target = pass.cascadeTargets[cascade];
        if (!target) {
            restore();
            error = "Shadow cascade target is missing.";
            return false;
        }
        context->OMSetRenderTargets(0, nullptr, target);
        context->ClearDepthStencilView(target, D3D11_CLEAR_DEPTH, 1.0f, 0);
        if (pass.beginCascade) pass.beginCascade(cascade);
        if (pass.draw && !pass.draw(cascade, error)) {
            restore();
            return false;
        }
    }
    // Release the depth view before the same array is sampled as an SRV. The
    // immediate context would resolve the hazard, but one driver family
    // silently unbinds the pixel-shader view instead.
    restore();
    return true;
}

} // namespace rae::renderer::passes
