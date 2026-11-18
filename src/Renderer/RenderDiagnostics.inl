    // Included inside SceneRenderer::Impl. All allocations are optional.
    bool diagnosticsOn = false, collectingDiagnostics = false, captureRequested = false;
    bool gpuQueriesUnavailable = false;
    std::uint64_t diagnosticSequence = 0;
    RenderDiagnosticsFrame diagnosticFrame;
    RenderFrameCapture capture;
    FrameCaptureOptions captureOptions;
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
        rhi::TextureHandle image;
        std::vector<std::uint8_t> cpuBytes; // Offline raw payload, never a Live resource.
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
    RenderFrameCapture pendingCapture;
    std::vector<CapturedTexture> pendingTextures;
    std::string pendingCaptureError;
    std::uint64_t pendingCaptureBytes = 0;
    PipelineResource captureProduced = PipelineResource::None;
    std::uint64_t mainSceneSequence = 0, retireAfterSequence = 0;
    std::uint64_t captureBytes = 0;
    static constexpr std::uint64_t captureBudget = 256ull * 1024 * 1024;
    ComPtr<ID3D11ComputeShader> diagnosticDisplayCS;
    ComPtr<ID3D11Buffer> diagnosticDisplayConstants;

    void retireCapturedTextures() {
        if (capturedTextures.empty()) return;
        for (auto& texture : capturedTextures) retiredCapturedTextures.push_back(std::move(texture));
        capturedTextures.clear();
        retireAfterSequence = mainSceneSequence + 2;
    }

    void pollGpuTimings() {
        struct CaptureTimingUpdate {
            Impl& state;
            ~CaptureTimingUpdate() {
                if (!state.capture.valid || state.capture.offline || state.diagnosticFrame.gpuFrameId != state.capture.frameId) return;
                state.capture.gpuFrameId = state.diagnosticFrame.gpuStatus == GpuTimingStatus::Unavailable
                    ? 0 : state.diagnosticFrame.gpuFrameId;
                state.capture.gpuStatus = state.diagnosticFrame.gpuStatus;
                state.capture.gpuTotalMs = state.diagnosticFrame.gpuTotalMs;
                state.capture.gpuPasses = state.diagnosticFrame.gpuPasses;
            }
        } captureTimingUpdate{*this};
        if (native12 || !device || !context) {
            if (native12 && !gpuQueriesUnavailable) {
                try {
                    if (auto result = native12->pollDiagnosticTiming()) {
                        diagnosticFrame.gpuFrameId = result->frameId;
                        diagnosticFrame.gpuStatus = GpuTimingStatus::Ready;
                        diagnosticFrame.gpuTotalMs = result->totalMs;
                        diagnosticFrame.gpuPasses.clear();
                        for (const auto& [name, ms] : result->passes) {
                            RenderPassTiming pass; pass.name = name; pass.gpuMs = ms;
                            diagnosticFrame.gpuPasses.push_back(std::move(pass));
                        }
                    }
                    return;
                } catch (...) { gpuQueriesUnavailable = true; }
            }
            diagnosticFrame.gpuStatus = GpuTimingStatus::Unavailable;
            diagnosticFrame.gpuFrameId = 0;
            diagnosticFrame.gpuTotalMs = 0;
            diagnosticFrame.gpuPasses.clear();
            activeTiming = nullptr;
            return;
        }
        for (auto& slot : timingRing) {
            if (!slot.busy) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const auto result = context->GetData(slot.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_FALSE) continue;
            if (FAILED(result) || disjoint.Disjoint || disjoint.Frequency == 0) {
                if (capture.valid && !capture.offline && capture.frameId == slot.frameId) {
                    capture.gpuStatus = FAILED(result) ? GpuTimingStatus::Unavailable : GpuTimingStatus::Disjoint;
                    capture.gpuFrameId = FAILED(result) ? 0 : slot.frameId;
                    capture.gpuTotalMs = 0; capture.gpuPasses.clear();
                }
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
            const double scale = 1000.0 / static_cast<double>(disjoint.Frequency);
            auto gpuPasses = slot.passes;
            for (std::size_t i = 0; i < gpuPasses.size(); ++i)
                gpuPasses[i].gpuMs = static_cast<double>(ticks[3 + i * 2] - ticks[2 + i * 2]) * scale;
            // Several slots can finish in one poll, in ring rather than frame
            // order. Preserve the captured frame even if Live advances later.
            if (capture.valid && !capture.offline && capture.frameId == slot.frameId) {
                capture.gpuFrameId = slot.frameId; capture.gpuStatus = GpuTimingStatus::Ready;
                capture.gpuTotalMs = static_cast<double>(ticks[1] - ticks[0]) * scale;
                capture.gpuPasses = gpuPasses;
            }
            if (slot.frameId >= diagnosticFrame.gpuFrameId) {
                diagnosticFrame.gpuFrameId = slot.frameId;
                diagnosticFrame.gpuStatus = GpuTimingStatus::Ready;
                diagnosticFrame.gpuTotalMs = static_cast<double>(ticks[1] - ticks[0]) * scale;
                diagnosticFrame.gpuPasses = std::move(gpuPasses);
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
        sceneCommands();
        inspection->start(captureRequested);
        if (!collectingDiagnostics) return;
        diagnosticStart = Clock::now();
        diagnosticFrame.frameId = rae::diagnostics::currentFrameId();
        if (!diagnosticFrame.frameId) diagnosticFrame.frameId = ++diagnosticSequence;
        diagnosticFrame.cpuSubmitMs = 0;
        diagnosticFrame.drawCalls = diagnosticFrame.triangles = 0;
        diagnosticFrame.droppedEvents = 0;
        diagnosticFrame.passes.clear(); diagnosticFrame.events.clear();
        activePass = static_cast<std::size_t>(-1);
        activeTiming = nullptr;
        if (native12) {
            if (!gpuQueriesUnavailable) try {
                native12->beginDiagnosticFrame(diagnosticFrame.frameId);
                if (!diagnosticFrame.gpuFrameId) diagnosticFrame.gpuStatus = GpuTimingStatus::Pending;
            } catch (...) { gpuQueriesUnavailable = true; diagnosticFrame.gpuStatus = GpuTimingStatus::Unavailable; }
            return;
        }
        if (!device || !context || gpuQueriesUnavailable) return;
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
        if (activeTiming && context) context->End(activeTiming->timestamps[3 + activePass * 2].Get());
        if (native12 && !gpuQueriesUnavailable) try { native12->endDiagnosticPass(); } catch (...) { gpuQueriesUnavailable = true; }
        diagnosticFrame.passes[activePass].lastEventId = diagnosticFrame.events.size();
        diagnosticPassTrace.reset();
        activePass = static_cast<std::size_t>(-1);
    }

    void beginDiagnosticPass(std::string_view name) {
        if (!collectingDiagnostics) return;
        endDiagnosticPass();
        // Native stages do not call the RHI pipeline binding helpers. Never
        // label them with the previous pass's Shader entry points.
        diagnosticVS.clear(); diagnosticPS.clear();
        if (diagnosticFrame.passes.size() == maxDiagnosticPasses) return;
        activePass = diagnosticFrame.passes.size();
        diagnosticFrame.passes.push_back({std::string(name)});
        passStart = Clock::now();
        diagnosticFrame.passes.back().cpuStartMs = std::chrono::duration<double, std::milli>(passStart - diagnosticStart).count();
        diagnosticFrame.passes.back().firstEventId = diagnosticFrame.events.size() + 1;
        for (const auto& declaration : pipeline.passes) if (declaration.name == name) {
            diagnosticFrame.passes.back().reads.push_back(describePipelineResources(declaration.reads));
            diagnosticFrame.passes.back().writes.push_back(describePipelineResources(declaration.writes));
        }
        if (rae::diagnostics::profiler().isRecording())
            diagnosticPassTrace = std::make_unique<rae::diagnostics::TraceScope>(
                std::string(name).c_str(), "Renderer CPU");
        if (activeTiming && context) context->End(activeTiming->timestamps[2 + activePass * 2].Get());
        if (native12 && !gpuQueriesUnavailable) try { native12->beginDiagnosticPass(name); } catch (...) { gpuQueriesUnavailable = true; }
    }

    RenderDrawEvent* recordDraw(std::uint64_t actor, std::uint64_t mesh, std::uint64_t material,
                    std::uint32_t indices, std::uint32_t vertices) {
        if (!collectingDiagnostics || activePass == static_cast<std::size_t>(-1)) return nullptr;
        ++diagnosticFrame.drawCalls;
        diagnosticFrame.triangles += (indices ? indices : vertices) / 3;
        ++diagnosticFrame.passes[activePass].drawCalls;
        if (diagnosticFrame.events.size() < 65536) {
            diagnosticFrame.events.push_back({diagnosticFrame.passes[activePass].name, actor, mesh, material,
                indices, vertices, revision});
            auto& event = diagnosticFrame.events.back();
            event.eventId = diagnosticFrame.events.size();
            event.passIndex = static_cast<std::uint32_t>(activePass);
            event.command = indices ? "DrawIndexed (native)" : "Draw (native)";
            if (inspection) inspection->describe(event);
            event.vertexShader = diagnosticVS; event.pixelShader = diagnosticPS;
            return &event;
        } else ++diagnosticFrame.droppedEvents;
        return nullptr;
    }

    void finishDiagnostics() {
        if (!collectingDiagnostics) return;
        endDiagnosticPass();
        diagnosticFrame.cpuSubmitMs = std::chrono::duration<double, std::milli>(Clock::now() - diagnosticStart).count();
        if (activeTiming && context) {
            activeTiming->passes = diagnosticFrame.passes;
            context->End(activeTiming->timestamps[1].Get());
            context->End(activeTiming->disjoint.Get());
            activeTiming = nullptr;
        }
        if (native12 && !gpuQueriesUnavailable) try { native12->endDiagnosticFrame(); } catch (...) { gpuQueriesUnavailable = true; }
        collectingDiagnostics = false;
    }

#include "FrameCaptureResources.inl"

    bool ensureDiagnosticDisplay(std::string& error) {
        if (native12 || !device || !context) {
            error = native12
                ? "Frame Inspector texture display is not implemented for the native D3D12 backend."
                : "Frame Inspector texture display requires an initialized rendering device.";
            return false;
        }
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
