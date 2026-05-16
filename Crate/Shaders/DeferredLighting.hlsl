Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gMaterial : register(t2);
Texture2D gPosition : register(t3);
Texture2DArray gShadowMap : register(t5);
Texture2D gShadowOverlay : register(t6);
SamplerState gsamPointClamp : register(s0);
SamplerComparisonState gsamShadow : register(s1);
SamplerState gsamOverlayClamp : register(s2);

#define NUM_DIR_LIGHTS 1
#define NUM_POINT_LIGHTS 256
#define NUM_SPOT_LIGHTS 2
#define NUM_CASCADES 4

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

cbuffer cbDeferredParams : register(b2)
{
    uint gActivePointLights;
    float gShadowOverlayStrength;
    float2 gDeferredPad;
}

cbuffer cbShadow : register(b3)
{
    float4x4 gLightViewProj[NUM_CASCADES];
    float4 gCascadeSplits;
    float3 gLightDirectionW;
    float gCameraNearZ;
    float2 gShadowMapInvSize;
    float gCameraFarZ;
    float gShadowPad0;
}

struct DeferredLightGpu
{
    float3 Strength;
    float Type;
    float3 Position;
    float FalloffStart;
    float3 Direction;
    float FalloffEnd;
    float SpotPower;
    float3 Padding;
};

StructuredBuffer<DeferredLightGpu> gLights : register(t4);

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

struct ShadowEval
{
    float Factor;
    float2 OverlayUV;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut vout;

    float2 pos = float2((id << 1) & 2, id & 2);
    vout.TexC = pos;
    vout.PosH = float4(pos * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return vout;
}

float GetViewDepth(float3 posW)
{
    float3 posV = mul(float4(posW, 1.0f), gView).xyz;
    return abs(posV.z);
}

uint SelectCascade(float viewDepth)
{
    const float depth = viewDepth;
    uint cascade = NUM_CASCADES - 1u;
    [unroll]
    for (uint ci = 0; ci < NUM_CASCADES; ++ci)
    {
        if (depth < gCascadeSplits[ci])
        {
            cascade = ci;
            break;
        }
    }
    return cascade;
}

float2 ShadowUVFromPosW(uint cascade, float3 samplePosW)
{
    float4 shadowH = mul(float4(samplePosW, 1.0f), gLightViewProj[cascade]);
    shadowH.xyz /= max(shadowH.w, 1e-6f);
    return shadowH.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
}

float SampleShadowCascade(uint cascade, float3 samplePosW, float3 biasNormal, out float2 overlayUV)
{
    float3 lightDir = normalize(-gLightDirectionW);
    float ndotl = saturate(dot(biasNormal, lightDir));

    overlayUV = ShadowUVFromPosW(cascade, samplePosW);

    const float depthBias = 0.0001f + 0.0005f * (1.0f - ndotl);
    float4 shadowH = mul(float4(samplePosW, 1.0f), gLightViewProj[cascade]);
    shadowH.xyz /= max(shadowH.w, 1e-6f);
    const float depth = saturate(shadowH.z - depthBias);

    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * gShadowMapInvSize;
            shadow += gShadowMap.SampleCmp(gsamShadow, float3(overlayUV + offset, (float)cascade), depth).r;
        }
    }
    return shadow / 9.0f;
}

ShadowEval EvalShadow(float3 posW, float3 shadingNormalW)
{
    ShadowEval result;
    result.Factor = 1.0f;
    result.OverlayUV = float2(0.5f, 0.5f);

    const float viewDepth = GetViewDepth(posW);
    const uint cascade = SelectCascade(viewDepth);

    float3 lightDir = normalize(-gLightDirectionW);
    float ndotl = saturate(dot(shadingNormalW, lightDir));
    float3 samplePosW = posW + shadingNormalW * (0.0006f * (1.0f - ndotl) + 0.00015f);

    result.Factor = SampleShadowCascade(cascade, samplePosW, shadingNormalW, result.OverlayUV);
    return result;
}

float3 ComputeDirectional(
    DeferredLightGpu lightSrc,
    float3 normalW,
    float3 toEye,
    float3 diffuse,
    float3 fresnelR0,
    float roughness,
    float shadowFactor)
{
    float3 lightVec = normalize(-lightSrc.Direction);
    float ndotl = saturate(dot(normalW, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normalW, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    return (diffuse + specColor) * lightSrc.Strength * ndotl * shadowFactor;
}

float3 ComputePoint(DeferredLightGpu lightSrc, float3 posW, float3 normal, float3 toEye, float3 diffuse, float3 fresnelR0, float roughness)
{
    float3 result = 0.0f;
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
        return result;

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    float ndotl = saturate(dot(normal, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normal, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    result = (diffuse + specColor) * lightSrc.Strength * ndotl * att;
    return result;
}

float3 SampleShadowOverlayColor(float2 shadowUV)
{
    const float3 texRgb = gShadowOverlay.Sample(gsamOverlayClamp, shadowUV).rgb;
    if (dot(texRgb, float3(0.299f, 0.587f, 0.114f)) > 0.04f)
        return texRgb;

    const float2 p = shadowUV * 4.0f;
    const float2 cell = floor(p);
    const bool checker = fmod(cell.x + cell.y, 2.0f) < 1.0f;
    return checker ? float3(0.38f, 0.38f, 0.42f) : float3(0.24f, 0.24f, 0.26f);
}

bool IsValidGBufferSurface(float4 albedo, float4 posData)
{
    const float viewZ = abs(posData.w);
    return viewZ > 1e-2f && dot(albedo.rgb, float3(1.0f, 1.0f, 1.0f)) > 0.02f;
}

bool IsShadowMapUVInBounds(float2 shadowUV)
{
    return shadowUV.x >= 0.02f && shadowUV.x <= 0.98f &&
           shadowUV.y >= 0.02f && shadowUV.y <= 0.98f;
}

float3 ComputeSpot(DeferredLightGpu lightSrc, float3 posW, float3 normal, float3 toEye, float3 diffuse, float3 fresnelR0, float roughness)
{
    float3 result = 0.0f;
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
        return result;

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    float spot = pow(saturate(dot(normalize(lightSrc.Direction), -lightVec)), lightSrc.SpotPower);
    float ndotl = saturate(dot(normal, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normal, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    result = (diffuse + specColor) * lightSrc.Strength * ndotl * att * spot;
    return result;
}

float4 PS(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float4 albedo = gAlbedo.Sample(gsamPointClamp, uv);
    float3 normal = normalize(gNormal.Sample(gsamPointClamp, uv).xyz * 2.0f - 1.0f);
    float4 material = gMaterial.Sample(gsamPointClamp, uv);
    float4 posData = gPosition.Sample(gsamPointClamp, uv);
    float3 posW = posData.xyz;
    float3 toEye = normalize(gEyePosW - posW);
    float3 fresnelR0 = material.xyz;
    float roughness = material.w;

    const ShadowEval shadow = EvalShadow(posW, normal);

    const float3 ambientPart = gAmbientLight.rgb * albedo.rgb;
    float3 dirPart = 0.0f;
    float3 otherLights = 0.0f;

    [unroll]
    for (int dirLi = 0; dirLi < NUM_DIR_LIGHTS; ++dirLi)
    {
        dirPart += ComputeDirectional(gLights[dirLi], normal, toEye, albedo.rgb, fresnelR0, roughness, shadow.Factor);
    }

    const int activePointCount = min((int)gActivePointLights, NUM_POINT_LIGHTS);
    for (int ptLi = 0; ptLi < activePointCount; ++ptLi)
    {
        otherLights += ComputePoint(gLights[NUM_DIR_LIGHTS + ptLi], posW, normal, toEye, albedo.rgb, fresnelR0, roughness);
    }

    [unroll]
    for (int spLi = 0; spLi < NUM_SPOT_LIGHTS; ++spLi)
    {
        otherLights += ComputeSpot(gLights[NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + spLi], posW, normal, toEye, albedo.rgb, fresnelR0, roughness);
    }

    float3 lighting = ambientPart + dirPart + otherLights;

    if (!IsValidGBufferSurface(albedo, posData))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    const float shadowAmt = saturate(1.0f - shadow.Factor);
    const float overlayStrength = saturate(gShadowOverlayStrength);
    if (shadowAmt > 0.35f && overlayStrength > 0.0f && IsShadowMapUVInBounds(shadow.OverlayUV))
    {
        const float3 projectedTex = SampleShadowOverlayColor(shadow.OverlayUV);
        const float3 projectedOnSurface = albedo.rgb * projectedTex;

        const float3 shadowSurface =
            ambientPart + otherLights + projectedOnSurface * 0.55f;

        const float mask = saturate((shadowAmt - 0.35f) * 10.0f) * overlayStrength;
        lighting = lerp(lighting, shadowSurface, mask);
    }

    return float4(lighting, albedo.a);
}
