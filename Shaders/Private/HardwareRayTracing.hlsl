// Hardware scene pass. Lighting, material source, display transform and debug
// modes are shared with the raster pipeline; intersections use DXR acceleration
// structures through HardwareTrace.hlsli.
#define RAE_HARDWARE_RAYTRACING 1
#include "Scene.hlsl"

RWTexture2D<float4> hardwareOutput : register(u0);
// Persistent linear-HDR mean. Alpha records the accumulated sample count for
// inspection; CPU b5 remains the authoritative sample/reset contract.
RWTexture2D<float4> pathAccumulation : register(u1);

#include "PathTracing.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 thread : SV_DispatchThreadID)
{
    if (any(thread.xy >= outputInfo.xy)) return;
    if (accumulationInfo.w == 1 && viewportDebug.z < 0.5)
    {
        uint previousSamples = accumulationInfo.x;
        uint batchSamples = accumulationInfo.y;
        float3 mean = 0.0;
        if (batchSamples != 0)
        {
            float3 batchSum = 0.0;
            [loop] for (uint sampleOrdinal = 0; sampleOrdinal < batchSamples; ++sampleOrdinal)
                batchSum += PathRadiance(thread.xy, previousSamples + sampleOrdinal);
            float3 batchMean = batchSum / float(batchSamples);
            // No read from an uninitialized or invalidated history allocation.
            if (previousSamples == 0) mean = batchMean;
            else mean = lerp(pathAccumulation[thread.xy].rgb, batchMean,
                float(batchSamples) / float(previousSamples + batchSamples));
            pathAccumulation[thread.xy] = float4(mean, float(previousSamples + batchSamples));
        }
        else if (previousSamples != 0)
        {
            // Resolve still runs after convergence. Exposure changes do not
            // need new paths or invalidate the linear radiance accumulated here.
            mean = pathAccumulation[thread.xy].rgb;
        }
        hardwareOutput[thread.xy] = float4(DisplayColor(mean), 1.0);
        return;
    }
    // Disabled GI and material/debug views preserve the exact preview path.
    // Debug output never enters radiance accumulation.
    float2 uv = (float2(thread.xy) + 0.5) / float2(outputInfo.xy);
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    // Start at the raster camera's near plane and stop at its far plane, so
    // primary visibility agrees with the scene depth used by editor overlays.
    float4 nearPoint = mul(float4(ndc, 0.0, 1.0), inverseViewProjection);
    float4 farPoint = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    float3 origin = nearPoint.xyz / nearPoint.w;
    float3 farPosition = farPoint.xyz / farPoint.w;
    float3 direction = normalize(farPosition - origin);
    float distance;
    float3 normal;
    int object;
    if (!TraceScene(origin, direction, distance, normal, object, false)
        || distance > length(farPosition - origin))
    {
        hardwareOutput[thread.xy] = Background(uv);
        return;
    }
    RayTriangle tri = rayTriangles[-object - 2];
    float3 position = origin + direction * distance;
    Surface material = MaterialAt(position, normal, tri.color, tri.properties);
    hardwareOutput[thread.xy] = OutputSurface(position, normal, material, uv, false);
}
