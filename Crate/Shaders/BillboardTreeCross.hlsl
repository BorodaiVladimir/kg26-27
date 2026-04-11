// Близкий LOD: два перпендикулярных квада (крест) в мировых осях — «меш».
// Дальний LOD: см. BillboardTree.hlsl (камера-к билборд).

#define MaxLights 16

Texture2D gDiffuseMap : register(t0);
Texture2D gHeightNormalMap : register(t1);
Texture2D gDiffuseMapB : register(t2);
SamplerState gsamLinear : register(s0);

struct TreeInstance
{
    float3 WorldPos;
    float Pad;
};

StructuredBuffer<TreeInstance> gTreeInstances : register(t3);

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

struct VsOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float3 PosV : POSITION1;
    float MipLod : TEXCOORD1;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 Position : SV_Target3;
};

VsOut VS(VertexIn vin, uint instId : SV_InstanceID)
{
    VsOut vout = (VsOut)0.0f;

    float3 center = gTreeInstances[instId].WorldPos;
    float dist = distance(center, gEyePosW);

    float mipLod;
    if (dist < 14.0f)
        mipLod = 0.0f;
    else if (dist < 28.0f)
        mipLod = 1.0f;
    else
        mipLod = 2.0f;

    const float meshScale = 2.15f;
    float3 posW = center + vin.PosL * meshScale;
    float3 normalW = normalize(vin.NormalL);

    float4 posW4 = float4(posW, 1.0f);
    vout.PosW = posW;
    vout.NormalW = normalW;
    vout.PosV = mul(posW4, gView).xyz;
    vout.PosH = mul(posW4, gViewProj);
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = texC.xy;
    vout.MipLod = mipLod;
    return vout;
}

GBufferOut PS(VsOut pin)
{
    GBufferOut gout;

    float4 albedo = gDiffuseMap.SampleLevel(gsamLinear, pin.TexC, pin.MipLod) * gDiffuseAlbedo;
    clip(albedo.a - 0.08f);

    float3 n = normalize(pin.NormalW);
    gout.Albedo = float4(albedo.rgb, 1.0f);
    gout.Normal = float4(n * 0.5f + 0.5f, 1.0f);
    gout.Material = float4(gFresnelR0, saturate(gRoughness));
    gout.Position = float4(pin.PosV, 1.0f);
    return gout;
}
