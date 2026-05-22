Texture2D gHeightNormalMap : register(t0);
SamplerState gsamLinear : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    float4x4 gTexTransformDisp;
};

cbuffer cbShadowPass : register(b1)
{
    float4x4 gLightViewProj;
};

cbuffer cbPass : register(b3)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
};

cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
    float4 gTessParams;
    float4 gChessboard;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC : TEXCOORD;
};

struct VsOut
{
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float2 TexCDisp : TEXCOORD1;
};

struct HsPatch
{
    float Edge[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

struct DsOut
{
    float4 PosH : SV_POSITION;
};

VsOut VS(VertexIn vin)
{
    VsOut o;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    o.PosW = posW.xyz;
    o.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    float4 texCDisp = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransformDisp);
    o.TexC = texC.xy;
    o.TexCDisp = texCDisp.xy;
    return o;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConst")]
VsOut HS(InputPatch<VsOut, 3> p, uint i : SV_OutputControlPointID)
{
    return p[i];
}

HsPatch HSConst(InputPatch<VsOut, 3> p, uint patchId : SV_PrimitiveID)
{
    HsPatch o;
    if (gTessParams.w < 0.5f)
    {
        o.Edge[0] = 1.0f;
        o.Edge[1] = 1.0f;
        o.Edge[2] = 1.0f;
        o.Inside = 1.0f;
        return o;
    }

    const float tf = (gTessParams.w > 0.5f) ? 4.0f : 1.0f;
    o.Edge[0] = tf;
    o.Edge[1] = tf;
    o.Edge[2] = tf;
    o.Inside = tf;
    return o;
}

[domain("tri")]
DsOut DS(HsPatch patchConst, float3 bary : SV_DomainLocation, const OutputPatch<VsOut, 3> p)
{
    DsOut o;
    float3 posW = p[0].PosW * bary.x + p[1].PosW * bary.y + p[2].PosW * bary.z;
    float3 normalW = normalize(p[0].NormalW * bary.x + p[1].NormalW * bary.y + p[2].NormalW * bary.z);
    float2 uvDisp = p[0].TexCDisp * bary.x + p[1].TexCDisp * bary.y + p[2].TexCDisp * bary.z;

    float h = gHeightNormalMap.SampleLevel(gsamLinear, uvDisp, 0).r;
    float disp = h * gTessParams.x + gTessParams.y;
    posW += normalW * disp;

    o.PosH = mul(float4(posW, 1.0f), gLightViewProj);
    return o;
}
