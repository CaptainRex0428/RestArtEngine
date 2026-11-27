// Each world voxel has a packed surface record and two outgoing diffuse values,
// one per side. The grid is fitted to scene geometry independently of the camera.
Texture2D<float4> worldProbeSurface : register(t22);
Texture2D<float4> worldProbeRadiance : register(t23);
cbuffer WorldVolumeConstants : register(b10)
{
    float4 worldVoxelOriginPitch;
    float4 worldVoxelDimensions; // xyz dimensions, w enables light visibility
};

int2 WorldVoxelTexel(int3 cell)
{
    return int2(2 * (cell.x + cell.z * (int)worldVoxelDimensions.x), cell.y);
}

// Amanatides-Woo traversal in voxel units. Test the triangle plane in each
// occupied voxel to avoid treating thin surfaces as whole opaque cubes.
bool TraceWorldVoxels(float3 origin, float3 direction, float maxDistance, out int2 hit)
{
    hit = 0;
    int3 dimensions = (int3)worldVoxelDimensions.xyz;
    float3 p = (origin - worldVoxelOriginPitch.xyz) / worldVoxelOriginPitch.w;
    bool3 parallelAxis = abs(direction) < 1e-7;
    bool parallelMiss = any(parallelAxis & ((p < 0.0) | (p >= dimensions)));
    int3 stepAxis = int3(direction.x >= 0 ? 1 : -1, direction.y >= 0 ? 1 : -1, direction.z >= 0 ? 1 : -1);
    float3 reciprocal = (float3)stepAxis / max(abs(direction), 1e-7);
    float3 first = -p * reciprocal;
    float3 last = ((float3)dimensions - p) * reciprocal;
    float3 nearTime = min(first, last), farTime = max(first, last);
    nearTime = float3(parallelAxis.x ? -1e20 : nearTime.x, parallelAxis.y ? -1e20 : nearTime.y, parallelAxis.z ? -1e20 : nearTime.z);
    farTime = float3(parallelAxis.x ? 1e20 : farTime.x, parallelAxis.y ? 1e20 : farTime.y, parallelAxis.z ? 1e20 : farTime.z);
    float t = max(0.0, max(nearTime.x, max(nearTime.y, nearTime.z)));
    float end = min(maxDistance / worldVoxelOriginPitch.w, min(farTime.x, min(farTime.y, farTime.z)));
    bool found = false;
    int3 cell = clamp((int3)floor(p + direction * (t + 1e-4)), int3(0, 0, 0), dimensions - 1);
    float3 nextEdge = ((float3)cell + saturate((float3)stepAxis) - p) * reciprocal;
    nextEdge = float3(parallelAxis.x ? 1e20 : nextEdge.x, parallelAxis.y ? 1e20 : nextEdge.y, parallelAxis.z ? 1e20 : nextEdge.z);
    [loop] for (uint i = 0; i < 193u && !parallelMiss && t < end; ++i)
    {
        float exitTime = min(end, min(nextEdge.x, min(nextEdge.y, nextEdge.z)));
        int2 texel = WorldVoxelTexel(cell);
        if (worldProbeSurface.Load(int3(texel, 0)).a > 0)
        {
            float4 plane = worldProbeSurface.Load(int3(texel + int2(1, 0), 0)) * 2.0 - 1.0;
            float3 normal = normalize(plane.xyz);
            float facing = dot(normal, direction);
            float distanceToPlane = dot(normal, (float3)cell + 0.5 - (p + direction * t)) + plane.w;
            // Plane offset/normal are eight-bit. Bound quantization error in
            // plane-distance units so a grazing ray never gets a distant hit.
            float exitDistance = distanceToPlane - facing * max(0.0, exitTime - t);
            if (min(distanceToPlane, exitDistance) <= 0.012 && max(distanceToPlane, exitDistance) >= -0.012)
            {
                hit = texel + int2(facing > 0 ? 1 : 0, 0);
                found = true;
                break;
            }
        }
        if (exitTime >= end) break;
        bool3 advance = nextEdge <= exitTime + 1e-6;
        int3 axes = int3(advance.x, advance.y, advance.z);
        cell += stepAxis * axes;
        if (any(cell < 0) || any(cell >= dimensions)) break;
        nextEdge += abs(reciprocal) * (float3)axes;
        t = exitTime;
    }
    return found;
}

float3 WorldProbeRay(float3 origin, float3 direction)
{
    int2 hit = 0;
    float3 radiance = 0.0;
    if (TraceWorldVoxels(origin, direction, 1e20, hit))
        radiance = worldProbeRadiance.Load(int3(hit, 0)).rgb;
    return radiance;
}

float4 PSWorldProbeLighting(FullscreenOutput input) : SV_TARGET
{
    int2 pixel = (int2)input.position.xy;
    bool back = (pixel.x & 1) != 0;
    int2 texel = int2(pixel.x & ~1, pixel.y);
    float4 material = worldProbeSurface.Load(int3(texel, 0));
    if (material.a == 0 || (back && material.a < 0.75)) return 0.0;
    uint column = uint(texel.x) >> 1u;
    uint3 cell = uint3(column % (uint)worldVoxelDimensions.x, texel.y, column / (uint)worldVoxelDimensions.x);
    float4 plane = worldProbeSurface.Load(int3(texel + int2(1, 0), 0)) * 2.0 - 1.0;
    float3 normal = normalize(plane.xyz);
    float3 position = worldVoxelOriginPitch.xyz + ((float3)cell + 0.5 + normal * plane.w) * worldVoxelOriginPitch.w;
    if (back) normal = -normal;
    float3 outgoing = max(skyColor.rgb, 0.0) * 0.25;
    [loop] for (uint lightIndex = 0; lightIndex < (uint)lightInfo.x; ++lightIndex)
    {
        SceneLight light = sceneLights[lightIndex];
        [loop] for (uint sampleIndex = 0; sampleIndex < SceneLightSampleCount(light); ++sampleIndex)
        {
            SceneLightSample incident;
            if (!SampleSceneLight(light, sampleIndex, position, incident)) continue;
            float cosine = saturate(dot(normal, incident.direction));
            if (cosine == 0.0) continue;
            float bias = 0.06 * worldVoxelOriginPitch.w;
            int2 blocker;
            [branch] if (worldVoxelDimensions.w > 0.5 && light.spotShadow.z > 0.5) {
                if (TraceWorldVoxels(position + normal * bias, incident.direction, incident.distance - bias, blocker)) continue;
            }
            outgoing += incident.irradiance * (cosine / PI);
        }
    }
    return float4(min(outgoing * material.rgb, 65504.0), 1);
}
