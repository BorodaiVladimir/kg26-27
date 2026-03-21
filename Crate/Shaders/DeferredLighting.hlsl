Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gMaterial : register(t2);
Texture2D gPosition : register(t3);
SamplerState gsamPointClamp : register(s0);

#define NUM_DIR_LIGHTS 1
#define NUM_POINT_LIGHTS 4
#define NUM_SPOT_LIGHTS 2

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

float3 ComputeDirectional(DeferredLightGpu lightSrc, float3 normal, float3 toEye, float3 diffuse, float3 fresnelR0, float roughness)
{
    float3 lightVec = normalize(-lightSrc.Direction);
    float ndotl = saturate(dot(normal, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normal, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    return (diffuse + specColor) * lightSrc.Strength * ndotl;
}

float3 ComputePoint(DeferredLightGpu lightSrc, float3 posW, float3 normal, float3 toEye, float3 diffuse, float3 fresnelR0, float roughness)
{
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
    {
        return 0.0f;
    }

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    float ndotl = saturate(dot(normal, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normal, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    return (diffuse + specColor) * lightSrc.Strength * ndotl * att;
}

float3 ComputeSpot(DeferredLightGpu lightSrc, float3 posW, float3 normal, float3 toEye, float3 diffuse, float3 fresnelR0, float roughness)
{
    float3 toLight = lightSrc.Position - posW;
    float dist = length(toLight);
    if (dist > lightSrc.FalloffEnd)
    {
        return 0.0f;
    }

    float3 lightVec = toLight / max(dist, 1e-4f);
    float att = saturate((lightSrc.FalloffEnd - dist) / (lightSrc.FalloffEnd - lightSrc.FalloffStart));
    float spot = pow(saturate(dot(normalize(lightSrc.Direction), -lightVec)), lightSrc.SpotPower);
    float ndotl = saturate(dot(normal, lightVec));
    float3 h = normalize(lightVec + toEye);
    float spec = pow(saturate(dot(normal, h)), lerp(64.0f, 4.0f, roughness));
    float3 specColor = fresnelR0 * spec;
    return (diffuse + specColor) * lightSrc.Strength * ndotl * att * spot;
}

float4 PS(VSOut pin) : SV_Target
{
    float2 uv = pin.TexC;
    float4 albedo = gAlbedo.Sample(gsamPointClamp, uv);
    float3 normal = normalize(gNormal.Sample(gsamPointClamp, uv).xyz * 2.0f - 1.0f);
    float4 material = gMaterial.Sample(gsamPointClamp, uv);
    float3 posW = gPosition.Sample(gsamPointClamp, uv).xyz;

    float3 toEye = normalize(gEyePosW - posW);
    float3 fresnelR0 = material.xyz;
    float roughness = material.w;

    float3 lighting = gAmbientLight.rgb * albedo.rgb;

    [unroll]
    for (int i = 0; i < NUM_DIR_LIGHTS; ++i)
    {
        lighting += ComputeDirectional(gLights[i], normal, toEye, albedo.rgb, fresnelR0, roughness);
    }

    [unroll]
    for (int i = 0; i < NUM_POINT_LIGHTS; ++i)
    {
        lighting += ComputePoint(gLights[NUM_DIR_LIGHTS + i], posW, normal, toEye, albedo.rgb, fresnelR0, roughness);
    }

    [unroll]
    for (int i = 0; i < NUM_SPOT_LIGHTS; ++i)
    {
        lighting += ComputeSpot(gLights[NUM_DIR_LIGHTS + NUM_POINT_LIGHTS + i], posW, normal, toEye, albedo.rgb, fresnelR0, roughness);
    }

    return float4(lighting, albedo.a);
}
