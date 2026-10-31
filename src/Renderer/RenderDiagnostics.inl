    // Included inside SceneRenderer::Impl. All allocations are optional.
    bool diagnosticsOn = false, collectingDiagnostics = false, captureRequested = false;
    bool gpuQueriesUnavailable = false;
    std::uint64_t diagnosticSequence = 0;
    RenderDiagnosticsFrame diagnosticFrame;
    RenderFrameCapture capture;
    Clock::time_point diagnosticStart{}, passStart{};
    std::unique_ptr<rae::diagnostics::TraceScope> diagnosticPassTrace;
    std::size_t activePass = static_cast<std::size_t>(-1);
    static constexpr std::size_t maxDiagnosticPasses = 12;
    struct TimingSlot {
        ComPtr<ID3D11Query> disjoint;
        std::array<ComPtr<ID3D11Query>, 2 + maxDiagnosticPasses * 2> timestamps;
        bool busy = false;
        std::uint64_t frameId = 0;
        std::vector<RenderPassTiming> passes;
    };
    std::array<TimingSlot, 4> timingRing;
    TimingSlot* activeTiming = nullptr;
    struct CapturedTexture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        std::array<PreviewImage, 5> displays;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        unsigned kind = 0; // color, albedo+roughness, normal+metallic, position, depth
    };
    std::vector<CapturedTexture> capturedTextures;
    // ImGui may already hold raw SRVs in this frame's not-yet-submitted draw
    // list when Clear/Capture replaces the snapshot. Retire one previous set.
    std::vector<CapturedTexture> retiredCapturedTextures;
    std::uint64_t mainSceneSequence = 0, retireAfterSequence = 0;
    std::uint64_t captureBytes = 0;
    static constexpr std::uint64_t captureBudget = 256ull * 1024 * 1024;
    ComPtr<ID3D11ComputeShader> diagnosticDisplayCS;
    ComPtr<ID3D11Buffer> diagnosticDisplayConstants;

    void retireCapturedTextures() {
        if (capturedTextures.empty()) return;
        retiredCapturedTextures = std::move(capturedTextures);
        retireAfterSequence = mainSceneSequence + 2;
    }

    void pollGpuTimings() {
        if (!context) return;
        for (auto& slot : timingRing) {
            if (!slot.busy) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const auto result = context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_FALSE) continue;
            if (FAILED(result) || disjoint.Disjoint || disjoint.Frequency == 0) {
                if (slot.frameId >= diagnosticFrame.gpuFrameId) {
                    diagnosticFrame.gpuFrameId = slot.frameId;
                    diagnosticFrame.gpuStatus = FAILED(result) ? GpuTimingStatus::Unavailable : GpuTimingStatus::Disjoint;
                    diagnosticFrame.gpuPasses.clear();
                }
                slot.busy = false;
                continue;
            }
            std::array<UINT64, 2 + maxDiagnosticPasses * 2> ticks{};
            bool ready = true;
            for (std::size_t i = 0; i < 2 + slot.passes.size() * 2; ++i) {
                if (context->GetData(slot.timestamps[i].Get(), &ticks[i], sizeof(UINT64),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ready = false; break; }
            }
            if (!ready) continue;
            if (slot.frameId >= diagnosticFrame.gpuFrameId) {
                diagnosticFrame.gpuFrameId = slot.frameId;
                diagnosticFrame.gpuStatus = GpuTimingStatus::Ready;
                const double scale = 1000.0 / static_cast<double>(disjoint.Frequency);
                diagnosticFrame.gpuTotalMs = static_cast<double>(ticks[1] - ticks[0]) * scale;
                diagnosticFrame.gpuPasses = slot.passes;
                for (std::size_t i = 0; i < slot.passes.size(); ++i)
                    diagnosticFrame.gpuPasses[i].gpuMs = static_cast<double>(ticks[3 + i * 2] - ticks[2 + i * 2]) * scale;
            }
            slot.busy = false;
        }
    }

    void beginDiagnostics(bool mainScene) {
        if (!mainScene) return;
        ++mainSceneSequence;
        if (mainSceneSequence >= retireAfterSequence) retiredCapturedTextures.clear();
        pollGpuTimings();
        collectingDiagnostics = diagnosticsOn || captureRequested || rae::diagnostics::profiler().isRecording();
        if (!collectingDiagnostics) return;
        diagnosticStart = Clock::now();
        diagnosticFrame.frameId = rae::diagnostics::currentFrameId();
        if (!diagnosticFrame.frameId) diagnosticFrame.frameId = ++diagnosticSequence;
        diagnosticFrame.cpuSubmitMs = 0;
        diagnosticFrame.drawCalls = diagnosticFrame.triangles = 0;
        diagnosticFrame.passes.clear(); diagnosticFrame.events.clear();
        activePass = static_cast<std::size_t>(-1);
        activeTiming = nullptr;
        if (gpuQueriesUnavailable) return;
        for (auto& slot : timingRing) {
            if (slot.busy) continue;
            if (!slot.disjoint) {
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                if (FAILED(device->CreateQuery(&desc, &slot.disjoint))) { gpuQueriesUnavailable = true; break; }
                desc.Query = D3D11_QUERY_TIMESTAMP;
                for (auto& query : slot.timestamps) {
                    if (FAILED(device->CreateQuery(&desc, &query))) { gpuQueriesUnavailable = true; break; }
                }
                if (gpuQueriesUnavailable) break;
            }
            activeTiming = &slot;
            slot.busy = true;
            slot.frameId = diagnosticFrame.frameId;
            slot.passes.clear();
            context->Begin(slot.disjoint.Get());
            context->End(slot.timestamps[0].Get());
            if (!diagnosticFrame.gpuFrameId) diagnosticFrame.gpuStatus = GpuTimingStatus::Pending;
            break;
        }
    }

    void endDiagnosticPass() {
        if (!collectingDiagnostics || activePass == static_cast<std::size_t>(-1)) return;
        diagnosticFrame.passes[activePass].cpuMs =
            std::chrono::duration<double, std::milli>(Clock::now() - passStart).count();
        if (activeTiming) context->End(activeTiming->timestamps[3 + activePass * 2].Get());
        diagnosticPassTrace.reset();
        activePass = static_cast<std::size_t>(-1);
    }

    void beginDiagnosticPass(std::string_view name) {
        if (!collectingDiagnostics) return;
        endDiagnosticPass();
        if (diagnosticFrame.passes.size() == maxDiagnosticPasses) return;
        activePass = diagnosticFrame.passes.size();
        diagnosticFrame.passes.push_back({std::string(name)});
        passStart = Clock::now();
        if (rae::diagnostics::profiler().isRecording())
            diagnosticPassTrace = std::make_unique<rae::diagnostics::TraceScope>(
                std::string(name).c_str(), "Renderer CPU");
        if (activeTiming) context->End(activeTiming->timestamps[2 + activePass * 2].Get());
    }

    void recordDraw(std::uint64_t actor, std::uint64_t mesh, std::uint64_t material,
                    std::uint32_t indices, std::uint32_t vertices) {
        if (!collectingDiagnostics || activePass == static_cast<std::size_t>(-1)) return;
        ++diagnosticFrame.drawCalls;
        diagnosticFrame.triangles += (indices ? indices : vertices) / 3;
        ++diagnosticFrame.passes[activePass].drawCalls;
        if (diagnosticFrame.events.size() < 65536)
            diagnosticFrame.events.push_back({diagnosticFrame.passes[activePass].name, actor, mesh, material,
                indices, vertices, revision});
    }

    void finishDiagnostics() {
        if (!collectingDiagnostics) return;
        endDiagnosticPass();
        diagnosticFrame.cpuSubmitMs = std::chrono::duration<double, std::milli>(Clock::now() - diagnosticStart).count();
        if (activeTiming) {
            activeTiming->passes = diagnosticFrame.passes;
            context->End(activeTiming->timestamps[1].Get());
            context->End(activeTiming->disjoint.Get());
            activeTiming = nullptr;
        }
        collectingDiagnostics = false;
    }

    bool copyFrameCapture(const SceneSettings& settings, std::string& error) {
        rae::diagnostics::TraceScope trace("Frame capture copy", "Renderer CPU");
        RenderFrameCapture candidate;
        candidate.frameId = diagnosticFrame.frameId;
        candidate.shaderRevision = revision;
        candidate.renderPath = settings.path == RenderPath::Deferred ? "Deferred" : "Forward";
        candidate.cpuSubmitMs = diagnosticFrame.cpuSubmitMs;
        candidate.events = diagnosticFrame.events;
        std::vector<CapturedTexture> images;
        std::uint64_t bytes = 0;
        const auto copy = [&](ID3D11Resource* source, const char* name, unsigned kind) {
            CapturedTexture image;
            ComPtr<ID3D11Texture2D> original;
            if (!source || FAILED(source->QueryInterface(IID_PPV_ARGS(&original)))) {
                error = "Capture source unavailable."; return false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            original->GetDesc(&desc);
            const bool half = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
            const std::uint64_t size = static_cast<std::uint64_t>(desc.Width) * desc.Height * (half ? 8 : 4);
            if (size > captureBudget - bytes) { error = "Frame capture exceeds the 256 MiB resource budget. Reduce the window size."; return false; }
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.CPUAccessFlags = desc.MiscFlags = 0;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (failed(device->CreateTexture2D(&desc, nullptr, &image.texture), "Create capture snapshot", error)) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = desc.Format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : desc.Format;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            if (failed(device->CreateShaderResourceView(image.texture.Get(), &srv, &image.view), "Create capture view", error)) return false;
            context->CopyResource(image.texture.Get(), original.Get());
            image.format = desc.Format;
            image.kind = kind;
            candidate.resources.push_back({name, half ? "RGBA16F" : kind == 4 ? "R32_FLOAT" : "RGBA8_UNORM",
                desc.Width, desc.Height, size});
            images.push_back(std::move(image));
            bytes += size;
            return true;
        };
        ComPtr<ID3D11Resource> color;
        if (settings.editorViewport || settings.offscreenTarget) color = sceneTexture;
        else backbuffer->GetResource(&color);
        if (!copy(color.Get(), "SceneColor (after viewport debug / outline)", 0)) return false;
        if (settings.path == RenderPath::Deferred) {
            const char* names[] = {"GBuffer0: Albedo.rgb / Roughness.a", "GBuffer1: WorldNormal.xyz / Metallic.a", "GBuffer2: WorldPosition.xyz / Valid.a"};
            for (std::size_t i = 0; i < gbufferTargets.size(); ++i) {
                ComPtr<ID3D11Resource> source;
                gbufferTargets[i]->GetResource(&source);
                if (!copy(source.Get(), names[i], static_cast<unsigned>(i + 1))) return false;
            }
        }
        ComPtr<ID3D11Resource> depthSource;
        depth->GetResource(&depthSource);
        if (!copy(depthSource.Get(), "Depth (raw device depth; preview linear 0..100m)", 4)) return false;
        candidate.valid = true;
        candidate.status = "Captured final pass outputs. No event replay or per-draw images. Export is JSON metadata only.";
        if (diagnosticFrame.events.size() == 65536) candidate.status += " Event list limited to 65536 draws.";
        // Release the old snapshot only after all new resources succeeded.
        retireCapturedTextures();
        capture = std::move(candidate);
        capturedTextures = std::move(images);
        captureBytes = bytes;
        return true;
    }

    bool ensureDiagnosticDisplay(std::string& error) {
        if (diagnosticDisplayCS) return true;
        static constexpr char source[] = R"(
Texture2D<float4> inputImage : register(t0);
RWTexture2D<float4> outputImage : register(u0);
cbuffer DisplaySettings : register(b0) { uint width; uint height; uint channel; uint kind; };
[numthreads(8,8,1)] void main(uint3 p : SV_DispatchThreadID) {
    if (p.x >= width || p.y >= height) return;
    float4 raw = inputImage.Load(int3(p.xy,0));
    float3 rgb = raw.rgb;
    if (kind == 1) rgb = pow(saturate(rgb), 1.0/2.2);
    if (kind == 2) rgb = rgb * 0.5 + 0.5;
    if (kind == 3) rgb = frac(abs(rgb) * 0.25);
    if (kind == 4) rgb = (0.05 / max(100.0 - raw.r * 99.95, 0.00001)).xxx;
    if (channel != 0) {
        float v = channel == 1 ? raw.r : channel == 2 ? raw.g : channel == 3 ? raw.b : raw.a;
        if (kind == 2 && channel < 4) v = v * 0.5 + 0.5;
        if (kind == 3 && channel < 4) v = frac(abs(v) * 0.25);
        if (kind == 4 && channel == 1) v = rgb.r;
        rgb = v.xxx;
    }
    outputImage[p.xy] = float4(saturate(rgb),1);
})";
        ComPtr<ID3DBlob> bytecode, diagnostics;
        if (failed(D3DCompile(source, sizeof(source) - 1, "DiagnosticDisplay", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &diagnostics), "Compile diagnostic display", error)) return false;
        ComPtr<ID3D11ComputeShader> shader;
        if (failed(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader),
            "Create diagnostic display", error)) return false;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 16; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (failed(device->CreateBuffer(&desc, nullptr, &diagnosticDisplayConstants), "Create diagnostic display constants", error)) return false;
        diagnosticDisplayCS = std::move(shader);
        return true;
    }
