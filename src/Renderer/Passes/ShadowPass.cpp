#include "ShadowPass.h"

#include <array>
#include <exception>

namespace rae::renderer::passes {

bool executeShadowPass(const ShadowPassContext& pass, std::string& error) {
    error.clear();
    bool ownsPass = false;
    bool success = false;
    const auto appendError = [&error](const std::string& message) {
        if (!error.empty()) error += " ";
        error += message;
    };
    const auto execute = [&]() -> bool {
        if (!pass.commands) {
            error = "Shadow pass has no command context.";
            return false;
        }
        if (!pass.shadowConstants || pass.shadowConstants->description().usage != rhi::BufferUsage::Constant) {
            error = "Shadow pass requires a ShadowConstants constant buffer.";
            return false;
        }
        if (!pass.depthArray || pass.cascadeCount == 0) {
            error = "Shadow pass has no depth array or cascades.";
            return false;
        }
        const auto& texture = pass.depthArray->description();
        if (texture.format != rhi::Format::Depth32Float || !texture.sampled ||
            pass.cascadeCount > texture.arrayLayers) {
            error = "Shadow pass requires a sampled depth array with one layer per cascade.";
            return false;
        }
        if (pass.mapSize == 0 || texture.width != pass.mapSize || texture.height != pass.mapSize) {
            error = "Shadow pass map size does not match its depth array dimensions.";
            return false;
        }

        const std::array constants{pass.shadowConstants};
        for (std::uint32_t cascade = 0; cascade < pass.cascadeCount; ++cascade) {
            rhi::RenderPassDesc declaration;
            declaration.name = "Shadow cascade " + std::to_string(cascade);
            declaration.depth = rhi::DepthAttachment{pass.depthArray, cascade,
                rhi::LoadOp::Clear, rhi::StoreOp::Store, 1.0f};
            pass.commands->beginRenderPass(declaration);
            ownsPass = true;
            pass.commands->setViewport({0.0f, 0.0f,
                static_cast<float>(pass.mapSize), static_cast<float>(pass.mapSize), 0.0f, 1.0f});
            pass.commands->bindConstantBuffers(rhi::ShaderStage::Vertex, 3, constants);
            pass.commands->bindConstantBuffers(rhi::ShaderStage::Pixel, 3, constants);
            if (pass.beginCascade) pass.beginCascade(cascade);
            if (pass.draw && !pass.draw(cascade, error)) {
                if (error.empty()) error = "Shadow cascade draw failed.";
                return false;
            }
            pass.commands->endRenderPass();
            ownsPass = false;
        }
        return true;
    };

    try {
        success = execute();
    } catch (const std::exception& failure) {
        appendError(std::string("Shadow pass failed: ") + failure.what());
    } catch (...) {
        appendError("Shadow pass failed with an unknown exception.");
    }

    // Close only a pass successfully acquired above. A rejected nested begin
    // must never close a render pass owned by the caller.
    if (ownsPass) {
        try {
            pass.commands->endRenderPass();
        } catch (const std::exception& failure) {
            success = false;
            appendError(std::string("Shadow pass cleanup failed: ") + failure.what());
        } catch (...) {
            success = false;
            appendError("Shadow pass cleanup failed with an unknown exception.");
        }
    }
    // Restore even after validation, callback, command, or cleanup failures.
    // Keep an earlier diagnostic if the restore callback also fails.
    if (pass.restoreSceneConstants) {
        try {
            pass.restoreSceneConstants();
        } catch (const std::exception& failure) {
            success = false;
            appendError(std::string("Shadow scene-constant restore failed: ") + failure.what());
        } catch (...) {
            success = false;
            appendError("Shadow scene-constant restore failed with an unknown exception.");
        }
    }
    return success;
}

} // namespace rae::renderer::passes
