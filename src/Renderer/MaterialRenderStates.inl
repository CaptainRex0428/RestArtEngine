    // Included inside the current DX11 renderer implementation. Asset enums
    // remain API-independent; only this binding layer knows native states.
    std::array<ComPtr<ID3D11RasterizerState>, 6> materialRasterizers;
    std::array<ComPtr<ID3D11DepthStencilState>, 18> materialDepthStates;

    bool createMaterialRenderStates(std::string& error) {
        for (unsigned index = 0; index < materialRasterizers.size(); ++index) {
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.DepthClipEnable = TRUE;
            const auto mode = static_cast<assets::MaterialCullMode>(index / 2);
            raster.CullMode = mode == assets::MaterialCullMode::Off ? D3D11_CULL_NONE :
                mode == assets::MaterialCullMode::Front ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
            raster.FrontCounterClockwise = (index & 1u) ? FALSE : TRUE;
            if (failed(device->CreateRasterizerState(&raster, &materialRasterizers[index]),
                       "Create material culling state", error)) return false;
        }
        constexpr D3D11_COMPARISON_FUNC functions[]{D3D11_COMPARISON_NEVER,
            D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
            D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL,
            D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_ALWAYS};
        for (unsigned index = 0; index < materialDepthStates.size(); ++index) {
            D3D11_DEPTH_STENCIL_DESC depthState{};
            const unsigned comparison = index / 2;
            depthState.DepthEnable = comparison != static_cast<unsigned>(assets::MaterialDepthTest::Disabled);
            depthState.DepthWriteMask = depthState.DepthEnable && (index & 1u)
                ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
            depthState.DepthFunc = functions[comparison];
            if (failed(device->CreateDepthStencilState(&depthState, &materialDepthStates[index]),
                       "Create material depth state", error)) return false;
        }
        return true;
    }

    void bindMaterialRenderState(const assets::MaterialAsset& material, bool mirrored, bool shadowPass) {
        const auto rasterIndex = 2u * static_cast<unsigned>(material.effectiveCullMode()) + (mirrored ? 1u : 0u);
        context->RSSetState(materialRasterizers[rasterIndex].Get());
        if (shadowPass) {
            // A shadow map stores nearest light-space depth regardless of the
            // material's camera-facing overlay/depth comparison policy.
            context->OMSetDepthStencilState(depthEnabled.Get(), 0);
        } else {
            auto comparison = static_cast<unsigned>(material.depthTest);
            if (comparison >= 9u) comparison = static_cast<unsigned>(assets::MaterialDepthTest::Less);
            const auto depthIndex = comparison * 2u + (material.depthWrite ? 1u : 0u);
            context->OMSetDepthStencilState(materialDepthStates[depthIndex].Get(), 0);
        }
    }

    static bool intersectsView(const scene::Actor& actor, const assets::MeshAsset& mesh,
                               const XMFLOAT4X4& viewProjection) {
        const auto& bounds = mesh.localBounds();
        if (!bounds.valid) return true; // Unknown bounds must never hide geometry.
        const XMMATRIX transform = actorMatrix(actor) * XMLoadFloat4x4(&viewProjection);
        unsigned outsideAll = 0x3fu;
        for (unsigned corner = 0; corner < 8; ++corner) {
            const XMVECTOR point = XMVectorSet(
                (corner & 1u) ? bounds.maximum[0] : bounds.minimum[0],
                (corner & 2u) ? bounds.maximum[1] : bounds.minimum[1],
                (corner & 4u) ? bounds.maximum[2] : bounds.minimum[2], 1.0f);
            XMFLOAT4 clip;
            XMStoreFloat4(&clip, XMVector4Transform(point, transform));
            if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
                !std::isfinite(clip.z) || !std::isfinite(clip.w)) return true;
            // Homogeneous D3D clip planes; no divide by w, including corners
            // behind the eye. A small tolerance keeps boundary tests conservative.
            const float margin = 1e-5f * std::max(1.0f, std::abs(clip.w));
            unsigned outside = 0;
            if (clip.x < -clip.w - margin) outside |= 1u;
            if (clip.x >  clip.w + margin) outside |= 2u;
            if (clip.y < -clip.w - margin) outside |= 4u;
            if (clip.y >  clip.w + margin) outside |= 8u;
            if (clip.z < -margin) outside |= 16u;
            if (clip.z > clip.w + margin) outside |= 32u;
            outsideAll &= outside;
            if (!outsideAll) return true;
        }
        return outsideAll == 0;
    }
