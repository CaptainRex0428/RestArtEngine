// Included in SceneRenderer.cpp after the private Impl definition.
void SceneRenderer::setDiagnosticsEnabled(bool enabled) { impl_->diagnosticsOn = enabled; }
bool SceneRenderer::diagnosticsEnabled() const { return impl_->diagnosticsOn; }
const RenderDiagnosticsFrame& SceneRenderer::diagnosticsFrame() const { return impl_->diagnosticFrame; }
void SceneRenderer::requestFrameCapture() {
    if (!impl_->rhiDevice) {
        impl_->captureRequested = false;
        impl_->capture = {};
        impl_->retireCapturedTextures();
        impl_->captureBytes = 0;
        impl_->capture.status = "Frame Inspector resource capture requires an initialized rendering device.";
        return;
    }
    impl_->captureRequested = true;
    impl_->capture.status = "Capture pending: next main scene frame.";
}
void SceneRenderer::setFrameCaptureOptions(const FrameCaptureOptions& options) {
    impl_->captureOptions = options;
    impl_->captureOptions.budgetBytes = std::clamp<std::uint64_t>(options.budgetBytes, 1, Impl::captureBudget);
}
const FrameCaptureOptions& SceneRenderer::frameCaptureOptions() const { return impl_->captureOptions; }
const RenderFrameCapture& SceneRenderer::frameCapture() const { return impl_->capture; }
void SceneRenderer::clearFrameCapture() {
    impl_->captureRequested = false;
    impl_->capture = {};
    impl_->retireCapturedTextures();
    impl_->captureBytes = 0;
    impl_->pendingCapture = {};
    impl_->pendingTextures.clear();
    impl_->pendingCaptureError.clear();
    impl_->pendingCaptureBytes = 0;
}

void* SceneRenderer::capturedTexture(std::size_t resourceIndex, DebugTextureChannel channel) {
    auto& state = *impl_;
    if (!state.rhiDevice) return nullptr;
    const auto channelIndex = static_cast<std::size_t>(channel);
    if (!state.capture.valid || resourceIndex >= state.capturedTextures.size() || channelIndex >= 5) return nullptr;
    auto& resource = state.capturedTextures[resourceIndex];
    if (state.native12 || !resource.cpuBytes.empty() || resource.kind == 4)
        return state.capturedCpuDisplay(resourceIndex, channelIndex);
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
    if (!state.rhiDevice) {
        error = "Frame Inspector pixel readback requires an initialized rendering device.";
        return false;
    }
    if (!state.capture.valid || resourceIndex >= state.capturedTextures.size()) {
        error = "No captured resource selected."; return false;
    }
    const auto& source = state.capturedTextures[resourceIndex];
    // D3D11 depth-stencil resources require whole-subresource copies; a 1x1
    // source box would be invalid even for a typeless frozen depth texture.
    if (state.native12 || !source.cpuBytes.empty() || source.kind >= 4) {
        const auto& metadata = state.capture.resources[resourceIndex];
        if (x >= metadata.width || y >= metadata.height) { error = "Pixel coordinates are outside the captured texture."; return false; }
        rae::diagnostics::TraceScope trace("Captured pixel synchronous readback", "GPU readback wait");
        std::vector<std::uint8_t> raw;
        if (!state.readCaptureBytes(resourceIndex, raw, error)) return false;
        const auto stride = metadata.format == "RGBA16F" ? 8u : 4u;
        const auto pixel = Impl::decodeCapturePixel(raw.data() + (std::size_t(y) * metadata.width + x) * stride, metadata.format);
        std::copy(pixel.begin(), pixel.end(), std::begin(value));
        return true;
    }
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

bool SceneRenderer::readCapturedDisplayPixel(std::size_t index, DebugTextureChannel channel,
    std::uint32_t x, std::uint32_t y, float (&value)[4], std::string& error) {
    error.clear();
    const auto component = static_cast<std::size_t>(channel);
    if (!impl_->capture.valid || index >= impl_->capture.resources.size() || component >= 5) {
        error = "No captured display selected."; return false;
    }
    const auto& metadata = impl_->capture.resources[index];
    if (x >= metadata.width || y >= metadata.height) { error = "Pixel coordinates are outside the captured display."; return false; }
    if (!capturedTexture(index, channel)) { error = impl_->capture.status; return false; }
    rae::diagnostics::TraceScope trace("Captured display synchronous readback", "GPU readback wait");
    try {
        const auto& display = impl_->capturedTextures[index].displays[component];
        std::array<std::uint8_t, 4> bytes{};
        if (impl_->native12) {
            const auto raw = impl_->native12->readTexture(display.image);
            std::memcpy(bytes.data(), raw.bytes.data() + std::size_t(y) * raw.rowPitch + std::size_t(x) * 4, 4);
        } else {
            D3D11_TEXTURE2D_DESC desc{}; display.texture->GetDesc(&desc);
            desc.Width = desc.Height = 1; desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (failed(impl_->device->CreateTexture2D(&desc, nullptr, &staging), "Create display pixel readback", error)) return false;
            const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
            impl_->context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, display.texture.Get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (failed(impl_->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read captured display pixel", error)) return false;
            std::memcpy(bytes.data(), mapped.pData, 4); impl_->context->Unmap(staging.Get(), 0);
        }
        for (std::size_t c = 0; c < 4; ++c) value[c] = bytes[c] / 255.f;
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}

bool SceneRenderer::exportFrameCapture(const std::filesystem::path& path, std::string& error) const {
    error.clear();
    const auto& capture = impl_->capture;
    if (!capture.valid) { error = "No frame captured."; return false; }
    rae::diagnostics::TraceScope trace("Export frame metadata", "File IO");
    try {
        auto data = nlohmann::json::parse(encodeCaptureMetadata(capture));
        data["format"] = "RestArtEngine.FrameInspector.Metadata";
        data["metadataOnly"] = true;
        data["metadataOnlyExport"] = true;
        data["containsImagePayloads"] = false;
        data["resourceState"] = "Each resource identifies its own captured pass, boundary and layer; no replay";
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

bool SceneRenderer::saveFrameCapture(const std::filesystem::path& path, std::string& error) const {
    error.clear();
    if (!impl_->capture.valid) { error = "No frame captured."; return false; }
    rae::diagnostics::TraceScope trace("Save offline frame capture", "GPU readback / File IO");
    try {
        captureArchive::Archive archive;
        archive.metadataJson = encodeCaptureMetadata(impl_->capture);
        for (std::size_t i = 0; i < impl_->capture.resources.size(); ++i) {
            archive.payloads.emplace_back();
            if (!impl_->readCaptureBytes(i, archive.payloads.back(), error)) return false;
        }
        return captureArchive::save(path, archive, error);
    } catch (const std::exception& e) { error = std::string("Frame capture export failed: ") + e.what(); return false; }
}

bool SceneRenderer::loadFrameCapture(const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (!impl_->rhiDevice) { error = "Open a rendering session before inspecting an offline capture."; return false; }
    rae::diagnostics::TraceScope trace("Load offline frame capture", "File IO");
    try {
        captureArchive::Archive archive;
        RenderFrameCapture candidate;
        if (!captureArchive::load(path, archive, error) || !decodeCaptureMetadata(archive.metadataJson, candidate, error)) return false;
        if (candidate.resources.size() != archive.payloads.size()) { error = "Capture resource/payload count mismatch."; return false; }
        std::vector<Impl::CapturedTexture> textures;
        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < candidate.resources.size(); ++i) {
            const auto& resource = candidate.resources[i];
            if (resource.bytes != archive.payloads[i].size()) { error = "Capture payload size does not match its resource description."; return false; }
            Impl::CapturedTexture image;
            image.cpuBytes = std::move(archive.payloads[i]); image.kind = resource.displayKind;
            textures.push_back(std::move(image)); bytes += resource.bytes;
        }
        candidate.valid = true; candidate.offline = true;
        candidate.status = "Offline snapshot loaded. Resources, recorded bindings/constants and CPU scopes are frozen; no GPU replay.";
        impl_->retireCapturedTextures();
        impl_->capturedTextures = std::move(textures); impl_->capture = std::move(candidate);
        impl_->captureBytes = bytes; impl_->captureRequested = false;
        return true;
    } catch (const std::exception& e) { error = std::string("Frame capture load failed: ") + e.what(); return false; }
}

std::vector<std::string> SceneRenderer::debugMessages() const {
    if (impl_->native12) return impl_->native12->debugMessages();
    std::vector<std::string> result;
    if (!impl_->infoQueue) {
        if (impl_->hardwareRayTracing) {
            const auto hardware = impl_->hardwareRayTracing->debugMessages();
            result.insert(result.end(), hardware.begin(), hardware.end());
        }
        return result;
    }
    const auto total = impl_->infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    const auto first = total > 128 ? total - 128 : 0;
    for (auto i = first; i < total; ++i) {
        SIZE_T size = 0;
        if (FAILED(impl_->infoQueue->GetMessage(i, nullptr, &size)) || size > 1024 * 1024) continue;
        std::vector<std::uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (SUCCEEDED(impl_->infoQueue->GetMessage(i, message, &size))) {
            const char* severity = message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ? "Corruption"
                : message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ? "Error" : message->Severity == D3D11_MESSAGE_SEVERITY_WARNING ? "Warning" : "Info";
            result.push_back(std::string(severity) + " [" + std::to_string(message->ID) + "] " + std::string(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0));
        }
    }
    if (impl_->hardwareRayTracing) {
        const auto hardware = impl_->hardwareRayTracing->debugMessages();
        result.insert(result.end(), hardware.begin(), hardware.end());
    }
    return result;
}
