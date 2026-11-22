cbuffer SceneConstants : register(b0)
{
    row_major float4x4 viewProjection;
    float4 cameraPosition;
    float4 baseColor;
    float4 lightDirectionIntensity;
    float4 parameters;
    float4 viewportDebug;
    float4 cameraForward; // xyz forward, w orthographic
    float4 cameraLensParams; // near, far, ortho height, tan(vertical FOV / 2)
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
    // Previous-frame world positions for per-object motion vectors.
    float3 previousPosition0;
    float3 previousPosition1;
    float3 previousPosition2;
};
StructuredBuffer<RayTriangle> rayTriangles : register(t4);
// UV vertices are shared by mesh instances; only UV-bearing triangles have lookup records.
StructuredBuffer<float4> rayUvVertices : register(t10);
StructuredBuffer<uint4> rayUvTriangles : register(t11);
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

#if !defined(RAE_HARDWARE_RAYTRACING)
// Algorithm-independent raster consumer: a resolved, shaded indirect-radiance
// image in linear HDR, aligned with the current scene viewport.
cbuffer GlobalIlluminationConstants : register(b6)
{
    float4 giViewport; // effective realtime GI, local width, local height, reserved
    // enabled, world-space march radius, thickness, artistic intensity
    float4 giScreenSpace;
    row_major float4x4 giPreviousViewProjection;
    // history valid, frame index, current-frame weight, spatial taps
    float4 giHistory;
};
Texture2D<float4> indirectLightingBuffer : register(t9);
Texture2D<float4> ssgiDirectHistory : register(t16);
Texture2D<float4> ssgiDirectCurrent : register(t17);
Texture2D<float4> ssgiRawIndirect : register(t18);
Texture2D<float4> ssgiHistoryIndirect : register(t19);
Texture2D<float4> probeAtlasRead : register(t14);
Texture2D<float4> probeColorSource : register(t15);
Texture2D<float4> probeDepthAtlas : register(t20);
cbuffer ProbeConstants : register(b9)
{
    float4 probeGrid;    // x, y, z probe counts, history hysteresis
    float4 probeOrigin;  // grid origin xyz, world spacing
    float4 probeParameters;  // enabled, previous atlas valid, intensity, mode
    float4 probeLayout;      // x = SH order (1 or 2), yzw reserved
};
#endif

static const float PI = 3.14159265359;

static const float RAY_EPSILON = 0.003;

float CameraDepth(float3 position) { return dot(position - cameraPosition.xyz, cameraForward.xyz); }
float3 CameraViewDirection(float3 position) {
    return cameraForward.w > 0.5 ? -cameraForward.xyz : normalize(cameraPosition.xyz - position);
}
float CameraPixelFootprint(float3 position, float imageHeight) {
    float worldHeight = cameraForward.w > 0.5 ? cameraLensParams.z
        : 2.0 * max(CameraDepth(position), cameraLensParams.x) * cameraLensParams.w;
    return max(worldHeight / max(imageHeight, 1.0), RAY_EPSILON);
}
void CameraPrimaryRay(float2 ndc, out float3 origin, out float3 direction, out float farDistance) {
    // Extract right/up divided by projection scale without subtracting large
    // world-space positions. This also avoids far-plane inverse precision loss.
    float3 horizontal = float3(viewProjection[0][0],viewProjection[1][0],viewProjection[2][0]);
    float3 vertical = float3(viewProjection[0][1],viewProjection[1][1],viewProjection[2][1]);
    horizontal /= max(dot(horizontal,horizontal),1e-30);
    vertical /= max(dot(vertical,vertical),1e-30);
    float3 offset = horizontal*ndc.x+vertical*ndc.y;
    bool orthographic = cameraForward.w > 0.5;
    direction = orthographic ? cameraForward.xyz : normalize(cameraForward.xyz+offset);
    origin = cameraPosition.xyz + cameraForward.xyz*cameraLensParams.x
        + offset*(orthographic ? 1.0 : cameraLensParams.x);
    farDistance = (cameraLensParams.y-cameraLensParams.x)/max(dot(direction,cameraForward.xyz),1e-6);
}

#include "Lighting.hlsli"

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 uv01 : TEXCOORD0;
    float4 uv23 : TEXCOORD1;
};

struct GeometryOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 uv01 : TEXCOORD2;
    float4 uv23 : TEXCOORD3;
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
    // Imported coordinates in source convention; absent channels are zero.
    float2 uv0, uv1, uv2, uv3;
    uint uvMask;
};

#include "../Materials/Standard.hlsli"

GeometryOutput VSMain(VertexInput input)
{
    GeometryOutput output;
    output.worldPosition = mul(float4(input.position, 1.0), worldMatrix).xyz;
    output.position = mul(float4(output.worldPosition, 1.0), viewProjection);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0), normalMatrix).xyz);
    output.uv01 = input.uv01; output.uv23 = input.uv23;
    return output;
}

FullscreenOutput VSFullscreen(uint vertexId : SV_VertexID)
{
    FullscreenOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.999999, 1.0);
    return output;
}

// properties.w is an integer value: bit 0 Grid, bit 1 Cull Off, bit 2 Cull Front.
bool IsTwoSided(float4 properties)
{
    return ((uint)properties.w & 2u) != 0;
}

bool IsFrontCulled(float4 properties)
{
    return !IsTwoSided(properties) && ((uint)properties.w & 4u) != 0;
}

bool AcceptsMaterialFace(float facing, float4 properties, bool shadowRay)
{
    if (IsTwoSided(properties)) return true;
    // Shadow maps observe light -> receiver; visibility rays travel in the
    // opposite direction, so their accepted face is reversed as well.
    bool acceptBack = shadowRay != IsFrontCulled(properties);
    return acceptBack ? facing > 0.0 : facing < 0.0;
}

Surface MaterialAtUv(float3 position, float3 normal, float4 color, float4 properties,
                     float4 uv01, float4 uv23, uint uvMask)
{
    Surface material = (Surface)0;
    material.albedo = max(color.rgb, 0.0);
    material.roughness = properties.x;
    material.metallic = properties.y;
    material.grid = (float)((uint)properties.w & 1u);
    material.uv0 = uv01.xy; material.uv1 = uv01.zw;
    material.uv2 = uv23.xy; material.uv3 = uv23.zw; material.uvMask = uvMask;
    material = EvaluateMaterial(position, normal, material);
    material.albedo = max(material.albedo, 0.0);
    material.roughness = clamp(material.roughness, 0.045, 1.0);
    material.metallic = saturate(material.metallic);
    return material;
}

Surface MaterialAt(float3 position, float3 normal, float4 color, float4 properties)
{
    return MaterialAtUv(position, normal, color, properties, 0, 0, 0);
}

Surface TriangleMaterialAt(float3 position, float3 normal, uint triangleIndex)
{
    RayTriangle tri = rayTriangles[triangleIndex];
    float4 uv01 = 0, uv23 = 0;
    uint mask = 0;
    uint token = asuint(tri.color.w);
    if (token != 0)
    {
        uint4 attributes = rayUvTriangles[token - 1];
        float3 edge1 = tri.position1 - tri.position0, edge2 = tri.position2 - tri.position0;
        float3 area = cross(edge1, edge2), local = position - tri.position0;
        float inverseArea = rcp(max(dot(area, area), 1e-30));
        float u = dot(cross(local, edge2), area) * inverseArea;
        float v = dot(cross(edge1, local), area) * inverseArea;
        float3 weights = float3(1.0 - u - v, u, v);
        uv01 = rayUvVertices[attributes.x * 2] * weights.x + rayUvVertices[attributes.y * 2] * weights.y +
            rayUvVertices[attributes.z * 2] * weights.z;
        uv23 = rayUvVertices[attributes.x * 2 + 1] * weights.x + rayUvVertices[attributes.y * 2 + 1] * weights.y +
            rayUvVertices[attributes.z * 2 + 1] * weights.z;
        mask = attributes.w;
    }
    return MaterialAtUv(position, normal, tri.color, tri.properties, uv01, uv23, mask);
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
    return sky * max(skyColor.rgb, 0.0);
}

#if defined(RAE_HARDWARE_RAYTRACING)
#include "HardwareTrace.hlsli"
#else
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
        bool farSurface = !twoSided && (shadowRay != IsFrontCulled(rayObjects[i].properties));
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
                // Front culling selects the inside/back surface. The choice
                // reverses for a receiver -> light visibility ray.
                t = (-b + (farSurface ? 1.0 : -1.0) * sqrt(discriminant)) / a;
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
                t = farSurface ? farDistance :
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
            if (!AcceptsMaterialFace(facing, rayObjects[i].properties, shadowRay)) continue;
            distance = t;
            hitNormal = normalize(mul(float4(localNormal, 0), rayObjects[i].normalTransform).xyz);
            if ((twoSided || IsFrontCulled(rayObjects[i].properties)) && dot(hitNormal, direction) > 0.0)
                hitNormal = -hitNormal;
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
        if (!AcceptsMaterialFace(facing, tri.properties, shadowRay)) continue;
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
            if ((twoSided || IsFrontCulled(tri.properties)) && facing > 0.0) hitNormal = -hitNormal;
            hitObject = -(j + 2);
        }
    }
    return hitObject != -1;
}
#endif

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
    const bool secondCascade = useCascade && CameraDepth(position) > cascadeParameters.x;
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
    // PCF, 1 is a single hard comparison, 2 is a 3x3 PCF kernel, 3 is
    // PCSS, 4 marks the two-cascade path (which uses the 2x2 kernel) and 5
    // is the 4x4 soft PCF kernel. Keeping the filter in the existing
    // constant buffer lets Forward and Deferred share the same shadow-map
    // pass and makes the mode switch inexpensive.
    if (shadowParameters.w > 2.5 && shadowParameters.w < 3.5)
        return ShadowMapPCSSVisibility(uv, depth, texel, cascade);
    if (shadowParameters.w > 0.5 && shadowParameters.w < 1.5)
        return shadowMap.SampleCmpLevelZero(shadowSampler, float3(uv, cascade), depth);
    float visibility = 0.0;
    if (shadowParameters.w > 4.5 && shadowParameters.w < 5.5)
    {
        // Deterministic 4x4 grid. Each comparison is already bilinear (2x2
        // hardware PCF), so the effective footprint is eight texels wide;
        // fixed offsets avoid temporal shimmer on still shadows.
        [unroll] for (int y = 0; y < 4; ++y)
            [unroll] for (int x = 0; x < 4; ++x)
                visibility += shadowMap.SampleCmpLevelZero(shadowSampler,
                    float3(uv + texel * float2(x - 1.5, y - 1.5), cascade), depth);
        return visibility / 16.0;
    }
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
#if defined(RAE_HARDWARE_RAYTRACING)
    if (outputInfo.w == 0) return 1.0;
    return TraceSceneAny(position + normal * RAY_EPSILON, light) ? 0.0 : 1.0;
#else
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
#endif
}

float3 Fresnel(float cosine, float3 reflectance)
{
    float factor = pow(1.0 - saturate(cosine), 5.0);
    return reflectance + (1.0 - reflectance) * factor;
}

float SceneLightVisibility(uint index, SceneLight source, SceneLightSample lightSample,
    float3 position, float3 normal, float2 uv, bool deferred)
{
    if (source.spotShadow.z < 0.5) return 1.0;
#if defined(RAE_HARDWARE_RAYTRACING)
    if (outputInfo.w == 0) return 1.0;
#else
    if (viewportDebug.w > 1.5) return 1.0;
    if (source.positionType.w == 0.0 &&
        (viewportDebug.w > 0.5 || (int)index == (int)lightInfo.z))
        return ShadowVisibility(position, normal, lightSample.direction, uv, deferred);
    // Only the selected sun owns the raster map. Other lights need actual
    // scene rays, selected explicitly by the AnalyticRay shadow setting.
    if (viewportDebug.w < 0.5) return 1.0;
#endif
    float hitDistance; float3 hitNormal; int hitObject;
    return !TraceScene(position + normal * RAY_EPSILON, lightSample.direction,
        hitDistance, hitNormal, hitObject, true) || hitDistance >= lightSample.distance - RAY_EPSILON ? 1.0 : 0.0;
}

float3 ShadeSurfaceContributions(float3 position, float3 normal, float3 view, Surface material, float2 uv,
    bool deferred, out float3 directDiffuse, out float3 directSpecular)
{
    float nv = max(dot(normal, view), 0.001);
    float alpha = material.roughness * material.roughness;
    float alphaSquared = alpha * alpha;
    float k = (material.roughness + 1.0) * (material.roughness + 1.0) / 8.0;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic);
    directDiffuse = 0.0;
    directSpecular = 0.0;
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        SceneLight source = sceneLights[index];
        [loop] for (uint sampleIndex = 0; sampleIndex < SceneLightSampleCount(source); ++sampleIndex)
        {
            SceneLightSample lightSample;
            if (!SampleSceneLight(source, sampleIndex, position, lightSample)) continue;
            float nl = saturate(dot(normal, lightSample.direction));
            if (nl <= 0.0) continue;
            float3 sum = lightSample.direction + view;
            if (dot(sum, sum) < 1e-12) continue;
            float3 halfway = normalize(sum);
            float nh = saturate(dot(normal, halfway));
            float vh = saturate(dot(view, halfway));
            float denominator = nh * nh * (alphaSquared - 1.0) + 1.0;
            float distribution = alphaSquared / max(PI * denominator * denominator, 0.000001);
            float geometry = (nv / (nv * (1.0 - k) + k)) * (nl / max(nl * (1.0 - k) + k, 0.001));
            float3 fresnel = Fresnel(vh, f0);
            float3 specular = distribution * geometry * fresnel / max(4.0 * nv * nl, 0.001);
            float3 diffuse = (1.0 - fresnel) * (1.0 - material.metallic) * material.albedo / PI;
            float visibility = SceneLightVisibility(index, source, lightSample, position, normal, uv, deferred);
            float3 irradiance = nl * lightSample.irradiance * visibility;
            directDiffuse += diffuse * irradiance;
            directSpecular += specular * irradiance;
        }
    }
    float3 direct = directDiffuse + directSpecular;

#if !defined(RAE_HARDWARE_RAYTRACING)
    if (giViewport.x > 0.5)
    {
        float4 clip = mul(float4(position, 1.0), viewProjection);
        float2 localUv = clip.xy / clip.w * float2(0.5, -0.5) + 0.5;
        uint2 pixel = min(uint2(saturate(localUv) * giViewport.yz), uint2(giViewport.yz) - 1u);
        return direct + indirectLightingBuffer.Load(int3(pixel, 0)).rgb;
    }
#endif

#if defined(RAE_HARDWARE_RAYTRACING)
    float ambientOcclusion = 1.0;
#else
    float ambientOcclusion = deferred && cascadeParameters.z > 0.5 && cascadeParameters.z < 1.5 ?
        ScreenSpaceGTAO(position, normal, uv) : 1.0;
#endif
    float3 ambientDiffuse = Environment(normal, 1.0) * material.albedo * (1.0 - material.metallic) * 0.58 * ambientOcclusion;
    float3 reflection = reflect(-view, normal);
    float3 ambientSpecular = Environment(reflection, material.roughness) * Fresnel(nv, f0) * (1.0 - material.roughness * 0.45);
    return direct + ambientDiffuse + ambientSpecular;
}

float3 ShadeSurface(float3 position, float3 normal, float3 view, Surface material, float2 uv, bool deferred)
{
    float3 directDiffuse, directSpecular;
    return ShadeSurfaceContributions(position, normal, view, material, uv, deferred,
        directDiffuse, directSpecular);
}

float3 Shade(float3 position, float3 normal, Surface material, float2 uv, bool deferred)
{
    float3 view = CameraViewDirection(position);
    float3 color = ShadeSurface(position, normal, view, material, uv, deferred);
#if defined(RAE_HARDWARE_RAYTRACING)
    if (traceOptions.x > 0.5)
#else
    if (parameters.w > 0.5 && giViewport.x < 0.5)
#endif
    {
        float3 ray = reflect(-view, normal);
        float distance; float3 hitNormal; int hitObject;
        if (TraceScene(position + normal * RAY_EPSILON, ray, distance, hitNormal, hitObject, false))
        {
            float3 hitPosition = position + normal * RAY_EPSILON + ray * distance;
#if defined(RAE_HARDWARE_RAYTRACING)
            float4 hitColor = rayTriangles[-hitObject - 2].color;
            float4 hitProperties = rayTriangles[-hitObject - 2].properties;
#else
            float4 hitColor = hitObject >= 0 ? rayObjects[hitObject].color : rayTriangles[-hitObject - 2].color;
            float4 hitProperties = hitObject >= 0 ? rayObjects[hitObject].properties : rayTriangles[-hitObject - 2].properties;
#endif
            Surface hitMaterial;
            if (hitObject <= -2)
                hitMaterial = TriangleMaterialAt(hitPosition, hitNormal, (uint)(-hitObject - 2));
            else
                hitMaterial = MaterialAt(hitPosition, hitNormal, hitColor, hitProperties);
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
    if (viewportDebug.z > 3.5 && viewportDebug.z < 4.5) return float4(frac(abs(position) * 0.25), 1.0);
    if (viewportDebug.z > 4.5 && viewportDebug.z < 5.5) return float4(material.roughness.xxx, 1.0);
    if (viewportDebug.z > 5.5 && viewportDebug.z < 6.5) return float4(material.metallic.xxx, 1.0);
    if (viewportDebug.z > 6.5 && viewportDebug.z < 7.5)
        return float4(pow(saturate(lerp(float3(0.04, 0.04, 0.04), material.albedo, material.metallic)), 1.0 / 2.2), 1.0);
    if (viewportDebug.z > 7.5 && viewportDebug.z < 8.5)
    {
        // A fixed 100m display scale works for both projection families.
        float linearDepth = saturate(CameraDepth(position) / 100.0);
        return float4(linearDepth.xxx, 1.0);
    }
    if (viewportDebug.z > 8.5 && viewportDebug.z < 10.5)
    {
        float3 directDiffuse, directSpecular;
        ShadeSurfaceContributions(position, normal, CameraViewDirection(position), material,
            uv, deferred, directDiffuse, directSpecular);
        return float4(DisplayColor(viewportDebug.z < 9.5 ? directDiffuse : directSpecular), 1.0);
    }
    return float4(DisplayColor(Shade(position, normal, material, uv, deferred)), 1.0);
}

float4 Background(float2 uv)
{
    // Diagnostic material/lighting modes have no surface value in the sky.
    if (viewportDebug.z > 4.5) return float4(0.0, 0.0, 0.0, 1.0);
    // cameraPosition.w is the solid-black backdrop flag written by the renderer
    // (the field is otherwise unused by the shaders). Scenes that ask for a
    // black backdrop skip the sky gradient entirely.
    if (cameraPosition.w > 0.5) return float4(0.0, 0.0, 0.0, 1.0);
    float3 sky = lerp(float3(0.035, 0.055, 0.09), float3(0.15, 0.22, 0.32), saturate(uv.y));
    float vignette = 1.0 - 0.22 * dot(uv - 0.5, uv - 0.5);
    return float4(DisplayColor(sky * vignette * max(skyColor.rgb, 0.0)), 1.0);
}

float4 PSBackground(FullscreenOutput input) : SV_TARGET
{
    return Background(input.uv);
}

float3 SurfaceNormal(float3 normal, bool frontFace)
{
    normal = normalize(normal);
    return (IsTwoSided(objectParameters) || IsFrontCulled(objectParameters)) && !frontFace ? -normal : normal;
}

float4 PSForward(GeometryOutput input, bool frontFace : SV_IsFrontFace) : SV_TARGET
{
    float3 normal = SurfaceNormal(input.worldNormal, frontFace);
    Surface material = MaterialAtUv(input.worldPosition, normal, objectColor, objectParameters,
        input.uv01, input.uv23, (uint)objectColor.w);
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
    Surface material = MaterialAtUv(input.worldPosition, normal, objectColor, objectParameters,
        input.uv01, input.uv23, (uint)objectColor.w);
    output.albedo = float4(material.albedo, material.roughness);
    output.normal = float4(normal, material.metallic);
    output.position = float4(input.worldPosition, 1.0);
    return output;
}

#if !defined(RAE_HARDWARE_RAYTRACING)
// Screen-space diffuse GI trace. The depth/normal buffer and previous-frame
// direct colour are current; the indirect result is accumulated separately so
// the trace never samples its own previous indirect contribution.
float3 ScreenSpaceTrace(uint2 pixel, float3 position, float3 normal)
{
    if (giScreenSpace.x < 0.5 || giHistory.x < 0.5 || giViewport.w < 0.5) return 0.0;
    uint steps = max((uint)giViewport.w, 1u);
    float3 tangent = normalize(cross(abs(normal.y) > 0.9 ? float3(1, 0, 0) : float3(0, 1, 0), normal));
    float3 bitangent = cross(normal, tangent);
    float seed = frac(sin(dot(float2(pixel) + giHistory.y * 37.0, float2(12.9898, 78.233))) * 43758.5453);
    float3 sum = 0.0;
    float weight = 0.0;
    [loop] for (uint i = 0; i < steps; ++i)
    {
        float angle = seed * 6.2831853 + float(i) * 2.3999632;
        float radial = sqrt(frac(seed + float(i) * 0.6180339));
        float3 direction = normalize(tangent * cos(angle) * radial + bitangent * sin(angle) * radial + normal * 0.75);
        float travel = giScreenSpace.y * (float(i) + 1.0) / float(steps);
        float3 samplePosition = position + direction * travel;
        float4 clip = mul(float4(samplePosition, 1.0), viewProjection);
        if (clip.w <= 0.0001) continue;
        float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        float4 scenePosition = positionBuffer.SampleLevel(gbufferSampler, uv, 0);
        if (scenePosition.w < 0.5) continue;
        float3 delta = scenePosition.xyz - samplePosition;
        // Keep a minimum world-space thickness so a small UI value cannot reject
        // every screen-space sample on large scenes.
        float thickness = max(giScreenSpace.z, 0.5 * max(giScreenSpace.y, 0.5));
        if (dot(delta, delta) > thickness * thickness) continue;
        float3 radiance = ssgiDirectCurrent.SampleLevel(gbufferSampler, uv, 0).rgb;
        // Reject emitter-like white pixels; with no G-buffer object/emission id
        // this is the cheapest guard against SSGI sampling a light fixture as
        // ordinary diffuse radiance.
        float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
        if (luminance > 0.85) continue;
        sum += min(radiance, 0.5);
        weight += 1.0;
    }
    return min(weight > 0.0 ? sum / weight : 0.0, 16.0);
}

float4 PSScreenSpaceGi(FullscreenOutput input) : SV_TARGET
{
    int3 pixel = int3(int2(input.position.xy), 0);
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5) return 0.0;
    float3 normal = normalize(normalBuffer.Load(pixel).xyz);
    return float4(ScreenSpaceTrace(uint2(pixel.xy), position.xyz, normal), 1.0);
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
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5)
    {
        output.color = Background(input.uv);
        output.history = 0.0;
        return output;
    }
    float4 normal = normalBuffer.Load(pixel);
    float3 normalValue = normalize(normal.xyz);
    float3 raw = ssgiRawIndirect.Load(pixel).rgb;
    float3 spatialSum = raw;
    float spatialWeight = 1.0;
    float3 low = raw;
    float3 high = raw;
    int2 coordinate = int2(pixel.xy);
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        if (x == 0 && y == 0) continue;
        int2 neighbour = coordinate + int2(x, y);
        if (any(neighbour < 0) || any(neighbour >= int2(giViewport.yz))) continue;
        int3 neighbourPixel = int3(neighbour, 0);
        float4 otherPosition = positionBuffer.Load(neighbourPixel);
        if (otherPosition.w < 0.5) continue;
        float4 otherNormal = normalBuffer.Load(neighbourPixel);
        float cosine = dot(normalValue, normalize(otherNormal.xyz));
        if (cosine < 0.9) continue;
        float3 delta = otherPosition.xyz - position.xyz;
        float plane = abs(dot(delta, normalValue));
        if (plane > 0.75) continue;
        float3 sample = ssgiRawIndirect.Load(neighbourPixel).rgb;
        float weight = 1.0 / (1.0 + length(delta) + abs(x) + abs(y));
        spatialSum += sample * weight;
        spatialWeight += weight;
        low = min(low, sample);
        high = max(high, sample);
    }
    float3 indirect = spatialSum / max(spatialWeight, 1e-5);
    if (giHistory.x > 0.5 && giHistory.w < 1.5)
    {
        float4 clip = mul(float4(position.xyz, 1.0), giPreviousViewProjection);
        if (clip.w > 0.0001)
        {
            float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5);
            if (all(uv > 0.0) && all(uv < 1.0))
            {
                float3 history = ssgiHistoryIndirect.SampleLevel(gbufferSampler, uv, 0).rgb;
                history = clamp(history, low, high);
                indirect = lerp(history, indirect, saturate(giHistory.z));
            }
        }
    }
    output.history = float4(indirect, 1.0);
    float3 direct = ssgiDirectCurrent.Load(pixel).rgb;
    indirect *= albedoBuffer.Load(pixel).rgb;
    output.color = giHistory.w > 1.5 ? float4(indirect, 1.0)
        : float4(saturate(direct + indirect * giScreenSpace.w), 1.0);
    return output;
}

static const float kShY00 = 0.2820947918;
static const float kShY1 = 0.4886025119;
static const float kShY2 = 1.0925484306;
static const float kShY20 = 0.3153915653;
static const float kShY22 = 0.5462742153;

float3 ProbeshIrradiance(float3 c0, float3 c1, float3 c2, float3 c3,
    float3 c4, float3 c5, float3 c6, float3 c7, float3 c8, int order, float3 normal)
{
    float3 n = normalize(normal);
    float3 irradiance = PI * c0 * kShY00 +
        (2.0 * PI / 3.0) * kShY1 * (c1 * n.y + c2 * n.z + c3 * n.x);
    if (order > 1)
    {
        float3 l2 = c4 * (kShY2 * n.x * n.y) +
            c5 * (kShY2 * n.y * n.z) +
            c6 * (kShY20 * (3.0 * n.z * n.z - 1.0)) +
            c7 * (kShY2 * n.x * n.z) +
            c8 * (kShY22 * (n.x * n.x - n.y * n.y));
        irradiance += (PI / 4.0) * l2;
    }
    return irradiance / PI;
}

float3 ProbeIrradiance(float3 position, float3 normal)
{
    if (probeParameters.x < 0.5 || probeParameters.y < 0.5) return 0.0;
    int shOrder = probeLayout.x > 1.5 ? 2 : 1;
    int coefficientCount = shOrder == 2 ? 9 : 4;
    int3 dimensions = int3(probeGrid.xyz);
    float3 local = (position - probeOrigin.xyz) / max(probeOrigin.w, 1e-4) - 0.5;
    int3 base = int3(floor(local));
    float3 fraction = saturate(local - floor(local));
    float3 c[9];
    [unroll] for (int shIndex = 0; shIndex < 9; ++shIndex) c[shIndex] = 0.0;
    float weightSum = 0.0;
    [unroll] for (int y = 0; y < 2; ++y)
    [unroll] for (int z = 0; z < 2; ++z)
    [unroll] for (int x = 0; x < 2; ++x)
    {
        int3 coordinate = clamp(base + int3(x, y, z), int3(0, 0, 0), dimensions - 1);
        float weight = (x ? fraction.x : 1.0 - fraction.x) *
            (y ? fraction.y : 1.0 - fraction.y) * (z ? fraction.z : 1.0 - fraction.z);
        int2 probeCoordinate = int2(coordinate.x + coordinate.z * dimensions.x, coordinate.y);
        int atlasBase = probeCoordinate.x * coefficientCount;
        float confidence = probeParameters.w > 1.5 && probeParameters.w < 2.5 ? 1.0 :
            probeAtlasRead.Load(int3(atlasBase, probeCoordinate.y, 0)).a;
        if (probeParameters.w > 1.5 && probeParameters.w < 2.5)
        {
            // Chebyshev visibility still uses the per-probe depth moments.
            float4 depthSample = probeDepthAtlas.Load(int3(probeCoordinate, 0));
            float3 probeCenter = probeOrigin.xyz + (float3(coordinate) + 0.5) * probeOrigin.w;
            float receiverDistance = length(position - probeCenter);
            float meanDistance = depthSample.x;
            float meanSquared = depthSample.y;
            float variance = max(meanSquared - meanDistance * meanDistance, 1e-4);
            float difference = meanDistance - receiverDistance;
            float visibility = difference >= 0.0 ? 1.0
                : saturate(variance / (variance + difference * difference));
            confidence *= saturate(0.35 + 0.65 * visibility * saturate(depthSample.z));
        }
        float weighted = weight * confidence;
        [loop] for (int coefficient = 0; coefficient < coefficientCount; ++coefficient)
            c[coefficient] += probeAtlasRead.Load(int3(
                atlasBase + coefficient, probeCoordinate.y, 0)).rgb * weighted;
        weightSum += weighted;
    }
    float inverseWeight = 1.0 / max(weightSum, 1e-5);
    return ProbeshIrradiance(c[0] * inverseWeight, c[1] * inverseWeight,
        c[2] * inverseWeight, c[3] * inverseWeight, c[4] * inverseWeight,
        c[5] * inverseWeight, c[6] * inverseWeight, c[7] * inverseWeight,
        c[8] * inverseWeight, shOrder, normal) * probeParameters.z;
}

float3 BoundProbeIndirect(float3 directColor, float3 indirect)
{
    const float3 luminanceWeights = float3(0.2126, 0.7152, 0.0722);
    float directLuminance = dot(directColor, luminanceWeights);
    float indirectLuminance = dot(indirect, luminanceWeights);
    float limit = max(directLuminance * 1.5, 0.08);
    return indirect * min(1.0, limit / max(indirectLuminance, 1e-4));
}

// Software ray-traced probe fallback: trace a small set of world-space
// directions against the current depth/position buffer. This is not DXR, but it
// replaces the old single projected-pixel copy with an actual multi-ray
// integration and works on native D3D12 raster as well as strict D3D11.
struct ProbeSH
{
    float3 c0;
    float3 c1;
    float3 c2;
    float3 c3;
    float3 c4;
    float3 c5;
    float3 c6;
    float3 c7;
    float3 c8;
};

ProbeSH SoftwareProbeTrace(int3 probe, int3 dimensions)
{
    float spacing = max(probeOrigin.w, 0.01);
    float3 world = probeOrigin.xyz + (float3(probe) + 0.5) * spacing;
    ProbeSH sh;
    sh.c0 = 0.0; sh.c1 = 0.0; sh.c2 = 0.0; sh.c3 = 0.0;
    sh.c4 = 0.0; sh.c5 = 0.0; sh.c6 = 0.0; sh.c7 = 0.0; sh.c8 = 0.0;
    const uint directionCount = 64u;
    const uint stepCount = 24u;
    const float shProjection = 4.0 * PI / float(directionCount);
    float probeSeed = frac(sin(dot(float3(probe), float3(12.9898, 78.233, 37.719))) * 43758.5453);
    float stepSize = max(spacing * 0.75, 0.2);
    float tolerance = max(spacing * 1.5, stepSize * 1.25);
    [loop] for (uint directionIndex = 0; directionIndex < directionCount; ++directionIndex)
    {
        float z = 1.0 - 2.0 * (float(directionIndex) + 0.5) / float(directionCount);
        float radial = sqrt(max(1.0 - z * z, 0.0));
        float phi = (float(directionIndex) + probeSeed) * 2.39996323;
        float3 direction = float3(radial * cos(phi), z, radial * sin(phi));
        float3 radiance = Environment(direction, 0.0);
        float3 sampleWorld = world + direction * stepSize;
        [loop] for (uint step = 0; step < stepCount; ++step)
        {
            float4 clip = mul(float4(sampleWorld, 1.0), viewProjection);
            if (clip.w > 0.0001)
            {
                float2 sampleUv = float2(clip.x / clip.w * 0.5 + 0.5,
                    -clip.y / clip.w * 0.5 + 0.5);
                if (all(sampleUv >= 0.0) && all(sampleUv <= 1.0))
                {
                    float4 scenePosition = positionBuffer.SampleLevel(gbufferSampler, sampleUv, 0);
                    if (scenePosition.w >= 0.5 &&
                        distance(scenePosition.xyz, sampleWorld) <= tolerance)
                    {
                        radiance = probeColorSource.SampleLevel(gbufferSampler, sampleUv, 0).rgb;
                        break;
                    }
                }
            }
            sampleWorld += direction * stepSize;
        }
        sh.c0 += radiance * (shProjection * kShY00);
        sh.c1 += radiance * (shProjection * kShY1 * direction.y);
        sh.c2 += radiance * (shProjection * kShY1 * direction.z);
        sh.c3 += radiance * (shProjection * kShY1 * direction.x);
        sh.c4 += radiance * (shProjection * kShY2 * direction.x * direction.y);
        sh.c5 += radiance * (shProjection * kShY2 * direction.y * direction.z);
        sh.c6 += radiance * (shProjection * kShY20 * (3.0 * direction.z * direction.z - 1.0));
        sh.c7 += radiance * (shProjection * kShY2 * direction.x * direction.z);
        sh.c8 += radiance * (shProjection * kShY22 *
            (direction.x * direction.x - direction.y * direction.y));
    }
    return sh;
}

float4 PSProbeUpdate(FullscreenOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    int3 dimensions = int3(probeGrid.xyz);
    int coefficientCount = probeLayout.x > 1.5 ? 9 : 4;
    int coefficient = pixel.x % coefficientCount;
    int probeX = pixel.x / coefficientCount;
    int ix = probeX % max(dimensions.x, 1);
    int iz = probeX / max(dimensions.x, 1);
    int iy = pixel.y;
    if (iz >= dimensions.z || iy >= dimensions.y) return 0.0;
    float4 previous = probeParameters.y > 0.5 ?
        probeAtlasRead.Load(int3(pixel, 0)) : float4(0.0, 0.0, 0.0, 1.0);
    if (probeParameters.x < 0.5) return previous;
    if (probeParameters.w > 2.5)
    {
        ProbeSH sh = SoftwareProbeTrace(int3(ix, iy, iz), dimensions);
        float3 current;
        if (coefficient == 0) current = sh.c0;
        else if (coefficient == 1) current = sh.c1;
        else if (coefficient == 2) current = sh.c2;
        else if (coefficient == 3) current = sh.c3;
        else if (coefficient == 4) current = sh.c4;
        else if (coefficient == 5) current = sh.c5;
        else if (coefficient == 6) current = sh.c6;
        else if (coefficient == 7) current = sh.c7;
        else current = sh.c8;
        float blend = probeParameters.y > 0.5 ? saturate(1.0 - probeGrid.w) : 1.0;
        return float4(lerp(previous.rgb, current, blend), 1.0);
    }
    // The raster probe update path is either SH software DDGI (above) or the
    // DXR atlas copied in by the hardware transport; no projected fallback
    // writes this atlas anymore.
    return previous;
}

float4 PSProbeComposite(FullscreenOutput input) : SV_TARGET
{
    int3 pixel = int3(int2(input.position.xy), 0);
    int debugMode = (int)(viewportDebug.z + 0.5);
    float4 position = positionBuffer.Load(pixel);
    float4 direct = ssgiDirectCurrent.Load(pixel);
    if (position.w < 0.5) return debugMode == 11 ? float4(0.0, 0.0, 0.0, 1.0) : direct;
    float4 directLinear = probeColorSource.Load(pixel);
    float4 albedo = albedoBuffer.Load(pixel);
    float3 normal = normalize(normalBuffer.Load(pixel).xyz);
    float3 indirect = ProbeIrradiance(position.xyz, normal) * albedo.rgb;
    indirect = BoundProbeIndirect(directLinear.rgb, indirect);
    if (debugMode == 11) return float4(DisplayColor(indirect * 2.0), 1.0);
    return float4(DisplayColor(directLinear.rgb + indirect), 1.0);
}

float4 PSProbeLinear(FullscreenOutput input) : SV_TARGET
{
    int3 pixel = int3(int2(input.position.xy), 0);
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5) return 0.0;
    float4 albedo = albedoBuffer.Load(pixel);
    float4 normal = normalBuffer.Load(pixel);
    Surface material = (Surface)0;
    material.albedo = albedo.rgb;
    material.roughness = albedo.a;
    material.metallic = normal.a;
    material.grid = 0;
    return float4(Shade(position.xyz, normalize(normal.xyz), material, input.uv, false), 1.0);
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
    float3 worldPosition : TEXCOORD0;
    float3 worldNormal : TEXCOORD1;
    float4 color : COLOR;
};

float3 ProbeMarkerLighting(float3 position, float3 normal)
{
    float3 lighting = 0.0;
    // Directional term keeps the spheres readable even when the scene only has
    // an environment/area light that is not represented in sceneLights.
    float3 sunDirection = normalize(lightDirectionIntensity.xyz);
    float sunNdl = saturate(dot(normal, sunDirection));
    lighting += float3(0.7, 0.7, 0.7) * sunNdl * max(lightDirectionIntensity.w, 0.5);
    [loop] for (uint index = 0; index < min((uint)lightInfo.x, 32u); ++index)
    {
        SceneLight source = sceneLights[index];
        [loop] for (uint sampleIndex = 0; sampleIndex < SceneLightSampleCount(source); ++sampleIndex)
        {
            SceneLightSample lightSample;
            if (!SampleSceneLight(source, sampleIndex, position, lightSample)) continue;
            float ndl = saturate(dot(normal, lightSample.direction));
            lighting += lightSample.irradiance * ndl;
        }
    }
    lighting += Environment(normal, 1.0) * 0.35;
    return lighting;
}

ProbeDebugOutput VSProbeDebug(ProbeDebugVertex input)
{
    ProbeDebugOutput output;
    float3 worldPosition = input.instancePosition + input.position * input.radius.x;
    float4 clip = mul(float4(worldPosition, 1.0), viewProjection);
    if (clip.w <= 0.0001)
    {
        output.position = float4(2.0, 2.0, 2.0, 1.0);
        output.worldPosition = worldPosition;
        output.worldNormal = input.normal;
        output.color = 0.0;
        return output;
    }
    clip.z -= 0.0001 * clip.w;
    output.position = clip;
    output.worldPosition = worldPosition;
    output.worldNormal = input.normal;
    int2 probeCoordinate = int2(input.atlas);
    int probeStride = probeLayout.x > 1.5 ? 9 : 4;
    float4 probe = probeAtlasRead.Load(int3(probeCoordinate.x * probeStride, probeCoordinate.y, 0));
    output.color = float4(saturate(probe.rgb), 1.0);
    return output;
}

float4 PSProbeDebug(ProbeDebugOutput input) : SV_TARGET
{
    float3 normal = normalize(input.worldNormal);
    float3 lighting = ProbeMarkerLighting(input.worldPosition, normal);
    // Keep the marker readable against dark scenes and show the key-light
    // direction through a small specular highlight. Probe irradiance tints the
    // otherwise neutral sphere.
    float3 keyDirection = normalize(float3(0.35, 0.8, 0.45));
    float keyNdl = saturate(dot(normal, keyDirection));
    float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    float3 halfDirection = normalize(keyDirection + viewDirection);
    float specular = pow(saturate(dot(normal, halfDirection)), 24.0) * 0.35;
    float3 albedo = lerp(float3(0.5, 0.5, 0.5), saturate(input.color.rgb), 0.65);
    float3 color = albedo * (0.35 + 0.65 * keyNdl) + albedo * lighting + specular;
    return float4(saturate(color), 1.0);
}
#endif

float4 PSDeferred(FullscreenOutput input) : SV_TARGET
{
    int3 pixel = int3(int2(input.position.xy), 0);
    int debugMode = (int)(viewportDebug.z + 0.5);
    float4 position = positionBuffer.Load(pixel);
    if (position.w < 0.5) return debugMode == 11 ? float4(0.0, 0.0, 0.0, 1.0) : Background(input.uv);
#if !defined(RAE_HARDWARE_RAYTRACING)
    if (debugMode == 11)
    {
        if (probeParameters.x > 0.5)
        {
            // Hardware DDGI publishes a complete atlas before Deferred runs, so
            // its debug view can resolve here. Software DDGI needs the current
            // direct color as its trace source and is composited in
            // PSProbeComposite instead.
            if (probeParameters.w < 2.5)
                return float4(saturate(ProbeIrradiance(position.xyz,
                    normalBuffer.Load(pixel).xyz) * albedoBuffer.Load(pixel).rgb * 4.0), 1.0);
        }
        else if (giViewport.x > 0.5)
            return float4(DisplayColor(indirectLightingBuffer.Load(pixel).rgb), 1.0);
        // SSGI's trace consumes the direct-lit colour. Keep the normal direct
        // output here; the composite pass replaces the final colour with the
        // indirect-only debug result.
        else if (giViewport.w < 0.5) return 0.0;
    }
#endif
    float4 albedo = albedoBuffer.Load(pixel);
    float4 normal = normalBuffer.Load(pixel);
    Surface material = (Surface)0;
    material.albedo = albedo.rgb;
    material.roughness = albedo.a;
    material.metallic = normal.a;
    material.grid = 0; // Grid color is already evaluated in the G-buffer pass.
    float4 color = OutputSurface(position.xyz, normalize(normal.xyz), material, input.uv, true);
    return color;
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
        // the pixel we paint on, which keeps interior details out and leaves the
        // front occluder untouched. outlineParams.w > 0.5 opts into the symmetric
        // test, so geometry in front of the selection is outlined as well.
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
    // The ring where an occluder crosses the selection stays dashed in both
    // modes; the option only widens the solid silhouette test above.
    const bool dashOn = ((pixel.x + pixel.y) & 7) < 4;
    float alpha = 0.0;
    if (solid > 0.5) alpha = outlineColor.a;
    else if (hiddenEdge > 0.5 && dashOn) alpha = outlineColor.a * 0.9;
    return alpha > 0.0 ? float4(outlineColor.rgb, alpha) : float4(0.0, 0.0, 0.0, 0.0);
}

#include "DepthOfField.hlsli"
#include "AntiAliasing.hlsli"
