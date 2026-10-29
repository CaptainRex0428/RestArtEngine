// Display-stage depth of field for raster and realtime GI. Progressive paths
// use a sampled lens instead and never pass through this approximation.
#if !defined(RAE_HARDWARE_RAYTRACING)
cbuffer DepthOfFieldConstants : register(b8)
{
    float4 dofViewport; // origin xy and image dimensions zw in target pixels
    float4 dofParameters; // focus metres, signed CoC radius scale, max radius, reserved
};
Texture2D<float4> dofColor : register(t12);

int2 DofClampPixel(int2 pixel)
{
    return clamp(pixel, int2(dofViewport.xy), int2(dofViewport.xy+dofViewport.zw)-1);
}
float DofDepth(int2 pixel)
{
    float depth = depthBuffer.Load(int3(DofClampPixel(pixel),0)).r;
    return cameraLensParams.x*cameraLensParams.y /
        max(cameraLensParams.y-depth*(cameraLensParams.y-cameraLensParams.x),cameraLensParams.x);
}
float DofRadius(float depth)
{
    return clamp(dofParameters.y*(depth-dofParameters.x)/max(depth,cameraLensParams.x),
        -dofParameters.z,dofParameters.z);
}
float3 DofLinearColor(int2 pixel)
{
    // Color is already tone-mapped. Blend in linear display light, not gamma.
    return pow(max(dofColor.Load(int3(DofClampPixel(pixel),0)).rgb,0.0),2.2);
}
float4 PSDepthOfField(FullscreenOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float depth = DofDepth(pixel);
    float centerRadius = DofRadius(depth);
    float radius = abs(centerRadius);
    // Let a nearby defocused foreground spread into a focused background.
    // The gather cannot recover surfaces hidden behind the foreground: the
    // physically sampled progressive mode covers that case.
    [unroll] for (int i=0;i<8;++i) {
        float2 direction = float2(cos(i*0.7853981634),sin(i*0.7853981634));
        int2 neighbour = pixel+int2(round(direction*dofParameters.z*.5));
        float nearby = -DofRadius(DofDepth(neighbour));
        if (nearby > dofParameters.z*.5) radius = max(radius,nearby);
    }
    if (radius < .5) return dofColor.Load(int3(pixel,0));
    float3 sum = DofLinearColor(pixel);
    float weights = 1;
    [loop] for (int sampleIndex=0;sampleIndex<64;++sampleIndex) {
        float tapRadius = sqrt((sampleIndex+.5)/64.0)*radius;
        float angle = sampleIndex*2.39996323;
        int2 offset = int2(round(float2(cos(angle),sin(angle))*tapRadius));
        float distancePixels = length(float2(offset));
        int2 samplePixel = DofClampPixel(pixel+offset);
        float sampleDepth = DofDepth(samplePixel);
        float sampleRadius = DofRadius(sampleDepth);
        float weight = 0;
        if (sampleRadius < -.5 && sampleDepth <= depth*1.02) {
            // Near samples contribute only when their own aperture footprint
            // reaches this pixel, preventing distant colors leaking in front.
            weight = saturate(abs(sampleRadius)-distancePixels+1.0);
        } else if (centerRadius > .5 && sampleDepth >= depth*.98) {
            weight = saturate(centerRadius-distancePixels+1.0);
        }
        sum += DofLinearColor(samplePixel)*weight;
        weights += weight;
    }
    return float4(pow(max(sum/weights,0.0),1.0/2.2),1.0);
}
#endif
