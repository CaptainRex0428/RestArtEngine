// Matches Renderer/Lighting.h. Enum values match Scene/Light.h:
// Directional=0, Sky=1 (aggregated), Rect=2, Spot=3, Point=4.
// Shared by Forward, Deferred, preview rays and hardware path transport.
struct SceneLight
{
    float4 positionType;
    float4 directionRange;
    float4 colorIntensity;
    float4 axisUWidth;
    float4 axisVHeight;
    float4 spotShadow;
};
cbuffer LightingConstants : register(b7)
{
    float4 lightInfo; // count, authored/override mode, shadow-map index, overflow
    float4 skyColor; // accumulated tint * intensity; w = actor-authored mode
    SceneLight sceneLights[32];
};

struct SceneLightSample
{
    float3 direction;
    float3 irradiance; // Incident radiance * source solid-angle/point falloff.
    float distance;
};

uint SceneLightSampleCount(SceneLight light)
{
    return light.positionType.w == 2.0 ? 4u : 1u;
}

// Rectangles use a deterministic four-point area quadrature. This approximation
// is intentionally identical in raster and preview ray paths. Progressive and
// realtime path transport instead use paired area/BRDF MIS estimators in
// PathTracing.hlsli. This quadrature is not LTC integration. Emitting side: -Z.
bool SampleSceneLight(SceneLight light, uint sampleIndex, float3 position,
    out SceneLightSample result)
{
    result.direction = float3(0, 1, 0);
    result.irradiance = 0;
    result.distance = 1e20;
    float3 radiance = max(light.colorIntensity.rgb, 0.0) * max(light.colorIntensity.w, 0.0);
    if (!any(radiance > 0.0)) return false;
    if (light.positionType.w == 0.0)
    {
        result.direction = -light.directionRange.xyz;
        result.irradiance = radiance;
        return true;
    }
    float3 source = light.positionType.xyz;
    bool rectangle = light.positionType.w == 2.0;
    if (rectangle)
    {
        float2 offset = float2((sampleIndex & 1u) ? 1.0 : -1.0,
            (sampleIndex & 2u) ? 1.0 : -1.0) * 0.28867513459;
        source += light.axisUWidth.xyz * light.axisUWidth.w * offset.x
            + light.axisVHeight.xyz * light.axisVHeight.w * offset.y;
    }
    float3 delta = source - position;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared < 1e-8) return false;
    result.distance = sqrt(distanceSquared);
    result.direction = delta / result.distance;
    float rangeFade = 1.0;
    if (light.directionRange.w > 0.0)
    {
        float relative = result.distance / light.directionRange.w;
        rangeFade = saturate(1.0 - relative * relative * relative * relative);
        rangeFade *= rangeFade;
    }
    else if (!rectangle) return false;
    float attenuation = rangeFade / max(distanceSquared, 0.01);
    if (light.positionType.w == 3.0)
    {
        float cosine = dot(-result.direction, light.directionRange.xyz);
        float inner = light.spotShadow.x;
        float outer = light.spotShadow.y;
        float cone = inner - outer < 1e-6 ? step(outer, cosine)
            : smoothstep(outer, inner, cosine);
        attenuation *= cone;
    }
    if (rectangle)
    {
        float emittingCosine = saturate(dot(-result.direction, light.directionRange.xyz));
        attenuation *= emittingCosine * light.axisUWidth.w * light.axisVHeight.w * 0.25;
    }
    result.irradiance = radiance * attenuation;
    return any(result.irradiance > 0.0);
}
