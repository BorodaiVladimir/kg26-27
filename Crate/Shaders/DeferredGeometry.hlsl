#define MaxLights 16

Texture2D gDiffuseMap : register(t0);
Texture2D gHeightNormalMap : register(t1);
Texture2D gDiffuseMapB : register(t2);
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
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC : TEXCOORD;
    float2 TexCDisp : TEXCOORD1;
    float3 PosV : POSITION1;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 Position : SV_Target3;
};

VsOut VS(VertexIn vin)
{
    VsOut vout;
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    float4 texCDisp = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransformDisp);
    vout.TexC = texC.xy;
    vout.TexCDisp = texCDisp.xy;
    return vout;
}

[domain("tri")]
[partitioning("fractional_odd")]
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

    float3 c = (p[0].PosW + p[1].PosW + p[2].PosW) / 3.0f;
    float dist = distance(c, gEyePosW);
    float t = saturate(dist / max(gTessParams.z, 1.0f));
    float tf = lerp(7.0f, 1.0f, t);
    o.Edge[0] = tf;
    o.Edge[1] = tf;
    o.Edge[2] = tf;
    o.Inside = tf;
    return o;
}

[domain("tri")]
DsOut DS(HsPatch patchConst, float3 bary : SV_DomainLocation, const OutputPatch<VsOut, 3> p)
{
    DsOut o = (DsOut)0.0f;
    float3 posW = p[0].PosW * bary.x + p[1].PosW * bary.y + p[2].PosW * bary.z;
    float3 normalW = normalize(p[0].NormalW * bary.x + p[1].NormalW * bary.y + p[2].NormalW * bary.z);
    float2 uv = p[0].TexC * bary.x + p[1].TexC * bary.y + p[2].TexC * bary.z;
    float2 uvDisp = p[0].TexCDisp * bary.x + p[1].TexCDisp * bary.y + p[2].TexCDisp * bary.z;

    float h = gHeightNormalMap.SampleLevel(gsamLinear, uvDisp, 0).r;
    float disp = h * gTessParams.x + gTessParams.y;
    posW += normalW * disp;

    float4 posW4 = float4(posW, 1.0f);
    o.PosW = posW;
    o.NormalW = normalW;
    o.TexC = uv;
    o.TexCDisp = uvDisp;
    o.PosV = mul(posW4, gView).xyz;
    o.PosH = mul(posW4, gViewProj);
    return o;
}

GBufferOut PS(DsOut pin)
{
    GBufferOut gout;
    float3 normal = normalize(pin.NormalW);

    // 
    float2 uvD = pin.TexCDisp;
    float2 texel = 1.0f / float2(2048.0f, 2048.0f);
    float hL = gHeightNormalMap.Sample(gsamLinear, uvD + float2(-texel.x, 0)).r;
    float hR = gHeightNormalMap.Sample(gsamLinear, uvD + float2(texel.x, 0)).r;
    float hD = gHeightNormalMap.Sample(gsamLinear, uvD + float2(0, -texel.y)).r;
    float hU = gHeightNormalMap.Sample(gsamLinear, uvD + float2(0, texel.y)).r;
    float bumpStrength = max(abs(gTessParams.x) * 40.0f, 1.0f);
    float3 nTS = normalize(float3((hL - hR) * bumpStrength, (hD - hU) * bumpStrength, 1.0f));

    float3 up = abs(normal.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 t = normalize(cross(up, normal));
    float3 b = normalize(cross(normal, t));
    float3 bumped = normalize(t * nTS.x + b * nTS.y + normal * nTS.z);

    float4 albedo;
    if (gChessboard.z > 0.5f)
    {
        float2 tileCount = max(gChessboard.xy, float2(1.0f, 1.0f));
        float2 uvScaled = pin.TexC * tileCount;
        float2 tileId = floor(uvScaled);
        float2 localUV = frac(uvScaled);
        int2 ti = int2(tileId);
        float4 c0 = gDiffuseMap.Sample(gsamLinear, localUV) * gDiffuseAlbedo;
        float4 c1 = gDiffuseMapB.Sample(gsamLinear, localUV) * gDiffuseAlbedo;
        bool useB = ((ti.x + ti.y) & 1) != 0;
        albedo = useB ? c1 : c0;
    }
    else
    {
        albedo = gDiffuseMap.Sample(gsamLinear, pin.TexC) * gDiffuseAlbedo;
    }
    gout.Albedo = albedo;
    gout.Normal = float4(bumped * 0.5f + 0.5f, 1.0f);
    gout.Material = float4(gFresnelR0, saturate(gRoughness));
    gout.Position = float4(pin.PosV, 1.0f);
    return gout;
}
