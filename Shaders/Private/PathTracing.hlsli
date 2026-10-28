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
    bool includeDiffuse, bool includeSpecular, out float specularPdf)
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
    if (!includeSpecular) return diffuse;
    float geometry = PathSmithG1(nv, alphaSquared) * PathSmithG1(nl, alphaSquared);
    specularPdf = distribution * nh / max(4.0 * vh, 1e-8);
    return diffuse + fresnel * distribution * geometry / max(4.0 * nv * nl, 1e-8);
}

bool PathSampleBrdf(float3 normal, float3 geometricNormal, float3 view,
    Surface material, bool includeDiffuse, bool includeSpecular,
    inout uint randomState, out float3 incoming, out float3 weight)
{
    incoming = normal;
    weight = 0.0;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    float specularEnergy = includeSpecular
        ? PathLuminance(Fresnel(saturate(dot(normal, view)), f0)) : 0.0;
    float diffuseEnergy = includeDiffuse ? PathLuminance(material.albedo) * (1.0 - material.metallic) : 0.0;
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
    float3 brdf = PathBrdf(normal, view, incoming, material, includeDiffuse, includeSpecular, specularPdf);
    float pdf = specularProbability * specularPdf + (1.0 - specularProbability) * nl / PI;
    if (pdf <= 1e-12) return false;
    weight = brdf * nl / pdf;
    return all(isfinite(weight)) && any(weight > 0.0);
}

float3 PathDirectLight(float3 position, float3 normal, float3 geometricNormal,
    float3 view, Surface material)
{
    float3 light = normalize(lightDirectionIntensity.xyz);
    float nl = dot(normal, light);
    if (nl <= 0.0 || dot(geometricNormal, light) <= 0.0) return 0.0;
    if (outputInfo.w != 0 && TraceSceneAny(position + geometricNormal * RAY_EPSILON, light)) return 0.0;
    float unusedPdf;
    // GI lobe switches control indirect transport. Direct highlights remain
    // part of the material's direct lighting in both modes.
    float3 brdf = PathBrdf(normal, view, light, material, true, true, unusedPdf);
    return brdf * nl * max(lightDirectionIntensity.w, 0.0) * float3(1.0, 0.87, 0.72);
}

void PathPrimaryRay(uint2 pixel, float2 jitter, out float3 origin,
    out float3 direction, out float farDistance)
{
    float2 uv = (float2(pixel) + jitter) / float2(outputInfo.xy);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 nearPoint = mul(float4(ndc, 0.0, 1.0), inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    origin = nearPoint.xyz / nearPoint.w;
    float3 farPosition = farPoint.xyz / farPoint.w;
    direction = normalize(farPosition - origin);
    farDistance = length(farPosition - origin);
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
        Surface material = MaterialAt(position, normal, tri.color, tri.properties);
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
            radiance += throughput * PathDirectLight(position, normal, geometricNormal, -direction, material);
        float3 incoming, weight;
        // Classify contributions at the receiving surface. Subsequent path
        // vertices retain both lobes, including diffuse GI seen in a mirror.
        bool includeDiffuse = depth != 0 || (giOptions.y & 1u) != 0;
        bool includeSpecular = depth != 0 || (giOptions.y & 2u) != 0;
        if (!PathSampleBrdf(normal, geometricNormal, -direction, material, includeDiffuse, includeSpecular,
            randomState, incoming, weight)) break;
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
