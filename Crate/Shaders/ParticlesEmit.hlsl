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

cbuffer ParticleCB : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    float gEmitRate;
    float gGravity;
    float3 gEmitterPos;
    float gMaxLife;
    uint gConsumeCount;
    uint gEmitCount;
    uint gMaxParticles;
    uint gPad;
};

RWStructuredBuffer<Particle> gParticlePool : register(u0);
AppendStructuredBuffer<uint> gAliveOut : register(u2);
ConsumeStructuredBuffer<uint> gDeadList : register(u3);
AppendStructuredBuffer<uint> gSortList : register(u4);

float Hash01(uint x)
{
    x ^= x >> 17;
    x *= 0xed5ad4bbU;
    x ^= x >> 11;
    x *= 0xac4c1b51U;
    x ^= x >> 15;
    x *= 0x31848babU;
    x ^= x >> 14;
    return (x & 0x00ffffff) / 16777215.0f;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gEmitCount)
        return;

    uint seed = i + (uint)(gTotalTime * 1000.0f) * 747796405u;
    float angle = Hash01(seed) * 6.2831853f;
    float radius = Hash01(seed * 3u + 11u) * 1.1f;
    float speed = lerp(2.2f, 4.8f, Hash01(seed * 7u + 23u));

    Particle p;
    p.Pos = gEmitterPos + float3(cos(angle) * radius, 0.0f, sin(angle) * radius);
    p.Age = 0.0f;
    p.Vel = float3(cos(angle) * 0.9f, speed, sin(angle) * 0.9f);
    p.Life = lerp(1.2f, gMaxLife, Hash01(seed * 13u + 31u));
    p.Color = float4(1.0f, 0.7f + 0.25f * Hash01(seed * 17u), 0.3f, 1.0f);
    p.Size = lerp(0.18f, 0.42f, Hash01(seed * 19u + 41u));
    p.Pad = 0.0f.xxx;

    uint particleIndex = gDeadList.Consume();
    gParticlePool[particleIndex] = p;
    gAliveOut.Append(particleIndex);
    gSortList.Append(particleIndex);
}
