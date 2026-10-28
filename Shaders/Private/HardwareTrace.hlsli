// D3D12 DXR 1.1 implementation of the common scene-intersection contract.
// Raster and analytic builds never include these API-specific declarations.
cbuffer HardwareConstants : register(b5)
{
    row_major float4x4 inverseViewProjection;
    uint4 outputInfo; // width, height, triangle count, hardware shadows enabled
    float4 traceOptions; // reflections enabled, reserved
    uint4 accumulationInfo; // sample offset, sample batch, max surface bounces, GI method
    uint4 giOptions; // update mode, first-surface lobe mask, realtime pass, independent sample sequence
};
RaytracingAccelerationStructure hardwareScene : register(t7);
StructuredBuffer<float3> hardwareVertexNormals : register(t8);

bool TraceScene(float3 origin, float3 direction, out float distance,
    out float3 hitNormal, out int hitObject, bool shadowRay)
{
    distance = 1e20;
    hitNormal = float3(0, 1, 0);
    hitObject = -1;
    if (outputInfo.z == 0) return false;
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = RAY_EPSILON;
    ray.TMax = 1e20;
    // Non-opaque geometry exposes triangle candidates. Face filtering uses the
    // same outward geometric normals as the raster/material contract, including
    // mirrored instances and the reversed direction of receiver -> light rays.
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
    query.TraceRayInline(hardwareScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        uint index = query.CandidatePrimitiveIndex();
        RayTriangle tri = rayTriangles[index];
        float facing = dot(direction, tri.normal);
        if (IsTwoSided(tri.properties) || (shadowRay ? facing > 0.0 : facing < 0.0))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    uint index = query.CommittedPrimitiveIndex();
    float2 bary = query.CommittedTriangleBarycentrics();
    float3 geometricNormal = rayTriangles[index].normal;
    float3 smoothNormal = hardwareVertexNormals[index * 3] * (1.0 - bary.x - bary.y)
        + hardwareVertexNormals[index * 3 + 1] * bary.x
        + hardwareVertexNormals[index * 3 + 2] * bary.y;
    hitNormal = dot(smoothNormal, smoothNormal) > 1e-12 ? normalize(smoothNormal) : geometricNormal;
    if (IsTwoSided(rayTriangles[index].properties) && dot(hitNormal, direction) > 0.0)
        hitNormal = -hitNormal;
    distance = query.CommittedRayT();
    hitObject = -((int)index + 2);
    return true;
}
