// Progressive opaque-surface path integrator. This is independent of the
// preview lighting approximation: radiance comes from the directional light
// and traced paths that escape to the environment, never a local ambient term.
// The material model uses Lambert diffuse plus GGX/Trowbridge-Reitz specular,
// separable Smith masking and Schlick Fresnel. Both lobes share one mixture PDF.

uint PathHash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float PathRandom(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return float(PathHash(state) >> 8) * (1.0 / 16777216.0);
}

float3 PathToWorld(float3 localDirection, float3 normal)
{
    float3 tangent = normalize(cross(abs(normal.z) < 0.999
        ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * localDirection.x + bitangent * localDirection.y
        + normal * localDirection.z);
}

float PathLuminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float PathGgxDistribution(float normalHalf, float alphaSquared)
{
    float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 1e-12);
}

float PathSmithG1(float normalDirection, float alphaSquared)
{
    return 2.0 * normalDirection / max(normalDirection
        + sqrt(alphaSquared + (1.0 - alphaSquared) * normalDirection * normalDirection), 1e-8);
}

float3 PathBrdf(float3 normal, float3 view, float3 incoming, Surface material,
    bool includeDiffuse, bool includeSpecular, float diffuseIntensity, out float specularPdf)
{
    specularPdf = 0.0;
    float nv = dot(normal, view);
    float nl = dot(normal, incoming);
    if (nv <= 1e-6 || nl <= 1e-6) return 0.0;
    float3 sum = view + incoming;
    if (dot(sum, sum) <= 1e-12) return 0.0;
    float3 halfway = normalize(sum);
    float nh = saturate(dot(normal, halfway));
    float vh = saturate(dot(view, halfway));
    float alpha = material.roughness * material.roughness;
    float alphaSquared = alpha * alpha;
    float distribution = PathGgxDistribution(nh, alphaSquared);
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    float3 fresnel = Fresnel(vh, f0);
    float3 diffuse = (1.0 - fresnel) * (1.0 - material.metallic) * material.albedo / PI;
    if (!includeDiffuse) diffuse = 0.0;
    diffuse *= diffuseIntensity;
    if (!includeSpecular) return diffuse;
    float geometry = PathSmithG1(nv, alphaSquared) * PathSmithG1(nl, alphaSquared);
    specularPdf = distribution * nh / max(4.0 * vh, 1e-8);
    return diffuse + fresnel * distribution * geometry / max(4.0 * nv * nl, 1e-8);
}

bool PathSampleBrdf(float3 normal, float3 geometricNormal, float3 view,
    Surface material, bool includeDiffuse, bool includeSpecular, float diffuseIntensity,
    inout uint randomState, out float3 incoming, out float3 weight, out float pdf)
{
    incoming = normal;
    weight = 0.0;
    pdf = 0.0;
    // Zero gain has the same lobe/PDF and random sequence as disabling diffuse.
    includeDiffuse = includeDiffuse && diffuseIntensity > 0.0;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    float specularEnergy = includeSpecular
        ? PathLuminance(Fresnel(saturate(dot(normal, view)), f0)) : 0.0;
    float diffuseEnergy = includeDiffuse
        ? PathLuminance(material.albedo) * (1.0 - material.metallic) * diffuseIntensity : 0.0;
    if (specularEnergy + diffuseEnergy <= 1e-8) return false;
    float specularProbability = includeSpecular
        ? (diffuseEnergy <= 1e-8 ? 1.0 : clamp(specularEnergy / (specularEnergy + diffuseEnergy), 0.05, 0.95))
        : 0.0;
    float selection = PathRandom(randomState);
    float u = PathRandom(randomState);
    float v = PathRandom(randomState);
    float phi = 2.0 * PI * v;
    if (selection < specularProbability)
    {
        float alpha = material.roughness * material.roughness;
        float alphaSquared = alpha * alpha;
        float cosine = sqrt((1.0 - u) / max(1.0 + (alphaSquared - 1.0) * u, 1e-8));
        float sine = sqrt(max(0.0, 1.0 - cosine * cosine));
        float3 halfway = PathToWorld(float3(cos(phi) * sine, sin(phi) * sine, cosine), normal);
        incoming = reflect(-view, halfway);
    }
    else
    {
        float radius = sqrt(u);
        incoming = PathToWorld(float3(cos(phi) * radius, sin(phi) * radius, sqrt(1.0 - u)), normal);
    }
    // Smooth normals define the BRDF, but a path cannot leave through the back
    // of the actual triangle. Rejected samples contribute zero; they are not
    // resampled, which preserves the unconditioned mixture PDF.
    float nl = dot(normal, incoming);
    if (nl <= 1e-6 || dot(geometricNormal, incoming) <= 1e-6) return false;
    float specularPdf;
    float3 brdf = PathBrdf(normal, view, incoming, material, includeDiffuse, includeSpecular, diffuseIntensity, specularPdf);
    pdf = specularProbability * specularPdf + (1.0 - specularProbability) * nl / PI;
    if (pdf <= 1e-12) return false;
    weight = brdf * nl / pdf;
    return all(isfinite(weight)) && any(weight > 0.0);
}

struct PathAreaEmitter
{
    float3 center;
    float3 halfU;
    float3 halfV;
    float3 normal;
    float3 radiance;
    float range;
    bool castShadows;
};

bool PathLegacyArea()
{
    // Actor-authored lighting suppresses the whole legacy rig, even if its
    // hidden last light leaves no direct emitters. Legacy b5 retains its MIS.
    return skyColor.w < 0.5 && areaLightCenter.w > 0.5;
}

PathAreaEmitter PathAreaAt(int index)
{
    PathAreaEmitter emitter;
    if (index < 0)
    {
        emitter.center = areaLightCenter.xyz;
        emitter.halfU = areaLightU.xyz;
        emitter.halfV = areaLightV.xyz;
        emitter.normal = normalize(cross(areaLightU.xyz, areaLightV.xyz));
        emitter.radiance = areaLightRadiance.xyz;
        emitter.range = 0.0;
        emitter.castShadows = true;
    }
    else
    {
        SceneLight source = sceneLights[index];
        emitter.center = source.positionType.xyz;
        emitter.halfU = source.axisUWidth.xyz * source.axisUWidth.w * 0.5;
        emitter.halfV = source.axisVHeight.xyz * source.axisVHeight.w * 0.5;
        // Authored local X/Y has a +Z cross product, while the emitting side
        // is -Z; use the explicit shared direction instead of cross(U,V).
        emitter.normal = source.directionRange.xyz;
        emitter.radiance = max(source.colorIntensity.rgb, 0.0) * max(source.colorIntensity.w, 0.0);
        emitter.range = source.directionRange.w;
        emitter.castShadows = source.spotShadow.z > 0.5;
    }
    return emitter;
}

bool PathAreaHitOne(PathAreaEmitter emitter, float3 origin, float3 direction, out float distance)
{
    distance = 1e20;
    float3 crossed = cross(emitter.halfU, emitter.halfV);
    float denominator = dot(crossed, direction);
    if (abs(denominator) < 1e-10) return false;
    float candidate = dot(crossed, emitter.center - origin) / denominator;
    if (candidate <= RAY_EPSILON) return false;
    float3 offset = origin + candidate * direction - emitter.center;
    if (abs(dot(offset, emitter.halfU)) > dot(emitter.halfU, emitter.halfU) ||
        abs(dot(offset, emitter.halfV)) > dot(emitter.halfV, emitter.halfV)) return false;
    distance = candidate;
    return true;
}

bool PathAreaHit(float3 origin, float3 direction, out float distance, out int emitterIndex)
{
    distance = 1e20;
    emitterIndex = -2;
    if (PathLegacyArea())
    {
        if (PathAreaHitOne(PathAreaAt(-1), origin, direction, distance)) emitterIndex = -1;
    }
    else
    {
        [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
        {
            if (sceneLights[index].positionType.w != 2.0) continue;
            float candidate;
            if (PathAreaHitOne(PathAreaAt((int)index), origin, direction, candidate) && candidate < distance)
            {
                distance = candidate;
                emitterIndex = (int)index;
            }
        }
    }
    return emitterIndex != -2;
}

float PathAreaPdf(PathAreaEmitter emitter, float3 direction, float distance)
{
    float3 crossed = cross(emitter.halfU, emitter.halfV);
    float area = 4.0 * length(crossed);
    float cosine = dot(emitter.normal, -direction);
    return cosine > 1e-8 && area > 1e-8 ? distance * distance / (area * cosine) : 0.0;
}

float PathAreaRange(PathAreaEmitter emitter, float distance)
{
    if (emitter.range <= 0.0) return 1.0;
    float relative = distance / emitter.range;
    float fade = saturate(1.0 - relative * relative * relative * relative);
    return fade * fade;
}

float PathFullBrdfPdf(float3 normal, float3 view, float3 incoming, Surface material, float specularPdf)
{
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    float specularEnergy = PathLuminance(Fresnel(saturate(dot(normal, view)), f0));
    float diffuseEnergy = PathLuminance(material.albedo) * (1.0 - material.metallic);
    float probability = diffuseEnergy <= 1e-8 ? 1.0
        : clamp(specularEnergy / max(specularEnergy + diffuseEnergy, 1e-8), 0.05, 0.95);
    return probability * specularPdf + (1.0 - probability) * saturate(dot(normal, incoming)) / PI;
}

float PathMisWeight(float sampledPdf, float otherPdf)
{
    // Ratio form avoids squaring large solid-angle densities near grazing rays.
    float ratio = otherPdf / max(sampledPdf, 1e-30);
    return 1.0 / (1.0 + ratio * ratio);
}

bool PathAreaVisible(PathAreaEmitter emitter, float3 origin, float3 direction, float distance)
{
    if (outputInfo.w == 0 || !emitter.castShadows) return true;
    float sceneDistance;
    float3 ignoredNormal;
    int ignoredObject;
    // Test only the segment ending at the sampled light point. Geometry behind
    // the ceiling emitter must not shadow it. Use the existing shadow face rules.
    if (TraceScene(origin, direction, sceneDistance, ignoredNormal, ignoredObject, true)
        && sceneDistance < distance - RAY_EPSILON) return false;
    int ignoredEmitter;
    return !PathAreaHit(origin, direction, sceneDistance, ignoredEmitter) || sceneDistance >= distance - RAY_EPSILON;
}

float3 PathAreaDirectOne(PathAreaEmitter emitter, float3 position, float3 normal, float3 geometricNormal,
    float3 view, Surface material, inout uint randomState)
{
    float3 origin = position + geometricNormal * RAY_EPSILON;
    float3 radiance = 0.0;
    float u = PathRandom(randomState) * 2.0 - 1.0;
    float v = PathRandom(randomState) * 2.0 - 1.0;
    float3 delta = emitter.center + u * emitter.halfU + v * emitter.halfV - origin;
    float distance = length(delta);
    float3 incoming = delta / max(distance, 1e-8);
    float lightPdf = PathAreaPdf(emitter, incoming, distance);
    float cosine = dot(normal, incoming);
    if (lightPdf > 0.0 && cosine > 0.0 && dot(geometricNormal, incoming) > 0.0 &&
        PathAreaVisible(emitter, origin, incoming, distance))
    {
        float specularPdf;
        float3 brdf = PathBrdf(normal, view, incoming, material, true, true, 1.0, specularPdf);
        float brdfPdf = PathFullBrdfPdf(normal, view, incoming, material, specularPdf);
        radiance += brdf * cosine * emitter.radiance * PathAreaRange(emitter, distance) / lightPdf * PathMisWeight(lightPdf, brdfPdf);
    }
    // A second direct-light estimator resolves small, near-mirror highlights.
    // Use full BRDF here: GI contribution switches only govern indirect paths.
    // The ordinary continuation below does not add emitter radiance again.
    float3 weight;
    float brdfPdf;
    if (PathSampleBrdf(normal, geometricNormal, view, material, true, true, 1.0,
        randomState, incoming, weight, brdfPdf) && PathAreaHitOne(emitter, origin, incoming, distance))
    {
        lightPdf = PathAreaPdf(emitter, incoming, distance);
        if (lightPdf > 0.0 && PathAreaVisible(emitter, origin, incoming, distance))
            radiance += weight * emitter.radiance * PathAreaRange(emitter, distance) * PathMisWeight(brdfPdf, lightPdf);
    }
    return radiance;
}

float3 PathAreaDirect(float3 position, float3 normal, float3 geometricNormal,
    float3 view, Surface material, inout uint randomState)
{
    if (PathLegacyArea())
        return PathAreaDirectOne(PathAreaAt(-1), position, normal, geometricNormal, view, material, randomState);
    float3 radiance = 0.0;
    // Each rectangle has its own paired area/BRDF estimators. There is no
    // random light selection, hence no light-selection PDF or missing factor.
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        if (sceneLights[index].positionType.w != 2.0) continue;
        radiance += PathAreaDirectOne(PathAreaAt((int)index), position, normal, geometricNormal, view, material, randomState);
    }
    return radiance;
}

float3 PathDirectLight(float3 position, float3 normal, float3 geometricNormal,
    float3 view, Surface material)
{
    float3 radiance = 0.0;
    float3 origin = position + geometricNormal * RAY_EPSILON;
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        SceneLight source = sceneLights[index];
        // Rectangles exclusively use the area/BRDF MIS estimators above;
        // applying the raster four-point rule here would count their light twice.
        if (source.positionType.w == 2.0) continue;
        SceneLightSample lightSample;
        if (!SampleSceneLight(source, 0, position, lightSample)) continue;
        float nl = dot(normal, lightSample.direction);
        if (nl <= 0.0 || dot(geometricNormal, lightSample.direction) <= 0.0) continue;
        if (outputInfo.w != 0 && source.spotShadow.z > 0.5)
        {
            float blockerDistance; float3 ignoredNormal; int ignoredObject;
            if (TraceScene(origin, lightSample.direction, blockerDistance, ignoredNormal, ignoredObject, true)
                && blockerDistance < lightSample.distance - RAY_EPSILON) continue;
            if (PathAreaHit(origin, lightSample.direction, blockerDistance, ignoredObject)
                && blockerDistance < lightSample.distance - RAY_EPSILON) continue;
        }
        float unusedPdf;
        float3 brdf = PathBrdf(normal, view, lightSample.direction, material, true, true, 1.0, unusedPdf);
        radiance += brdf * nl * lightSample.irradiance;
    }
    return radiance;
}

void PathPrimaryRay(uint2 pixel, float2 jitter, out float3 origin,
    out float3 direction, out float farDistance)
{
    float2 uv = (float2(pixel) + jitter) / float2(outputInfo.xy);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    CameraPrimaryRay(ndc,origin,direction,farDistance);
}

float3 PathRadiance(uint2 pixel, uint sampleIndex, out float4 primaryGeometry, out float4 primaryMaterial)
{
    primaryGeometry = 0.0;
    primaryMaterial = 0.0;
    bool realtime = giOptions.x == 1;
    uint randomState = PathHash(pixel.x + pixel.y * outputInfo.x)
        ^ PathHash(sampleIndex + 0x9e3779b9u);
    float2 jitter;
    jitter.x = PathRandom(randomState);
    jitter.y = PathRandom(randomState);
    // Match the raster surface at the pixel centre. Progressive reference
    // rendering retains stochastic primary antialiasing.
    if (realtime) jitter = 0.5;
    float3 origin, direction;
    float farDistance;
    PathPrimaryRay(pixel, jitter, origin, direction, farDistance);
    if (!realtime && thinLens.z > .5 && cameraForward.w < .5)
    {
        // A uniformly sampled circular aperture aims every ray at the same
        // focal plane. Clip the new ray to the authored near/far planes, not
        // to distance measured from the aperture or its off-axis focus point.
        float radius = sqrt(PathRandom(randomState))*thinLens.y;
        float angle = PathRandom(randomState)*(2.0*PI);
        float3 right = normalize(float3(viewProjection[0][0],viewProjection[1][0],viewProjection[2][0]));
        float3 up = normalize(float3(viewProjection[0][1],viewProjection[1][1],viewProjection[2][1]));
        float3 apertureOffset = radius*(cos(angle)*right+sin(angle)*up);
        float3 focusVector = direction*(thinLens.x/max(dot(direction,cameraForward.xyz),1e-6));
        direction = normalize(focusVector-apertureOffset);
        float forwardCosine = max(dot(direction,cameraForward.xyz),1e-6);
        origin = cameraPosition.xyz+apertureOffset+direction*(cameraLensParams.x/forwardCosine);
        farDistance = (cameraLensParams.y-cameraLensParams.x)/forwardCosine;
    }
    float3 radiance = 0.0;
    float3 throughput = 1.0;
    // maxBounces counts shaded surfaces. A value of one evaluates direct light
    // at the primary hit and its sampled environment, but no second surface.
    uint maximumSurfaces = max(accumulationInfo.z, 1u);
    [loop] for (uint depth = 0; depth <= maximumSurfaces; ++depth)
    {
        float distance;
        float3 normal;
        int object;
        bool hit = TraceScene(origin, direction, distance, normal, object, false);
        float emitterDistance;
        int emitterIndex;
        if (PathAreaHit(origin, direction, emitterDistance, emitterIndex) && emitterDistance < distance &&
            (depth != 0 || emitterDistance <= farDistance))
        {
            // The rectangle is opaque from both sides but emits from cross(U,V).
            // Secondary direct visibility is already evaluated by the paired
            // MIS estimators at the preceding surface; do not count it twice.
            PathAreaEmitter emitter = PathAreaAt(emitterIndex);
            if (depth == 0 && dot(emitter.normal, -direction) > 0.0)
                radiance += throughput * emitter.radiance;
            break;
        }
        if (depth == 0 && distance > farDistance) hit = false;
        if (!hit)
        {
            // Black backdrop is a camera-visibility preference, matching the
            // preview contract; it does not remove environment illumination.
            if (depth != 0 || (!realtime && cameraPosition.w < 0.5))
                radiance += throughput * Environment(direction, 0.0);
            break;
        }
        if (depth == maximumSurfaces) break;
        RayTriangle tri = rayTriangles[-object - 2];
        float3 position = origin + direction * distance;
        float3 geometricNormal = tri.normal;
        if (dot(geometricNormal, direction) > 0.0) geometricNormal = -geometricNormal;
        if (dot(normal, geometricNormal) < 0.0) normal = -normal;
        if (dot(normal, -direction) <= 1e-6) normal = geometricNormal;
        Surface material = TriangleMaterialAt(position, normal, (uint)(-object - 2));
        // Albedo is a reflectance in the path integrator. Emission has a
        // separate physical meaning and is not currently a material field.
        material.albedo = saturate(material.albedo);
        if (depth == 0)
        {
            primaryGeometry = float4(normal, distance);
            primaryMaterial = float4(material.albedo, material.roughness);
        }
        // Realtime exports only indirect radiance; the raster lighting pass
        // owns the primary direct light, shadows and background.
        if (depth != 0 || !realtime)
        {
            radiance += throughput * PathDirectLight(position, normal, geometricNormal, -direction, material);
            radiance += throughput * PathAreaDirect(position, normal, geometricNormal, -direction, material, randomState);
        }
        float3 incoming, weight;
        float continuationPdf;
        // Classify contributions at the receiving surface. Subsequent path
        // vertices retain both lobes, including diffuse GI seen in a mirror.
        bool includeDiffuse = depth != 0 || (giOptions.y & 1u) != 0;
        bool includeSpecular = depth != 0 || (giOptions.y & 2u) != 0;
        // Artistic diffuse intensity modifies the receiving lobe only once:
        // f = gain * f_diffuse + f_specular. Scaling the complete continuation
        // would also amplify mirrors, and repeating it at every bounce would
        // exponentially amplify transport. Direct estimators above stay at 1.
        float diffuseIntensity = depth == 0 ? traceOptions.z : 1.0;
        if (!PathSampleBrdf(normal, geometricNormal, -direction, material, includeDiffuse, includeSpecular, diffuseIntensity,
            randomState, incoming, weight, continuationPdf)) break;
        throughput *= weight;
        if (!all(isfinite(throughput)) || !any(throughput > 0.0)) break;
        if (depth >= 3)
        {
            float survival = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
            if (PathRandom(randomState) >= survival) break;
            throughput /= survival;
        }
        origin = position + geometricNormal * RAY_EPSILON;
        direction = normalize(incoming);
    }
    return all(isfinite(radiance)) ? max(radiance, 0.0) : 0.0;
}
