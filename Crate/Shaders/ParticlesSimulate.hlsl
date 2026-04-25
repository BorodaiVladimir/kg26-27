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
ConsumeStructuredBuffer<uint> gAliveIn : register(u1);
AppendStructuredBuffer<uint> gAliveOut : register(u2);
AppendStructuredBuffer<uint> gDeadList : register(u3);
AppendStructuredBuffer<uint> gSortList : register(u4);

[numthreads(256, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint i = dtid.x;
    if (i >= gConsumeCount)
        return;

    uint idx = gAliveIn.Consume();
    Particle p = gParticlePool[idx];

    p.Age += gDeltaTime;
    if (p.Age >= p.Life)
    {
        gDeadList.Append(idx);
        return;
    }

    p.Vel += float3(0.0f, gGravity, 0.0f) * gDeltaTime;
    p.Pos += p.Vel * gDeltaTime;
    p.Color.a = saturate(1.0f - p.Age / max(p.Life, 1e-4f));
    float prevRemaining = max(p.Life - (p.Age - gDeltaTime), 1e-4f);
    float remaining = max(p.Life - p.Age, 0.0f);
    p.Size *= remaining / prevRemaining;

    gParticlePool[idx] = p;
    gAliveOut.Append(idx);
    gSortList.Append(idx);
}
