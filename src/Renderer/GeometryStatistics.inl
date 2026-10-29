    // Included inside SceneRenderer::Impl. The lightweight topology/query
    // snapshot remains independent of optional Profiler and frame captures.
    SceneGeometryStatistics geometryStatistics;
    enum class GeometryView { Inactive, Raster, HardwareImage };
    GeometryView geometryView = GeometryView::Inactive;
    std::uint64_t geometrySampleEpoch = 0;
    bool geometryQueriesUnavailable = false;
    struct GeometryQuerySlot {
        ComPtr<ID3D11Query> query;
        std::uint64_t frameId = 0, epoch = 0;
        bool busy = false;
    };
    std::array<GeometryQuerySlot, 4> geometryQueries;
    GeometryQuerySlot* activeGeometryQuery = nullptr;

    void beginGeometryFrame(const scene::World& world, const assets::AssetLibrary& assets) {
        ++geometryStatistics.frameId;
        geometryStatistics.sceneTriangles = 0;
        for (const auto& actor : world.actors) {
            if (actor.light) continue;
            if (const auto* mesh = assets.findMesh(actor.mesh.meshId))
                geometryStatistics.sceneTriangles += static_cast<std::uint64_t>(mesh->indices.size() / 3);
        }
    }

    void endGeometrySample() {
        if (!activeGeometryQuery) return;
        context->End(activeGeometryQuery->query.Get());
        activeGeometryQuery = nullptr;
    }

    void discardGeometrySamples(GeometrySampleStatus status, const char* message) {
        endGeometrySample();
        ++geometrySampleEpoch;
        // Retire pending query objects instead of reusing an older generation.
        // D3D retains any command references until the GPU has finished them.
        for (auto& slot : geometryQueries) slot = {};
        geometryStatistics.gpuFrameId = 0;
        geometryStatistics.inputTriangles = geometryStatistics.postClipTriangles = 0;
        geometryStatistics.status = status;
        geometryStatistics.statusMessage = message;
    }

    void resetGeometryViewport() {
        geometryView = GeometryView::Inactive;
        discardGeometrySamples(GeometrySampleStatus::NoViewport, "No active scene viewport.");
    }

    void failGeometryFrame() {
        geometryView = GeometryView::Inactive;
        discardGeometrySamples(GeometrySampleStatus::Unavailable, "The main scene render did not complete.");
    }

    void pollGeometrySamples() {
        if (geometryQueriesUnavailable) return;
        for (auto& slot : geometryQueries) {
            if (!slot.busy) continue;
            D3D11_QUERY_DATA_PIPELINE_STATISTICS result{};
            const HRESULT ready = context->GetData(slot.query.Get(), &result, sizeof(result),
                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (ready == S_FALSE) continue;
            slot.busy = false;
            if (FAILED(ready)) {
                geometryQueriesUnavailable = true;
                discardGeometrySamples(GeometrySampleStatus::Unavailable, "GPU primitive statistics queries are unavailable.");
                return;
            }
            if (slot.epoch != geometrySampleEpoch || slot.frameId < geometryStatistics.gpuFrameId) continue;
            geometryStatistics.gpuFrameId = slot.frameId;
            geometryStatistics.inputTriangles = result.IAPrimitives;
            geometryStatistics.postClipTriangles = result.CPrimitives;
            geometryStatistics.status = GeometrySampleStatus::Ready;
            geometryStatistics.statusMessage = "Main raster geometry after GPU clipping; not depth/occlusion visibility.";
        }
    }

    void prepareGeometryStatistics(bool hardwareImage) {
        const auto next = hardwareImage ? GeometryView::HardwareImage : GeometryView::Raster;
        if (next != geometryView) {
            geometryView = next;
            discardGeometrySamples(hardwareImage ? GeometrySampleStatus::NotApplicable : GeometrySampleStatus::Pending,
                hardwareImage ? "The displayed hardware ray-traced image has no raster clipping count."
                              : "Waiting for an asynchronous main-view GPU sample.");
        }
        if (hardwareImage) return;
        if (geometryQueriesUnavailable) {
            geometryStatistics.status = GeometrySampleStatus::Unavailable;
            geometryStatistics.statusMessage = "GPU primitive statistics queries are unavailable.";
            return;
        }
        pollGeometrySamples();
    }

    void beginGeometrySample() {
        if (geometryView != GeometryView::Raster || geometryQueriesUnavailable) return;
        for (auto& slot : geometryQueries) {
            if (slot.busy) continue;
            if (!slot.query) {
                const D3D11_QUERY_DESC description{D3D11_QUERY_PIPELINE_STATISTICS, 0};
                if (FAILED(device->CreateQuery(&description, &slot.query))) {
                    geometryQueriesUnavailable = true;
                    discardGeometrySamples(GeometrySampleStatus::Unavailable, "GPU primitive statistics queries are unavailable.");
                    return;
                }
            }
            slot.busy = true;
            slot.frameId = geometryStatistics.frameId;
            slot.epoch = geometrySampleEpoch;
            activeGeometryQuery = &slot;
            context->Begin(slot.query.Get());
            return;
        }
        // A full ring skips this sample. Never flush, wait, or reuse a busy query.
    }
