// Raster probe trace, SH projection, irradiance lookup and debug display.
float ProbeVisibility(int2 probeCoordinate, float3 probeToReceiver, float spacing)
{
    float3 offsetInCells = probeToReceiver / spacing;
    float distanceInCells = length(offsetInCells);
    float3 direction = offsetInCells / max(distanceInCells, 1e-4);
    float2 moments = 0.0;
    float weights = 0.0;
    [unroll] for (int axis = 0; axis < 3; ++axis)
    {
        int bin = 2 * axis + (direction[axis] < 0.0 ? 1 : 0);
        float weight = ProbeDistanceWeight(abs(direction[axis]));
        int depthX = probeCoordinate.x * kProbeDistanceDirections + bin;
        moments += probeDepthAtlas.Load(int3(depthX, probeCoordinate.y, 0)).xy * weight;
        weights += weight;
    }
    moments /= max(weights, 1e-5);
    // A fraction of one cell keeps surfaces on the measured depth from
    // self-shadowing. A squared Chebyshev bound reduces distant leaks.
    float difference = max(distanceInCells - moments.x - 0.06, 0.0);
    float variance = max(moments.y - moments.x * moments.x, 0.0025);
    float visible = variance / (variance + difference * difference);
    return visible * visible;
}

// Blend incident SH in linear space before evaluating the irradiance lobe.
// Coefficients 4..8 contribute only to the L2 result for this receiver.
struct BlendedProbeSH
{
    float3 dc;
    float3 x, y, z;
    float3 quadratic;
};

void AddProbeSH(inout BlendedProbeSH sh, int2 slot, float3 first, float weight,
    int count, float3 normal)
{
    sh.dc += weight * first;
    sh.y += weight * probeAtlasRead.Load(int3(slot + int2(1, 0), 0)).rgb;
    sh.z += weight * probeAtlasRead.Load(int3(slot + int2(2, 0), 0)).rgb;
    sh.x += weight * probeAtlasRead.Load(int3(slot + int2(3, 0), 0)).rgb;
    [loop] for (int i = 4; i < count; ++i)
        sh.quadratic += weight * ProbeCosineBasis(i, normal) *
            probeAtlasRead.Load(int3(slot + int2(i, 0), 0)).rgb;
}

float3 EvaluateProbeSH(BlendedProbeSH sh, float3 normal, int count)
{
    float3 mean = max(sh.dc * 0.2820947918, 0.0);
    float3 directional = sh.x * normal.x + sh.y * normal.y + sh.z * normal.z;
    if (count == 9)
        return max(mean + directional * (0.4886025119 * 2.0 / 3.0) + sh.quadratic, 0.0);

    // Nonlinear L1 diffuse reconstruction (Geomerics). A linear lobe can
    // go negative over a whole cone of normals. Reconstruct a positive lobe
    // with the same spherical mean; zero incident energy still returns zero.
    float3 magnitude = sqrt(sh.x * sh.x + sh.y * sh.y + sh.z * sh.z);
    float3 concentration = saturate(magnitude / max(1.732050808 * sh.dc, 1e-12));
    float3 q = saturate(0.5 + 0.5 * directional / max(magnitude, 1e-12));
    float3 power = 1.0 + 2.0 * concentration;
    float3 isotropic = (1.0 - concentration) / (1.0 + concentration);
    return mean * lerp((power + 1.0) * pow(q, power), 1.0, isotropic);
}

float3 ProbeIrradiance(float3 position, float3 normal)
{
    if (probeParameters.x < 0.5 || probeParameters.y < 0.5)
        return float3(0.0, 0.0, 0.0);
    int coefficientCount = probeLayout.x > 1.5 ? 9 : 4;
    int3 dimensions = int3(probeGrid.xyz);
    float spacing = max(probeOrigin.w, 1e-4);
    bool hardware = probeParameters.w > 1.5 && probeParameters.w < 2.5;
    float3 cellSize = max(probeCellSize.xyz, 0.01);
    float3 surfaceCell = (position - probeOrigin.xyz) / cellSize - 0.5;
    float coverage = ProbeGridCoverage(surfaceCell, float3(dimensions));
    if (coverage <= 0.0) return float3(0.0, 0.0, 0.0);
    // Use one point on the lit side for both the interpolation footprint and
    // its visibility rays. Coverage belongs to the actual receiver position.
    float3 receiverBias = hardware ? normal * (0.08 / length(normal / cellSize)) : 0.0;
    float3 local = surfaceCell + receiverBias / cellSize;
    // A hardware probe carries directional visibility, so the footprint can
    // cross more than one cell without blindly sharing light across walls.
    // The quadratic B-spline is continuous in both value and first derivative.
    // Screen traces lack those moments and retain the narrow trilinear footprint.
    int3 base = (int3)floor(local);
    float3 fraction = frac(local);
    float3 taps[3];
    taps[0] = 1.0 - fraction;
    taps[1] = fraction;
    taps[2] = 0.0;
    int tapCount = 2;
    if (hardware)
    {
        int3 nearest = (int3)floor(local + 0.5);
        float3 delta = local - float3(nearest);
        base = nearest - 1;
        taps[0] = 0.5 * (0.5 - delta) * (0.5 - delta);
        taps[1] = 0.75 - delta * delta;
        taps[2] = 0.5 * (0.5 + delta) * (0.5 + delta);
        tapCount = 3;
    }
    BlendedProbeSH sh = (BlendedProbeSH)0;
    float weightSum = 0.0;
    [loop] for (int y = 0; y < tapCount; ++y)
    [loop] for (int z = 0; z < tapCount; ++z)
    [loop] for (int x = 0; x < tapCount; ++x)
    {
        int3 coordinate = clamp(base + int3(x, y, z), int3(0, 0, 0), dimensions - 1);
        float weight = taps[x].x * taps[y].y * taps[z].z;
        float3 center = probeOrigin.xyz + (float3(coordinate) + 0.5) * cellSize;
        float3 toProbe = center - position;
        float facing = dot(normal, toProbe) / max(length(toProbe), spacing * 0.05);
        // Prefer probes on the receiver-facing side of the surface. Soften
        // the plane test near the receiver so the weights vary continuously.
        float normalWeight = 0.05 + 0.95 * smoothstep(-0.15, 0.35, facing);
        weight *= normalWeight;
        int2 probeCoordinate = int2(coordinate.x + coordinate.z * dimensions.x, coordinate.y);
        int atlasBase = probeCoordinate.x * coefficientCount;
        float4 first = probeAtlasRead.Load(int3(atlasBase, probeCoordinate.y, 0));
        weight *= saturate(first.a);
        if (hardware)
        {
            weight *= ProbeVisibility(probeCoordinate, receiverBias - toProbe, spacing);
        }
        AddProbeSH(sh, int2(atlasBase, probeCoordinate.y), first.rgb, weight, coefficientCount, normal);
        weightSum += weight;
    }
    // Avoid renormalizing a set of uniformly occluded probes to full energy.
    return EvaluateProbeSH(sh, normal, coefficientCount) *
        (coverage * probeParameters.z / max(weightSum, 0.18));
}

bool ProjectProbeSample(float3 position, float2 viewportSize, out int3 pixel)
{
    pixel = 0;
    float4 clip = mul(float4(position, 1.0), viewProjection);
    if (clip.w <= 0.0001 || clip.z < 0.0 || clip.z > clip.w) return false;
    float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv >= 1.0)) return false;
    pixel = int3(int2(giTrace.zw) + int2(uv * viewportSize), 0);
    return true;
}

// Rays can enter the raster viewport from an out-of-frame probe. Check the
// whole step against the visible plane, then confirm visibility at the hit.
float3 ScreenProbeRay(float3 origin, float3 direction, float spacing, float range, uint steps)
{
    const float nearDistance = max(spacing * 0.07, RAY_EPSILON * 2.0);
    const float2 viewportSize = max(giViewport.yz, 1.0);
    bool enteredViewport = false;
    float previousEnd = nearDistance;
    [loop] for (uint step = 1; step <= steps; ++step)
    {
        float phase = float(step) / float(steps);
        float segmentStart = previousEnd;
        float segmentEnd = lerp(nearDistance, range, phase * phase);
        previousEnd = segmentEnd;
        float3 sampleWorld = origin + direction * ((segmentStart + segmentEnd) * 0.5);
        int3 pixel;
        if (!ProjectProbeSample(sampleWorld, viewportSize, pixel))
        {
            if (enteredViewport) break;
            continue;
        }
        enteredViewport = true;
        float4 visiblePosition = positionBuffer.Load(pixel);
        if (visiblePosition.w < 0.5) continue;
        float3 normal = normalBuffer.Load(pixel).xyz;
        float incidence = dot(normal, direction);
        if (incidence >= -0.04) continue;
        float hitDistance = dot(normal, visiblePosition.xyz - origin) / incidence;
        if (hitDistance < segmentStart || hitDistance > segmentEnd) continue;

        float3 hit = origin + direction * hitDistance;
        int3 hitPixel;
        if (!ProjectProbeSample(hit, viewportSize, hitPixel)) continue;
        float4 surface = positionBuffer.Load(hitPixel);
        if (surface.w < 0.5) continue;
        float3 hitNormal = normalBuffer.Load(hitPixel).xyz;
        if (dot(hitNormal, normal) < 0.9 || dot(hitNormal, direction) >= -0.04) continue;
        float3 delta = surface.xyz - hit;
        const float footprint = CameraPixelFootprint(hit, viewportSize.y);
        const float viewCosine = max(0.15, abs(dot(hitNormal, CameraViewDirection(hit))));
        // G-buffer positions are FP16. Allow one pixel's depth uncertainty
        // without making the hit thickness a function of the probe density.
        const float precision = dot(abs(surface.xyz), abs(hitNormal)) / 1024.0;
        if (abs(dot(hitNormal, delta)) > max(footprint, precision) ||
            abs(dot(cameraForward.xyz, delta)) > (footprint + precision) / viewCosine) continue;
        return probeColorSource.Load(hitPixel).rgb;
    }
    return 0.0;
}

float3 ScreenProbeDirection(uint ray, uint rayCount, int3 coordinate, uint frame)
{
    // Neighboring probes contribute to the same receiver in this frame.
    // Give each of the eight cells complementary angular strata.
    uint3 parity = uint3(coordinate) & 1u;
    uint stratum = parity.x + 2u * parity.y + 4u * parity.z;
    float2 phase = ProbeSamplePhase(coordinate / 2, frame);
    phase = frac(phase + float2(stratum, (stratum * 5u) & 7u) / 8.0);
    return ProbeDirectionFromPhase(ray, rayCount, phase);
}

// Scramble the four phases per group and layer. Every probe has a guaranteed
// slot once per cycle; an extra slot halfway through serves changing probes.
uint ScreenProbePhase(uint group, uint layer)
{
    uint hash = group * 0x9e3779b9u + layer * 0x85ebca6bu;
    return ((hash >> 15) ^ hash ^ (uint)probeSchedule.y) & 3u;
}

float4 PSProbeRayTrace(FullscreenOutput input) : SV_TARGET
{
    uint2 pixel = uint2(input.position.xy);
    uint3 dimensions = uint3(probeGrid.xyz);
    uint rayCount = clamp((uint)probeLayout.y, 1u, 32u);
    uint layer = pixel.y / rayCount;
    uint column = pixel.x;
    if (probeSchedule.x > 1.5)
    {
        uint group = pixel.x >> 1u;
        uint lane = pixel.x & 1u;
        column = group * 4u + (ScreenProbePhase(group, layer) ^ (lane * 2u));
        if (column >= dimensions.x * dimensions.z) return 0.0;
        // Coefficient 1's spare alpha stores the average relative DC change
        // between completed SH updates. Alpha on coefficient 0 is the window.
        uint stride = probeLayout.x > 1.5 ? 9u : 4u;
        if (lane != 0u && probeAtlasRead.Load(int3(column * stride + 1u, layer, 0)).a < 0.025)
            return 0.0;
    }
    int3 coordinate = int3(column % dimensions.x, layer, column / dimensions.x);
    float3 direction = ScreenProbeDirection(pixel.y % rayCount, rayCount, coordinate, (uint)probeLayout.w);
    float spacing = max(probeOrigin.w, 0.01);
    float3 origin = probeOrigin.xyz + (float3(coordinate) + 0.5) * probeCellSize.xyz;
    // Keep transport range comparable when an auto-fit volume is split into
    // more cells at a higher quality setting.
    float range = spacing * max(5.0, 0.5 * float(max(dimensions.x, max(dimensions.y, dimensions.z))));
    uint steps = clamp((uint)probeLayout.z, 1u, 24u);
    [branch] if (probeParameters.w > 4.5) return float4(WorldProbeRay(origin, direction), 1.0);
    return float4(clamp(ScreenProbeRay(origin, direction, spacing, range, steps), 0.0, 65504.0), 1);
}

float4 PSProbeUpdate(FullscreenOutput input) : SV_TARGET
{
    uint2 pixel = uint2(input.position.xy);
    uint3 dimensions = uint3(probeGrid.xyz);
    uint coefficientCount = probeLayout.x > 1.5 ? 9u : 4u;
    uint coefficient = pixel.x % coefficientCount;
    uint column = pixel.x / coefficientCount;
    if (column >= dimensions.x * dimensions.z || pixel.y >= dimensions.y) return 0.0;
    if (probeParameters.x < 0.5 || probeParameters.w < 2.5) return 0.0;
    int3 coordinate = int3(column % dimensions.x, pixel.y, column / dimensions.x);
    uint rayCount = clamp((uint)probeLayout.y, 1u, 32u);
    uint rayColumn = column;
    if (probeSchedule.x > 1.5)
    {
        uint group = column >> 2u;
        uint relative = (column & 3u) ^ ScreenProbePhase(group, pixel.y);
        rayColumn = group * 2u + (relative >> 1u);
        // A scheduled miss still writes alpha 1.  Alpha 0 means no job ran.
        if ((relative & 1u) != 0u ||
            probeRayRadiance.Load(int3(rayColumn, pixel.y * rayCount, 0)).a == 0.0)
            return probeAtlasRead.Load(int3(pixel, 0));
    }
    float3 estimate = 0.0;
    float3 dcEstimate = 0.0;
    bool variability = coefficient == 1u && probeParameters.w > 2.5 && probeParameters.w < 3.5;
    // All SH coefficients reuse the same incident radiance samples.
    [loop] for (uint ray = 0; ray < rayCount; ++ray)
    {
        float3 direction = ScreenProbeDirection(ray, rayCount, coordinate, (uint)probeLayout.w);
        float3 radiance = probeRayRadiance.Load(int3(rayColumn, pixel.y * rayCount + ray, 0)).rgb;
        estimate += radiance * ProbeSHBasis(coefficient, direction);
        if (variability) dcEstimate += radiance;
    }
    float3 current = clamp(estimate * (4.0 * PI / float(rayCount)), -65504.0, 65504.0);
    float window = 1.0;
    float change = 1.0;
    if (probeParameters.y > 0.5)
    {
        float4 previous = probeAtlasRead.Load(int3(pixel, 0));
        float4 dc = variability ? probeAtlasRead.Load(int3(pixel.x - 1u, pixel.y, 0)) : previous;
        window = ProbeUpdateWindow(probeGrid.w, dc.a, probeCellSize.w);
        // A changed view changes which donors exist in the G-buffer. Keep
        // at most two previous updates' worth of the old visibility.
        if (probeCellSize.w < 0.5) window = min(window, 3.0);
        current = lerp(previous.rgb, current, rcp(window));
        if (variability)
        {
            float3 freshDc = dcEstimate * ((4.0 * PI * 0.2820947918) / float(rayCount));
            float relativeChange = length(freshDc - dc.rgb) /
                (window * max(max(length(freshDc), length(dc.rgb)), 0.02));
            change = lerp(previous.a, relativeChange, 0.25);
        }
    }
    return float4(current, variability ? change : window);
}

bool ProbeFilterCrossesVisibleSurface(float3 center, float3 neighbor, float2 viewportSize)
{
    int3 midpointPixel;
    if (!ProjectProbeSample((center + neighbor) * 0.5, viewportSize, midpointPixel)) return false;
    float4 plane = positionBuffer.Load(midpointPixel);
    if (plane.w < 0.5) return false;
    float3 normal = normalBuffer.Load(midpointPixel).xyz;
    float centerSide = dot(normal, center - plane.xyz);
    float neighborSide = dot(normal, neighbor - plane.xyz);
    // Reject only when the centers bracket the plane. Validate the resulting
    // crossing against the visible G-buffer surface, not an infinite plane.
    if (centerSide * neighborSide >= 0.0) return false;
    float3 crossing = lerp(center, neighbor, centerSide / (centerSide - neighborSide));
    int3 crossingPixel;
    if (!ProjectProbeSample(crossing, viewportSize, crossingPixel)) return false;
    float4 visible = positionBuffer.Load(crossingPixel);
    if (visible.w < 0.5) return false;
    float3 visibleNormal = normalBuffer.Load(crossingPixel).xyz;
    if (dot(visibleNormal, normal) < 0.9) return false;
    float footprint = CameraPixelFootprint(crossing, viewportSize.y);
    float viewCosine = max(0.15, abs(dot(visibleNormal, CameraViewDirection(crossing))));
    float precision = dot(abs(visible.xyz), abs(visibleNormal)) / 1024.0;
    float3 delta = visible.xyz - crossing;
    return abs(dot(visibleNormal, delta)) <= max(footprint, precision) &&
        abs(dot(cameraForward.xyz, delta)) <= (footprint + precision) / viewCosine;
}

float4 PSProbeSpatialFilter(FullscreenOutput input) : SV_TARGET
{
    uint2 pixel = uint2(input.position.xy);
    uint3 dimensions = uint3(probeGrid.xyz);
    uint coefficientCount = probeLayout.x > 1.5 ? 9u : 4u;
    uint coefficient = pixel.x % coefficientCount;
    uint column = pixel.x / coefficientCount;
    if (column >= dimensions.x * dimensions.z || pixel.y >= dimensions.y) return 0.0;
    float4 center = probeAtlasRead.Load(int3(pixel, 0));
    bool variability = coefficient == 1u && probeParameters.w > 2.5 && probeParameters.w < 3.5;
    // Filter window and validity must be identical for every SH band.
    float centerWindow = variability ? probeAtlasRead.Load(int3(pixel.x - 1u, pixel.y, 0)).a : center.a;
    if (centerWindow < 0.5) return center;

    int3 coordinate = int3(column % dimensions.x, pixel.y, column / dimensions.x);
    float3 centerPosition = probeOrigin.xyz + (float3(coordinate) + 0.5) * probeCellSize.xyz;
    float2 viewportSize = max(giViewport.yz, 1.0);
    float3 sum = center.rgb;
    float weights = 1.0;
    bool screenFallback = probeParameters.w > 3.5;
    // A dense probe volume can share a broader footprint without spreading
    // radiance across a large fraction of the scene. Limit each axis
    // independently so a shallow grid retains its contact lighting.
    int3 radius = screenFallback ? int3(1, 1, 1) : clamp(int3(dimensions) / 8, 1, 2);
    [loop] for (int y = -radius.y; y <= radius.y; ++y)
    [loop] for (int z = -radius.z; z <= radius.z; ++z)
    [loop] for (int x = -radius.x; x <= radius.x; ++x)
    {
        int3 offset = int3(x, y, z);
        int3 neighbor = coordinate + offset;
        if (all(offset == 0) || any(neighbor < 0) || any(neighbor >= int3(dimensions))) continue;
        int distanceSquared = x * x + y * y + z * z;
        if (distanceSquared > 6) continue;
        float4 donor = probeAtlasRead.Load(int3(
            (neighbor.x + neighbor.z * dimensions.x) * coefficientCount + coefficient, neighbor.y, 0));
        // Full initialization populates every active software probe before
        // this pass. Coefficient 1 alpha may be zero for settled probes.
        if (!variability && donor.a < 0.5) continue;
        float3 neighborPosition = centerPosition + float3(offset) * probeCellSize.xyz;
        if (ProbeFilterCrossesVisibleSurface(centerPosition, neighborPosition, viewportSize)) continue;

        // Filter signed SH coefficients linearly with the same geometric
        // weight for all bands. The center retains its own sample count.
        float weight = screenFallback ? rcp(1.0 + float(distanceSquared)) :
            exp2(-0.38 * float(distanceSquared));
        sum += donor.rgb * weight;
        weights += weight;
    }
    // The coarser SSGI fallback covers a larger world-space area per cell.
    // Keep its footprint stable as the screen-space history converges.
    float strength = screenFallback ? 0.8 : lerp(1.0, 0.9, smoothstep(4.0, 48.0, centerWindow));
    return float4(lerp(center.rgb, sum / weights, strength), centerWindow);
}

struct ProbeDebugVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 instancePosition : TEXCOORD1;
    float2 atlas : TEXCOORD2;
    float2 radius : TEXCOORD3;
};

struct ProbeDebugOutput
{
    float4 position : SV_POSITION;
    float3 worldNormal : TEXCOORD1;
    nointerpolation int2 probeCoordinate : TEXCOORD2;
};

ProbeDebugOutput VSProbeDebug(ProbeDebugVertex input)
{
    ProbeDebugOutput output;
    float3 worldPosition = input.instancePosition + input.position * input.radius.x;
    float4 clip = mul(float4(worldPosition, 1.0), viewProjection);
    if (clip.w <= 0.0001)
    {
        output.position = float4(2.0, 2.0, 2.0, 1.0);
        output.worldNormal = input.normal;
        output.probeCoordinate = 0;
        return output;
    }
    clip.z -= 0.0001 * clip.w;
    output.position = clip;
    output.worldNormal = input.normal;
    output.probeCoordinate = int2(input.atlas);
    return output;
}

float4 PSProbeDebug(ProbeDebugOutput input) : SV_TARGET
{
    float3 normal = normalize(input.worldNormal);
    int stride = probeLayout.x > 1.5 ? 9 : 4;
    int2 slot = int2(input.probeCoordinate.x * stride, input.probeCoordinate.y);
    BlendedProbeSH sh = (BlendedProbeSH)0;
    AddProbeSH(sh, slot, probeAtlasRead.Load(int3(slot, 0)).rgb, 1.0, stride, normal);
    float3 irradiance = EvaluateProbeSH(sh, normal, stride);
    // White diffuse receiver with no direct light, using this probe alone.
    // A faint neutral shape keeps empty probes visible against a black scene.
    float outline = 0.02 + 0.03 * saturate(dot(normal, normalize(float3(0.35, 0.8, 0.45))));
    return float4(max(DisplayColor(max(irradiance, 0.0)), outline), 1.0);
}
