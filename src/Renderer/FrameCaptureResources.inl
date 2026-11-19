    // Included inside Impl. Candidates are committed only after every requested
    // GPU copy succeeds. A failed capture never destroys the previous frame.
    void beginFrameCapture(const SceneSettings& settings, const camera::Lens& lens) {
        if (!captureRequested) return;
        pendingCapture = {};
        pendingTextures.clear(); pendingCaptureError.clear(); pendingCaptureBytes = 0;
        captureProduced = PipelineResource::None;
        pendingCapture.frameId = diagnosticFrame.frameId;
        pendingCapture.shaderRevision = revision;
        pendingCapture.renderPath = settings.path == RenderPath::Deferred ? "Deferred" : "Forward";
        pendingCapture.backend = native12 ? "D3D12" : "D3D11";
        pendingCapture.width = settings.editorViewport || settings.offscreenTarget ? sceneWidth : width;
        pendingCapture.height = settings.editorViewport || settings.offscreenTarget ? sceneHeight : height;
        pendingCapture.nearPlane = lens.nearClip; pendingCapture.farPlane = lens.farClip;
        pendingCapture.orthographic = lens.projection == camera::Projection::Orthographic;
        pendingCapture.materialShaderSource = materialSource;
        pendingCapture.pipelineShaderSource = sceneSource;
    }

    void appendCaptureImage(const rhi::TextureHandle& source, const std::string& name,
        unsigned kind, const std::string& pass, const char* boundary, std::uint32_t layer = 0) {
        if (!pendingCaptureError.empty()) return;
        try {
            if (!source) throw std::runtime_error("Requested capture attachment is unavailable.");
            const auto& desc = source->description();
            const auto size = std::uint64_t(desc.width) * desc.height * (desc.format == rhi::Format::RGBA16Float ? 8 : 4);
            if (size > captureOptions.budgetBytes - pendingCaptureBytes)
                throw std::runtime_error("Frame capture resource budget exceeded. Select fewer attachments or a smaller viewport.");
            CapturedTexture image;
            image.kind = kind;
            image.image = rhiDevice->createTexture({desc.width, desc.height, 1, desc.format, true, "Frozen " + name});
            if (native12) native12->copyTextureRegion(image.image, source, 0, 0, desc.width, desc.height, layer);
            else {
                image.texture = d3d11::nativeTexture(image.image);
                image.view = d3d11::nativeShaderResource(image.image);
                image.format = desc.format == rhi::Format::RGBA16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT
                    : desc.format == rhi::Format::Depth32Float ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
                context->OMSetRenderTargets(0, nullptr, nullptr);
                context->CopySubresourceRegion(image.texture.Get(), 0, 0, 0, 0, d3d11::nativeTexture(source), layer, nullptr);
            }
            CapturedRenderResource resource;
            resource.name = name; resource.format = desc.format == rhi::Format::RGBA16Float ? "RGBA16F"
                : desc.format == rhi::Format::Depth32Float ? "R32_FLOAT" : "RGBA8_UNORM";
            resource.width = desc.width; resource.height = desc.height; resource.bytes = size;
            resource.pass = pass; resource.boundary = boundary; resource.layer = layer; resource.displayKind = kind;
            resource.eventId = diagnosticFrame.events.size();
            pendingCapture.resources.push_back(std::move(resource));
            pendingTextures.push_back(std::move(image));
            pendingCaptureBytes += size;
        } catch (const std::exception& e) { pendingCaptureError = e.what(); }
    }

    void capturePassBoundary(const RenderPassDeclaration& pass, bool before,
        const rhi::TextureHandle& color, std::uint32_t shadowLayers) {
        if (!captureRequested || captureOptions.pass.empty() || captureOptions.pass != pass.name ||
            (before && !captureOptions.beforeAfter)) return;
        auto resources = before ? (pass.reads | pass.writes) & captureProduced : pass.writes;
        const char* boundary = before ? "Before" : "After";
        const auto add = [&](PipelineResource bit, const rhi::TextureHandle& image, const char* name, unsigned kind) {
            if (contains(resources, bit)) appendCaptureImage(image, std::string(pass.name) + " / " + boundary + " / " + name,
                kind, std::string(pass.name), boundary);
        };
        add(PipelineResource::SceneColor, color, "SceneColor", 0);
        add(PipelineResource::AntiAliasingSource,aaSourceImage,"AntiAliasingSource",0);
        add(PipelineResource::AntiAliasingHistory,aaResolvedImage,"AntiAliasingHistory RGB / linear depth",0);
        if (before && aaHistoryValid && contains(pass.reads,PipelineResource::PreviousAntiAliasingHistory))
            appendCaptureImage(aaHistoryImages[aaHistoryRead],std::string(pass.name)+" / Before / PreviousAntiAliasingHistory",
                0,std::string(pass.name),boundary);
        add(PipelineResource::SceneDepth, sceneDepthImage, "Depth", 4);
        add(PipelineResource::GBufferAlbedo, gbufferImages[0], "GBuffer0 Albedo / Roughness", 1);
        add(PipelineResource::GBufferNormal, gbufferImages[1], "GBuffer1 WorldNormal / Metallic", 2);
        add(PipelineResource::GBufferPosition, gbufferImages[2], "GBuffer2 WorldPosition / Valid", 3);
        if (contains(resources, PipelineResource::ShadowMap)) for (std::uint32_t i = 0; i < shadowLayers; ++i)
            appendCaptureImage(shadowImage, std::string(pass.name) + " / " + boundary + " / Shadow Depth layer " + std::to_string(i),
                5, std::string(pass.name), boundary, i);
    }

    bool copyFrameCapture(const SceneSettings& settings, std::string& error) {
        if (!rhiDevice) { error = "Capture requires an initialized device."; return false; }
        rae::diagnostics::TraceScope trace("Frame capture copy", "Renderer CPU");
        if (captureOptions.pass.empty()) {
            const auto color = settings.editorViewport || settings.offscreenTarget ? sceneColorImage : swapchainImage;
            appendCaptureImage(color, "SceneColor (after viewport debug / outline)", 0, "Final outputs", "Final");
            if (settings.path == RenderPath::Deferred) {
                const char* names[] = {"GBuffer0: Albedo.rgb / Roughness.a", "GBuffer1: WorldNormal.xyz / Metallic.a", "GBuffer2: WorldPosition.xyz / Valid.a"};
                for (std::size_t i = 0; i < 3; ++i) appendCaptureImage(gbufferImages[i], names[i], static_cast<unsigned>(i + 1), "Final outputs", "Final");
            }
            appendCaptureImage(sceneDepthImage, "Depth (raw device depth; preview linear 0..100m)", 4, "Final outputs", "Final");
        }
        if (pendingCaptureError.empty() && pendingTextures.empty()) pendingCaptureError = "Selected pass did not produce any supported attachment in this frame.";
        if (!pendingCaptureError.empty()) { error = pendingCaptureError; pendingTextures.clear(); return false; }
        pendingCapture.cpuSubmitMs = diagnosticFrame.cpuSubmitMs;
        pendingCapture.events = diagnosticFrame.events;
        pendingCapture.passes = diagnosticFrame.passes;
        pendingCapture.droppedEvents = diagnosticFrame.droppedEvents;
        pendingCapture.gpuStatus = gpuQueriesUnavailable ? GpuTimingStatus::Unavailable : GpuTimingStatus::Pending;
        pendingCapture.gpuFrameId = gpuQueriesUnavailable ? 0 : pendingCapture.frameId;
        pendingCapture.cpuTrace = rae::diagnostics::profiler().snapshot(4096);
        pendingCapture.valid = true;
        pendingCapture.status = captureOptions.pass.empty()
            ? "Captured final pass outputs. Select a draw to inspect its recorded state; images are not per-draw replay."
            : "Captured selected pass boundaries. Before images include only resources initialized earlier in this frame. No event replay.";
        retireCapturedTextures();
        capture = std::move(pendingCapture); capturedTextures = std::move(pendingTextures);
        captureBytes = pendingCaptureBytes;
        rae::diagnostics::runtimeDiagnostics().log(rae::diagnostics::MessageSeverity::Info, "Frame Inspector", capture.status);
        return true;
    }

    bool readCaptureBytes(std::size_t index, std::vector<std::uint8_t>& bytes, std::string& error) const {
        if (!capture.valid || index >= capturedTextures.size()) { error = "No captured resource selected."; return false; }
        const auto& image = capturedTextures[index];
        if (!image.cpuBytes.empty()) { bytes = image.cpuBytes; return true; }
        try {
            const auto& metadata = capture.resources[index];
            const auto rowBytes = metadata.width * (metadata.format == "RGBA16F" ? 8u : 4u);
            bytes.resize(static_cast<std::size_t>(metadata.bytes));
            if (native12) {
                auto raw = native12->readTexture(image.image);
                for (std::uint32_t y = 0; y < metadata.height; ++y)
                    std::memcpy(bytes.data() + std::size_t(y) * rowBytes, raw.bytes.data() + std::size_t(y) * raw.rowPitch, rowBytes);
            } else {
                D3D11_TEXTURE2D_DESC desc{}; image.texture->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> staging;
                if (failed(device->CreateTexture2D(&desc, nullptr, &staging), "Create resource readback", error)) return false;
                context->CopyResource(staging.Get(), image.texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (failed(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read frozen resource", error)) return false;
                for (std::uint32_t y = 0; y < metadata.height; ++y)
                    std::memcpy(bytes.data() + std::size_t(y) * rowBytes, static_cast<const std::uint8_t*>(mapped.pData) + std::size_t(y) * mapped.RowPitch, rowBytes);
                context->Unmap(staging.Get(), 0);
            }
            return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }

    static std::array<float, 4> decodeCapturePixel(const std::uint8_t* bytes, const std::string& format) {
        std::array<float, 4> result{0, 0, 0, 1};
        if (format == "RGBA16F") {
            std::array<DirectX::PackedVector::HALF, 4> half{};
            std::memcpy(half.data(), bytes, 8);
            for (std::size_t i = 0; i < 4; ++i) result[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
        } else if (format == "R32_FLOAT") std::memcpy(&result[0], bytes, 4);
        else for (std::size_t i = 0; i < 4; ++i) result[i] = float(bytes[i]) / 255.f;
        return result;
    }

    void* capturedCpuDisplay(std::size_t index, std::size_t channel) {
        auto& image = capturedTextures[index];
        auto& display = image.displays[channel];
        if (display.image) return reinterpret_cast<void*>(static_cast<std::uintptr_t>(native12->uiTextureId(display.image)));
        if (display.view) return display.view.Get();
        const auto& metadata = capture.resources[index];
        const auto displayBytes = std::uint64_t(metadata.width) * metadata.height * 4;
        if (displayBytes > captureBudget - captureBytes) { capture.status = "Capture/display budget reached. Raw snapshot retained."; return nullptr; }
        std::string error;
        std::vector<std::uint8_t> bytes;
        if (!readCaptureBytes(index, bytes, error)) { capture.status = "Readback failed: " + error; return nullptr; }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(displayBytes));
        const auto stride = metadata.format == "RGBA16F" ? 8u : 4u;
        for (std::size_t pixel = 0; pixel < std::size_t(metadata.width) * metadata.height; ++pixel) {
            const auto raw = decodeCapturePixel(bytes.data() + pixel * stride, metadata.format);
            auto mapped = raw;
            for (std::size_t c = 0; c < 3; ++c) {
                if (image.kind == 1) mapped[c] = std::pow(std::clamp(raw[c], 0.f, 1.f), 1.f / 2.2f);
                else if (image.kind == 2) mapped[c] = raw[c] * .5f + .5f;
                else if (image.kind == 3) mapped[c] = std::fmod(std::abs(raw[c]) * .25f, 1.f);
                else if (image.kind == 4) {
                    const float n = capture.nearPlane, f = capture.farPlane;
                    const float z = capture.orthographic ? n + raw[0] * (f - n) : n * f / std::max(f - raw[0] * (f - n), .000001f);
                    mapped[c] = z / 100.f;
                }
            }
            for (std::size_t c = 0; c < 3; ++c) {
                float value = mapped[c];
                if (channel) {
                    const auto component = channel - 1;
                    value = component < 3 ? mapped[component] : raw[3];
                    if (image.kind == 1 && component < 3) value = raw[component];
                    // Only R stores camera depth. RGB is a grayscale preview,
                    // but selecting G/B must still report their raw channels.
                    if (image.kind == 4 && component > 0) value = raw[component];
                }
                rgba[pixel * 4 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(std::isfinite(value) ? value : 0.f, 0.f, 1.f) * 255.f));
            }
            rgba[pixel * 4 + 3] = 255;
        }
        try {
            if (native12) display.image = native12->uploadRgba8(metadata.width, metadata.height,
                std::as_bytes(std::span(rgba)), "Frozen resource display");
            else {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = metadata.width; desc.Height = metadata.height; desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initial{rgba.data(), metadata.width * 4, 0};
                if (failed(device->CreateTexture2D(&desc, &initial, &display.texture), "Upload frozen display", error) ||
                    failed(device->CreateShaderResourceView(display.texture.Get(), nullptr, &display.view), "Create frozen display view", error)) {
                    capture.status = error; return nullptr;
                }
            }
            captureBytes += displayBytes;
            return native12 ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(native12->uiTextureId(display.image))) : display.view.Get();
        } catch (const std::exception& e) { capture.status = e.what(); return nullptr; }
    }
