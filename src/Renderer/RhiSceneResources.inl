// Scene targets are created and owned through RHI. Native aliases below are
// transitional interop for UI, diagnostics, outline and the DXR shared bridge.
bool createSceneTargets(std::uint32_t targetWidth, std::uint32_t targetHeight, std::string& error) {
    try {
        SceneTargets candidate;
        candidate.depthImage = rhiDevice->createTexture({targetWidth, targetHeight, 1,
            rhi::Format::Depth32Float, true, "Scene depth"});
        if (device) {
            candidate.depthTarget = d3d11::nativeDepthTarget(candidate.depthImage);
            candidate.depthResource = d3d11::nativeShaderResource(candidate.depthImage);
        }
        candidate.colorImage = rhiDevice->createTexture({targetWidth, targetHeight, 1,
            rhi::Format::RGBA8Unorm, true, "Scene color"});
        candidate.historyImage = rhiDevice->createTexture({targetWidth, targetHeight, 1,
            rhi::Format::RGBA8Unorm, true, "SSGI previous color"});
        candidate.probeAtlasA = rhiDevice->createTexture({1024, 32, 1,
            rhi::Format::RGBA16Float, true, "DDGI probe atlas A"});
        candidate.probeAtlasB = rhiDevice->createTexture({1024, 32, 1,
            rhi::Format::RGBA16Float, true, "DDGI probe atlas B"});
        if (device) {
            candidate.color = d3d11::nativeTexture(candidate.colorImage);
            candidate.colorTarget = d3d11::nativeColorTarget(candidate.colorImage);
            candidate.colorView = d3d11::nativeShaderResource(candidate.colorImage);
        }
        for (std::size_t i = 0; i < candidate.images.size(); ++i) {
            candidate.images[i] = rhiDevice->createTexture({targetWidth, targetHeight, 1,
                rhi::Format::RGBA16Float, true, "GBuffer " + std::to_string(i)});
            if (device) {
                candidate.gbufferTargets[i] = d3d11::nativeColorTarget(candidate.images[i]);
                candidate.gbufferViews[i] = d3d11::nativeShaderResource(candidate.images[i]);
            }
        }
        // Publish complete sets only; UI may still borrow the old generation.
        retireSceneTargets();
        sceneColorImage = std::move(candidate.colorImage);
        sceneDepthImage = std::move(candidate.depthImage);
        ssgiHistoryImage = std::move(candidate.historyImage);
        probeAtlasA = std::move(candidate.probeAtlasA);
        probeAtlasB = std::move(candidate.probeAtlasB);
        gbufferImages = std::move(candidate.images);
        sceneTarget = std::move(candidate.colorTarget);
        sceneTexture = std::move(candidate.color);
        sceneView = std::move(candidate.colorView);
        depth = std::move(candidate.depthTarget);
        depthView = std::move(candidate.depthResource);
        gbufferTargets = std::move(candidate.gbufferTargets);
        gbufferViews = std::move(candidate.gbufferViews);
        sceneWidth = targetWidth; sceneHeight = targetHeight;
        ssgiHistoryValid = false;
        probeAtlasValid = false;
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}

bool createTargets(std::string& error) {
    try {
        if (native12) swapchainImage = native12->backbuffer();
        else {
        ComPtr<ID3D11Texture2D> texture;
        if (failed(swapChain->GetBuffer(0, IID_PPV_ARGS(&texture)), "Get backbuffer", error)) return false;
        swapchainImage = d3d11::importTexture(*rhiDevice, texture.Get(), "Swapchain color");
        backbuffer = d3d11::nativeColorTarget(swapchainImage);
        }
        if (!createSceneTargets(width, height, error)) return false;
        if (!shadowImage) {
            shadowImage = rhiDevice->createTexture({shadowMapSize, shadowMapSize, shadowCascadeCount,
                rhi::Format::Depth32Float, true, "Directional shadow array"});
            if (device) {
            shadowTexture = d3d11::nativeTexture(shadowImage);
            for (UINT cascade = 0; cascade < shadowCascadeCount; ++cascade)
                shadowDepthSlices[cascade] = d3d11::nativeDepthTarget(shadowImage, cascade);
            shadowDepth = shadowDepthSlices[0];
            shadowView = d3d11::nativeShaderResource(shadowImage);
            }
        }
        return true;
    } catch (const std::exception& failure) { error = failure.what(); return false; }
}
