void releaseWorldProbeScene() {
    worldProbeSurface.reset();
    worldProbeRadiance.reset();
    worldProbeVolumeBuffer.reset();
    sceneCommands().bindConstantBuffers(rhi::ShaderStage::Pixel, 10, std::span(&worldProbeVolumeBuffer, 1));
    worldProbeInputs = std::vector<RaySceneInput>{};
    worldProbeSurfaceBytes = std::vector<std::uint8_t>{};
    worldProbeMaterialOwners = std::vector<std::uint32_t>{};
    worldLightValid = false;
}

// World-space surface cache for software probe transport. Called on geometry/material
// changes only. Voxel coordinates and the slice layout are shared with WorldProbeGi.hlsli.
bool prepareWorldProbeScene(const scene::World& world, const assets::AssetLibrary& assets,
    const assets::MaterialAsset* preview, unsigned resolution, std::string& error) {
    try {
        auto keys = makeRaySceneInputs(world, assets, preview);
        if (worldProbeSurface && resolution == worldProbeResolution && keys == worldProbeInputs) return true;
        const auto encode = [](float value) {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        };
        const auto materialColor = [&](const RaySceneInput& input) {
            return std::array{encode(input.values[9] * (1.0f - input.values[13])),
                encode(input.values[10] * (1.0f - input.values[13])), encode(input.values[11] * (1.0f - input.values[13]))};
        };
        const auto sameGeometry = [](const RaySceneInput& a, const RaySceneInput& b) {
            return a.actor == b.actor && a.mesh == b.mesh && a.geometry == b.geometry && a.slot == b.slot &&
                a.twoSided == b.twoSided && a.cullFront == b.cullFront &&
                std::equal(a.values.begin(), a.values.begin() + 9, b.values.begin());
        };
        if (worldProbeSurface && resolution == worldProbeResolution &&
            std::equal(keys.begin(), keys.end(), worldProbeInputs.begin(), worldProbeInputs.end(), sameGeometry)) {
            // A material edit changes the occupied cells' reflectance without
            // touching triangle coverage, face planes or the volume layout.
            std::vector<std::array<std::uint8_t, 3>> colors;
            colors.reserve(keys.size());
            bool changed = false;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                colors.push_back(materialColor(keys[i]));
                changed |= colors.back() != materialColor(worldProbeInputs[i]);
            }
            if (changed) {
                auto surface = worldProbeSurfaceBytes;
                for (std::size_t cell = 0; cell < worldProbeMaterialOwners.size(); ++cell) {
                    const auto owner = worldProbeMaterialOwners[cell];
                    if (owner < colors.size())
                        std::copy(colors[owner].begin(), colors[owner].end(), surface.begin() + 8 * cell);
                }
                const auto& size = worldProbeSurface->description();
                worldProbeSurface = rhiDevice->uploadRgba8(size.width, size.height,
                    std::as_bytes(std::span(surface)), "World probe voxel surfaces");
                worldProbeSurfaceBytes = std::move(surface);
                worldLightValid = false;
            }
            worldProbeInputs = std::move(keys);
            return true;
        }
        rae::diagnostics::TraceScope trace("Voxelize world probe surfaces", "Renderer CPU");
        XMVECTOR lower = XMVectorReplicate(FLT_MAX), upper = XMVectorReplicate(-FLT_MAX);
        bool hasBounds = false;
        for (const auto& actor : world.actors) {
            if (!actor.visible || actor.camera || actor.light || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            if (!mesh || !mesh->localBounds().valid) continue;
            const auto& bounds = mesh->localBounds();
            const auto matrix = actorMatrix(actor);
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto p = XMVector3TransformCoord(XMVectorSet(
                    (corner & 1u) ? bounds.maximum[0] : bounds.minimum[0],
                    (corner & 2u) ? bounds.maximum[1] : bounds.minimum[1],
                    (corner & 4u) ? bounds.maximum[2] : bounds.minimum[2], 1), matrix);
                lower = XMVectorMin(lower, p);
                upper = XMVectorMax(upper, p);
            }
            hasBounds = true;
        }
        if (!hasBounds) lower = upper = XMVectorZero();
        XMFLOAT3 extent{};
        XMStoreFloat3(&extent, XMVectorSubtract(upper, lower));
        const float pitch = std::max(0.001f, std::max({extent.x, extent.y, extent.z}) / float(resolution - 3));
        if (!std::isfinite(pitch)) throw std::runtime_error("World probe scene has non-finite bounds.");
        const auto dimension = [&](float length) {
            return std::min(resolution, static_cast<unsigned>(std::ceil(length / pitch)) + 3);
        };
        const unsigned nx = dimension(extent.x), ny = dimension(extent.y), nz = dimension(extent.z);
        const unsigned atlasWidth = 2 * nx * nz;
        const XMVECTOR origin = XMVectorSubtract(lower, XMVectorReplicate(1.25f * pitch));
        WorldVolumeConstants volume{};
        XMStoreFloat4(&volume.originPitch, XMVectorSetW(origin, pitch));
        volume.dimensions = XMFLOAT4(float(nx), float(ny), float(nz), 0);
        std::vector<std::uint8_t> surface(static_cast<std::size_t>(atlasWidth) * ny * 4, 0);
        std::vector<float> distance(static_cast<std::size_t>(nx) * ny * nz, FLT_MAX);
        constexpr auto noOwner = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> owners(distance.size(), noOwner);
        std::map<std::pair<scene::ActorId, std::uint32_t>, std::uint32_t> keyIndices;
        for (std::size_t i = 0; i < keys.size(); ++i)
            keyIndices.emplace(std::make_pair(keys[i].actor, keys[i].slot), static_cast<std::uint32_t>(i));
        for (const auto& actor : world.actors) {
            if (!actor.visible || actor.camera || actor.light || !validTransform(actor)) continue;
            const auto* mesh = assets.findMesh(actor.mesh.meshId);
            if (!mesh || mesh->indices.empty()) continue;
            meshAttributes::validateSections(*mesh);
            std::vector<std::uint32_t> materialIndices(mesh->materialSlotCount(), noOwner);
            for (std::size_t slot = 0; slot < materialIndices.size(); ++slot) {
                const auto found = keyIndices.find({actor.id, static_cast<std::uint32_t>(slot)});
                if (found != keyIndices.end()) materialIndices[slot] = found->second;
            }
            const auto localToGrid = actorMatrix(actor) * XMMatrixTranslationFromVector(XMVectorNegate(origin)) *
                XMMatrixScaling(1 / pitch, 1 / pitch, 1 / pitch);
            std::vector<XMFLOAT3> positions(mesh->vertices.size());
            for (std::size_t i = 0; i < positions.size(); ++i) {
                const auto* v = mesh->vertices[i].position;
                const auto p = XMVector3TransformCoord(XMVectorSet(v[0], v[1], v[2], 1), localToGrid);
                if (XMVector3IsNaN(p) || XMVector3IsInfinite(p))
                    throw std::runtime_error("Non-finite vertex in world probe mesh: " + mesh->name);
                XMStoreFloat3(&positions[i], p);
            }
            if (mesh->indices.size() % 3 != 0)
                throw std::runtime_error("Non-triangular world probe mesh: " + mesh->name);
            const bool mirrored = mirroredTransform(actor);
            std::size_t section = 0;
            for (std::size_t i = 0; i < mesh->indices.size(); i += 3) {
                if (section + 1 < mesh->sections.size() && mesh->sections[section + 1].firstIndex == i) ++section;
                const auto owner = materialIndices[mesh->sections.empty() ? 0u : mesh->sections[section].materialSlot];
                if (owner == noOwner) continue;
                const auto& material = keys[owner];
                const auto a = XMLoadFloat3(&positions.at(mesh->indices[i]));
                const auto b = XMLoadFloat3(&positions.at(mesh->indices[i + 1]));
                const auto c = XMLoadFloat3(&positions.at(mesh->indices[i + 2]));
                const auto cross = XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a));
                if (XMVectorGetX(XMVector3LengthSq(cross)) < 1e-16f) continue;
                auto normal = XMVector3Normalize(cross);
                if (mirrored != material.cullFront) normal = XMVectorNegate(normal);
                worldVoxels::visitTriangle(a, b, c, normal, {int(nx), int(ny), int(nz)}, [&](int x, int y, int z) {
                    const auto cell = static_cast<std::size_t>(x + nx * (z + nz * y));
                    const auto center = XMVectorSet(float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f, 0.0f);
                    const float offset = XMVectorGetX(XMVector3Dot(normal, XMVectorSubtract(a, center)));
                    if (std::abs(offset) >= distance[cell]) return;
                    distance[cell] = std::abs(offset);
                    const auto texel = 8 * cell;
                    owners[cell] = owner;
                    surface[texel + 3] = material.twoSided ? 255 : 128;
                    XMFLOAT3 n{};
                    XMStoreFloat3(&n, normal);
                    surface[texel + 4] = encode(n.x * 0.5f + 0.5f);
                    surface[texel + 5] = encode(n.y * 0.5f + 0.5f);
                    surface[texel + 6] = encode(n.z * 0.5f + 0.5f);
                    surface[texel + 7] = encode(offset * 0.5f + 0.5f);
                });
            }
        }
        for (std::size_t cell = 0; cell < owners.size(); ++cell) {
            if (owners[cell] == noOwner) continue;
            const auto color = materialColor(keys[owners[cell]]);
            std::copy(color.begin(), color.end(), surface.begin() + 8 * cell);
        }
        auto uploaded = rhiDevice->uploadRgba8(atlasWidth, ny, std::as_bytes(std::span(surface)), "World probe voxel surfaces");
        auto radiance = rhiDevice->createTexture({atlasWidth, ny, 1, rhi::Format::RGBA16Float, true, "World probe surface radiance"});
        if (!worldProbeVolumeBuffer)
            worldProbeVolumeBuffer = rhiDevice->createBuffer({sizeof(volume), rhi::BufferUsage::Constant, false, "World probe volume"});
        worldProbeSurface = std::move(uploaded);
        worldProbeRadiance = std::move(radiance);
        worldProbeVolume = volume;
        worldProbeInputs = std::move(keys);
        worldProbeResolution = resolution;
        worldProbeSurfaceBytes = std::move(surface);
        worldProbeMaterialOwners = std::move(owners);
        worldLightValid = false;
        return true;
    } catch (const std::exception& failure) {
        error = std::string("World probe voxelization: ") + failure.what();
        return false;
    }
}
