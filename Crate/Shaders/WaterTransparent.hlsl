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

float WaterWaveHeight(float2 xz, float t)
{
    float2 w = max(gChessboard.xy, float2(0.01f, 0.01f));
    float2 p = xz * float2(0.06f, 0.06f) * w;
    float h = sin(p.x * 2.2f + t * 1.1f) * cos(p.y * 1.9f + t * 0.85f);
    h += 0.35f * sin(dot(p, float2(1.1f, -0.9f)) * 2.5f + t * 1.4f);
    h += 0.2f * sin(length(p * 1.3f) * 3.0f - t * 1.9f);
    return h * gTessParams.x;
}

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

    float disp = WaterWaveHeight(posW.xz, gTotalTime);
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

static const float3 kSunDirW = float3(0.45f, -0.72f, 0.52f);
static const float3 kSunStrength = float3(0.20f, 0.35f, 1.40f);

float4 WaterPS(DsOut pin) : SV_Target
{
    const float eps = 0.06f;
    float2 xz = pin.PosW.xz;
    float t = gTotalTime;
    float h0 = WaterWaveHeight(xz, t);
    float hx = WaterWaveHeight(xz + float2(eps, 0.0f), t);
    float hz = WaterWaveHeight(xz + float2(0.0f, eps), t);
    float3 nW = normalize(float3(-(hx - h0) / eps, 1.0f, -(hz - h0) / eps));

    float3 L = normalize(-kSunDirW);
    float3 V = normalize(gEyePosW - pin.PosW);
    float NdotL = saturate(dot(nW, L));
    float NdotV = saturate(dot(nW, V));
    float3 H = normalize(L + V);
    float NdotH = saturate(dot(nW, H));

    float3 base = float3(0.1f, 0.36f, 0.48f) * gDiffuseAlbedo.rgb;
    float3 diffuse = base * (gAmbientLight.rgb + kSunStrength * NdotL);

    // Френель: к краю воды сильнее «стеклянный» блик и отражение неба
    float F0 = 0.02f;
    float F = F0 + (1.0f - F0) * pow(1.0f - NdotV, 5.0f);

    // Ложное отражение неба по вектору отражения (волны крутят нормаль — блики «ездят»)
    float3 R = reflect(-V, nW);
    float skyGrad = saturate(R.y * 0.5f + 0.5f);
    float3 skyZenith = float3(0.5f, 0.68f, 0.95f);
    float3 skyHorizon = float3(0.32f, 0.42f, 0.52f);
    float3 skyCol = lerp(skyHorizon, skyZenith, pow(skyGrad, 0.65f));
    float3 reflSky = skyCol * F * 0.9f;

    // Солнечные блики: широкий блеск + узкое пятно + очень острое «зерно»
    float rough = max(gRoughness, 0.02f);
    float specWide = pow(NdotH, lerp(96.0f, 24.0f, rough));
    float specMid = pow(NdotH, 256.0f);
    float specSharp = pow(NdotH, 1024.0f);
    float3 sunSpec = kSunStrength * F * (specWide * 0.12f + specMid * 0.35f + specSharp * 0.55f);
    sunSpec += kSunStrength * pow(NdotH, 2048.0f) * 0.25f;

    float3 matSpec = gFresnelR0 * specWide * 2.0f;

    float3 rgb = diffuse + reflSky + sunSpec + matSpec;
    const float alpha = 0.42f;
    return float4(rgb, alpha);
}
