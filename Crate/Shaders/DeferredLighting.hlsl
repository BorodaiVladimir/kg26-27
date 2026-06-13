Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gMaterial : register(t2);
Texture2D gPosition : register(t3);
Texture2DArray gShadowMap : register(t5);
Texture2D gShadowOverlay : register(t6);
TextureCube gIrradianceMap : register(t7);
TextureCube gPrefilterEnvMap : register(t8);
Texture2D gIntegrationMap : register(t9);
SamplerState gsamPointClamp : register(s0);
SamplerComparisonState gsamShadow : register(s1);
SamplerState gsamOverlayWrap : register(s2);
SamplerState gsamLinearClamp : register(s3);

#include "PBRUtil.hlsl"

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
    float gEnableIbl;
    float gIblMaxReflectionLod;
    float gUseBeckmannDistribution;
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

bool IsParticlePixel(float materialFlags)
{
    return materialFlags < 0.0f;
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

float SampleShadowCascade(uint cascade, float3 samplePosW, float3 biasNormal)
{
    float3 lightDir = normalize(-gLightDirectionW);
    float ndotl = saturate(dot(biasNormal, lightDir));

    float4 shadowH = mul(float4(samplePosW, 1.0f), gLightViewProj[cascade]);
    shadowH.xyz /= max(shadowH.w, 1e-6f);

    float2 shadowUV = shadowH.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

    const float depthBias = 0.0001f + 0.0005f * (1.0f - ndotl);
    const float depth = saturate(shadowH.z - depthBias);

    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 offset = float2(x, y) * gShadowMapInvSize;
            shadow += gShadowMap.SampleCmp(gsamShadow, float3(shadowUV + offset, (float)cascade), depth).r;
        }
    }
    return shadow / 9.0f;
}

float CalcShadowFactor(float3 posW, float3 shadingNormalW)
{
    const float viewDepth = GetViewDepth(posW);
    const uint cascade = SelectCascade(viewDepth);

    float3 lightDir = normalize(-gLightDirectionW);
    float ndotl = saturate(dot(shadingNormalW, lightDir));
    float3 samplePosW = posW + shadingNormalW * (0.0006f * (1.0f - ndotl) + 0.00015f);

    float shadow = SampleShadowCascade(cascade, samplePosW, shadingNormalW);

    if (cascade > 0u)
    {
        const float prevSplit = gCascadeSplits[cascade - 1u];
        const float blendWidth = max(prevSplit * 0.08f, 0.35f);
        const float blendStart = prevSplit - blendWidth;
        const float t = saturate((viewDepth - blendStart) / max(blendWidth, 1e-4f));
        if (t < 1.0f)
        {
            const float shadowPrev = SampleShadowCascade(cascade - 1u, samplePosW, shadingNormalW);
            shadow = lerp(shadowPrev, shadow, t);
        }
    }

    return shadow;
}

float3 ComputeDirectionalPbr(
    DeferredLightGpu lightSrc,
    float3 normalW,
    float3 toEye,
    float3 albedo,
    float metallic,
    float roughness,
    float shadowFactor)
{
    float3 lightVec = normalize(-lightSrc.Direction);
    return ComputePbrRadiance(
        lightSrc.Strength,
        lightVec,
        normalW,
        toEye,
        albedo,
        metallic,
        roughness,
        1.0f,
        shadowFactor,
        gUseBeckmannDistribution);
}

float3 ComputePointPbr(
    DeferredLightGpu lightSrc,
    float3 posW,
    float3 normal,
    float3 toEye,
    float3 albedo,
    float metallic,
    float roughness)
{
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
        return 0.0f;

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    return ComputePbrRadiance(
        lightSrc.Strength,
        lightVec,
        normal,
        toEye,
        albedo,
        metallic,
        roughness,
        att,
        1.0f,
        gUseBeckmannDistribution);
}

float3 ComputeSpotPbr(
    DeferredLightGpu lightSrc,
    float3 posW,
    float3 normal,
    float3 toEye,
    float3 albedo,
    float metallic,
    float roughness)
{
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
        return 0.0f;

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    float spot = pow(saturate(dot(normalize(lightSrc.Direction), -lightVec)), lightSrc.SpotPower);
    return ComputePbrRadiance(
        lightSrc.Strength,
        lightVec,
        normal,
        toEye,
        albedo,
        metallic,
        roughness,
        att * spot,
        1.0f,
        gUseBeckmannDistribution);
}

float4 PS(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float4 albedo = gAlbedo.Sample(gsamPointClamp, uv);
    float3 normal = normalize(gNormal.Sample(gsamPointClamp, uv).xyz * 2.0f - 1.0f);
    float4 material = gMaterial.Sample(gsamPointClamp, uv);
    float4 posData = gPosition.Sample(gsamPointClamp, uv);
    float3 posW = posData.xyz;

    if (!IsSurfacePixel(posW))
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    float3 toEye = normalize(gEyePosW - posW);
    const bool isParticle = IsParticlePixel(material.w);
    const float metallic = isParticle ? 0.0f : material.r;
    const float roughness = isParticle ? 0.55f : material.g;

    const float shadowFactor = CalcShadowFactor(posW, normal);

    float3 ambient = gAmbientLight.rgb * albedo.rgb;
    if (gEnableIbl > 0.5f)
    {
        AmbientIbl ibl = ComputeAmbientIBLEx(
            gIrradianceMap,
            gPrefilterEnvMap,
            gIntegrationMap,
            gsamLinearClamp,
            normal,
            toEye,
            albedo.rgb,
            metallic,
            roughness,
            gIblMaxReflectionLod);
        // Diffuse IBL stays tied to scene ambient; specular env reflections stay at full strength.
        ambient = ibl.Diffuse * gAmbientLight.rgb + ibl.Specular * 1.35f;
    }

    float3 lighting = ambient;

    [unroll]
    for (int dirLi = 0; dirLi < NUM_DIR_LIGHTS; ++dirLi)
    {
        lighting += ComputeDirectionalPbr(
            gLights[dirLi], normal, toEye, albedo.rgb, metallic, roughness, shadowFactor);
    }

    const int activePointCount = min((int)gActivePointLights, NUM_POINT_LIGHTS);
    for (int ptLi = 0; ptLi < activePointCount; ++ptLi)
    {
        lighting += ComputePointPbr(
            gLights[NUM_DIR_LIGHTS + ptLi], posW, normal, toEye, albedo.rgb, metallic, roughness);
    }

    [unroll]
    for (int spLi = 0; spLi < NUM_SPOT_LIGHTS; ++spLi)
    {
        lighting += ComputeSpotPbr(
            gLights[NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + spLi],
            posW,
            normal,
            toEye,
            albedo.rgb,
            metallic,
            roughness);
    }

    if (!isParticle)
    {
        const float shadowAmt = saturate(1.0f - shadowFactor);
        const float overlayBlend = smoothstep(0.78f, 0.92f, shadowAmt);
        if (overlayBlend > 0.001f)
        {
            const float2 overlayUV = floor(posW.xz * 0.06f * 48.0f) / 48.0f;
            const float3 overlay = gShadowOverlay.Sample(gsamOverlayWrap, overlayUV).rgb;

            const float overlayAmbScale = lerp(0.5f, 1.0f, shadowFactor);
            float3 overlayLit = gAmbientLight.rgb * overlay * overlayAmbScale;
            if (gEnableIbl > 0.5f)
            {
                AmbientIbl overlayIbl = ComputeAmbientIBLEx(
                    gIrradianceMap,
                    gPrefilterEnvMap,
                    gIntegrationMap,
                    gsamLinearClamp,
                    normal,
                    toEye,
                    overlay,
                    metallic,
                    roughness,
                    gIblMaxReflectionLod);
                overlayLit = overlayIbl.Diffuse * gAmbientLight.rgb * overlayAmbScale
                    + overlayIbl.Specular * overlayAmbScale;
            }
            [unroll]
            for (int ovLi = 0; ovLi < NUM_DIR_LIGHTS; ++ovLi)
            {
                overlayLit += ComputeDirectionalPbr(
                    gLights[ovLi], normal, toEye, overlay, metallic, roughness, shadowFactor);
            }

            lighting = lerp(lighting, overlayLit, overlayBlend);
        }
    }

    return float4(lighting, albedo.a);
}
