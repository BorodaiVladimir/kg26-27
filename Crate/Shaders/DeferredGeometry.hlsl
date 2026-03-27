#define MaxLights 16

Texture2D gDiffuseMap : register(t0);
Texture2D gCheckerAltMap : register(t1);
SamplerState gsamLinear : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
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
    float4x4 gMatTransform;
}

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VertexOut
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

VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut)0.0f;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.PosV = mul(posW, gView).xyz;
    vout.PosH = mul(posW, gViewProj);

    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = texC.xy;

    return vout;
}

GBufferOut PS(VertexOut pin)
{
    GBufferOut gout;

    float3 normal = normalize(pin.NormalW);
    float2 floorUv = pin.PosW.xz * 0.12f;
    float checkerCell = fmod(floor(floorUv.x) + floor(floorUv.y), 2.0f);
    float floorMask = (normal.y > 0.75f && pin.PosW.y < -0.75f) ? 1.0f : 0.0f;

    float4 texColorA = gDiffuseMap.Sample(gsamLinear, pin.TexC);
    float4 texColorB = gCheckerAltMap.Sample(gsamLinear, pin.TexC * 2.0f);
    float4 checkerMix = lerp(texColorA, texColorB, checkerCell);
    float4 texColor = lerp(texColorA, checkerMix, floorMask);
    float4 albedo = texColor * gDiffuseAlbedo;

    gout.Albedo = albedo;
    gout.Normal = float4(normalize(normal) * 0.5f + 0.5f, 1.0f);
    gout.Material = float4(gFresnelR0, saturate(gRoughness));
    gout.Position = float4(pin.PosV, 1.0f);

    return gout;
}
