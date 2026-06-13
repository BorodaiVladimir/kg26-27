#define MaxLights 16

Texture2D gDiffuseMap : register(t0);
Texture2D gHeightNormalMap : register(t1);
Texture2D gDiffuseMapB : register(t2);
Texture2D gMetallicMap : register(t3);
Texture2D gRoughnessMap : register(t4);
SamplerState gsamLinear : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    float4x4 gTexTransformDisp;
}

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
    float4 gLights[MaxLights * 2];
}

cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float gMetallic;
    float3 gMatPad;
    float4x4 gMatTransform;
    float4 gTessParams;
    float4 gChessboard;
}

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct PsIn
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float3 PosV : POSITION1;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 Position : SV_Target3;
};

PsIn VS(VertexIn vin)
{
    PsIn vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = texC.xy;
    vout.PosV = mul(float4(vout.PosW, 1.0f), gView).xyz;
    vout.PosH = mul(float4(vout.PosW, 1.0f), gViewProj);
    return vout;
}

GBufferOut PS(PsIn pin)
{
    GBufferOut gout;
    float3 normal = normalize(pin.NormalW);
    float4 albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC) * gDiffuseAlbedo;
    gout.Albedo = albedo;
    gout.Normal = float4(normal * 0.5f + 0.5f, 1.0f);
    float metallic = saturate(gMetallic);
    float roughness = saturate(gRoughness);
    gout.Material = float4(metallic, roughness, 1.0f, 1.0f);
    gout.Position = float4(pin.PosW, pin.PosV.z);
    return gout;
}
