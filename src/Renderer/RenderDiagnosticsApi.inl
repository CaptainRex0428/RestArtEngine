// Included in SceneRenderer.cpp after the private Impl definition.
void SceneRenderer::setDiagnosticsEnabled(bool enabled) { impl_->diagnosticsOn = enabled; }
bool SceneRenderer::diagnosticsEnabled() const { return impl_->diagnosticsOn; }
const RenderDiagnosticsFrame& SceneRenderer::diagnosticsFrame() const { return impl_->diagnosticFrame; }
void SceneRenderer::requestFrameCapture() {
    impl_->captureRequested = true;
    impl_->capture.status = "Capture pending: next main scene frame.";
}
const RenderFrameCapture& SceneRenderer::frameCapture() const { return impl_->capture; }
void SceneRenderer::clearFrameCapture() {
    impl_->captureRequested = false;
    impl_->capture = {};
    impl_->retireCapturedTextures();
    impl_->captureBytes = 0;
}

void* SceneRenderer::capturedTexture(std::size_t resourceIndex, DebugTextureChannel channel) {
    auto& state = *impl_;
    const auto channelIndex = static_cast<std::size_t>(channel);
    if (!state.capture.valid || resourceIndex >= state.capturedTextures.size() || channelIndex >= 5) return nullptr;
    auto& resource = state.capturedTextures[resourceIndex];
    auto& display = resource.displays[channelIndex];
    if (display.view) return display.view.Get();
    rae::diagnostics::TraceScope trace("Captured resource display conversion", "Renderer CPU");
    std::string error;
    const auto fail = [&state, &error]() -> void* {
        state.capture.status = "Display failed: " + error + " Raw snapshot retained.";
        return nullptr;
    };
    if (!state.ensureDiagnosticDisplay(error)) return fail();
    D3D11_TEXTURE2D_DESC desc{};
    resource.texture->GetDesc(&desc);
    const auto bytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * 4;
    if (bytes > Impl::captureBudget - state.captureBytes) {
        error = "256 MiB capture/display budget reached. Clear the capture and reduce the window size.";
        return fail();
    }
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    Impl::PreviewImage candidate;
    ComPtr<ID3D11UnorderedAccessView> output;
    if (failed(state.device->CreateTexture2D(&desc, nullptr, &candidate.texture), "Create capture display texture", error) ||
        failed(state.device->CreateShaderResourceView(candidate.texture.Get(), nullptr, &candidate.view), "Create capture display SRV", error) ||
        failed(state.device->CreateUnorderedAccessView(candidate.texture.Get(), nullptr, &output), "Create capture display UAV", error)) return fail();

    // Conversion uses only CS bindings and restores those borrowed bindings.
    // It does not change the main viewport, graphics shaders or render targets.
    ComPtr<ID3D11ComputeShader> oldShader;
    ComPtr<ID3D11ShaderResourceView> oldSource;
    ComPtr<ID3D11UnorderedAccessView> oldOutput;
    ComPtr<ID3D11Buffer> oldConstants;
    state.context->CSGetShader(&oldShader, nullptr, nullptr);
    state.context->CSGetShaderResources(0, 1, &oldSource);
    state.context->CSGetUnorderedAccessViews(0, 1, &oldOutput);
    state.context->CSGetConstantBuffers(0, 1, &oldConstants);
    const UINT settings[] = {desc.Width, desc.Height, static_cast<UINT>(channelIndex), resource.kind};
    state.context->UpdateSubresource(state.diagnosticDisplayConstants.Get(), 0, nullptr, settings, 0, 0);
    ID3D11Buffer* constants = state.diagnosticDisplayConstants.Get();
    ID3D11ShaderResourceView* input = resource.view.Get();
    ID3D11UnorderedAccessView* destination = output.Get();
    state.context->CSSetShader(state.diagnosticDisplayCS.Get(), nullptr, 0);
    state.context->CSSetConstantBuffers(0, 1, &constants);
    state.context->CSSetShaderResources(0, 1, &input);
    state.context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    state.context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, 1);
    destination = nullptr; input = nullptr;
    state.context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    state.context->CSSetShaderResources(0, 1, &input);
    destination = oldOutput.Get(); input = oldSource.Get(); constants = oldConstants.Get();
    state.context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
    state.context->CSSetShaderResources(0, 1, &input);
    state.context->CSSetConstantBuffers(0, 1, &constants);
    state.context->CSSetShader(oldShader.Get(), nullptr, 0);
    display = std::move(candidate);
    state.captureBytes += bytes;
    return display.view.Get();
}

bool SceneRenderer::readCapturedPixel(std::size_t resourceIndex, std::uint32_t x, std::uint32_t y,
                                    float (&value)[4], std::string& error) const {
    error.clear();
    const auto& state = *impl_;
    if (!state.capture.valid || resourceIndex >= state.capturedTextures.size()) {
        error = "No captured resource selected."; return false;
    }
    const auto& source = state.capturedTextures[resourceIndex];
    D3D11_TEXTURE2D_DESC desc{};
    source.texture->GetDesc(&desc);
    if (x >= desc.Width || y >= desc.Height) { error = "Pixel coordinates are outside the captured texture."; return false; }
    rae::diagnostics::TraceScope trace("Captured pixel synchronous readback", "GPU readback wait");
    desc.Width = desc.Height = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (failed(state.device->CreateTexture2D(&desc, nullptr, &staging), "Create pixel readback", error)) return false;
    const D3D11_BOX region{x, y, 0, x + 1, y + 1, 1};
    state.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source.texture.Get(), 0, &region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (failed(state.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read captured pixel", error)) return false;
    float result[4] = {0, 0, 0, 1};
    if (source.format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto* half = static_cast<const DirectX::PackedVector::HALF*>(mapped.pData);
        for (unsigned i = 0; i < 4; ++i) result[i] = DirectX::PackedVector::XMConvertHalfToFloat(half[i]);
    } else if (source.format == DXGI_FORMAT_R32_TYPELESS || source.format == DXGI_FORMAT_R32_FLOAT) {
        result[0] = *static_cast<const float*>(mapped.pData);
    } else {
        const auto* bytes = static_cast<const unsigned char*>(mapped.pData);
        for (unsigned i = 0; i < 4; ++i) result[i] = static_cast<float>(bytes[i]) / 255.0f;
    }
    state.context->Unmap(staging.Get(), 0);
    std::copy(std::begin(result), std::end(result), std::begin(value));
    return true;
}

bool SceneRenderer::exportFrameCapture(const std::filesystem::path& path, std::string& error) const {
    error.clear();
    const auto& capture = impl_->capture;
    if (!capture.valid) { error = "No frame captured."; return false; }
    rae::diagnostics::TraceScope trace("Export frame metadata", "File IO");
    try {
        nlohmann::json data{{"format", "RestArtEngine.FrameInspector.Metadata"}, {"version", 1},
            {"metadataOnly", true}, {"containsImagePayloads", false}, {"supportsReplay", false},
            {"frameId", capture.frameId}, {"shaderRevision", capture.shaderRevision},
            {"renderPath", capture.renderPath}, {"cpuSubmitMs", capture.cpuSubmitMs},
            {"resourceState", "Final scene-pass outputs, before UI composition"}};
        data["resources"] = nlohmann::json::array();
        for (const auto& resource : capture.resources)
            data["resources"].push_back({{"name", resource.name}, {"format", resource.format},
                {"width", resource.width}, {"height", resource.height}, {"bytes", resource.bytes}});
        data["events"] = nlohmann::json::array();
        for (const auto& event : capture.events)
            data["events"].push_back({{"pass", event.pass}, {"actorId", event.actorId}, {"meshId", event.meshId},
                {"materialId", event.materialId}, {"indexCount", event.indexCount}, {"vertexCount", event.vertexCount},
                {"shaderRevision", event.shaderRevision}});
        auto temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) { error = "Could not open frame metadata output."; return false; }
            stream << data.dump(2) << '\n';
            stream.close();
            if (!stream) { error = "Could not write complete frame metadata."; return false; }
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Could not finalize frame metadata output.";
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Frame metadata export failed: ") + exception.what(); return false;
    }
}
