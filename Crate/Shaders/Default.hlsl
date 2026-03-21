#define MaxLights 16

struct Light
{
    float3 Strength;
    float FalloffStart;
    float3 Direction;
    float FalloffEnd;
    float3 Position;
    float SpotPower;
};

Texture2D gDiffuseMap : register(t0);
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
    Light gLights[MaxLights];
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
};

float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
    float cosIncident = saturate(dot(normal, lightVec));
    float f0 = 1.0f - cosIncident;
    float3 reflectPercent = R0 + (1.0f - R0) * (f0 * f0 * f0 * f0 * f0);
    return reflectPercent;
}

float3 BlinnPhong(float3 lightStrength, float3 lightVec, float3 normal, float3 toEye, float3 fresnelR0, float shininess, float3 diffuseAlbedo)
{
    const float m = shininess * 256.0f;
    float3 halfVec = normalize(toEye + lightVec);
    float specFactor = pow(max(dot(normal, halfVec), 0.0f), m);
    float3 fresnel = SchlickFresnel(fresnelR0, halfVec, lightVec);
    float3 specAlbedo = fresnel * specFactor;
    specAlbedo = specAlbedo / ((m + 7.0f) / 8.0f);
    return (diffuseAlbedo + specAlbedo) * lightStrength;
}

float3 ComputeDirectionalLight(Light L, float3 normal, float3 toEye, float3 fresnelR0, float shininess, float3 diffuseAlbedo)
{
    float3 lightVec = -L.Direction;
    float ndotl = max(dot(lightVec, normal), 0.0f);
    float3 lightStrength = L.Strength * ndotl;
    return BlinnPhong(lightStrength, lightVec, normal, toEye, fresnelR0, shininess, diffuseAlbedo);
}

VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut) 0.0f;
    
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3) gWorld);
    vout.PosH = mul(posW, gViewProj);
    
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = texC.xy;

    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    float4 texColor = gDiffuseMap.Sample(gsamLinear, pin.TexC);
    
    pin.NormalW = normalize(pin.NormalW);
    float3 toEyeW = normalize(gEyePosW - pin.PosW);
    float4 ambient = gAmbientLight * texColor * gDiffuseAlbedo;

    const float shininess = 1.0f - gRoughness;
    float3 fresnelR0 = gFresnelR0;
    float3 diffuse = texColor.rgb * gDiffuseAlbedo.rgb;

    float3 directLight = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        directLight += ComputeDirectionalLight(gLights[i], pin.NormalW, toEyeW, fresnelR0, shininess, diffuse);
    }

    float4 litColor = ambient + float4(directLight, 0.0f);
    litColor.a = texColor.a * gDiffuseAlbedo.a;

    return litColor;
}