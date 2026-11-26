// Hardware scene pass. Lighting, material source, display transform and debug
// modes are shared with the raster pipeline; intersections use DXR acceleration
// structures through HardwareTrace.hlsli.
#define RAE_HARDWARE_RAYTRACING 1
#include "Scene.hlsl"

// The raster scene header hides the DDGI constants in the hardware build, but
// the probe-update dispatch shares the same 80-byte b9 layout.
cbuffer ProbeConstants : register(b9)
{
    float4 probeGrid;
    float4 probeOrigin;
    float4 probeParameters;
    float4 probeLayout;
    float4 probeCellSize;
};

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
// Signed SH radiance coefficients; alpha holds the effective sample window.
RWTexture2D<float4> probeAtlasOutput : register(u10);
// Six cosine^4 bins, +/- for each world axis. XY are normalized distance
// moments in grid-cell units. Z is their mean sample weight across frames.
RWTexture2D<float4> probeDepthOutput : register(u11);
Texture2D<float4> probeAtlasPrevious : register(t16);
Texture2D<float4> probeDepthPrevious : register(t17);

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

//------------------------------------------------------------------------------
// One thread traces each probe and projects the same incident rays into
// signed SH and directional depth moments. The raster receiver evaluates
// the clamped-cosine convolution for its surface normal.
//------------------------------------------------------------------------------
float3 ProbeDirectDiffuse(float3 position, float3 normal, float3 geometricNormal,
    float3 view, Surface material)
{
    // Match the screen-space probe's direct-diffuse source. A rare GGX
    // highlight on the donor would otherwise spread over an entire SH cell.
    float3 diffuse = material.albedo * (1.0 - material.metallic) / PI;
    if (!any(diffuse > 0.0)) return 0.0;
    float3 origin = position + geometricNormal * RAY_EPSILON;
    float3 f0 = lerp(0.04.xxx, material.albedo, material.metallic);
    float3 radiance = 0.0;
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        SceneLight source = sceneLights[index];
        [loop] for (uint sampleIndex = 0; sampleIndex < SceneLightSampleCount(source); ++sampleIndex)
        {
            SceneLightSample lightSample;
            if (!SampleSceneLight(source, sampleIndex, origin, lightSample)) continue;
            float cosine = dot(normal, lightSample.direction);
            float3 halfVector = lightSample.direction + view;
            if (cosine <= 0.0 || dot(geometricNormal, lightSample.direction) <= 0.0 ||
                dot(halfVector, halfVector) <= 1e-12) continue;
            // Use the geometric normal for the ray offset and the actual light
            // sample distance. Preserve both geometry and analytic emitter blockers.
            if (SceneLightVisibility(index, source, lightSample, position, geometricNormal, 0.0, false) <= 0.0)
                continue;
            if (outputInfo.w != 0u && source.spotShadow.z > 0.5)
            {
                float distance;
                int emitter;
                if (PathAreaHit(origin, lightSample.direction, distance, emitter) &&
                    distance < lightSample.distance - RAY_EPSILON) continue;
            }
            float3 fresnel = Fresnel(saturate(dot(view, normalize(halfVector))), f0);
            radiance += (1.0 - fresnel) * diffuse * cosine * lightSample.irradiance;
        }
    }
    return radiance;
}

void ResolveProbeAtlas(uint2 pixel)
{
    int3 dimensions = int3(probeGrid.xyz);
    if (pixel.x >= uint(dimensions.x * dimensions.z) || pixel.y >= uint(dimensions.y)) return;
    int3 coordinate = int3(int(pixel.x) % dimensions.x, int(pixel.y), int(pixel.x) / dimensions.x);
    int coefficientCount = probeLayout.x > 1.5 ? 9 : 4;
    uint atlasBase = pixel.x * uint(coefficientCount);
    float spacing = max(probeOrigin.w, 0.01);
    float3 world = probeOrigin.xyz + (float3(coordinate) + 0.5) * probeCellSize.xyz;
    float3 coefficients[9];
    [unroll] for (int coefficient = 0; coefficient < 9; ++coefficient)
        coefficients[coefficient] = 0.0;
    float3 distanceMoments[kProbeDistanceDirections];
    [unroll] for (int bin = 0; bin < kProbeDistanceDirections; ++bin)
        distanceMoments[bin] = 0.0;
    uint directionCount = clamp((uint)probeParameters.x, 1u, 32u);
    uint frame = (uint)probeLayout.w;
    [loop] for (uint directionIndex = 0; directionIndex < directionCount; ++directionIndex)
    {
        float3 direction = ProbeRayDirection(directionIndex, directionCount, coordinate, frame);
        float distance;
        float3 hitNormal;
        int object;
        float3 radiance = 0.0;
        bool hit = TraceScene(world, direction, distance, hitNormal, object, false);
        float emitterDistance;
        int emitterIndex;
        if (PathAreaHit(world, direction, emitterDistance, emitterIndex) && emitterDistance < distance)
        {
            // A directly visible light is already part of the primary
            // surface's direct-light estimator. It can still occlude a probe.
            distance = emitterDistance;
        }
        else if (hit)
        {
            float3 position = world + direction * distance;
            uint triangleIndex = (uint)(-object - 2);
            float3 geometricNormal = rayTriangles[triangleIndex].normal;
            if (dot(geometricNormal, direction) > 0.0) geometricNormal = -geometricNormal;
            if (dot(hitNormal, geometricNormal) < 0.0) hitNormal = -hitNormal;
            if (dot(hitNormal, -direction) < 1e-5) hitNormal = geometricNormal;
            Surface material = TriangleMaterialAt(position, hitNormal, triangleIndex);
            radiance = ProbeDirectDiffuse(position, hitNormal, geometricNormal, -direction, material);
        }
        [loop] for (int coefficient = 0; coefficient < coefficientCount; ++coefficient)
            coefficients[coefficient] += radiance * ProbeSHBasis(coefficient, direction);
        // Misses count as clear to the integration limit. Keeping moments in
        // cell units avoids oversaturating the half-float squared distances.
        float cellDistance = min(distance / spacing, kProbeDistanceLimit);
        float3 sample = float3(cellDistance, cellDistance * cellDistance, 1.0);
        [unroll] for (int bin = 0; bin < kProbeDistanceDirections; ++bin)
        {
            float cosine = direction[bin / 2] * (bin % 2 == 0 ? 1.0 : -1.0);
            distanceMoments[bin] += sample * ProbeDistanceWeight(cosine);
        }
    }
    bool historyValid = probeParameters.y > 0.5;
    float4 previousDC = 0.0;
    if (historyValid)
        previousDC = probeAtlasPrevious.Load(int3(atlasBase, pixel.y, 0));
    float window = ProbeUpdateWindow(probeGrid.w, previousDC.a, probeLayout.w);
    float blend = rcp(window);
    float shProjection = 4.0 * PI / float(directionCount);
    [loop] for (int coefficient = 0; coefficient < coefficientCount; ++coefficient)
    {
        int2 slot = int2(atlasBase + coefficient, pixel.y);
        float3 current = clamp(coefficients[coefficient] * shProjection, -65504.0, 65504.0);
        if (historyValid)
        {
            float3 previous = coefficient == 0 ? previousDC.rgb : probeAtlasPrevious.Load(int3(slot, 0)).rgb;
            current = lerp(previous, current, blend);
        }
        probeAtlasOutput[slot] = float4(current, window);
    }
    [unroll] for (int bin = 0; bin < kProbeDistanceDirections; ++bin)
    {
        int2 slot = int2(pixel.x * kProbeDistanceDirections + bin, pixel.y);
        // Pool weighted sums before normalizing. Averaging per-frame ratios
        // overweights sparse directional bins and biases visibility near planes.
        float3 estimate = distanceMoments[bin] / float(directionCount);
        if (historyValid)
        {
            float3 previous = probeDepthPrevious.Load(int3(slot, 0)).xyz;
            estimate = lerp(float3(previous.xy * previous.z, previous.z), estimate, blend);
        }
        float2 moments = estimate.z > 1e-6 ? estimate.xy / estimate.z :
            float2(kProbeDistanceLimit, kProbeDistanceLimit * kProbeDistanceLimit);
        probeDepthOutput[slot] = float4(moments, estimate.z, 0.0);
    }
}

// A wide, weak reflection averages fine penumbra structure. Evaluate each
// rectangle at its centre for this lobe only. Approximate solid angle with
// an equal projected-area disk; it tends to A*cos(theta)/d^2 in the far
// field and remains bounded by a hemisphere near the emitting surface.
float3 BroadReflectionDirect(float3 position, float3 normal, float3 view, Surface material)
{
    float3 radiance = PathDirectLight(position, normal, normal, view, material);
    float3 origin = position + normal * RAY_EPSILON;
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        if (sceneLights[index].positionType.w != 2.0) continue;
        PathAreaEmitter emitter = PathAreaAt((int)index);
        float3 delta = emitter.center - origin;
        float distance = length(delta);
        float3 incoming = delta / max(distance, 1e-8);
        float cosine = dot(normal, incoming);
        float pdf = PathAreaPdf(emitter, incoming, distance);
        if (cosine <= 0.0 || pdf <= 0.0 || !PathAreaVisible(emitter, origin, incoming, distance)) continue;
        float ratio = rcp(PI * pdf);
        float radial = sqrt(1.0 + ratio);
        float solidAngle = (2.0 * PI * ratio) / (radial * (1.0 + radial));
        float unusedPdf;
        float3 brdf = PathBrdf(normal, view, incoming, material, true, true, 1.0, unusedPdf);
        radiance += brdf * cosine * solidAngle * emitter.radiance * PathAreaRange(emitter, distance);
    }
    return radiance;
}

float4 TraceReflectionSample(float3 origin, float3 direction, float coneAlphaSquared, bool wide)
{
    float distance;
    float3 normal;
    int object;
    if (!TraceScene(origin, direction, distance, normal, object, false)) return 0.0;

    float3 position = origin + direction * distance;
    Surface material = TriangleMaterialAt(position, normal, (uint)(-object - 2));
    // The incident directions covered by one ray broaden the apparent GGX
    // highlight at its hit. Add the two squared lobe widths before shading;
    // four discrete light samples hitting a sharp unfiltered metal otherwise
    // paint four pinpoints even in a very rough wall's reflection.
    float hitAlpha = material.roughness * material.roughness;
    material.roughness = sqrt(sqrt(saturate(hitAlpha * hitAlpha + coneAlphaSquared)));
    if (wide) return float4(BroadReflectionDirect(position, normal, -direction, material), 1.0);
    // The single-frame raster composite has no specular accumulation. Use the
    // same deterministic rectangle quadrature as the visible raster surfaces.
    float3 diffuse, specular;
    ShadeSurfaceContributions(position, normal, -direction, material, 0.0, false, diffuse, specular);
    return float4(diffuse + specular, 1.0);
}

// Reflection dispatch shared by the raster SSGI and DDGI composites. It
// writes incident radiance reflected from the primary surface into the shared
// indirect texture; the raster composite applies its own Fresnel/roughness
// weighting, so this stays a pure radiance transport result.
void ResolveReflection(uint2 pixel)
{
    float3 origin, direction;
    float farDistance;
    PathPrimaryRay(pixel, 0.5, origin, direction, farDistance);
    float distance;
    float3 hitNormal;
    int object;
    bool hit = TraceScene(origin, direction, distance, hitNormal, object, false);
    if (!hit || distance > farDistance)
    {
        indirectLightingOutput[pixel] = 0.0;
        return;
    }
    RayTriangle tri = rayTriangles[-object - 2];
    float3 position = origin + direction * distance;
    float3 geometricNormal = tri.normal;
    if (dot(geometricNormal, direction) > 0.0) geometricNormal = -geometricNormal;
    float3 normal = normalize(hitNormal);
    if (dot(normal, geometricNormal) < 0.0) normal = geometricNormal;
    if (dot(normal, -direction) <= 1e-6) normal = geometricNormal;
    Surface material = TriangleMaterialAt(position, normal, (uint)(-object - 2));
    float3 view = normalize(-direction);
    float3 reflection = reflect(-view, normal);
    float3 rayOrigin = position + geometricNormal * RAY_EPSILON;
    // Limit the lobe near grazing angles so no sample traces through the
    // receiver. Fall back to its geometric reflection at a shading silhouette.
    if (dot(reflection, geometricNormal) <= 0.0) reflection = reflect(-view, geometricNormal);
    float alpha = material.roughness * material.roughness;
    float3 f0 = lerp(0.04.xxx, material.albedo, material.metallic);
    float3 fresnel = Fresnel(saturate(dot(view, normal)), f0);
    bool wide = UsesWideDxrReflection(material.roughness, material.metallic, fresnel);
    float spread = min(lerp(0.3, 0.8, smoothstep(0.3, 0.8, material.roughness)) * alpha,
        0.95 * saturate(dot(reflection, geometricNormal)));
    float coneAlphaSquared = alpha * alpha;
    float4 incident = 0.0;
    if (spread <= 0.003)
        incident = TraceReflectionSample(rayOrigin, reflection, coneAlphaSquared, wide);
    else
    {
        float3 tangent = PathToWorld(float3(1.0, 0.0, 0.0), reflection);
        float3 bitangent = cross(reflection, tangent);
        if (wide)
        {
            // Every 3x3 reconstruction neighborhood covers all nine strata.
            // The slow, continuous phase avoids stamping one set of reflected
            // silhouettes across an entire plane. Opposed rays cancel the tilt
            // of each pixel's estimate even where few neighbors survive.
            uint stratum = (pixel.x % 3u) + 3u * (pixel.y % 3u);
            float angle = PI * frac(float(stratum) * 0.61803399 +
                (float(pixel.x) + 0.5 * float(pixel.y)) / 768.0);
            float radius = sqrt((float(stratum) + 0.5) / 9.0);
            float3 offset = (cos(angle) * tangent + sin(angle) * bitangent) * (spread * radius);
            float residual = max(0.0, coneAlphaSquared - 0.5 * spread * spread);
            incident = 0.5 * (TraceReflectionSample(rayOrigin, normalize(reflection + offset), residual, true)
                + TraceReflectionSample(rayOrigin, normalize(reflection - offset), residual, true));
        }
        else
        {
            [loop] for (uint tap = 0; tap < 4u; ++tap)
            {
                float3 offset = (tap & 1u) ? bitangent : tangent;
                if (tap & 2u) offset = -offset;
                float3 ray = normalize(reflection + spread * offset);
                incident += 0.25 * TraceReflectionSample(rayOrigin, ray, max(0.0, coneAlphaSquared - spread * spread), false);
            }
        }
    }
    // Alpha carries fractional geometry coverage so the composite adds the
    // environment only for the rays that missed the scene.
    indirectLightingOutput[pixel] = clamp(incident, 0.0, 65504.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (probeParameters.w > 0.5)
    {
        ResolveProbeAtlas(thread.xy);
        return;
    }
    if (any(thread.xy >= outputInfo.xy)) return;
    if (traceOptions.x > 1.5)
    {
        ResolveReflection(thread.xy);
        return;
    }
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
                bool spatialReuse = (((uint)traceOptions.w / 1000000u) % 10u) > 0u;
                if (restirHistory && spatialReuse)
                {
                    float3 spatialSum = 0.0;
                    float spatialCount = 0.0;
                    int2 coordinate = int2(thread.xy);
                    [unroll] for (int y = -1; y <= 1; ++y)
                    [unroll] for (int x = -1; x <= 1; ++x)
                    {
                        if (x == 0 && y == 0) continue;
                        int2 neighbour = coordinate + int2(x, y);
                        if (any(neighbour < 0) || any(neighbour >= int2(outputInfo.xy))) continue;
                        float4 reservoir = restirPreviousReservoir.Load(int3(neighbour, 0));
                        if (reservoir.w > 0.0)
                        {
                            spatialSum += reservoir.rgb;
                            spatialCount += 1.0;
                        }
                    }
                    if (spatialCount > 0.0) batchMean = lerp(batchMean, spatialSum / spatialCount, 0.2);
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
