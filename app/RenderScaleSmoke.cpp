#include "RenderScaleSmoke.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <RestArtEngine/Assets/AssetLibrary.h>
#include <RestArtEngine/Core/TraceProfiler.h>
#include <RestArtEngine/Renderer/SceneRenderer.h>
#include <RestArtEngine/Scene/World.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using Pixels = std::vector<std::uint8_t>;
constexpr std::uint32_t width = 1920, height = 1080;
constexpr std::uint32_t cellsX = 2500, cellsZ = 2000;
constexpr std::uint64_t terrainTriangles = std::uint64_t{cellsX} * cellsZ * 2;
constexpr unsigned warmupFrames = 8, measurementFrames = 24;
constexpr rae::assets::AssetId terrainMeshId = 1001, floorMaterialId = 2001,
    redMaterialId = 2002, blueMaterialId = 2003;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("Render scale: " + message);
}
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    require(stream.good(), "Cannot write report: " + path.string());
}

Json videoMemory(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter.As(&adapter3)))
        return {{"available", false}, {"reason", "DXGI adapter memory queries are unavailable."}};
    Json result{{"available", true},
        {"scope", "DXGI per-process usage/budget; not resource allocation size or physical GPU residency"}};
    for (const auto segment : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        const auto queried = adapter3->QueryVideoMemoryInfo(0, segment, &memory);
        const char* name = segment == DXGI_MEMORY_SEGMENT_GROUP_LOCAL ? "local" : "nonlocal";
        if (FAILED(queried)) result[name] = {{"available", false}, {"hresult", static_cast<std::uint32_t>(queried)}};
        else result[name] = {{"available", true}, {"currentUsageBytes", memory.CurrentUsage},
            {"budgetBytes", memory.Budget}, {"currentReservationBytes", memory.CurrentReservation},
            {"availableForReservationBytes", memory.AvailableForReservation}};
    }
    return result;
}

// One EVENT after the frame waits for raster work AND the DX11 queue's wait on
// DXR. The measured interval includes CPU preparation and GPU completion. It is
// intentionally labelled synchronized wall time, not isolated GPU execution.
void completeFrame(rae::renderer::SceneRenderer& renderer, ID3D11Query* completion) {
    auto* context = renderer.context();
    context->End(completion);
    context->Flush();
    const auto started = Clock::now();
    for (;;) {
        BOOL completed = FALSE;
        const auto result = context->GetData(completion, &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        require(SUCCEEDED(result), "Frame completion query failed (HRESULT " + std::to_string(result) + ").");
        if (result == S_OK && completed) return;
        require(Clock::now() - started < std::chrono::seconds(120), "GPU frame completion timed out.");
        std::this_thread::yield();
    }
}

Pixels readScene(rae::renderer::SceneRenderer& renderer) {
    auto* view = static_cast<ID3D11ShaderResourceView*>(renderer.sceneTexture());
    require(view != nullptr, "The scene texture is unavailable.");
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture, staging;
    require(SUCCEEDED(resource.As(&texture)), "The scene resource is not a texture.");
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    require(description.Width == width && description.Height == height &&
        description.Format == DXGI_FORMAT_R8G8B8A8_UNORM, "The benchmark did not render a 1920x1080 RGBA8 scene.");
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = description.MiscFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&description, nullptr, &staging)), "Cannot create the image readback.");
    renderer.context()->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(renderer.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Cannot read the scene image.");
    Pixels pixels(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t row = 0; row < height; ++row)
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * width * 4,
            static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
            static_cast<std::size_t>(width) * 4);
    renderer.context()->Unmap(staging.Get(), 0);
    return pixels;
}

void saveImage(const std::filesystem::path& path, Pixels pixels) {
    require(pixels.size() == static_cast<std::size_t>(width) * height * 4, "Invalid image size.");
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) std::swap(pixels[offset], pixels[offset + 2]);
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height);
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixels.size());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    stream.close();
    require(stream.good(), "Cannot write the benchmark image.");
}

rae::assets::MeshAsset makeTerrain(Json& report) {
    rae::assets::MeshAsset mesh;
    mesh.id = terrainMeshId;
    mesh.name = "10M unique triangle terrain";
    mesh.primitive = rae::assets::MeshPrimitive::Imported;
    mesh.vertices.resize(static_cast<std::size_t>(cellsX + 1) * (cellsZ + 1));
    mesh.indices.resize(static_cast<std::size_t>(terrainTriangles) * 3);
    float minimumDx = std::numeric_limits<float>::max(), minimumDz = minimumDx;
    for (std::uint32_t z = 0; z <= cellsZ; ++z) {
        const float pz = -4.0f + 8.0f * static_cast<float>(z) / static_cast<float>(cellsZ);
        if (z != 0) {
            const float previousZ = mesh.vertices[static_cast<std::size_t>(z - 1) * (cellsX + 1)].position[2];
            minimumDz = std::min(minimumDz, pz - previousZ);
        }
        for (std::uint32_t x = 0; x <= cellsX; ++x) {
            const float px = -5.0f + 10.0f * static_cast<float>(x) / static_cast<float>(cellsX);
            const float sx = std::sin(px * .65f), cx = std::cos(px * .65f);
            const float sz = std::sin(pz * .75f), cz = std::cos(pz * .75f);
            const float nx = -.052f * cx * cz, nz = .06f * sx * sz;
            const float inverseLength = 1.0f / std::sqrt(nx * nx + 1.0f + nz * nz);
            const auto offset = static_cast<std::size_t>(z) * (cellsX + 1) + x;
            mesh.vertices[offset] = {{px, .08f * sx * cz, pz},
                {nx * inverseLength, inverseLength, nz * inverseLength}};
            if (x != 0) minimumDx = std::min(minimumDx, px - mesh.vertices[offset - 1].position[0]);
        }
    }
    // Each rectangular XZ cell owns two different upward-facing triangles.
    // Strictly increasing stored-float X/Z coordinates prove a positive XZ
    // projected area for every triangle, regardless of the small height wave.
    require(minimumDx > 0 && minimumDz > 0, "The generated grid has degenerate float coordinates.");
    for (std::uint32_t z = 0; z < cellsZ; ++z)
        for (std::uint32_t x = 0; x < cellsX; ++x) {
            const auto a = z * (cellsX + 1) + x;
            const auto b = a + 1, c = a + cellsX + 1, d = c + 1;
            const auto offset = (static_cast<std::size_t>(z) * cellsX + x) * 6;
            mesh.indices[offset] = a; mesh.indices[offset + 1] = c; mesh.indices[offset + 2] = b;
            mesh.indices[offset + 3] = b; mesh.indices[offset + 4] = c; mesh.indices[offset + 5] = d;
        }
    report["terrain"] = {{"cellsX", cellsX}, {"cellsZ", cellsZ},
        {"vertices", mesh.vertices.size()}, {"uniqueNondegenerateTriangles", mesh.indices.size() / 3},
        {"sourceVertexBytes", mesh.vertices.size() * sizeof(rae::assets::MeshVertex)},
        {"sourceIndexBytes", mesh.indices.size() * sizeof(std::uint32_t)},
        {"minimumProjectedTriangleDoubleArea", static_cast<double>(minimumDx) * minimumDz},
        {"construction", "One indexed height field; no repeated triangles or instancing used to reach 10M."}};
    return mesh;
}

Json verifySceneBoundsInFrustum(const rae::renderer::SceneSettings& settings) {
    using namespace DirectX;
    const auto target = XMVectorSet(settings.cameraTarget[0], settings.cameraTarget[1], settings.cameraTarget[2], 1);
    const auto eye = XMVectorAdd(target, XMVectorSet(
        settings.cameraDistance * std::cos(settings.cameraPitch) * std::sin(settings.cameraYaw),
        settings.cameraDistance * std::sin(settings.cameraPitch),
        settings.cameraDistance * std::cos(settings.cameraPitch) * std::cos(settings.cameraYaw), 0));
    const auto view = XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0));
    const auto projection = XMMatrixPerspectiveFovRH(XMConvertToRadians(50.0f),
        static_cast<float>(width) / height, .05f, 100.0f);
    float maxAbsX = 0, maxAbsY = 0, minimumZ = 1, maximumZ = 0;
    // This box encloses the terrain AND the two supporting cubes. A convex
    // frustum containing all eight corners contains every generated vertex.
    for (const float x : {-5.0f, 5.0f}) for (const float y : {-.08f, 1.8f}) for (const float z : {-4.0f, 4.0f}) {
        XMFLOAT4 clip;
        XMStoreFloat4(&clip, XMVector4Transform(XMVectorSet(x, y, z, 1), view * projection));
        require(clip.w > 0, "Generated geometry is behind the camera.");
        maxAbsX = std::max(maxAbsX, std::abs(clip.x / clip.w));
        maxAbsY = std::max(maxAbsY, std::abs(clip.y / clip.w));
        minimumZ = std::min(minimumZ, clip.z / clip.w);
        maximumZ = std::max(maximumZ, clip.z / clip.w);
    }
    require(maxAbsX < 1 && maxAbsY < 1 && minimumZ > 0 && maximumZ < 1,
        "The benchmark scene is not entirely inside the camera frustum.");
    return {{"allSceneBoundsInside", true}, {"maxAbsNdcX", maxAbsX}, {"maxAbsNdcY", maxAbsY},
        {"minNdcDepth", minimumZ}, {"maxNdcDepth", maximumZ},
        {"interpretation", "Frustum inclusion, not final depth/occlusion-visible triangle count."}};
}

std::uint64_t rayTriangleCount(const std::string& status) {
    constexpr const char* prefix = "active: ";
    const auto start = status.find(prefix);
    require(start != std::string::npos, "No active DXR scene count: " + status);
    const char* first = status.data() + start + std::strlen(prefix);
    const char* last = status.data() + status.size();
    std::uint64_t count = 0;
    const auto result = std::from_chars(first, last, count);
    require(result.ec == std::errc{} && std::string_view(result.ptr, static_cast<std::size_t>(last - result.ptr)).starts_with(" triangles."),
        "Cannot read the DXR scene triangle count: " + status);
    return count;
}

Json timingSummary(const std::vector<double>& times) {
    require(!times.empty(), "No timing samples.");
    auto sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) {
        return sorted[static_cast<std::size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1];
    };
    return {{"samples", times.size()}, {"meanMs", std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size())},
        {"minimumMs", sorted.front()}, {"medianMs", percentile(.5)}, {"p95Ms", percentile(.95)},
        {"maximumMs", sorted.back()}, {"frameMs", times}};
}
} // namespace

void runRenderScaleSmoke(rae::renderer::SceneRenderer& renderer, const std::filesystem::path& outputDirectory) {
    using namespace rae;
    using Method = renderer::GlobalIlluminationMethod;
    using Mode = renderer::GlobalIlluminationMode;
    const auto output = std::filesystem::absolute(outputDirectory);
    std::filesystem::create_directories(output);
    auto& profiler = diagnostics::profiler();
    const bool wasRecording = profiler.isRecording();
    const bool oldDiagnostics = renderer.diagnosticsEnabled();
    const auto oldWidth = renderer.sceneTextureWidth(), oldHeight = renderer.sceneTextureHeight();
    struct Restore {
        renderer::SceneRenderer& renderer;
        std::uint32_t width, height;
        bool diagnostics, recording, done = false;
        void restore() {
            if (!recording) rae::diagnostics::profiler().stop();
            renderer.setDiagnosticsEnabled(diagnostics);
            std::string error;
            require(renderer.resize(width, height, error), "Cannot restore render dimensions: " + error);
            renderer.resetGlobalIllumination();
            done = true;
        }
        ~Restore() { if (!done) try { restore(); } catch (...) {} }
    } restore{renderer, oldWidth, oldHeight, oldDiagnostics, wasRecording};
    Json report{{"format", "RestArtEngine.RenderScaleSmoke"}, {"version", 1}, {"passed", false},
        {"adapter", renderer.adapterName()}, {"width", width}, {"height", height},
        {"build", {
#ifdef _DEBUG
            {"configuration", "Debug"},
#else
            {"configuration", "Release or non-Debug"},
#endif
            {"graphicsValidationEnabled", renderer.validationEnabled()}}},
        {"fpsThreshold", nullptr}, {"shaderRevision", renderer.shaderRevision()},
        {"scope", "Generated in-memory scene; no project asset saves or edits."}};
    const auto saveReport = [&] { write(output / "render-scale-report.json", report.dump(2) + '\n'); };
    try {
        std::string error;
        renderer.setDiagnosticsEnabled(true);
        if (!wasRecording) profiler.start();
        ComPtr<ID3D11Query> completion;
        const D3D11_QUERY_DESC completionDescription{D3D11_QUERY_EVENT, 0};
        require(SUCCEEDED(renderer.device()->CreateQuery(&completionDescription, &completion)), "Cannot create a frame completion query.");
        assets::AssetLibrary assets;
        assets.createDefaults();
        assets.materials.clear();
        const auto addMaterial = [&](assets::AssetId id, const char* name, const std::array<float, 3>& colour, float roughness) {
            assets::MaterialAsset material;
            material.id = id; material.name = name;
            std::copy(colour.begin(), colour.end(), material.baseColor);
            material.metallic = 0; material.roughness = roughness; material.grid = false;
            material.twoSided = false;
            assets.materials.push_back(material);
        };
        addMaterial(floorMaterialId, "Neutral terrain", {.55f, .55f, .55f}, .65f);
        addMaterial(redMaterialId, "Red paint", {.75f, .04f, .04f}, .4f);
        addMaterial(blueMaterialId, "Blue paint", {.04f, .10f, .75f}, .4f);
        scene::World world;
        world.blackBackground = true;
        world.createActor("Hardware capability probe", 3, floorMaterialId);
        renderer::SceneSettings settings;
        settings.path = renderer::RenderPath::Deferred;
        settings.editorViewport = true;
        settings.viewportWidth = 320; settings.viewportHeight = 180;
        settings.cameraYaw = .35f; settings.cameraPitch = 1.15f; settings.cameraDistance = 18;
        settings.cameraTarget[1] = .4f;
        settings.hardwareRayTracing = true;
        settings.hardwareRayTracingReflections = settings.hardwareRayTracingShadows = true;
        settings.rayTracing = false;
        settings.shadowMethod = renderer::ShadowMethod::ShadowMap;
        settings.globalIllumination.method = Method::PathTracing;
        settings.globalIllumination.mode = Mode::Realtime;
        settings.globalIllumination.indirectDiffuse = settings.globalIllumination.indirectSpecular = true;
        settings.globalIllumination.samplesPerFrame = 1;
        settings.globalIllumination.targetSamples = 32;
        settings.globalIllumination.maxBounces = 3;
        report["settings"] = {{"path", "Deferred"}, {"hardwareRayTracing", true}, {"softwareRayTracing", false},
            {"shadowMethod", "ShadowMap"}, {"giMethod", "PathTracing"}, {"giMode", "Realtime"},
            {"indirectDiffuse", true}, {"indirectSpecular", true}, {"samplesPerFrame", 1},
            {"targetSamples", 32}, {"maxBounces", 3}};
        report["camera"] = {{"yaw", settings.cameraYaw}, {"pitch", settings.cameraPitch},
            {"distance", settings.cameraDistance}, {"target", {0, .4, 0}}};
        report["frustum"] = verifySceneBoundsInFrustum(settings);

        const auto renderFrame = [&] {
            const auto started = Clock::now();
            diagnostics::beginFrame();
            struct EndFrame { ~EndFrame() { diagnostics::endFrame(); } } endFrame;
            require(renderer.render(settings, world, assets, error), error);
            completeFrame(renderer, completion.Get());
            return milliseconds(started);
        };
        require(renderer.resize(320, 180, error), error);
        report["capabilityProbeMs"] = renderFrame();
        require(renderer.hardwareRayTracingSupported() && renderer.hardwareRayTracingActive() &&
            renderer.globalIlluminationStatus().effective == Method::PathTracing,
            "Hardware path tracing is unavailable; the 10M criterion was not exercised. " + renderer.hardwareRayTracingStatus());
        report["memoryBeforeLargeScene"] = videoMemory(renderer.device());

        const auto generationStart = Clock::now();
        assets.meshes.push_back(makeTerrain(report));
        report["terrainGenerationMs"] = milliseconds(generationStart);
        world.actors.clear();
        world.createActor("10M terrain", terrainMeshId, floorMaterialId);
        for (const auto material : {redMaterialId, blueMaterialId}) {
            const auto id = world.createActor(material == redMaterialId ? "Red paint cube" : "Blue paint cube", 3, material);
            auto& transform = world.findActor(id)->transform;
            transform.position[0] = material == redMaterialId ? -1.4f : 1.4f;
            transform.position[1] = .9f;
            for (auto& scale : transform.scale) scale = .8f;
        }
        std::uint64_t expectedTriangles = 0;
        for (const auto& actor : world.actors) expectedTriangles += assets.findMesh(actor.mesh.meshId)->indices.size() / 3;
        require(expectedTriangles >= terrainTriangles, "Generated scene is smaller than 10M triangles.");
        report["expectedSceneTriangles"] = expectedTriangles;
        require(renderer.resize(width, height, error), error);
        settings.viewportWidth = width; settings.viewportHeight = height;
        renderer.resetGlobalIllumination();
        const auto dispatchStart = renderer.hardwareRayTracingDispatchCount();
        const auto traceBeforeBuild = profiler.snapshot(65536);
        const auto lastEventId = [](const diagnostics::TraceSnapshot& snapshot) {
            std::uint64_t latest = 0;
            for (const auto& event : snapshot.events) latest = std::max(latest, event.eventId);
            return latest;
        };
        report["firstLargeSceneBuildAndFrameMs"] = renderFrame();
        const auto firstGi = renderer.globalIlluminationStatus();
        require(renderer.hardwareRayTracingActive() && firstGi.effective == Method::PathTracing &&
            firstGi.effectiveMode == Mode::Realtime, "10M scene did not activate realtime hardware GI: " + firstGi.reason);
        report["memoryAfterFirstLargeFrame"] = videoMemory(renderer.device());
        const auto traceAfterBuild = profiler.snapshot(65536);
        const auto staticTraceStart = lastEventId(traceAfterBuild);
        const auto initialTraceStart = lastEventId(traceBeforeBuild);
        std::uint64_t initialRebuilds = 0;
        for (const auto& event : traceAfterBuild.events)
            if (event.eventId > initialTraceStart && event.name == "DXR scene rebuild") ++initialRebuilds;
        require(initialRebuilds > 0, "The initial large DXR build was not observable in the trace.");
        std::vector<double> warmup, measured, cpuSubmission;
        warmup.reserve(warmupFrames); measured.reserve(measurementFrames); cpuSubmission.reserve(measurementFrames);
        Json gpuTimings = Json::array();
        std::uint64_t lastGpuFrame = 0;
        for (unsigned frame = 0; frame < warmupFrames + measurementFrames; ++frame) {
            const auto previousDispatches = renderer.hardwareRayTracingDispatchCount();
            const auto elapsed = renderFrame();
            require(renderer.hardwareRayTracingActive() && renderer.hardwareRayTracingDispatchCount() > previousDispatches,
                "Hardware dispatch stopped during the 10M run: " + renderer.hardwareRayTracingStatus());
            const auto gi = renderer.globalIlluminationStatus();
            require(gi.effective == Method::PathTracing && gi.effectiveMode == Mode::Realtime && gi.historyRevision == firstGi.historyRevision,
                "Realtime GI became inactive or unexpectedly restarted in the static scene.");
            if (frame < warmupFrames) warmup.push_back(elapsed);
            else {
                measured.push_back(elapsed);
                const auto& diagnostics = renderer.diagnosticsFrame();
                cpuSubmission.push_back(diagnostics.cpuSubmitMs);
                if (diagnostics.gpuStatus == renderer::GpuTimingStatus::Ready && diagnostics.gpuFrameId != lastGpuFrame) {
                    lastGpuFrame = diagnostics.gpuFrameId;
                    gpuTimings.push_back({{"frameId", diagnostics.gpuFrameId}, {"milliseconds", diagnostics.gpuTotalMs}});
                }
            }
        }
        const auto geometry = renderer.geometryStatistics();
        report["geometryStatistics"] = {{"frameId", geometry.frameId}, {"gpuFrameId", geometry.gpuFrameId},
            {"sceneTriangles", geometry.sceneTriangles}, {"inputTriangles", geometry.inputTriangles},
            {"postClipTriangles", geometry.postClipTriangles}, {"status", geometry.statusMessage}};
        require(geometry.status == renderer::GeometrySampleStatus::Ready && geometry.sceneTriangles == expectedTriangles &&
            geometry.inputTriangles == expectedTriangles && geometry.postClipTriangles >= terrainTriangles,
            "The main raster GPU query did not confirm at least 10M in-frustum submitted triangles.");
        const auto hardwareStatus = renderer.hardwareRayTracingStatus();
        const auto actualRayTriangles = rayTriangleCount(hardwareStatus);
        require(actualRayTriangles == expectedTriangles, "DXR did not contain all generated scene triangles.");
        require(hardwareStatus.find("indirect diffuse on") != std::string::npos &&
            hardwareStatus.find("indirect specular on") != std::string::npos, "Both realtime GI contributions must remain active.");
        const auto dispatches = renderer.hardwareRayTracingDispatchCount() - dispatchStart;
        require(dispatches >= 2 * (1 + warmupFrames + measurementFrames), "Missing realtime transport/resolve dispatches.");
        report["hardware"] = {{"status", hardwareStatus}, {"sceneTriangles", actualRayTriangles}, {"dispatches", dispatches},
            {"effectiveGi", "PathTracing"}, {"effectiveMode", "Realtime"},
            {"historyRevision", firstGi.historyRevision}, {"accumulatedSamples", renderer.globalIlluminationStatus().accumulatedSamples}};
        report["timing"] = {{"method", "CPU render submission through synchronized GPU completion; no Present/vsync or UI"},
            {"warmup", timingSummary(warmup)}, {"steadySynchronizedWall", timingSummary(measured)},
            {"steadyCpuSubmissionIncludingBridgeWaits", timingSummary(cpuSubmission)},
            {"asynchronousD3D11GpuQuerySamples", gpuTimings},
            {"gpuQueryNote", "D3D11 timeline including bridge waits; not an isolated DXR execution timer."}};
        report["memoryAfterSteadyFrames"] = videoMemory(renderer.device());
        const auto caches = renderer.cacheStats();
        report["rasterCache"] = {{"meshCount", caches.meshCount}, {"logicalGpuBytesEstimate", caches.meshGpuBytesEstimate},
            {"softwareRayTriangles", caches.rayTriangleCount}};
        const auto afterStatic = profiler.snapshot(65536);
        std::uint64_t staticRebuilds = 0, staticUploads = 0;
        for (const auto& event : afterStatic.events) if (event.eventId > staticTraceStart) {
            if (event.name == "DXR scene rebuild") ++staticRebuilds;
            if (event.name == "Mesh geometry upload") ++staticUploads;
        }
        report["staticCache"] = {{"initialDxrRebuildEvents", initialRebuilds}, {"staticDxrRebuildEvents", staticRebuilds},
            {"staticRasterUploadEvents", staticUploads}, {"newTraceDrops", afterStatic.droppedEvents - traceAfterBuild.droppedEvents}};
        require(staticRebuilds == 0 && staticUploads == 0 && afterStatic.droppedEvents == traceAfterBuild.droppedEvents,
            "Static frames rebuilt geometry or the evidence trace dropped events.");
        require(profiler.save(output / "render-scale-trace.json", error), error);
        auto pixels = readScene(renderer);
        std::uint64_t nonblack = 0;
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
            if (std::max({pixels[offset], pixels[offset + 1], pixels[offset + 2]}) > 8) ++nonblack;
        report["nonblackPixels"] = nonblack;
        require(nonblack > static_cast<std::uint64_t>(width) * height / 20,
            "The 10M scene image is empty or covers too few pixels against the black backdrop.");
        saveImage(output / "render-scale-10m.bmp", std::move(pixels));
        report["validationErrors"] = renderer.validationErrorCount();
        require(renderer.validationErrorCount() == 0, "Graphics validation reported errors.");
        restore.restore();
        report["passed"] = true;
        saveReport();
        std::cout << "PASS: 10M render scale: " << expectedTriangles << " triangles, 1920x1080, DXR + realtime GI, "
            << dispatches << " hardware dispatches; synchronized steady median "
            << report["timing"]["steadySynchronizedWall"]["medianMs"].get<double>() << " ms, p95 "
            << report["timing"]["steadySynchronizedWall"]["p95Ms"].get<double>() << " ms. No fixed FPS threshold.\n";
    } catch (const std::exception& exception) {
        report["error"] = exception.what();
        report["hardwareStatus"] = renderer.hardwareRayTracingStatus();
        report["validationErrors"] = renderer.validationErrorCount();
        try { report["memoryAtFailure"] = videoMemory(renderer.device()); saveReport(); } catch (...) {}
        throw;
    }
}
