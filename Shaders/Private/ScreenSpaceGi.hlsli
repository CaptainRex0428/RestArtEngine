// Deferred screen-space traces and reconstruction shared by SSGI and DDGI.
// SSGI hits sample outgoing direct diffuse radiance. DDGI screen reflections
// sample the raster lighting base, including direct specular. Both are linear HDR.
// SSGI history stores outgoing indirect diffuse radiance and camera depth for
// temporal accumulation and an attenuated secondary bounce.
int2 SsgiTraceSize()
{
    return int2(ceil(giViewport.yz / max(giTrace.y, 1.0)));
}

int3 SsgiPixelAt(float2 localUv)
{
    return int3(int2(localUv * giViewport.yz) + int2(giTrace.zw), 0);
}

int3 SsgiTraceReceiver(int2 tracePixel)
{
    return SsgiPixelAt((float2(tracePixel) + 0.5) / float2(SsgiTraceSize()));
}

bool SsgiProjectUv(float4 clip, out float2 uv)
{
    uv = 0.0;
    if (clip.w <= 0.0001 || clip.z < 0.0 || clip.z > clip.w) return false;
    uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
    return all(uv >= 0.0) && all(uv < 1.0);
}

bool SsgiProject(float4 clip, out int3 pixel)
{
    float2 uv;
    bool visible = SsgiProjectUv(clip, uv);
    pixel = SsgiPixelAt(uv);
    return visible;
}

float SsgiHistoryDepth(float4 clip)
{
    return cameraForward.w > 0.5
        ? cameraLensParams.x + clip.z * (cameraLensParams.y - cameraLensParams.x)
        : clip.w;
}

float SsgiReadHistory(int3 pixel, float footprint, bool filterReceiver,
    out float3 radiance, out float motionPixels)
{
    radiance = 0.0;
    motionPixels = 0.0;
    if (giHistory.x < 0.5) return 0.0;
    float3 previousWorld = motionBuffer.Load(pixel).xyz;
    float4 clip = mul(float4(previousWorld, 1.0), giPreviousViewProjection);
    float2 uv;
    if (!SsgiProjectUv(clip, uv)) return 0.0;
    float depth = SsgiHistoryDepth(clip);
    float tolerance = max(depth * 0.003, footprint * 1.5);
    if (!filterReceiver)
    {
        // The secondary bounce only needs one lookup per ray hit.
        float4 hit = ssgiHistoryIndirect.Load(SsgiPixelAt(uv));
        if (hit.a <= 0.0 || abs(hit.a - depth) > tolerance) return 0.0;
        radiance = hit.rgb;
        return 1.0;
    }

    float2 coordinate = uv * giViewport.yz + giTrace.zw - 0.5;
    motionPixels = length(coordinate - float2(pixel.xy));
    int2 base = int2(floor(coordinate));
    float2 fraction = frac(coordinate);
    int2 first = int2(giTrace.zw);
    int2 last = first + int2(giViewport.yz) - 1;
    float coverage = 0.0;
    [unroll] for (int y = 0; y <= 1; ++y)
    [unroll] for (int x = 0; x <= 1; ++x)
    {
        int2 tap = clamp(base + int2(x, y), first, last);
        float4 history = ssgiHistoryIndirect.Load(int3(tap, 0));
        if (history.a <= 0.0 || abs(history.a - depth) > tolerance) continue;
        float weight = (x ? fraction.x : 1.0 - fraction.x) * (y ? fraction.y : 1.0 - fraction.y);
        radiance += history.rgb * weight;
        coverage += weight;
    }
    radiance /= max(coverage, 1e-5);
    return coverage;
}

float SsgiStepDistance(float t, float radius)
{
    return radius * t * (0.35 + 0.65 * t);
}

bool SsgiMarch(float3 position, float3 normal, float3 direction,
    float phase, out int3 hitPixel, out float hitRadiusFraction)
{
    hitPixel = 0;
    hitRadiusFraction = 0.0;
    uint steps = max((uint)giViewport.w, 1u);
    float radius = max(giScreenSpace.y, 0.05);
    float thickness = max(giScreenSpace.z, RAY_EPSILON);
    float bias = max(RAY_EPSILON, 0.2 * CameraPixelFootprint(position, giViewport.z));
    float3 origin = position + normal * bias;
    float segmentEnd = 0.0;
    [loop] for (uint step = 1; step <= steps; ++step)
    {
        float t = (float(step - 1u) + phase) / float(steps);
        float travel = SsgiStepDistance(t, radius);
        float segmentStart = segmentEnd;
        // Each diffuse candidate owns a full interval around the sample. A
        // surface can intersect before or after the point used to find it.
        // Adjacent intervals share one boundary and cover the whole radius.
        segmentEnd = step == steps ? radius : SsgiStepDistance(t + 0.5 / float(steps), radius);
        float3 samplePosition = origin + direction * travel;
        int3 samplePixel;
        if (!SsgiProject(mul(float4(samplePosition, 1.0), viewProjection), samplePixel)) break;
        float4 surface = positionBuffer.Load(samplePixel);
        if (surface.w < 0.5) continue;

        // Intersect the ray segment with the visible surface's local plane,
        // then reproject the intersection to reject silhouette/depth jumps.
        float3 sampleNormal = normalBuffer.Load(samplePixel).xyz;
        float approach = dot(sampleNormal, direction);
        if (approach >= -0.05) continue;
        float hitTravel = dot(sampleNormal, surface.xyz - origin) / approach;
        if (hitTravel <= bias || hitTravel < segmentStart || hitTravel > segmentEnd) continue;
        float3 hitPosition = origin + direction * hitTravel;
        int3 refinedPixel;
        if (!SsgiProject(mul(float4(hitPosition, 1.0), viewProjection), refinedPixel)) continue;
        float4 refinedSurface = positionBuffer.Load(refinedPixel);
        if (refinedSurface.w < 0.5) continue;
        float3 refinedNormal = normalBuffer.Load(refinedPixel).xyz;
        if (dot(refinedNormal, sampleNormal) < 0.9 || dot(refinedNormal, direction) >= -0.05) continue;
        float3 delta = refinedSurface.xyz - hitPosition;
        float footprint = CameraPixelFootprint(hitPosition, giViewport.z);
        float viewSlope = rcp(max(0.15, abs(dot(refinedNormal, CameraViewDirection(hitPosition)))));
        if (abs(dot(delta, refinedNormal)) > thickness ||
            abs(dot(delta, cameraForward.xyz)) > thickness + footprint * viewSlope) continue;
        hitPixel = refinedPixel;
        hitRadiusFraction = hitTravel / radius;
        return true;
    }
    return false;
}

float2 SsgiSequence(uint2 pixel)
{
    // Integer mixing keeps the sample rotation stable over the full viewport.
    uint key = pixel.x * 1664525u ^ pixel.y * 1013904223u;
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    return (float2(key & 65535u, key >> 16) + 0.5) / 65536.0;
}

float3 SsgiTangent(float3 normal)
{
    return normalize(cross(abs(normal.y) > 0.9 ? float3(1, 0, 0) : float3(0, 1, 0), normal));
}

float4 PSSsgiDiffuseFilter(FullscreenOutput input) : SV_TARGET
{
    int2 tracePixel = int2(input.position.xy);
    int2 traceSize = SsgiTraceSize();
    int3 receiver = SsgiTraceReceiver(tracePixel);
    float4 position = positionBuffer.Load(receiver);
    float4 normalMetallic = normalBuffer.Load(receiver);
    if (position.w < 0.5 || normalMetallic.w >= 1.0) return 0.0;

    float3 normal = normalize(normalMetallic.xyz);
    float footprint = CameraPixelFootprint(position.xyz, giViewport.z) * max(giTrace.y, 1.0);
    float planeTolerance = max(0.01, 0.65 * footprint);
    float depthTolerance = 5.0 * footprint /
        max(abs(dot(normal, CameraViewDirection(position.xyz))), 0.2);
    // Alpha holds the relative standard error of this frame's ray estimate.
    // A quiet tap next to a noisy one still needs a broad footprint.
    float4 center = ssgiRawIndirect.Load(int3(tracePixel, 0));
    // A moving donor can change a stationary receiver. Use the full current
    // footprint on scene changes, then narrow it as local history recovers.
    int2 last = traceSize - 1;
    float noise = center.a;
    noise = max(noise, ssgiRawIndirect.Load(int3(min(tracePixel + int2(1, 0), last), 0)).a);
    noise = max(noise, ssgiRawIndirect.Load(int3(min(tracePixel + int2(0, 1), last), 0)).a);
    float3 previousRadiance;
    float motionPixels;
    float coverage = SsgiReadHistory(receiver, footprint, true, previousRadiance, motionPixels);
    float trust = coverage * (1.0 - smoothstep(0.75, 4.0, motionPixels));
    trust *= 1.0 - smoothstep(0.25, 0.5, giHistory.z);
    float broad = max(1.0 - trust, smoothstep(0.04, 0.30, noise));
    int radius = broad < 0.01 ? 1 : 3;
    static const float gaussian[4] = {1.0, 0.895, 0.641, 0.411};
    static const float narrow[4] = {1.0, 0.5, 0.0, 0.0};
    float3 sum = center.rgb;
    float weightSum = 1.0;
    [loop] for (int y = -radius; y <= radius; ++y)
    [loop] for (int x = -radius; x <= radius; ++x)
    {
        if (x == 0 && y == 0) continue;
        int2 tap = tracePixel + int2(x, y);
        if (any(tap < 0) || any(tap >= traceSize)) continue;
        int3 tapReceiver = SsgiTraceReceiver(tap);
        float4 tapPosition = positionBuffer.Load(tapReceiver);
        if (tapPosition.w < 0.5) continue;
        float3 delta = tapPosition.xyz - position.xyz;
        float planeWeight = saturate(1.0 - abs(dot(delta, normal)) / planeTolerance);
        if (planeWeight <= 0.0 || abs(dot(delta, cameraForward.xyz)) > depthTolerance) continue;
        float4 tapNormalMetallic = normalBuffer.Load(tapReceiver);
        float cosine = dot(normal, tapNormalMetallic.xyz);
        if (cosine <= 0.94 || tapNormalMetallic.w >= 1.0) continue;
        float normalWeight = smoothstep(0.94, 0.995, cosine);
        float kernel = lerp(narrow[abs(x)] * narrow[abs(y)], gaussian[abs(x)] * gaussian[abs(y)], broad);
        float weight = kernel * planeWeight * normalWeight;
        sum += ssgiRawIndirect.Load(int3(tap, 0)).rgb * weight;
        weightSum += weight;
    }
    return float4(sum / weightSum, center.a);
}

float4 PSScreenSpaceGi(FullscreenOutput input) : SV_TARGET
{
    if (giScreenSpace.x < 0.5) return 0.0;
    int2 tracePixel = int2(input.position.xy);
    int3 pixel = SsgiTraceReceiver(tracePixel);
    float4 position = positionBuffer.Load(pixel);
    float4 normalAndMetallic = normalBuffer.Load(pixel);
    if (position.w < 0.5 || normalAndMetallic.w >= 1.0) return 0.0;
    float3 normal = normalize(normalAndMetallic.xyz);
    float3 tangent = SsgiTangent(normal);
    float3 bitangent = cross(normal, tangent);
    uint rayCount = clamp((uint)giTrace.x, 1u, 8u);
    // Four adjacent trace pixels cover complementary parts of the hemisphere.
    // Scramble each tile and rotate over time so accumulation fills its strata.
    uint2 cell = uint2(tracePixel);
    uint stratum = (cell.x & 1u) + 2u * (cell.y & 1u);
    float frame = float((uint)giHistory.y & 1023u);
    float2 rotation = SsgiSequence(cell >> 1);
    rotation = frac(rotation + frame * float2(0.75487767, 0.56984029));
    rotation = frac(rotation + 0.25 * float2(stratum, (3u * stratum) & 3u));
    float marchRotation = frac(SsgiSequence(cell).y + frame * 0.41421356);
    float3 sum = 0.0;
    float screenWeight = 0.0;
    float secondMoment = 0.0;
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    [loop] for (uint index = 0; index < rayCount; ++index)
    {
        float phi = frac(rotation.x + float(index) * 0.61803399) * (2.0 * PI);
        float cosTheta = sqrt((float(index) + rotation.y) / float(rayCount));
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        float3 direction = (tangent * cos(phi) + bitangent * sin(phi)) * sinTheta + normal * cosTheta;
        int3 hitPixel;
        float hitRadiusFraction;
        float phase = frac(marchRotation + float(index) * 0.73205081);
        if (SsgiMarch(position.xyz, normal, direction, phase, hitPixel, hitRadiusFraction))
        {
            float weight = 1.0 - smoothstep(0.75, 1.0, hitRadiusFraction);
            screenWeight += weight;
            float3 radiance = probeColorSource.Load(hitPixel).rgb;
            float3 bounce;
            float unusedMotion;
            float footprint = CameraPixelFootprint(positionBuffer.Load(hitPixel).xyz, giViewport.z);
            if (SsgiReadHistory(hitPixel, footprint, false, bounce, unusedMotion) > 0.0)
                radiance += bounce * 0.35;
            sum += radiance * weight;
            float luminance = dot(radiance, luma);
            secondMoment += luminance * luminance * weight;
        }
    }
    // Crossfade at the radius into the same indirect term used by ray misses.
    float missingWeight = float(rayCount) - screenWeight;
    if (probeParameters.x > 0.5 && missingWeight > 0.0)
    {
        float3 fallback = ProbeIrradiance(position.xyz, normal);
        sum += fallback * missingWeight;
        float fallbackLuma = dot(fallback, luma);
        secondMoment += fallbackLuma * fallbackLuma * missingWeight;
    }
    // Cosine-weighted samples estimate E / pi. Weight each attempted ray
    // equally, including misses that resolve to the chosen fallback.
    float3 estimate = sum / float(rayCount);
    float mean = dot(estimate, luma);
    float variance = max(secondMoment / float(rayCount) - mean * mean, 0.0);
    float error = rayCount > 1 ? sqrt(variance / float(rayCount - 1)) / max(mean, 0.002) : 1.0;
    return float4(clamp(estimate, 0.0, 65504.0), saturate(error));
}

bool SsrTrace(float3 position, float3 normal, float3 direction, out int3 hitPixel,
    out float coverage)
{
    hitPixel = 0;
    coverage = 0.0;
    uint steps = max(1u, (uint)giViewport.w);
    float radius = max(giScreenSpace.y, 0.05);
    float thickness = max(giScreenSpace.z, RAY_EPSILON);
    float bias = max(RAY_EPSILON, 0.2 * CameraPixelFootprint(position, giViewport.z));
    float3 origin = position + normal * bias;
    float4 start = mul(float4(origin, 1.0), viewProjection);
    float4 ray = mul(float4(direction, 0.0), viewProjection);
    float4 end = start + ray * radius;
    // Clip the distance in homogeneous coordinates. The last sample remains
    // inside the image even when the unbounded ray would leave the viewport.
    float3 fromMin = float3(start.w + start.xy, start.z);
    float3 toMin = float3(end.w + end.xy, end.z);
    float3 fromMax = start.w - start.xyz;
    float3 toMax = end.w - end.xyz;
    float3 minClip = lerp(1.0.xxx, fromMin / max(fromMin - toMin, 1e-5), 1.0 - step(0.0, toMin));
    float3 maxClip = lerp(1.0.xxx, fromMax / max(fromMax - toMax, 1e-5), 1.0 - step(0.0, toMax));
    float3 boundary = min(minClip, maxClip);
    float limit = radius * saturate(min(boundary.x, min(boundary.y, boundary.z)));
    if (limit <= bias) return false;

    float2 pixelMargin = 0.5 / giViewport.yz;
    float previousTravel = 0.0;
    [loop] for (uint step = 1; step <= steps; ++step)
    {
        float travel = SsgiStepDistance(float(step) / float(steps), limit);
        float intervalStart = previousTravel;
        previousTravel = travel;
        // Move the last sample half a pixel inside the image to sample the
        // actual boundary texel when the clipped ray ends at uv == 1.
        float4 clip = start + ray * travel;
        if (clip.w <= 0.0001) break;
        float2 uv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        int3 samplePixel = SsgiPixelAt(clamp(uv, pixelMargin, 1.0 - pixelMargin));
        float4 surface = positionBuffer.Load(samplePixel);
        if (surface.w < 0.5) continue;
        float3 sampleNormal = normalBuffer.Load(samplePixel).xyz;
        float approach = dot(sampleNormal, direction);
        if (approach >= -0.025) continue;
        float hitTravel = dot(sampleNormal, surface.xyz - origin) / approach;
        // A neighboring texel can identify the same plane on either side of
        // the sample. Overlapping intervals prevent step-shaped hit gaps.
        float overlap = 0.65 * (travel - intervalStart);
        if (hitTravel <= bias || hitTravel > limit ||
            hitTravel < intervalStart - overlap || hitTravel > travel + overlap) continue;
        float3 hit = origin + direction * hitTravel;
        float2 hitUv;
        if (!SsgiProjectUv(start + ray * hitTravel, hitUv)) continue;
        int3 refinedPixel = SsgiPixelAt(hitUv);
        float4 refinedSurface = positionBuffer.Load(refinedPixel);
        if (refinedSurface.w < 0.5) continue;
        float3 refinedNormal = normalBuffer.Load(refinedPixel).xyz;
        if (dot(refinedNormal, sampleNormal) < 0.95 || dot(refinedNormal, direction) >= -0.025) continue;
        float footprint = CameraPixelFootprint(hit, giViewport.z);
        float precision = dot(abs(refinedSurface.xyz), abs(refinedNormal)) / 1024.0;
        float cameraSlope = rcp(max(0.12, abs(dot(refinedNormal, CameraViewDirection(hit)))));
        float3 separation = refinedSurface.xyz - hit;
        float tolerance = min(thickness, max(1.5 * footprint, precision));
        if (abs(dot(separation, refinedNormal)) > tolerance ||
            abs(dot(separation, cameraForward.xyz)) > (tolerance + footprint) * cameraSlope) continue;
        float2 edge = min(hitUv, 1.0 - hitUv) * giViewport.yz;
        float border = smoothstep(0.0, 6.0, min(edge.x, edge.y));
        coverage = border * (1.0 - smoothstep(0.9, 1.0, hitTravel / radius));
        hitPixel = refinedPixel;
        return true;
    }
    return false;
}

float4 PSScreenSpaceGiSpecular(FullscreenOutput input) : SV_TARGET
{
    int2 tracePixel = int2(input.position.xy);
    int3 pixel = SsgiTraceReceiver(tracePixel);
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5) return 0.0;
    float3 normal = normalize(normalBuffer.Load(pixel).xyz);
    float roughness = albedoBuffer.Load(pixel).a;
    float3 reflection = reflect(-CameraViewDirection(position.xyz), normal);
    // A single glossy ray produces high-contrast hit/miss speckle on
    // rough walls. Integrate a small stratified disk within this frame.
    uint2 cell = uint2(tracePixel);
    uint stratum = (cell.x & 1u) + 2u * (cell.y & 1u);
    float2 phase = SsgiSequence(cell >> 1u) + float(stratum) * 0.25;
    float3 tangent = SsgiTangent(reflection);
    float3 bitangent = cross(reflection, tangent);
    uint samples = roughness >= 0.45 ? 4u : (roughness >= 0.25 ? 2u : 1u);
    float coverage = 0.0;
    float3 bounced = 0.0;
    [loop] for (uint i = 0; i < samples; ++i)
    {
        float3 direction = reflection;
        if (roughness > 0.01)
        {
            float angle = (phase.x + float(i) / float(samples)) * (2.0 * PI);
            float radius = sqrt((frac(phase.y) + float((i + stratum) % samples)) / float(samples));
            float3 spread = (tangent * cos(angle) + bitangent * sin(angle)) * radius;
            float3 perturbed = normalize(reflection + spread * roughness * roughness * 0.45);
            if (dot(normal, perturbed) > 0.0) direction = perturbed;
        }
        int3 hitPixel;
        float hitCoverage;
        if (SsrTrace(position.xyz, normal, direction, hitPixel, hitCoverage))
        {
            bounced += probeColorSource.Load(hitPixel).rgb * hitCoverage;
            coverage += hitCoverage;
        }
    }
    bounced /= float(samples);
    coverage /= float(samples);
    // Misses keep the environment lobe of the full-resolution receiver. SH
    // can supplement a broad reflection with low-frequency scene radiance.
    if (coverage < 1.0 && roughness > 0.25 && probeParameters.w > 3.5 && probeParameters.w < 4.5)
        bounced += ProbeIrradiance(position.xyz, reflection) * roughness * roughness * (1.0 - coverage);
    return float4(bounced, coverage);
}

struct SsgiSpatialResolve
{
    float3 diffuse;
    float3 diffuseMin;
    float3 diffuseMax;
    float3 specular;
    float specularMiss;
};

SsgiSpatialResolve SsgiResolveSpatial(int2 pixel, float3 position, float3 normal, float4 albedo,
    float metallic)
{
    SsgiSpatialResolve result = (SsgiSpatialResolve)0;
    bool diffuseEnabled = giScreenSpace.x > 0.5 && metallic < 1.0;
    bool specularEnabled = giHistory.w > 0.5 && giHistory.w < 1.5;
    if (!diffuseEnabled && !specularEnabled) return result;

    int2 traceSize = SsgiTraceSize();
    float2 localUv = (float2(pixel) + 0.5 - giTrace.zw) / giViewport.yz;
    float2 coordinate = localUv * float2(traceSize) - 0.5;
    int2 nearest = int2(floor(coordinate + 0.5));
    float2 delta = coordinate - float2(nearest);
    // A quadratic B-spline shares nine half-resolution estimates across a
    // planar receiver. Mirror reflections use the narrower bilinear response.
    float2 spline[3];
    spline[0] = 0.5 * (0.5 - delta) * (0.5 - delta);
    spline[1] = 0.75 - delta * delta;
    spline[2] = 0.5 * (0.5 + delta) * (0.5 + delta);
    float footprint = CameraPixelFootprint(position, giViewport.z) * max(giTrace.y, 1.0);
    float planeTolerance = max(0.01, 0.65 * footprint);
    float3 low = 65504.0;
    float3 high = 0.0;
    float diffuseWeight = 0.0;
    float specularWeight = 0.0;
    // Broad lobes already integrate nearby directions. Share estimates from
    // the stratified trace tiles while retaining the tight mirror footprint.
    float roughSpecular = smoothstep(0.20, 0.70, albedo.a);
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x)
    {
        int2 offset = int2(x, y);
        float2 bilinear = saturate(1.0 - abs(delta - float2(offset)));
        float roughTap = spline[x + 1].x * spline[y + 1].y;
        float specularTap = lerp(bilinear.x * bilinear.y, roughTap, roughSpecular);
        if (!diffuseEnabled && specularTap <= 0.0) continue;
        int2 tracePixel = clamp(nearest + offset, int2(0, 0), traceSize - 1);
        int3 receiver = SsgiTraceReceiver(tracePixel);
        float4 tapPosition = positionBuffer.Load(receiver);
        if (tapPosition.w < 0.5) continue;
        float4 tapNormal = normalBuffer.Load(receiver);
        float cosine = dot(normal, normalize(tapNormal.xyz));
        float3 separation = tapPosition.xyz - position;
        if (cosine < 0.88 || abs(dot(separation, normal)) > planeTolerance) continue;
        if (diffuseEnabled && tapNormal.w < 1.0)
        {
            float weight = spline[x + 1].x * spline[y + 1].y;
            weight *= cosine * cosine;
            float3 sample = ssgiRawIndirect.Load(int3(tracePixel, 0)).rgb;
            result.diffuse += sample * weight;
            diffuseWeight += weight;
            low = min(low, sample);
            high = max(high, sample);
        }
        if (specularEnabled && specularTap > 0.0 && abs(tapNormal.w - metallic) < 0.2 &&
            abs(albedoBuffer.Load(receiver).a - albedo.a) < 0.12)
        {
            float4 trace = ssgiSpecularRaw.Load(int3(tracePixel, 0));
            float reflectedWeight = specularTap * cosine * cosine;
            result.specular += trace.rgb * reflectedWeight;
            result.specularMiss += (1.0 - trace.a) * reflectedWeight;
            specularWeight += reflectedWeight;
        }
    }
    if (diffuseEnabled)
    {
        if (diffuseWeight > 1e-5)
        {
            result.diffuse /= diffuseWeight;
            result.diffuseMin = low;
            result.diffuseMax = high;
        }
        else
        {
            // Thin silhouettes may have no matching trace representatives.
            if (probeParameters.x > 0.5) result.diffuse = ProbeIrradiance(position, normal);
            result.diffuseMin = result.diffuseMax = result.diffuse;
        }
    }
    if (specularWeight > 1e-5)
    {
        result.specular /= specularWeight;
        result.specularMiss /= specularWeight;
    }
    else if (specularEnabled)
        result.specularMiss = 1.0;
    return result;
}

// Reconstruct the broad lobe from neighboring DXR results, including their
// hit fractions. All source rays were traced against world geometry.
float4 ResolveDxrReflection(int2 localPixel, float3 position, float3 normal,
    float roughness, float metallic, float3 fresnel)
{
    float4 sum = ssgiSpecularRaw.Load(int3(localPixel, 0));
    if (!UsesWideDxrReflection(roughness, metallic, fresnel)) return sum;
    float weight = 1.0;
    float footprint = CameraPixelFootprint(position, giViewport.z);
    float viewSlope = rcp(max(abs(dot(normal, CameraViewDirection(position))), 0.1));
    [loop] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 localTap = localPixel + int2(x, y);
        if ((x == 0 && y == 0) || any(localTap < 0) || any(localTap >= int2(giViewport.yz))) continue;
        int3 tap = int3(localTap + int2(giTrace.zw), 0);
        float4 surface = positionBuffer.Load(tap);
        float4 normalMetal = normalBuffer.Load(tap);
        float4 albedo = albedoBuffer.Load(tap);
        float3 delta = surface.xyz - position;
        if (surface.w < 0.5 || dot(normal, normalMetal.xyz) < 0.99 ||
            abs(dot(delta, normal)) > footprint || abs(dot(delta, cameraForward.xyz)) > 2.0 * viewSlope * footprint ||
            abs(albedo.a - roughness) > 0.04 || abs(normalMetal.a - metallic) > 0.04) continue;
        float3 f0 = lerp(0.04.xxx, albedo.rgb, saturate(normalMetal.a));
        float3 tapFresnel = Fresnel(saturate(dot(normalMetal.xyz, CameraViewDirection(surface.xyz))), f0);
        if (!UsesWideDxrReflection(albedo.a, normalMetal.a, tapFresnel)) continue;
        sum += ssgiSpecularRaw.Load(int3(localTap, 0));
        weight += 1.0;
    }
    return sum / weight;
}

// Both GI methods reconstruct incident radiance by the same rules. The
// receiver applies its Fresnel once, and only missing rays take environment.
float3 ResolveIndirectReflection(int2 pixel, float3 position, float3 normal, float4 albedo,
    float metallic, float3 view, float3 fresnel, float3 incident, float miss)
{
    if (giHistory.w < 0.5) return 0.0;
    if (giHistory.w > 1.5)
    {
        const int2 localPixel = pixel - int2(giTrace.zw);
        const float4 hardware = ResolveDxrReflection(localPixel, position, normal, albedo.a, metallic, fresnel);
        incident = hardware.rgb;
        miss = 1.0 - hardware.a;
    }
    if (miss > 0.0)
        incident += Environment(reflect(-view, normal), albedo.a) * miss;
    return incident * fresnel * (1.0 - albedo.a * 0.45);
}

struct SSGICompositeOutput
{
    float4 color : SV_TARGET0;
    float4 history : SV_TARGET1;
};
SSGICompositeOutput PSScreenSpaceGiComposite(FullscreenOutput input)
{
    SSGICompositeOutput output;
    int3 pixel = int3(int2(input.position.xy), 0);
    float4 baseline = ssgiDirectCurrent.Load(pixel);
    bool debugIndirect = viewportDebug.z > 10.5 && viewportDebug.z < 11.5;
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5)
    {
        output.color = debugIndirect ? float4(0.0, 0.0, 0.0, 1.0) : baseline;
        output.history = 0.0;
        return output;
    }
    float4 normalAndMetallic = normalBuffer.Load(pixel);
    float3 normal = normalize(normalAndMetallic.xyz);
    float metallic = saturate(normalAndMetallic.a);
    float4 albedo = albedoBuffer.Load(pixel);
    float3 view = CameraViewDirection(position.xyz);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);
    float3 fresnel = Fresnel(saturate(dot(normal, view)), f0);
    float3 diffuseReflectance = saturate(albedo.rgb) * (1.0 - metallic) * (1.0 - saturate(fresnel));
    SsgiSpatialResolve spatial = SsgiResolveSpatial(pixel.xy, position.xyz, normal, albedo, metallic);
    float3 diffuse = spatial.diffuse * diffuseReflectance;
    if (giScreenSpace.x > 0.5 && metallic < 1.0)
    {
        float3 history;
        float motionPixels;
        float coverage = SsgiReadHistory(pixel, CameraPixelFootprint(position.xyz, giViewport.z),
            true, history, motionPixels);
        if (coverage > 0.0)
        {
            // A stationary receiver can legitimately see no bright hits for a
            // frame. Hard clipping that estimate to this frame's min/max would
            // restart accumulation. React faster when the footprint moves or
            // part of the reprojected surface fails its depth check.
            float sceneChange = saturate((giHistory.z - 0.25) * 4.0);
            float instability = max(sceneChange,
                max(1.0 - saturate(coverage), smoothstep(0.75, 4.0, motionPixels)));
            float3 clipped = clamp(history, spatial.diffuseMin * diffuseReflectance,
                spatial.diffuseMax * diffuseReflectance);
            history = lerp(history, clipped, instability);
            float currentWeight = max(giHistory.z, 0.60 * instability);
            diffuse = lerp(diffuse, history, saturate(coverage) * (1.0 - currentWeight));
        }
    }
    float depth = SsgiHistoryDepth(mul(float4(position.xyz, 1.0), viewProjection));
    output.history = float4(diffuse, min(depth, 65504.0));
    float3 specular = ResolveIndirectReflection(pixel.xy, position.xyz, normal, albedo,
        metallic, view, fresnel, spatial.specular, spatial.specularMiss);
    float3 indirect = diffuse * giScreenSpace.w + specular;
    output.color = float4(DisplayColor(debugIndirect ? indirect : baseline.rgb + indirect), 1.0);
    return output;
}
