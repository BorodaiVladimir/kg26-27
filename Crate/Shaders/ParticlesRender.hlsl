#define MaxLights 16

struct Particle
{
    float3 Pos;
    float Age;
    float3 Vel;
    float Life;
    float4 Color;
    float Size;
    float3 Pad;
};

StructuredBuffer<Particle> gParticlePool : register(t0);
StructuredBuffer<uint> gSortList : register(t1);

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
};

struct VSOut
{
    float3 PosW : POSITION;
    float4 Col : COLOR0;
    float Size : TEXCOORD0;
};

struct GSOut
{
    float4 PosH : SV_POSITION;
    float3 PosV : POSITION1;
    float3 NormalW : NORMAL;
    float4 Col : COLOR0;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Material : SV_Target2;
    float4 Position : SV_Target3;
};

VSOut VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    VSOut v;
    uint pidx = gSortList[instanceId];
    Particle p = gParticlePool[pidx];
    v.PosW = p.Pos;
    v.Col = p.Color;
    v.Size = p.Size;
    return v;
}

[maxvertexcount(4)]
void GSMain(point VSOut vin[1], inout TriangleStream<GSOut> tri)
{
    float3 center = vin[0].PosW;
    float sz = vin[0].Size;

    float3 toCam = normalize(gEyePosW - center);
    float3 upAxis = float3(0.0f, 1.0f, 0.0f);
    if (abs(toCam.y) > 0.98f)
    {
        upAxis = float3(1.0f, 0.0f, 0.0f);
    }

    float3 right = normalize(cross(upAxis, toCam));
    float3 up = normalize(cross(toCam, right));

    float2 c0 = float2(-1.0f, -1.0f);
    float2 c1 = float2(-1.0f, 1.0f);
    float2 c2 = float2(1.0f, -1.0f);
    float2 c3 = float2(1.0f, 1.0f);

    float3 p0 = center + (right * c0.x + up * c0.y) * sz;
    float3 p1 = center + (right * c1.x + up * c1.y) * sz;
    float3 p2 = center + (right * c2.x + up * c2.y) * sz;
    float3 p3 = center + (right * c3.x + up * c3.y) * sz;

    GSOut g;
    g.NormalW = toCam;
    g.Col = vin[0].Col;

    g.PosH = mul(float4(p0, 1.0f), gViewProj);
    g.PosV = mul(float4(p0, 1.0f), gView).xyz;
    tri.Append(g);

    g.PosH = mul(float4(p1, 1.0f), gViewProj);
    g.PosV = mul(float4(p1, 1.0f), gView).xyz;
    tri.Append(g);

    g.PosH = mul(float4(p2, 1.0f), gViewProj);
    g.PosV = mul(float4(p2, 1.0f), gView).xyz;
    tri.Append(g);

    g.PosH = mul(float4(p3, 1.0f), gViewProj);
    g.PosV = mul(float4(p3, 1.0f), gView).xyz;
    tri.Append(g);
}

GBufferOut PSMain(GSOut pin)
{
    GBufferOut o;
    float3 n = normalize(pin.NormalW);
    o.Albedo = float4(pin.Col.rgb, 1.0f);
    o.Normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.Material = float4(0.04f, 0.04f, 0.04f, 0.55f);
    o.Position = float4(pin.PosV, 1.0f);
    return o;
}
