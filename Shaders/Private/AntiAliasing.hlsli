// Display-domain spatial/temporal AA. Explicit bilinear loads keep filtering
// identical across backends whose scene samplers need not have the same state.
#if !defined(RAE_HARDWARE_RAYTRACING)
cbuffer AntiAliasingConstants : register(b5)
{
    row_major float4x4 aaInverseViewProjection;
    row_major float4x4 aaPreviousViewProjection;
    float4 aaCurrentEye;
    float4 aaCurrentForward;
    float4 aaPreviousEye;
    float4 aaPreviousForward;
    float4 aaViewport; // origin.xy, extent.zw in texture pixels
    float4 aaParameters; // current weight, valid history, raster jitter in pixels.zw
};
Texture2D<float4> aaSource : register(t7);
Texture2D<float4> aaHistory : register(t8);

int2 AaClampPixel(int2 p)
{
    return clamp(p, int2(aaViewport.xy), int2(aaViewport.xy + aaViewport.zw) - 1);
}
float4 AaBilinear(Texture2D<float4> image, float2 p)
{
    const float2 q = p - 0.5;
    const int2 i = int2(floor(q));
    const float2 t = frac(q);
    return lerp(lerp(image.Load(int3(AaClampPixel(i), 0)), image.Load(int3(AaClampPixel(i + int2(1,0)), 0)), t.x),
                lerp(image.Load(int3(AaClampPixel(i + int2(0,1)), 0)), image.Load(int3(AaClampPixel(i + 1), 0)), t.x), t.y);
}
float AaLuma(float3 color) { return dot(color, float3(0.299,0.587,0.114)); }

float4 PSFXAA(FullscreenOutput input) : SV_TARGET
{
    const float2 p = input.position.xy;
    const float3 center = aaSource.Load(int3(int2(p),0)).rgb;
    const float m = AaLuma(center);
    const float n = AaLuma(AaBilinear(aaSource, p + float2(0,-1)).rgb);
    const float s = AaLuma(AaBilinear(aaSource, p + float2(0,1)).rgb);
    const float w = AaLuma(AaBilinear(aaSource, p + float2(-1,0)).rgb);
    const float e = AaLuma(AaBilinear(aaSource, p + float2(1,0)).rgb);
    const float minimum = min(m,min(min(n,s),min(w,e)));
    const float maximum = max(m,max(max(n,s),max(w,e)));
    const float range = maximum-minimum;
    if (range < max(0.0312,maximum*0.125)) return float4(center,1);

    const float nw = AaLuma(AaBilinear(aaSource,p+float2(-1,-1)).rgb);
    const float ne = AaLuma(AaBilinear(aaSource,p+float2(1,-1)).rgb);
    const float sw = AaLuma(AaBilinear(aaSource,p+float2(-1,1)).rgb);
    const float se = AaLuma(AaBilinear(aaSource,p+float2(1,1)).rgb);
    const float horizontal = abs(nw+sw-2*w)+2*abs(n+s-2*m)+abs(ne+se-2*e);
    const float vertical = abs(nw+ne-2*n)+2*abs(w+e-2*m)+abs(sw+se-2*s);
    const bool edgeHorizontal = horizontal >= vertical;
    const float sideA = edgeHorizontal ? n : w;
    const float sideB = edgeHorizontal ? s : e;
    const bool useA = abs(sideA-m) >= abs(sideB-m);
    const float gradient = max(abs(sideA-m),abs(sideB-m));
    const float edgeMean = 0.5*(m+(useA ? sideA : sideB));
    const float2 normal = (edgeHorizontal ? float2(0,1) : float2(1,0))*(useA ? -1 : 1);
    const float2 tangent = edgeHorizontal ? float2(1,0) : float2(0,1);
    const float2 edge = p+normal*0.5;
    float distanceA = 1, distanceB = 1;
    float deltaA = 0, deltaB = 0;
    bool foundA = false, foundB = false;
    [unroll] for (int step = 0; step < 10; ++step)
    {
        if (!foundA) {
            deltaA = AaLuma(AaBilinear(aaSource,edge-tangent*distanceA).rgb)-edgeMean;
            foundA = abs(deltaA)>=gradient*0.25;
            if (!foundA) distanceA+=step<4 ? 1.0 : 2.0;
        }
        if (!foundB) {
            deltaB = AaLuma(AaBilinear(aaSource,edge+tangent*distanceB).rgb)-edgeMean;
            foundB = abs(deltaB)>=gradient*0.25;
            if (!foundB) distanceB+=step<4 ? 1.0 : 2.0;
        }
    }
    const float nearestDelta = distanceA<distanceB ? deltaA : deltaB;
    float edgeBlend = 0.5-min(distanceA,distanceB)/max(distanceA+distanceB,1.0);
    if ((nearestDelta<0)==(m<edgeMean)) edgeBlend=0;
    const float average = (2*(n+s+w+e)+nw+ne+sw+se)/12;
    float subpixel = saturate(abs(average-m)/range);
    subpixel = subpixel*subpixel*(3-2*subpixel);
    subpixel = subpixel*subpixel*0.75;
    return float4(AaBilinear(aaSource,p+normal*max(edgeBlend,subpixel)).rgb,1);
}

float4 PSTAA(FullscreenOutput input) : SV_TARGET
{
    // Integrate stochastic raster samples at stable output pixels. Dilate the
    // nearest surface depth for motion/depth validation at subpixel silhouettes;
    // otherwise alternating foreground/sky coverage is rejected every frame.
    const int2 p = int2(input.position.xy);
    const float3 current = aaSource.Load(int3(p,0)).rgb;
    float depth = depthBuffer.Load(int3(p,0)).r;
    [unroll] for (int dy=-1;dy<=1;++dy) [unroll] for (int dx=-1;dx<=1;++dx)
        depth=min(depth,depthBuffer.Load(int3(AaClampPixel(p+int2(dx,dy)),0)).r);
    const float2 ndc = ((input.position.xy-aaViewport.xy)/aaViewport.zw)*float2(2,-2)+float2(-1,1);
    float4 world = mul(float4(ndc,depth,1),aaInverseViewProjection);
    world /= max(abs(world.w),1e-8)*(world.w<0 ? -1 : 1);
    const bool background = depth>=1.0;
    const float linearDepth = background ? 0 : min(65504.0,max(0.0,dot(world.xyz-aaCurrentEye.xyz,aaCurrentForward.xyz)));
    float3 outputColor = PSFXAA(input).rgb;
    if (aaParameters.y>0.5)
    {
        const float4 previous = mul(float4(world.xyz,1),aaPreviousViewProjection);
        const float2 previousUv = previous.xy/max(previous.w,1e-8)*float2(0.5,-0.5)+0.5;
        // Previous history uses an unjittered projection. Subtract the current
        // unjittered sample location to get camera motion without raster jitter.
        const float2 historyPixel = aaViewport.xy+previousUv*aaViewport.zw+aaParameters.zw;
        const bool inside = previous.w>0 && all(historyPixel>=aaViewport.xy) && all(historyPixel<aaViewport.xy+aaViewport.zw);
        const float expectedDepth = min(65504.0,max(0.0,dot(world.xyz-aaPreviousEye.xyz,aaPreviousForward.xyz)));
        const float storedDepth = aaHistory.Load(int3(AaClampPixel(int2(historyPixel)),0)).a;
        const bool compatible = background ? storedDepth==0 : storedDepth>0 && abs(storedDepth-expectedDepth)<max(0.025,expectedDepth*0.015);
        if (inside && compatible)
        {
            float3 low=current, high=current;
            [unroll] for (int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
                const float3 neighbour=aaSource.Load(int3(AaClampPixel(p+int2(x,y)),0)).rgb;
                low=min(low,neighbour); high=max(high,neighbour);
            }
            const float3 history=clamp(AaBilinear(aaHistory,historyPixel).rgb,low,high);
            const float motionPixels=length(historyPixel-input.position.xy);
            const float currentWeight=max(aaParameters.x,saturate(motionPixels*.35));
            outputColor=lerp(history,current,currentWeight);
        }
    }
    return float4(outputColor,linearDepth);
}

float4 PSAntiAliasingPresent(FullscreenOutput input) : SV_TARGET
{
    return float4(aaSource.Load(int3(int2(input.position.xy),0)).rgb,1);
}
#endif
