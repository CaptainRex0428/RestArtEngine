// Included inside SceneRenderer::Impl. Temporary source color is private to the
// pass: only final scene/preview outputs are borrowed by UI command lists.
ComPtr<ID3D11Texture2D> dofSource;
ComPtr<ID3D11ShaderResourceView> dofSourceView;

bool renderDepthOfField(ID3D11RenderTargetView* target, const D3D11_VIEWPORT& viewport,
                       const camera::Lens& lens, std::string& error) {
    ComPtr<ID3D11Resource> destination;
    target->GetResource(&destination);
    ComPtr<ID3D11Texture2D> destinationTexture;
    if (failed(destination.As(&destinationTexture),"Read DOF target texture",error)) return false;
    D3D11_TEXTURE2D_DESC description{};
    destinationTexture->GetDesc(&description);
    D3D11_TEXTURE2D_DESC previous{};
    if (dofSource) dofSource->GetDesc(&previous);
    if (!dofSource || previous.Width != description.Width || previous.Height != description.Height
        || previous.Format != description.Format) {
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.CPUAccessFlags = description.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> resource;
        if (failed(device->CreateTexture2D(&description,nullptr,&texture),"Create DOF source",error)
            || failed(device->CreateShaderResourceView(texture.Get(),nullptr,&resource),"Create DOF source SRV",error)) return false;
        dofSource = std::move(texture); dofSourceView = std::move(resource);
    }
    context->OMSetRenderTargets(0,nullptr,nullptr);
    context->CopyResource(dofSource.Get(),destination.Get());
    const auto& dof = lens.depthOfField;
    const float sensorHeight = dof.sensorHeightMm*.001f;
    const float focalLength = sensorHeight/(2*std::tan(lens.verticalFovDegrees*.008726646259971648f));
    const float scale = .5f*focalLength*focalLength*viewport.Height /
        (dof.fStop*(dof.focusDistance-focalLength)*sensorHeight);
    DepthOfFieldConstants data{
        XMFLOAT4(viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height),
        XMFLOAT4(dof.focusDistance,scale,24.0f,0)};
    context->UpdateSubresource(dofConstants.Get(),0,nullptr,&data,0,0);
    ID3D11Buffer* buffer = dofConstants.Get();
    context->PSSetConstantBuffers(8,1,&buffer);
    ID3D11ShaderResourceView* color = dofSourceView.Get();
    ID3D11ShaderResourceView* depthResource = depthView.Get();
    context->PSSetShaderResources(12,1,&color);
    context->PSSetShaderResources(6,1,&depthResource);
    context->OMSetRenderTargets(1,&target,nullptr);
    context->RSSetViewports(1,&viewport);
    context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    drawFullscreen(dofPS.Get());
    ID3D11ShaderResourceView* empty = nullptr;
    context->PSSetShaderResources(12,1,&empty);
    context->PSSetShaderResources(6,1,&empty);
    return true;
}
