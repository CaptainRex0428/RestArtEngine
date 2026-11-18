// CPU-side conservative bounds rejection, shared by scene and shadow draws.
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
