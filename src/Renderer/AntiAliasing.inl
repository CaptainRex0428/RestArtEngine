// Included in SceneRenderer::Impl. All AA images/commands use the common RHI.
rhi::TextureHandle aaSourceImage;
rhi::TextureHandle aaResolvedImage;
std::array<rhi::TextureHandle, 2> aaHistoryImages;
AntiAliasingStatus aaStatus;
bool aaHistoryValid = false;
unsigned aaHistoryRead = 0;
std::uint32_t aaHistoryFrames = 0;
std::uint64_t aaHistoryRevision = 0, aaSceneKey = 0;
XMFLOAT4X4 aaPreviousViewProjection{};
XMFLOAT4 aaPreviousEye{}, aaPreviousForward{};
XMFLOAT2 aaJitterPixels{};

static float aaHalton(unsigned index, unsigned base) {
    float value = 0, fraction = 1;
    for (; index; index /= base) { fraction /= static_cast<float>(base); value += fraction * static_cast<float>(index % base); }
    return value;
}

void invalidateAntiAliasing() {
    aaHistoryValid = false;
    aaHistoryFrames = 0;
    ++aaHistoryRevision;
}

void ensureAntiAliasingTargets(unsigned w, unsigned h, bool temporal) {
    if (!aaSourceImage || aaSourceImage->description().width != w || aaSourceImage->description().height != h)
        aaSourceImage = rhiDevice->createTexture({w, h, 1, rhi::Format::RGBA8Unorm, true, "Anti-aliasing source"});
    if (temporal && (!aaHistoryImages[0] || aaHistoryImages[0]->description().width != w ||
        aaHistoryImages[0]->description().height != h)) {
        std::array<rhi::TextureHandle, 2> candidate;
        for (unsigned i = 0; i != 2; ++i)
            candidate[i] = rhiDevice->createTexture({w,h,1,rhi::Format::RGBA16Float,true,"TAA history " + std::to_string(i)});
        aaHistoryImages = std::move(candidate);
        invalidateAntiAliasing();
    }
}

// Hash the small render inputs, never entire mesh payloads. In-place geometry
// edits follow the same markGeometryModified() contract as GPU mesh caches.
std::uint64_t antiAliasingSceneKey(const SceneSettings& settings, const scene::World& world,
    const assets::AssetLibrary& assets, const assets::MaterialAsset* draftMaterial,
    const camera::View& camera, const LightingConstants& lighting, const XMFLOAT4& viewport) const {
    std::uint64_t hash = 1469598103934665603ull;
    const auto bytes = [&](const void* pointer, std::size_t size) {
        const auto* data = static_cast<const unsigned char*>(pointer);
        for (std::size_t i=0;i<size;++i) { hash ^= data[i]; hash *= 1099511628211ull; }
    };
    const auto value = [&](const auto& item) { bytes(&item,sizeof(item)); };
    value(revision); value(settings.viewId); value(settings.path); value(settings.shadowMethod);
    value(settings.rayTracing); value(settings.frustumCulling); value(settings.exposure); value(settings.debugView);
    value(settings.roughness); value(settings.metallic); value(settings.baseColor);
    value(settings.antiAliasing.temporalCurrentWeight); value(viewport); value(world.blackBackground);
    value(lighting); value(camera.projection);
    const auto& dof = camera.lens.depthOfField;
    value(dof.enabled); value(dof.focusDistance); value(dof.fStop); value(dof.sensorHeightMm);
    for (const auto& actor : world.actors) {
        value(actor.id); value(actor.visible); value(actor.transform);
        if (!actor.visible || actor.light || actor.camera) continue;
        value(actor.mesh.meshId); value(actor.mesh.materialId);
        for (const auto id : actor.mesh.materialOverrides) value(id);
        const auto* mesh = assets.findMesh(actor.mesh.meshId);
        if (!mesh) continue;
        const auto key = mesh->geometryKey();
        value(key.revision); value(key.vertexData); value(key.indexData); value(key.vertexCount); value(key.indexCount);
        value(key.uvData); value(key.uvCounts); value(key.sectionData); value(key.sectionCount);
        for (std::size_t slot=0;slot<mesh->materialSlotCount();++slot) {
            const auto* material = resolveMaterial(assets,actor.mesh.materialForSlot(*mesh,slot),draftMaterial);
            if (!material) continue;
            value(material->id); value(material->baseColor); value(material->roughness); value(material->metallic);
            value(material->grid); value(material->twoSided); value(material->cullFront);
            value(material->depthTest); value(material->depthWrite);
        }
    }
    return hash;
}

AntiAliasingStatus resolveAntiAliasing(const SceneSettings& settings, bool hardwareImage) const {
    AntiAliasingStatus status;
    status.requested = status.effective = settings.antiAliasing.method;
    status.historyRevision = aaHistoryRevision;
    if (hardwareImage && settings.globalIllumination.method == GlobalIlluminationMethod::PathTracing &&
        settings.globalIllumination.mode == GlobalIlluminationMode::Progressive && settings.debugView == 0) {
        status.effective = AntiAliasingMethod::Disabled;
        status.reason = "Progressive path tracing already accumulates jittered primary samples.";
    } else if (settings.debugView >= 2) {
        status.effective = AntiAliasingMethod::Disabled;
        status.reason = "Data debug views retain unfiltered pixel values.";
    } else if (status.requested == AntiAliasingMethod::TAA && settings.offscreenTarget) {
        status.effective = AntiAliasingMethod::FXAA;
        status.reason = "Tool previews use FXAA and do not consume the main TAA history.";
    } else if (status.requested == AntiAliasingMethod::TAA && hardwareImage) {
        status.effective = AntiAliasingMethod::FXAA;
        status.reason = "DXR image alignment currently requires stable primary rays; using FXAA to preserve GI history.";
    } else if (status.effective == AntiAliasingMethod::TAA)
        status.reason = "Depth-reprojected TAA; scene edits reset history (no object motion vectors).";
    else if (status.effective == AntiAliasingMethod::FXAA) status.reason = "Spatial FXAA.";
    else { status.effective = AntiAliasingMethod::Disabled; status.reason = "Anti-aliasing disabled."; }
    return status;
}

void prepareTemporalAntiAliasing(const SceneSettings& settings, const scene::World& world,
    const assets::AssetLibrary& assets, const assets::MaterialAsset* draftMaterial,
    const camera::View& camera, const LightingConstants& lighting, const XMFLOAT4& viewport,
    XMMATRIX& projection) {
    const auto key = antiAliasingSceneKey(settings,world,assets,draftMaterial,camera,lighting,viewport);
    float displacement = 0, facing = 0;
    for (unsigned i=0;i!=3;++i) {
        const float previousPosition[] = {aaPreviousEye.x,aaPreviousEye.y,aaPreviousEye.z};
        const float previousForward[] = {aaPreviousForward.x,aaPreviousForward.y,aaPreviousForward.z};
        const float delta = camera.position[i]-previousPosition[i]; displacement += delta*delta;
        facing += camera.forward[i]*previousForward[i];
    }
    if (!aaHistoryValid || key != aaSceneKey || displacement > 4 || facing < .8f) invalidateAntiAliasing();
    aaSceneKey = key;
    // Apply clip-space offsets to x/y using w, for perspective and orthographic.
    const unsigned sample = aaHistoryFrames % 16 + 1;
    const float jitterX = (aaHalton(sample,2)-.5f)*2/viewport.z;
    const float jitterY = (aaHalton(sample,3)-.5f)*2/viewport.w;
    aaJitterPixels = XMFLOAT2(jitterX*viewport.z*.5f,-jitterY*viewport.w*.5f);
    XMFLOAT4X4 offset; XMStoreFloat4x4(&offset,XMMatrixIdentity());
    offset._41 = jitterX; offset._42 = jitterY;
    projection = projection * XMLoadFloat4x4(&offset);
}

AntiAliasingConstants antiAliasingConstants(const SceneSettings& settings, const camera::View& camera,
    const XMMATRIX& viewProjection, const XMFLOAT4& viewport) const {
    AntiAliasingConstants result{};
    XMStoreFloat4x4(&result.inverseViewProjection,XMMatrixInverse(nullptr,viewProjection));
    result.previousViewProjection = aaPreviousViewProjection;
    result.currentEye = XMFLOAT4(camera.position[0],camera.position[1],camera.position[2],0);
    result.currentForward = XMFLOAT4(camera.forward[0],camera.forward[1],camera.forward[2],0);
    result.previousEye = aaPreviousEye; result.previousForward = aaPreviousForward;
    result.viewport = viewport;
    const float weight = std::isfinite(settings.antiAliasing.temporalCurrentWeight)
        ? std::clamp(settings.antiAliasing.temporalCurrentWeight,.02f,1.0f) : .1f;
    result.parameters = XMFLOAT4(std::max(weight,1.0f/(aaHistoryFrames+1)),aaHistoryValid ? 1.0f : 0.0f,
        aaJitterPixels.x,aaJitterPixels.y);
    return result;
}

void executeAntiAliasing(const rhi::TextureHandle& target, const AntiAliasingConstants& aaData,
                        bool temporal) {
    uploadConstants(aaBuffer, aaData);
    auto& commands = sceneCommands();
    commands.bindConstantBuffers(rhi::ShaderStage::Pixel,5,std::span(&aaBuffer,1));
    rhi::RenderPassDesc pass;
    pass.name = temporal ? "TAA resolve" : "FXAA resolve";
    pass.colors = {{temporal ? aaHistoryImages[1-aaHistoryRead] : target,rhi::LoadOp::Clear}};
    // Only the temporal history image may outlive this pass. FXAA resolves into
    // the caller's target (often the swapchain); caching that handle would keep a
    // swapchain image borrowed and make a later ResizeBuffers fail.
    aaResolvedImage = temporal ? pass.colors[0].texture : rhi::TextureHandle{};
    pass.reads = {{aaSourceImage,7},{sceneDepthImage,6}};
    if (temporal) pass.reads.push_back({aaHistoryImages[aaHistoryRead],8});
    commands.beginRenderPass(pass);
    commands.setViewport({aaData.viewport.x,aaData.viewport.y,aaData.viewport.z,aaData.viewport.w});
    drawRhiFullscreen(temporal ? 9 : 8,false);
    commands.endRenderPass();
}

void presentTemporalAntiAliasing(const rhi::TextureHandle& target, const AntiAliasingConstants& aaData,
    const XMMATRIX& viewProjection) {
    auto& commands = sceneCommands();
    rhi::RenderPassDesc pass;
    pass.name = "TAA present";
    pass.colors = {{target,rhi::LoadOp::Clear}};
    pass.reads = {{aaHistoryImages[1-aaHistoryRead],7}};
    commands.beginRenderPass(pass);
    commands.setViewport({aaData.viewport.x,aaData.viewport.y,aaData.viewport.z,aaData.viewport.w});
    drawRhiFullscreen(10,false);
    commands.endRenderPass();
    aaHistoryRead = 1-aaHistoryRead;
    aaHistoryValid = true;
    aaHistoryFrames = std::min(aaHistoryFrames+1,65536u);
    XMStoreFloat4x4(&aaPreviousViewProjection,viewProjection);
    aaPreviousEye = aaData.currentEye; aaPreviousForward = aaData.currentForward;
    aaStatus.accumulatedFrames = aaHistoryFrames;
    aaStatus.historyRevision = aaHistoryRevision;
}
