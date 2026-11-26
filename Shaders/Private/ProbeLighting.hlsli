// Shared signed, real spherical harmonics and ray sampling for probe transport.
// SH indices match the packed atlas columns on both raster and DXR.
static const int kProbeDistanceDirections = 6;
static const float kProbeDistanceLimit = 4.0; // in probe spacings

float ProbeGridCoverage(float3 local, float3 dimensions)
{
    // Each probe represents a whole cell. The outer wall of an auto-fit
    // volume lies half a cell beyond the last center and still has full GI.
    float3 outside = max(max(-0.5 - local, local - (dimensions - 0.5)), 0.0);
    return 1.0 - smoothstep(0.0, 1.0, max(outside.x, max(outside.y, outside.z)));
}

float ProbeSHBasis(int coefficient, float3 direction)
{
    switch (coefficient)
    {
    case 0: return 0.2820947918;
    case 1: return 0.4886025119 * direction.y;
    case 2: return 0.4886025119 * direction.z;
    case 3: return 0.4886025119 * direction.x;
    case 4: return 1.0925484306 * direction.x * direction.y;
    case 5: return 1.0925484306 * direction.y * direction.z;
    case 6: return 0.3153915653 * (3.0 * direction.z * direction.z - 1.0);
    case 7: return 1.0925484306 * direction.x * direction.z;
    default: return 0.5462742153 * (direction.x * direction.x - direction.y * direction.y);
    }
}

float ProbeCosineBasis(int coefficient, float3 normal)
{
    // Irradiance convolution divided by pi for a unit Lambertian receiver.
    float factor = coefficient == 0 ? 1.0 : (coefficient < 4 ? 2.0 / 3.0 : 0.25);
    return ProbeSHBasis(coefficient, normal) * factor;
}

float2 ProbeSamplePhase(int3 coordinate, uint frame)
{
    // Independent phase per probe and a low-discrepancy shift every update.
    uint hash = ((uint)coordinate.x * 521u + (uint)coordinate.y * 5279u +
        (uint)coordinate.z * 10501u) * 747796405u + 2891336453u;
    hash ^= hash >> 16u;
    float latitude = frac(float(hash & 65535u) / 65536.0 + float(frame & 1023u) * 0.7548776662);
    float azimuth = float(hash >> 16u) / 65536.0 + float(frame & 1023u) * 0.5698402910;
    return float2(latitude, azimuth);
}

float3 ProbeDirectionFromPhase(uint rayIndex, uint rayCount, float2 phase)
{
    // Antipodal samples integrate the first-order SH of a uniform field exactly.
    // Without the pair, a different directional error at each probe produces a
    // visible grid even under constant lighting. Odd custom budgets use the full sphere.
    bool paired = (rayCount & 1u) == 0u;
    uint stratum = paired ? (rayIndex >> 1u) : rayIndex;
    float y = 1.0 - 2.0 * (float(stratum) + phase.x) / float(rayCount);
    float radius = sqrt(saturate(1.0 - y * y));
    float phi = 6.283185307 * frac(phase.y + float(stratum) * 0.3819660113);
    float3 direction = float3(radius * cos(phi), y, radius * sin(phi));
    return paired && (rayIndex & 1u) != 0u ? -direction : direction;
}

float3 ProbeRayDirection(uint rayIndex, uint rayCount, int3 coordinate, uint frame)
{
    return ProbeDirectionFromPhase(rayIndex, rayCount, ProbeSamplePhase(coordinate, frame));
}

float ProbeDistanceWeight(float cosine)
{
    float cosineSquared = saturate(cosine) * saturate(cosine);
    return cosineSquared * cosineSquared;
}

// With changing inputs the requested hysteresis sets the moving average.
// Once the inputs settle, grow the sample window one update at a time to
// suppress noise from continuously rotating probe rays. Keep a bounded
// window in RGBA16F and continue to trace real incoming radiance every frame.
float ProbeUpdateWindow(float hysteresis, float previousWindow, float stableFrames)
{
    float movingWindow = rcp(max(1.0 - saturate(hysteresis), 1.0 / 1024.0));
    float settledWindow = min(1024.0, 1.0 + 16.0 * (movingWindow - 1.0));
    float window = min(settledWindow, movingWindow + stableFrames);
    // Raising the cap cannot give earlier samples more weight than they
    // actually had. The atlas stores this window alongside its estimate.
    return min(window, previousWindow + 1.0);
}
