// Pipeline variants share modules and are owned by the same device as targets
// and buffers. No native context is passed through this execution contract.
rhi::GraphicsPipelineHandle graphicsPipeline(unsigned shaderIndex,
    unsigned rasterIndex, unsigned depthIndex, bool fullscreen, bool hasDepth = true) {
    // 7 is the shadow pass with no pixel stage; 8..10 are AA shaders.
    if (shaderIndex >= pixelModules.size()) throw std::runtime_error("Unknown RHI scene pixel shader.");
    const auto key = std::make_tuple(shaderIndex, rasterIndex, depthIndex, fullscreen, hasDepth);
    if (const auto found = graphicsPipelines.find(key); found != graphicsPipelines.end()) return found->second;
    rhi::GraphicsPipelineDesc description;
    description.vertexShader = fullscreen ? fullscreenModule : geometryModule;
    if (shaderIndex != 7) description.pixelShader = pixelModules[shaderIndex];
    if (!fullscreen) description.attributes = {
        {"POSITION", 0, 0, 0, 3}, {"NORMAL", 0, 0, 12, 3},
        {"TEXCOORD", 0, 1, 0, 4}, {"TEXCOORD", 1, 1, 16, 4}};
    description.cullMode = rasterIndex / 2 == 1 ? rhi::CullMode::Front
        : rasterIndex / 2 == 2 ? rhi::CullMode::Back : rhi::CullMode::None;
    description.frontCounterClockwise = (rasterIndex & 1u) == 0;
    const auto comparison = depthIndex / 2;
    description.depthEnable = comparison != 8;
    description.depthWrite = description.depthEnable && (depthIndex & 1u);
    description.depthCompare = static_cast<rhi::CompareOperation>(std::min(comparison, 7u));
    if (hasDepth) description.depthFormat = rhi::Format::Depth32Float;
    if (shaderIndex == 1)
        description.colorFormats.assign(3, rhi::Format::RGBA16Float);
    else if (shaderIndex == 9 || shaderIndex == 11) description.colorFormats = {rhi::Format::RGBA16Float};
    else if (shaderIndex != 7) description.colorFormats = {rhi::Format::RGBA8Unorm};
    description.name = fullscreen ? "Scene fullscreen pipeline" : "Scene mesh pipeline";
    auto graphicsState = rhiDevice->createGraphicsPipeline(description);
    graphicsPipelines.emplace(key, graphicsState);
    return graphicsState;
}

void bindRhiMaterialPipeline(const assets::MaterialAsset& material, bool mirrored, unsigned shader) {
    diagnosticVS = "Private/Scene.hlsl::VSMain";
    static constexpr const char* entries[] = {"PSForward", "PSGBuffer", "PSDeferred", "PSBackground", "PSOutlineMask", "PSOutlineComposite", "PSDepthOfField", "Depth-only (no pixel shader)"};
    diagnosticPS = shader < std::size(entries) ? entries[shader] : "Unknown";
    const auto rasterIndex = 2u * static_cast<unsigned>(material.effectiveCullMode()) + (mirrored ? 1u : 0u);
    auto comparison = static_cast<unsigned>(material.depthTest);
    if (comparison >= 9u) comparison = static_cast<unsigned>(assets::MaterialDepthTest::Less);
    const auto depthIndex = shader == 7 ? 3u : comparison * 2u + (material.depthWrite ? 1u : 0u);
    sceneCommands().bindGraphicsPipeline(graphicsPipeline(shader, rasterIndex, depthIndex, false));
}

void bindRhiMesh(const GpuMesh& mesh) {
    const rhi::VertexBufferBinding streams[] = {
        {mesh.vertexBuffer, static_cast<std::uint32_t>(sizeof(assets::MeshVertex)), 0},
        {mesh.uvBuffer ? mesh.uvBuffer : zeroUv,
            mesh.uvBuffer ? static_cast<std::uint32_t>(sizeof(meshAttributes::PackedUv)) : 0u, 0}};
    sceneCommands().bindVertexBuffers(0, streams);
    sceneCommands().bindIndexBuffer(mesh.indexBuffer);
}

void drawRhiFullscreen(unsigned shader, bool hasDepth) {
    diagnosticVS = "Private/Scene.hlsl::VSFullscreen";
    static constexpr const char* entries[] = {"PSForward", "PSGBuffer", "PSDeferred", "PSBackground", "PSOutlineMask", "PSOutlineComposite", "PSDepthOfField", "Depth-only", "PSFXAA", "PSTAA", "PSAntiAliasingPresent", "PSProbeUpdate"};
    diagnosticPS = shader < std::size(entries) ? entries[shader] : "Unknown";
    sceneCommands().bindGraphicsPipeline(graphicsPipeline(shader, 0, 16, true, hasDepth));
    sceneCommands().draw(3);
    recordDraw(0, 0, 0, 0, 3);
}

template<class T> void uploadConstants(const rhi::BufferHandle& buffer, const T& value) {
    sceneCommands().writeBuffer(buffer, gpuBytes(&value, sizeof(value)));
}
