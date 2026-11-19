// Hardware scene pass. Lighting, material source, display transform and debug
// modes are shared with the raster pipeline; intersections use DXR acceleration
// structures through HardwareTrace.hlsli.
#define RAE_HARDWARE_RAYTRACING 1
#include "Scene.hlsl"

RWTexture2D<float4> hardwareOutput : register(u0);
// Persistent linear-HDR mean. Alpha records the accumulated sample count for
// inspection; CPU b5 remains the authoritative sample/reset contract.
RWTexture2D<float4> pathAccumulation : register(u1);
RWTexture2D<float4> primaryGeometry : register(u2);
// Linear HDR, shared with raster lighting. It never contains primary direct light.
RWTexture2D<float4> indirectLightingOutput : register(u3);
RWTexture2D<float4> primaryMaterial : register(u4);
// Previous-frame world position of the primary surface (w=0 for background).
RWTexture2D<float4> primaryPreviousPosition : register(u7);

// Realtime reconstruction and ReSTIR history. Declared before PathTracing.hlsli
// because the direct-light reservoir updates its history from that header.
cbuffer DenoiseConstants : register(b8)
{
    row_major float4x4 denoisePreviousViewProjection;
    // mode (0 off, 1 spatial, 2 temporal+spatial), current-frame weight,
    // reset history, spatial iterations.
    float4 denoiseOptions;
};
Texture2D<float4> denoisePreviousIndirect : register(t12);
Texture2D<float4> denoisePreviousGeometry : register(t13);
Texture2D<float4> restirPreviousReservoir : register(t14);
Texture2D<float4> restirPreviousDirect : register(t15);
RWTexture2D<float4> denoiseHistoryIndirect : register(u5);
RWTexture2D<float4> denoiseHistoryGeometry : register(u6);
RWTexture2D<float4> restirReservoir : register(u8);
RWTexture2D<float4> restirDirectReservoir : register(u9);

#include "PathTracing.hlsli"

float3 RealtimePosition(uint2 pixel, float distance)
{
    float3 origin, direction;
    float farDistance;
    PathPrimaryRay(pixel, 0.5, origin, direction, farDistance);
    return origin + direction * distance;
}

void ResolveRealtimeGi(uint2 pixel)
{
    float4 geometry = primaryGeometry[pixel];
    if (geometry.w <= 0.0)
    {
        indirectLightingOutput[pixel] = 0.0;
        if (denoiseOptions.x >= 1.5)
        {
            denoiseHistoryIndirect[pixel] = 0.0;
            denoiseHistoryGeometry[pixel] = geometry;
        }
        return;
    }
    float4 material = primaryMaterial[pixel];
    float3 position = RealtimePosition(pixel, geometry.w);
    float3 raw = pathAccumulation[pixel].rgb;
    float3 resolved = raw;
    if (denoiseOptions.x >= 0.5)
    {
        // Keep near-mirror reflections sharp. For rough surfaces filter only
        // neighbours on the same plane/normal and with similar material values.
        // Longer iteration counts add wider 5x5 taps (multi-scale approximation
        // of a-trous) instead of repeating the same one-pixel kernel.
        int radius = material.w < 0.12 ? 0 : 2;
        float footprint = CameraPixelFootprint(position, float(outputInfo.y));
        float3 sum = 0.0;
        float total = 0.0;
        uint iterations = clamp((uint)denoiseOptions.w, 1u, 4u);
        if (radius > 0)
        {
            [loop] for (uint iteration = 0; iteration < iterations; ++iteration)
            {
                int step = int(1u << min(iteration, 3u));
                float iterationWeight = exp(-float(iteration) * 0.5);
                [loop] for (int y = -radius; y <= radius; ++y)
                [loop] for (int x = -radius; x <= radius; ++x)
                {
                    int2 neighbour = int2(pixel) + int2(x, y) * step;
                    if (any(neighbour < 0) || any(neighbour >= int2(outputInfo.xy))) continue;
                    float4 otherGeometry = primaryGeometry[neighbour];
                    if (otherGeometry.w <= 0.0) continue;
                    float cosine = saturate(dot(geometry.xyz, otherGeometry.xyz));
                    if (cosine < 0.9) continue;
                    float4 otherMaterial = primaryMaterial[neighbour];
                    float3 delta = RealtimePosition(uint2(neighbour), otherGeometry.w) - position;
                    float planeDistance = abs(dot(delta, geometry.xyz));
                    float3 colorDelta = material.rgb - otherMaterial.rgb;
                    float weight = exp(-float(x*x + y*y) / 4.0) * iterationWeight
                        * pow(cosine, 32.0) * exp(-planeDistance / footprint)
                        * exp(-dot(colorDelta, colorDelta) * 32.0 - abs(material.w - otherMaterial.w) * 32.0);
                    sum += pathAccumulation[neighbour].rgb * weight;
                    total += weight;
                }
            }
            resolved = sum / max(total, 1e-8);
        }
        else
        {
            resolved = raw;
        }
    }
    if (denoiseOptions.x >= 1.5)
    {
        // Camera and per-object reprojection. Object motion can still exhibit
        // mild disocclusion trailing at depth/normal boundaries.
        float3 history = resolved;
        float historyWeight = 0.0;
        if (denoiseOptions.z < 0.5)
        {
            float4 previousPosition = primaryPreviousPosition[pixel];
            float3 reprojectionPosition = previousPosition.w > 0.5 ? previousPosition.xyz : position;
            float4 clip = mul(float4(reprojectionPosition, 1.0), denoisePreviousViewProjection);
            if (clip.w > 0.0001)
            {
                float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
                if (all(uv > 0.0) && all(uv < 1.0))
                {
                    int2 previousPixel = int2(uv * float2(outputInfo.xy));
                    float4 previousGeometry = denoisePreviousGeometry.Load(int3(previousPixel, 0));
                    float4 previousIndirect = denoisePreviousIndirect.Load(int3(previousPixel, 0));
                    float cosine = dot(normalize(geometry.xyz), normalize(previousGeometry.xyz));
                    float depthDelta = abs(previousGeometry.w - geometry.w) / max(geometry.w, 1.0);
                    if (previousGeometry.w > 0.0 && cosine > 0.9 && depthDelta < 0.05)
                    {
                        // Clamp history to the current spatial neighbourhood to
                        // reject disocclusions and stale lighting.
                        float3 low = raw, high = raw;
                        [loop] for (int y = -1; y <= 1; ++y)
                        [loop] for (int x = -1; x <= 1; ++x)
                        {
                            int2 neighbour = int2(pixel) + int2(x, y);
                            if (any(neighbour < 0) || any(neighbour >= int2(outputInfo.xy))) continue;
                            float4 otherGeometry = primaryGeometry[neighbour];
                            if (otherGeometry.w <= 0.0 || dot(geometry.xyz, otherGeometry.xyz) < 0.9) continue;
                            float3 other = pathAccumulation[neighbour].rgb;
                            low = min(low, other); high = max(high, other);
                        }
                        history = clamp(previousIndirect.rgb, low, high);
                        historyWeight = 1.0 - denoiseOptions.y;
                    }
                }
            }
        }
        resolved = lerp(resolved, history, saturate(historyWeight));
        denoiseHistoryIndirect[pixel] = float4(clamp(resolved, 0.0, 65504.0), 1.0);
        denoiseHistoryGeometry[pixel] = geometry;
    }
    // The shared consumer texture is FP16 HDR. Avoid converting very bright
    // finite path samples to infinity when resolving into that format.
    indirectLightingOutput[pixel] = float4(clamp(resolved, 0.0, 65504.0), 1.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= outputInfo.xy)) return;
    if (accumulationInfo.w == 1 && viewportDebug.z < 0.5)
    {
        bool realtime = giOptions.x == 1;
        if (realtime && giOptions.z == 1)
        {
            ResolveRealtimeGi(thread.xy);
            return;
        }
        uint previousSamples = accumulationInfo.x;
        uint batchSamples = accumulationInfo.y;
        float3 mean = 0.0;
        if (batchSamples != 0)
        {
            uint restirPacked = (uint)traceOptions.w;
            uint restirCandidates = min(restirPacked % 100u, 16u);
            bool restirIndirect = realtime && ((restirPacked / 1000u) % 10u) > 0u;
            bool restirHistory = ((restirPacked / 10000u) % 10u) > 0u;
            uint effectiveSamples = batchSamples;
            float3 batchMean = 0.0;
            float3 reservoirPreviousPosition = 0.0;
            if (restirIndirect && restirCandidates > 0u)
            {
                // Weighted-reservoir sampling over several indirect path
                // candidates. Primary guides come from the first candidate.
                effectiveSamples = restirCandidates;
                float3 selected = 0.0;
                float selectedWeight = 0.0;
                float totalWeight = 0.0;
                uint sampleBase = (realtime ? giOptions.w : previousSamples);
                [loop] for (uint candidate = 0; candidate < restirCandidates; ++candidate)
                {
                    float4 geometry, material;
                    float3 previousPosition;
                    float3 sample = PathRadiance(thread.xy, sampleBase + candidate,
                        geometry, material, previousPosition);
                    if (candidate == 0u)
                    {
                        primaryGeometry[thread.xy] = geometry;
                        primaryMaterial[thread.xy] = material;
                        primaryPreviousPosition[thread.xy] = float4(previousPosition, 1.0);
                        reservoirPreviousPosition = previousPosition;
                    }
                    float weight = dot(sample, float3(0.2126, 0.7152, 0.0722));
                    totalWeight += weight;
                    uint hash = PathHash(thread.x + thread.y * outputInfo.x + candidate * 9781u);
                    float random = float(hash & 0xffffffu) / 16777216.0;
                    if (weight > 0.0 && random * totalWeight <= weight)
                    {
                        selected = sample;
                        selectedWeight = weight;
                    }
                }
                batchMean = selectedWeight > 0.0
                    ? selected * (totalWeight / (selectedWeight * float(restirCandidates))) : 0.0;
                if (restirHistory)
                {
                    float4 clip = mul(float4(reservoirPreviousPosition, 1.0), denoisePreviousViewProjection);
                    if (clip.w > 0.0001)
                    {
                        float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
                        if (all(uv > 0.0) && all(uv < 1.0))
                        {
                            float4 previousReservoir = restirPreviousReservoir.Load(
                                int3(int2(uv * float2(outputInfo.xy)), 0));
                            if (previousReservoir.w > 0.0)
                                batchMean = lerp(previousReservoir.rgb, batchMean, 0.2);
                        }
                    }
                }
                restirReservoir[thread.xy] = float4(batchMean, 1.0);
            }
            else
            {
                float3 batchSum = 0.0;
                [loop] for (uint sampleOrdinal = 0; sampleOrdinal < batchSamples; ++sampleOrdinal)
                {
                    float4 geometry, material;
                    float3 previousPosition;
                    batchSum += PathRadiance(thread.xy,
                        (realtime ? giOptions.w : previousSamples) + sampleOrdinal, geometry, material, previousPosition);
                    if (realtime && sampleOrdinal == 0)
                    {
                        primaryGeometry[thread.xy] = geometry;
                        primaryMaterial[thread.xy] = material;
                        primaryPreviousPosition[thread.xy] = float4(previousPosition, 1.0);
                    }
                }
                batchMean = batchSum / float(batchSamples);
            }
            // No read from an uninitialized or invalidated history allocation.
            if (previousSamples == 0) mean = batchMean;
            // traceOptions.y is the history sample cap in realtime mode.
            uint count = realtime ? min(previousSamples + effectiveSamples, (uint)traceOptions.y)
                : previousSamples + effectiveSamples;
            if (previousSamples != 0) mean = lerp(pathAccumulation[thread.xy].rgb, batchMean,
                float(effectiveSamples) / float(max(count, 1u)));
            pathAccumulation[thread.xy] = float4(mean, float(count));
        }
        else if (previousSamples != 0)
        {
            // Resolve still runs after convergence. Exposure changes do not
            // need new paths or invalidate the linear radiance accumulated here.
            mean = pathAccumulation[thread.xy].rgb;
        }
        if (!realtime) hardwareOutput[thread.xy] = float4(DisplayColor(mean), 1.0);
        return;
    }
    // Disabled GI and material/debug views preserve the exact preview path.
    // Debug output never enters radiance accumulation.
    float2 uv = (float2(thread.xy) + 0.5) / float2(outputInfo.xy);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    // Start at the raster camera's near plane and stop at its far plane, so
    // primary visibility agrees with the scene depth used by editor overlays.
    float3 origin, direction;
    float farDistance;
    CameraPrimaryRay(ndc,origin,direction,farDistance);
    float distance;
    float3 normal;
    int object;
    if (!TraceScene(origin, direction, distance, normal, object, false)
        || distance > farDistance)
    {
        hardwareOutput[thread.xy] = Background(uv);
        return;
    }
    RayTriangle tri = rayTriangles[-object - 2];
    float3 position = origin + direction * distance;
    Surface material = TriangleMaterialAt(position, normal, (uint)(-object - 2));
    hardwareOutput[thread.xy] = OutputSurface(position, normal, material, uv, false);
}
