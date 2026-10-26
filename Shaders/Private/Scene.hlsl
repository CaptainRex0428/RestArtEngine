cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 baseColor;
    float4 lightDirectionIntensity;
    float4 parameters;
    float4 viewportDebug;
};

cbuffer ObjectConstants : register(b1)
{
    row_major float4x4 worldMatrix;
    row_major float4x4 normalMatrix;
    float4 objectColor;
    float4 objectParameters;
};
struct RayObject
{
    row_major float4x4 inverseWorld;
    row_major float4x4 normalTransform;
    float4 color;
    // roughness, metallic, primitive (sphere=0, plane=1, cube=2), material flags
    float4 properties;
};
cbuffer RayConstants : register(b2)
{
    float4 rayInfo;
    RayObject rayObjects[32];
};
// Triangle data is uploaded in world space by the D3D11 preview renderer.  It
// is intentionally a structured buffer rather than a second constant-buffer
// array so imported meshes and future mesh actor types use the same path.
struct RayTriangle
{
    float3 position0;
    float3 position1;
    float3 position2;
    float3 normal;
    float4 color;
    // roughness, metallic, primitive-reserved, material flags
    float4 properties;
};
StructuredBuffer<RayTriangle> rayTriangles : register(t4);
cbuffer ShadowConstants : register(b3)
{
    row_major float4x4 lightViewProjection;
    row_major float4x4 lightViewProjectionCascade1;
    float4 shadowParameters; // texel size, depth bias, normal bias, filter/cascade mode
    float4 cascadeParameters; // split distance, enabled, reserved, reserved
};

Texture2D<float4> albedoBuffer : register(t0);
Texture2D<float4> normalBuffer : register(t1);
Texture2D<float4> positionBuffer : register(t2);
Texture2DArray<float> shadowMap : register(t3);
SamplerState gbufferSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

static const float PI = 3.14159265359;

static const float RAY_EPSILON = 0.003;

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct GeometryOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
};

struct FullscreenOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct Surface
{
    float3 albedo;
    float roughness;
    float metallic;
    float grid;
};

#include "../Materials/Standard.hlsli"

GeometryOutput VSMain(VertexInput input)
{
    GeometryOutput output;
    output.worldPosition = mul(float4(input.position, 1.0), worldMatrix).xyz;
    output.position = mul(float4(output.worldPosition, 1.0), viewProjection);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0), normalMatrix).xyz);
    return output;
}

FullscreenOutput VSFullscreen(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.999999, 1.0);
    return output;
}

// properties.w is an integer value: bit 0 Grid, bit 1 Two-sided.
bool IsTwoSided(float4 properties)
{
    return ((uint)properties.w & 2u) != 0;
}

Surface MaterialAt(float3 position, float3 normal, float4 color, float4 properties)
{
    Surface material;
    material.albedo = max(color.rgb, 0.0);
    material.roughness = properties.x;
    material.metallic = properties.y;
    material.grid = (float)((uint)properties.w & 1u);
    material = EvaluateMaterial(position, normal, material);
    material.albedo = max(material.albedo, 0.0);
    material.roughness = clamp(material.roughness, 0.045, 1.0);
    material.metallic = saturate(material.metallic);
    return material;
}

float3 Environment(float3 direction, float roughness)
{
    float height = saturate(direction.y * 0.5 + 0.5);
    float3 sky = lerp(float3(0.17, 0.205, 0.29), float3(0.38, 0.57, 0.82), pow(height, 0.65));
    sky = lerp(float3(0.065, 0.075, 0.10), sky, smoothstep(-0.14, 0.14, direction.y));
    float3 keyDirection = normalize(lightDirectionIntensity.xyz);
    float key = pow(saturate(dot(direction, keyDirection)), lerp(420.0, 3.0, roughness * roughness));
    sky += float3(1.0, 0.83, 0.63) * key * (1.1 - roughness * 0.65);
    // Broad cool studio strip, visible as a second highlight on polished metal.
    float strip = pow(saturate(dot(direction, normalize(float3(1.0, 0.45, 0.7)))), lerp(72.0, 2.0, roughness));
    sky += float3(0.32, 0.50, 0.80) * strip * (1.0 - roughness * 0.55);
    return sky;
}

// Keep local ray directions unnormalized to preserve world-distance t under scale.
bool TraceScene(float3 origin, float3 direction, out float distance, out float3 hitNormal, out int hitObject,
                bool shadowRay)
{
    distance = 1e20;
    hitNormal = float3(0, 1, 0);
    hitObject = -1;
    [loop] for (int i = 0; i < (int)rayInfo.x; ++i)
    {
        float3 localOrigin = mul(float4(origin, 1), rayObjects[i].inverseWorld).xyz;
        float3 localDirection = mul(float4(direction, 0), rayObjects[i].inverseWorld).xyz;
        bool twoSided = IsTwoSided(rayObjects[i].properties);
        float t = 1e20;
        float3 localNormal = float3(0, 1, 0);
        if (rayObjects[i].properties.z < 0.5)
        {
            float a = dot(localDirection, localDirection);
            float b = dot(localOrigin, localDirection);
            float c = dot(localOrigin, localOrigin) - 1.0;
            float discriminant = b * b - a * c;
            if (discriminant >= 0 && a > 1e-12)
            {
                // A shadow visibility ray travels toward the light. For a
                // single-sided caster its light-facing surface is the far hit.
                t = (-b + (shadowRay && !twoSided ? 1.0 : -1.0) * sqrt(discriminant)) / a;
                if (t <= RAY_EPSILON) t = (-b + sqrt(discriminant)) / a;
                localNormal = localOrigin + localDirection * t;
            }
        }
        else if (rayObjects[i].properties.z < 1.5)
        {
            // The built-in plane is the finite XZ floor used by the demo.
            if (abs(localDirection.y) < 1e-8) continue;
            t = -localOrigin.y / localDirection.y;
            float3 hit = localOrigin + localDirection * t;
            if (max(abs(hit.x), abs(hit.z)) > 12.0) t = 1e20;
        }
        else
        {
            // Built-in cube bounds are [-1, 1] in local space.  Slab
            // intersection keeps cubes in the analytic scene so their faces
            // can receive shadows and appear in sphere/environment rays.
            // Handle parallel slabs explicitly.  0 * infinity can become
            // NaN when a ray starts exactly on a cube face, which would make
            // an otherwise valid reflected ray disappear.
            float3 minimum;
            float3 maximum;
            if (abs(localDirection.x) > 1e-8)
            {
                float inverse = 1.0 / localDirection.x;
                float a = (-1.0 - localOrigin.x) * inverse;
                float b = ( 1.0 - localOrigin.x) * inverse;
                minimum.x = min(a, b); maximum.x = max(a, b);
            }
            else
            {
                minimum.x = (localOrigin.x < -1.0 || localOrigin.x > 1.0) ? 1e20 : -1e20;
                maximum.x = (localOrigin.x < -1.0 || localOrigin.x > 1.0) ? -1e20 : 1e20;
            }
            if (abs(localDirection.y) > 1e-8)
            {
                float inverse = 1.0 / localDirection.y;
                float a = (-1.0 - localOrigin.y) * inverse;
                float b = ( 1.0 - localOrigin.y) * inverse;
                minimum.y = min(a, b); maximum.y = max(a, b);
            }
            else
            {
                minimum.y = (localOrigin.y < -1.0 || localOrigin.y > 1.0) ? 1e20 : -1e20;
                maximum.y = (localOrigin.y < -1.0 || localOrigin.y > 1.0) ? -1e20 : 1e20;
            }
            if (abs(localDirection.z) > 1e-8)
            {
                float inverse = 1.0 / localDirection.z;
                float a = (-1.0 - localOrigin.z) * inverse;
                float b = ( 1.0 - localOrigin.z) * inverse;
                minimum.z = min(a, b); maximum.z = max(a, b);
            }
            else
            {
                minimum.z = (localOrigin.z < -1.0 || localOrigin.z > 1.0) ? 1e20 : -1e20;
                maximum.z = (localOrigin.z < -1.0 || localOrigin.z > 1.0) ? -1e20 : 1e20;
            }
            float nearDistance = max(max(minimum.x, minimum.y), minimum.z);
            float farDistance = min(min(maximum.x, maximum.y), maximum.z);
            if (farDistance >= max(nearDistance, 0.0))
            {
                t = shadowRay && !twoSided ? farDistance :
                    (nearDistance > RAY_EPSILON ? nearDistance : farDistance);
                float3 hit = localOrigin + localDirection * t;
                float3 a = abs(hit);
                if (a.x >= a.y && a.x >= a.z) localNormal = float3(sign(hit.x), 0, 0);
                else if (a.y >= a.z) localNormal = float3(0, sign(hit.y), 0);
                else localNormal = float3(0, 0, sign(hit.z));
            }
        }
        if (t > RAY_EPSILON && t < distance)
        {
            float facing = dot(localDirection, localNormal);
            // Shadow-map geometry is viewed light -> receiver; visibility
            // rays run in the opposite direction and accept the opposite side.
            if (!twoSided && (shadowRay ? facing <= 0.0 : facing >= 0.0)) continue;
            distance = t;
            hitNormal = normalize(mul(float4(localNormal, 0), rayObjects[i].normalTransform).xyz);
            if (twoSided && dot(hitNormal, direction) > 0.0) hitNormal = -hitNormal;
            hitObject = i;
        }
    }
    // Imported and future mesh assets use the same world-space triangle path.
    // Keep the closest hit encoded as -(triangleIndex + 2): -1 remains the
    // miss sentinel, while the first triangle is represented by -2.
    [loop] for (int j = 0; j < (int)rayInfo.y; ++j)
    {
        RayTriangle tri = rayTriangles[j];
        bool twoSided = IsTwoSided(tri.properties);
        float facing = dot(direction, tri.normal);
        if (!twoSided && (shadowRay ? facing <= 0.0 : facing >= 0.0)) continue;
        float3 edge0 = tri.position1 - tri.position0;
        float3 edge1 = tri.position2 - tri.position0;
        float3 pvec = cross(direction, edge1);
        float determinant = dot(edge0, pvec);
        if (abs(determinant) < 1e-8) continue;
        float inverseDeterminant = 1.0 / determinant;
        float3 tvec = origin - tri.position0;
        float u = dot(tvec, pvec) * inverseDeterminant;
        if (u < 0.0 || u > 1.0) continue;
        float3 qvec = cross(tvec, edge0);
        float v = dot(direction, qvec) * inverseDeterminant;
        if (v < 0.0 || u + v > 1.0) continue;
        float t = dot(edge1, qvec) * inverseDeterminant;
        if (t > RAY_EPSILON && t < distance)
        {
            distance = t;
            hitNormal = normalize(tri.normal);
            if (twoSided && facing > 0.0) hitNormal = -hitNormal;
            hitObject = -(j + 2);
        }
    }
    return hitObject != -1;
}

bool TraceSceneAny(float3 origin, float3 direction)
{
    float distance; float3 normal; int object;
    // TraceScene uses -1 for a miss, so this helper also accepts generic
    // triangle hits (which are encoded as values at or below -2).
    return TraceScene(origin, direction, distance, normal, object, true);
}

float ShadowMapPCSSVisibility(float2 uv, float depth, float2 texel, float cascade)
{
    // Blocker search reads the existing depth SRV directly. This keeps PCSS
    // on the current shadow-map resource and comparison sampler without a
    // second texture or a new constant-buffer binding.
    const int mapSize = max(1, (int)round(1.0 / shadowParameters.x));
    const int2 mapLimit = int2(mapSize - 1, mapSize - 1);
    float blockerDepth = 0.0;
    float blockerCount = 0.0;
    [unroll] for (int y = -2; y <= 1; ++y)
    {
        [unroll] for (int x = -2; x <= 1; ++x)
        {
            float2 sampleUv = uv + texel * float2(x + 0.5, y + 0.5);
            if (all(sampleUv >= 0.0) && all(sampleUv <= 1.0))
            {
                int2 pixel = clamp((int2)floor(sampleUv * mapSize), int2(0, 0), mapLimit);
                float sampleDepth = shadowMap.Load(int4(pixel, (int)cascade, 0));
                if (sampleDepth < depth)
                {
                    blockerDepth += sampleDepth;
                    blockerCount += 1.0;
                }
            }
        }
    }
    if (blockerCount < 0.5) return 1.0;
    float averageBlocker = blockerDepth / blockerCount;
    float penumbra = saturate((depth - averageBlocker) / max(averageBlocker, 1e-4));
    float filterRadius = clamp(1.0 + penumbra * 24.0, 1.0, 8.0);
    float visibility = 0.0;
    [unroll] for (int y = -2; y <= 1; ++y)
    {
        [unroll] for (int x = -2; x <= 1; ++x)
            visibility += shadowMap.SampleCmpLevelZero(shadowSampler,
                float3(uv + texel * float2(x + 0.5, y + 0.5) * filterRadius, cascade), depth);
    }
    return visibility * (1.0 / 16.0);
}

float ShadowMapVisibility(float3 position, float3 normal)
{
    const bool useCascade = cascadeParameters.y > 0.5;
    const bool secondCascade = useCascade && distance(position, cameraPosition.xyz) > cascadeParameters.x;
    const float cascade = secondCascade ? 1.0 : 0.0;
    float4 shadowClip = secondCascade ?
        mul(float4(position + normal * shadowParameters.z, 1.0), lightViewProjectionCascade1) :
        mul(float4(position + normal * shadowParameters.z, 1.0), lightViewProjection);
    if (shadowClip.w <= 0.0001) return 1.0;
    float3 shadowNdc = shadowClip.xyz / shadowClip.w;
    // The shadow sampler has a white border, so points outside the directional
    // frustum remain lit. D3D's viewport Y axis is inverted relative to UV.
    float2 uv = float2(shadowNdc.x * 0.5 + 0.5, -shadowNdc.y * 0.5 + 0.5);
    if (shadowNdc.z <= 0.0 || shadowNdc.z >= 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float2 texel = float2(shadowParameters.x, shadowParameters.x);
    float depth = shadowNdc.z - shadowParameters.y;
    // shadowParameters.w selects the raster filter: 0 is the default 2x2
    // PCF, 1 is a single hard comparison, 2 is a 3x3 PCF kernel, and 3 is
    // PCSS. Keeping the filter in the existing constant buffer lets Forward
    // and Deferred share the same shadow-map pass and makes the mode switch
    // inexpensive.
    if (shadowParameters.w > 2.5 && shadowParameters.w < 3.5)
        return ShadowMapPCSSVisibility(uv, depth, texel, cascade);
    if (shadowParameters.w > 0.5 && shadowParameters.w < 1.5)
        return shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv, cascade), depth);
    float visibility = 0.0;
    if (shadowParameters.w > 1.5 && shadowParameters.w < 2.5)
    {
        // The fixed loop bounds compile to nine taps on the D3D11 shader
        // model; no dynamic indexing or extra resources are needed.
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
                visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + texel * float2(x, y), cascade), depth);
        return visibility / 9.0;
    }
    // Four-tap PCF is intentionally deterministic and inexpensive enough for
    // the default editor preview while softening the hard directional-map edge.
    visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + texel * float2(-0.5, -0.5), cascade), depth);
    visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + texel * float2( 0.5, -0.5), cascade), depth);
    visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + texel * float2(-0.5,  0.5), cascade), depth);
    visibility += shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv + texel * float2( 0.5,  0.5), cascade), depth);
    return visibility * 0.25;
}

float ScreenSpaceGTAO(float3 position, float3 normal, float2 uv)
{
    // Deferred-only horizon approximation. The position buffer is world
    // space, so this remains stable across camera distance and needs no extra
    // depth reconstruction pass.
    const float2 kernel[8] = {
        float2(-1.0, -0.2), float2(-0.5, 0.8), float2(0.2, 1.0), float2(0.9, 0.4),
        float2(1.0, -0.4), float2(0.4, -0.9), float2(-0.4, -1.0), float2(-0.9, -0.5)
    };
    const float radius = 0.75;
    float occlusion = 0.0;
    float samples = 0.0;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        float2 sampleUv = uv + kernel[i] * 7.0 / max(viewportDebug.xy, 1.0);
        float4 samplePosition = positionBuffer.SampleLevel(gbufferSampler, sampleUv, 0);
        if (samplePosition.w < 0.5) continue;
        float3 delta = samplePosition.xyz - position;
        float distanceToSample = length(delta);
        if (distanceToSample <= 1e-4 || distanceToSample > radius) continue;
        float behind = saturate((-dot(normal, delta) - 0.015) / 0.20);
        occlusion += behind * saturate(1.0 - distanceToSample / radius);
        samples += 1.0;
    }
    return samples > 0.0 ? lerp(1.0, saturate(1.0 - occlusion / samples), 0.85) : 1.0;
}

float ScreenSpaceContact(float3 position, float3 normal, float3 light)
{
    float visibility = 1.0;
    const int steps = 8;
    [unroll] for (int i = 1; i <= steps; ++i)
    {
        float3 probe = position + normal * RAY_EPSILON + light * (0.08 * i);
        float4 clip = mul(float4(probe, 1.0), viewProjection);
        if (clip.w <= 1e-4) continue;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
        if (ndc.z <= 0.0 || ndc.z >= 1.0 || any(uv < 0.0) || any(uv > 1.0)) continue;
        float4 scenePosition = positionBuffer.SampleLevel(gbufferSampler, uv, 0);
        if (scenePosition.w < 0.5) continue;
        float4 sceneClip = mul(float4(scenePosition.xyz, 1.0), viewProjection);
        float sceneDepth = sceneClip.z / max(sceneClip.w, 1e-4);
        if (sceneDepth + 0.0015 < ndc.z) return 0.25;
    }
    return visibility;
}

float ShadowVisibility(float3 position, float3 normal, float3 light, float2 uv, bool deferred)
{
    if (viewportDebug.w > 1.5) return 1.0;
    if (viewportDebug.w > 0.5) {
        // Analytic rays are selected explicitly; this remains independent of
        // the reflection toggle in parameters.w.
        float3 tangent = normalize(cross(light, abs(light.y) < 0.95 ? float3(0, 1, 0) : float3(1, 0, 0)));
        float3 bitangent = cross(light, tangent);
        float visibility = 0.0;
        [unroll] for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
        {
            float2 offset = float2((sampleIndex & 1) ? 1.0 : -1.0, (sampleIndex & 2) ? 1.0 : -1.0) * 0.025;
            float3 ray = normalize(light + tangent * offset.x + bitangent * offset.y);
            float distance; float3 hitNormal; int hitObject;
            visibility += TraceSceneAny(position + normal * RAY_EPSILON, ray) ? 0.0 : 0.25;
        }
        return visibility;
    }
    float visibility = ShadowMapVisibility(position, normal);
    if (deferred && cascadeParameters.z > 1.5)
        visibility *= ScreenSpaceContact(position, normal, light);
    return visibility;
}

float3 Fresnel(float cosine, float3 reflectance)
{
    float factor = pow(1.0 - saturate(cosine), 5.0);
    return reflectance + (1.0 - reflectance) * factor;
}

float3 ShadeSurface(float3 position, float3 normal, float3 view, Surface material, float2 uv, bool deferred)
{
    float3 light = normalize(lightDirectionIntensity.xyz);
    float3 halfway = normalize(light + view);
    float nv = max(dot(normal, view), 0.001);
    float nl = saturate(dot(normal, light));
    float nh = saturate(dot(normal, halfway));
    float vh = saturate(dot(view, halfway));
    float alpha = material.roughness * material.roughness;
    float alphaSquared = alpha * alpha;
    float denominator = nh * nh * (alphaSquared - 1.0) + 1.0;
    float distribution = alphaSquared / max(PI * denominator * denominator, 0.000001);
    float k = (material.roughness + 1.0) * (material.roughness + 1.0) / 8.0;
    float geometry = (nv / (nv * (1.0 - k) + k)) * (nl / max(nl * (1.0 - k) + k, 0.001));
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    float3 fresnel = Fresnel(vh, f0);
    float3 specular = distribution * geometry * fresnel / max(4.0 * nv * nl, 0.001);
    float3 diffuse = (1.0 - fresnel) * (1.0 - material.metallic) * material.albedo / PI;
    float visibility = ShadowVisibility(position, normal, light, uv, deferred);
    float3 direct = (diffuse + specular) * nl * max(lightDirectionIntensity.w, 0.0) * float3(1.0, 0.87, 0.72) * visibility;

    float ambientOcclusion = deferred && cascadeParameters.z > 0.5 && cascadeParameters.z < 1.5 ?
        ScreenSpaceGTAO(position, normal, uv) : 1.0;
    float3 ambientDiffuse = Environment(normal, 1.0) * material.albedo * (1.0 - material.metallic) * 0.58 * ambientOcclusion;
    float3 reflection = reflect(-view, normal);
    float3 ambientSpecular = Environment(reflection, material.roughness) * Fresnel(nv, f0) * (1.0 - material.roughness * 0.45);
    return direct + ambientDiffuse + ambientSpecular;
}

float3 Shade(float3 position, float3 normal, Surface material, float2 uv, bool deferred)
{
    float3 view = normalize(cameraPosition.xyz - position);
    float3 color = ShadeSurface(position, normal, view, material, uv, deferred);
    if (parameters.w > 0.5)
    {
        float3 ray = reflect(-view, normal);
        float distance; float3 hitNormal; int hitObject;
        if (TraceScene(position + normal * RAY_EPSILON, ray, distance, hitNormal, hitObject, false))
        {
            float3 hitPosition = position + normal * RAY_EPSILON + ray * distance;
            float4 hitColor = hitObject >= 0 ? rayObjects[hitObject].color : rayTriangles[-hitObject - 2].color;
            float4 hitProperties = hitObject >= 0 ? rayObjects[hitObject].properties : rayTriangles[-hitObject - 2].properties;
            Surface hitMaterial = MaterialAt(hitPosition, hitNormal, hitColor, hitProperties);
            float3 reflectedColor = ShadeSurface(hitPosition, hitNormal, -ray, hitMaterial, uv, deferred);
            float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
            float3 weight = Fresnel(saturate(dot(normal, view)), f0) * (1.0 - material.roughness) * (1.0 - material.roughness);
            // Replace the corresponding environment lobe with actual scene radiance.
            color += (reflectedColor - Environment(ray, material.roughness)) * weight;
        }
    }
    return max(color, 0.0);
}

float3 DisplayColor(float3 linearColor)
{
    float3 x = max(linearColor * max(parameters.z, 0.001), 0.0);
    float3 mapped = saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
    return pow(mapped, 1.0 / 2.2);
}

float4 OutputSurface(float3 position, float3 normal, Surface material, float2 uv, bool deferred)
{
    // Unlit is a viewport mode: display base color with the normal display
    // transform, bypassing all direct/ambient lighting, AO and ray effects.
    if (viewportDebug.z > 0.5 && viewportDebug.z < 1.5) return float4(DisplayColor(material.albedo), 1.0);
    // Albedo remains a raw material-buffer inspection (gamma encoding only).
    if (viewportDebug.z > 1.5 && viewportDebug.z < 2.5) return float4(pow(saturate(material.albedo), 1.0 / 2.2), 1.0);
    if (viewportDebug.z > 2.5 && viewportDebug.z < 3.5) return float4(normal * 0.5 + 0.5, 1.0);
    if (viewportDebug.z > 3.5) return float4(frac(abs(position) * 0.25), 1.0);
    return float4(DisplayColor(Shade(position, normal, material, uv, deferred)), 1.0);
}

float4 Background(float2 uv)
{
    // cameraPosition.w is the solid-black backdrop flag written by the renderer
    // (the field is otherwise unused by the shaders). Scenes that ask for a
    // black backdrop skip the sky gradient entirely.
    if (cameraPosition.w > 0.5) return float4(0.0, 0.0, 0.0, 1.0);
    float3 sky = lerp(float3(0.035, 0.055, 0.09), float3(0.15, 0.22, 0.32), saturate(uv.y));
    float vignette = 1.0 - 0.22 * dot(uv - 0.5, uv - 0.5);
    return float4(DisplayColor(sky * vignette), 1.0);
}

float4 PSBackground(FullscreenOutput input) : SV_TARGET
{
    return Background(input.uv);
}

float3 SurfaceNormal(float3 normal, bool frontFace)
{
    normal = normalize(normal);
    return IsTwoSided(objectParameters) && !frontFace ? -normal : normal;
}

float4 PSForward(GeometryOutput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    float3 normal = SurfaceNormal(input.worldNormal, frontFace);
    Surface material = MaterialAt(input.worldPosition, normal, objectColor, objectParameters);
    return OutputSurface(input.worldPosition, normal, material,
        input.position.xy / max(viewportDebug.xy, 1.0), false);
}

struct GBufferOutput
{
    float4 albedo : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 position : SV_TARGET2;
};

GBufferOutput PSGBuffer(GeometryOutput input, bool frontFace : SV_IsFrontFace)
{
    GBufferOutput output;
    float3 normal = SurfaceNormal(input.worldNormal, frontFace);
    Surface material = MaterialAt(input.worldPosition, normal, objectColor, objectParameters);
    output.albedo = float4(material.albedo, material.roughness);
    output.normal = float4(normal, material.metallic);
    output.position = float4(input.worldPosition, 1.0);
    return output;
}

float4 PSDeferred(FullscreenOutput input) : SV_TARGET
{
    int3 pixel = int3(int2(input.position.xy), 0);
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5) return Background(input.uv);
    float4 albedo = albedoBuffer.Load(pixel);
    float4 normal = normalBuffer.Load(pixel);
    Surface material;
    material.albedo = albedo.rgb;
    material.roughness = albedo.a;
    material.metallic = normal.a;
    material.grid = 0; // Grid color is already evaluated in the G-buffer pass.
    return OutputSurface(position.xyz, normalize(normal.xyz), material, input.uv, true);
}

// ---------------------------------------------------------------------------
// Editor-only selection outline.
//
// The runtime path never binds t5/b4: the renderer issues these entry points
// only when the editor supplies a selection, so the standalone scene render
// does not pay for the mask or the composite. PSOutlineMask writes the selected
// silhouette into a scene-sized mask; PSOutlineComposite dilates it into the
// scene colour so the ring hugs the object in screen space.
// ---------------------------------------------------------------------------
cbuffer OutlineConstants : register(b4)
{
    float4 outlineColor;  // rgb + alpha, authored by the editor theme
    float4 outlineParams; // ring radius in pixels, 1/width, 1/height, reserved
};
Texture2D<float4> outlineMask : register(t5); // r = visible silhouette, g = occluded
Texture2D<float4> depthBuffer : register(t6);

static const float2 kOutlineDirections[8] =
{
    float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
    float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071), float2(-0.7071, -0.7071)
};

float4 PSOutlineMask(GeometryOutput input) : SV_TARGET
{
    return float4(1.0, 1.0, 1.0, 1.0);
}

float4 PSOutlineComposite(FullscreenOutput input) : SV_TARGET
{
    // Pixel-space pass: the mask and the scene depth share the scene texture's
    // resolution, so integer loads keep the ring exactly aligned regardless of
    // the docked viewport size.
    const int2 pixel = int2(input.position.xy);
    const float4 center = outlineMask.Load(int3(pixel, 0));
    if (center.r > 0.5) return float4(0.0, 0.0, 0.0, 0.0);
    const float centerDepth = depthBuffer.Load(int3(pixel, 0)).r;
    const int radius = (int)max(1.0, round(outlineParams.x));
    float solid = 0.0;
    float hiddenEdge = 0.0;
    [unroll] for (int index = 0; index < 8; ++index)
    {
        const int2 offset = int2(round(kOutlineDirections[index] * (float)radius));
        const int2 samplePosition = max(pixel + offset, int2(0, 0));
        const float4 neighbour = outlineMask.Load(int3(samplePosition, 0));
        // Default: a silhouette needs the selected fragment to be *nearer* than
        // the pixel we paint on, which keeps interior details out and avoids
        // outlining an occluder in front. outlineParams.w > 0.5 switches to a
        // symmetric depth test, so front occluders are outlined as well.
        const float neighbourDepth = depthBuffer.Load(int3(samplePosition, 0)).r;
        const bool silhouette = outlineParams.w > 0.5
            ? abs(neighbourDepth - centerDepth) > 1e-5
            : neighbourDepth + 1e-6 < centerDepth;
        if (neighbour.r > 0.5 && silhouette)
            solid = 1.0;
        // G marks fragments hidden behind other geometry. Only the boundary of
        // that region becomes the dashed contour; its interior is left alone so
        // the hint stays a line instead of a hatched fill.
        if (neighbour.g > 0.5 && center.g < 0.5) hiddenEdge = 1.0;
    }
    // Occluded geometry is always a dashed contour: a hint along the silhouette,
    // never a fill of the hidden area.
    const bool dashOn = ((pixel.x + pixel.y) & 7) < 4;
    float alpha = 0.0;
    if (solid > 0.5) alpha = outlineColor.a;
    else if (hiddenEdge > 0.5 && dashOn) alpha = outlineColor.a * 0.9;
    return alpha > 0.0 ? float4(outlineColor.rgb, alpha) : float4(0.0, 0.0, 0.0, 0.0);
}