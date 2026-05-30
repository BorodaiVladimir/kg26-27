Texture2D gSceneColor : register(t0);
Texture2D gNormal : register(t1);
Texture2D gPosition : register(t2);
SamplerState gsamPointClamp : register(s0);

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
    float4 gAmbientLight;
    float4 gUnusedLights[32];
}

cbuffer cbPost : register(b0)
{
    float4 gEdgeAndPost; // x - strenght, y - threshold, z - vcr, w - vinette
    float4 gEnableFlags; // x edge, y vcr
}

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut vout;
    float2 pos = float2((id << 1) & 2, id & 2);
    vout.TexC = pos;
    vout.PosH = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

bool IsSurfacePixel(float3 posW)
{
    return dot(posW, posW) > 1e-4f;
}

float Hash21(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float SampleEdgeMetric(float2 uv, float3 nC, float depthC, float3 posC)
{
    float3 nN = normalize(gNormal.Sample(gsamPointClamp, uv).xyz * 2.0f - 1.0f);
    float4 pN = gPosition.Sample(gsamPointClamp, uv);
    float3 posN = pN.xyz;

    if (!IsSurfacePixel(posN))
        return 1.0f;
    if (!IsSurfacePixel(posC))
        return 0.0f;

    float depthN = pN.w;
    float depthDiff = abs(depthC - depthN) / max(depthC, 1e-3f);
    float normalDiff = 1.0f - saturate(dot(nC, nN));
    return depthDiff * 5.5f + normalDiff * 3.5f;
}

float DetectEdge(float2 uv)
{
    const float2 texel = gInvRenderTargetSize;

    float3 nC = normalize(gNormal.Sample(gsamPointClamp, uv).xyz * 2.0f - 1.0f);
    float4 pC = gPosition.Sample(gsamPointClamp, uv);
    float3 posC = pC.xyz;
    if (!IsSurfacePixel(posC))
        return 0.0f;

    float depthC = pC.w;
    float edge = 0.0f;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 offset;
        if (i == 0) offset = float2(texel.x, 0.0f);
        else if (i == 1) offset = float2(-texel.x, 0.0f);
        else if (i == 2) offset = float2(0.0f, texel.y);
        else if (i == 3) offset = float2(0.0f, -texel.y);
        else if (i == 4) offset = float2(texel.x, texel.y);
        else if (i == 5) offset = float2(-texel.x, texel.y);
        else if (i == 6) offset = float2(texel.x, -texel.y);
        else offset = float2(-texel.x, -texel.y);

        edge = max(edge, SampleEdgeMetric(uv + offset, nC, depthC, posC));
    }

    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        float2 wide = texel * 2.0f;
        float2 offset;
        if (j == 0) offset = float2(wide.x, 0.0f);
        else if (j == 1) offset = float2(-wide.x, 0.0f);
        else if (j == 2) offset = float2(0.0f, wide.y);
        else offset = float2(0.0f, -wide.y);

        edge = max(edge, SampleEdgeMetric(uv + offset, nC, depthC, posC) * 0.65f);
    }

    edge = saturate((edge - gEdgeAndPost.y) * 11.0f);
    return smoothstep(0.45f, 0.92f, edge);
}

float4 PS_Edge(VSOut pin) : SV_Target
{
    float4 color = gSceneColor.Sample(gsamPointClamp, pin.TexC);
    if (gEnableFlags.x < 0.5f)
        return color;

    float edge = DetectEdge(pin.TexC);
    edge = saturate(edge * gEdgeAndPost.x);
    color.rgb = lerp(color.rgb, float3(0.0f, 0.0f, 0.0f), edge * 0.82f);
    return color;
}

float4 PS_VCR(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float4 color = gSceneColor.Sample(gsamPointClamp, uv);
    if (gEnableFlags.y < 0.5f)
        return color;

    float intensity = saturate(gEdgeAndPost.z);
    float vignetteStrength = saturate(gEdgeAndPost.w);

    float aspect = gRenderTargetSize.x / max(gRenderTargetSize.y, 1.0f);
    float2 centeredUv = uv - 0.5f;
    float2 centered = float2(centeredUv.x * aspect, centeredUv.y);
    float dist = length(centered);

    // Chromatic aberration
    float2 aberration = centered * (0.0035f + dist * 0.012f) * intensity;
    float2 sampleOffset = float2(aberration.x / aspect, aberration.y);
    float r = gSceneColor.Sample(gsamPointClamp, uv + sampleOffset).r;
    float g = gSceneColor.Sample(gsamPointClamp, uv).g;
    float b = gSceneColor.Sample(gsamPointClamp, uv - sampleOffset).b;
    color.rgb = float3(r, g, b);

    // Scanlines 
    float scan = 0.78f + 0.22f * sin(uv.y * gRenderTargetSize.y * 2.6f + gTotalTime * 22.0f);
    color.rgb *= lerp(1.0f, scan, 0.58f * intensity);

    // Horizontal band 
    float screenY = uv.y * gRenderTargetSize.y;
    float bandId = floor(screenY / 5.0f);
    float bandPhase = Hash21(float2(bandId, floor(gTotalTime * 5.5f)));
    float bandActive = step(0.68f, bandPhase) * intensity;
    float bandShiftX = (Hash21(float2(bandId, gTotalTime * 28.0f)) - 0.5f) * 0.014f * bandActive;
    float3 bandColor = gSceneColor.Sample(gsamPointClamp, uv + float2(bandShiftX, 0.0f)).rgb;
    color.rgb = lerp(color.rgb, bandColor, bandActive * 0.7f);

    //glitch lines
    float glitchLine = floor(uv.y * gRenderTargetSize.y * 0.45f);
    float glitchRand = Hash21(float2(glitchLine, floor(gTotalTime * 18.0f)));
    float glitchOn = step(0.962f, glitchRand) * intensity;
    float glitchShiftX = (Hash21(float2(glitchLine + 31.0f, gTotalTime * 41.0f)) - 0.5f) * 0.03f;
    float3 glitchColor = gSceneColor.Sample(gsamPointClamp, uv + float2(glitchShiftX * glitchOn, 0.0f)).rgb;
    color.rgb = lerp(color.rgb, glitchColor, glitchOn * 0.8f);
    color.rgb += (Hash21(uv * 420.0f + gTotalTime * 160.0f) - 0.5f) * glitchOn * 0.22f;

    // Rollin bar
    float rollY = frac(gTotalTime * 0.11f);
    float rollMask = smoothstep(0.035f, 0.0f, abs(uv.y - rollY));
    float rollNoise = Hash21(float2(uv.x * gRenderTargetSize.x, gTotalTime * 85.0f));
    color.rgb = lerp(color.rgb, float3(rollNoise, rollNoise, rollNoise), rollMask * 0.4f * intensity);

    // Film grain / static noise
    float grain = Hash21(uv * gRenderTargetSize * 1.8f + gTotalTime * 97.0f);
    float grain2 = Hash21(uv * gRenderTargetSize * 3.3f - gTotalTime * 131.0f);
    color.rgb += (grain - 0.5f) * 0.10f * intensity;
    color.rgb += (grain2 - 0.5f) * 0.06f * intensity;

    float luma = dot(color.rgb, float3(0.299f, 0.587f, 0.114f));
    color.rgb = lerp(color.rgb, float3(luma, luma, luma), 0.07f * intensity);

    // виньетка
    float vignette = 1.0f - smoothstep(0.22f, 0.82f, dist);
    vignette = lerp(0.06f, 1.0f, saturate(vignette));
    vignette = pow(vignette, 1.25f);
    color.rgb *= lerp(1.0f, vignette, max(vignetteStrength, 0.01f));

    return color;
}
