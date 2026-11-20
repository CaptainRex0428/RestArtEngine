// Shared surface function. Apply compiles both Forward and Deferred pipelines.
// Inputs are per-actor material values in linear color space.
// Surface fields: float3 albedo, float roughness, float metallic, float grid.
// World-space XZ grid: one-unit cells, stronger lines every five units.
float GridLines(float2 position, float spacing, float halfWidth, float footprint)
{
    float2 distanceToLine = abs(frac(position / spacing + 0.5) - 0.5) * spacing;
    float distance = min(distanceToLine.x, distanceToLine.y);
    return (1.0 - smoothstep(max(0.0, halfWidth - footprint), halfWidth + footprint, distance))
        * (1.0 - smoothstep(spacing * 0.15, spacing * 0.45, footprint));
}
Surface EvaluateMaterial(float3 worldPosition, float3 normal, Surface material)
{
    if (material.grid > 0.5)
    {
        // Analytic pixel footprint also works for secondary reflection hits,
        // where screen derivatives are undefined across divergent ray paths.
        float3 view = cameraPosition.xyz - worldPosition;
        float footprint = max(0.002, length(view) * 0.93 / max(viewportDebug.y, 1.0));
        footprint /= max(abs(dot(normalize(view), normal)), 0.12);
        float minor = GridLines(worldPosition.xz, 1.0, 0.018, footprint);
        float major = GridLines(worldPosition.xz, 5.0, 0.045, footprint);
        material.albedo *= lerp(0.55, 0.10, minor);
        material.albedo *= lerp(1.0, 0.25, major);
    }
    // Example: material.albedo *= 0.5 + 0.5 * sin(worldPosition.y * 8.0);
    return material;
}
