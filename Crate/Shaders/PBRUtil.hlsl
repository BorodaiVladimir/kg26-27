#ifndef PBR_UTIL_HLSL
#define PBR_UTIL_HLSL

static const float PBR_PI = 3.14159265f;
static const float PBR_DEFAULT_REFLECTION_LOD = 4.0f;

struct AmbientIbl
{
    float3 Diffuse;
    float3 Specular;
};

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0f) + 1.0f);
    denom = PBR_PI * denom * denom;
    return a2 / max(denom, 1e-4f);
}

float DistributionBeckmann(float3 N, float3 H, float roughness)
{
    float a = max(roughness * roughness, 1e-4f);
    float NdotH = saturate(dot(N, H));
    float NdotH2 = NdotH * NdotH;
    float tan2 = (1.0f - NdotH2) / max(NdotH2, 1e-4f);
    return exp(-tan2 / a) / (PBR_PI * a * NdotH2 * NdotH2);
}

float DistributionNDF(float3 N, float3 H, float roughness, float useBeckmann)
{
    if (useBeckmann > 0.5f)
        return DistributionBeckmann(N, H, roughness);
    return DistributionGGX(N, H, roughness);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-4f);
}

float GeometrySchlickBeckmann(float NdotV, float roughness)
{
    float a = roughness * roughness;
    float k = a / 2.0f;
    return NdotV / max(NdotV * (1.0f - k) + k, 1e-4f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

float GeometrySmithBeckmann(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = saturate(dot(N, V));
    float NdotL = saturate(dot(N, L));
    float g1 = GeometrySchlickBeckmann(NdotV, roughness);
    float g2 = GeometrySchlickBeckmann(NdotL, roughness);
    return g1 * g2;
}

float GeometrySmithNDF(float3 N, float3 V, float3 L, float roughness, float useBeckmann)
{
    if (useBeckmann > 0.5f)
        return GeometrySmithBeckmann(N, V, L, roughness);
    return GeometrySmith(N, V, L, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 oneMinusRoughness = float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness);
    return F0 + (max(oneMinusRoughness, F0) - F0) * pow(1.0f - cosTheta, 5.0f);
}

float3 ComputeF0(float3 albedo, float metallic)
{
    return lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
}

float3 ComputePbrRadiance(
    float3 lightColor,
    float3 L,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness,
    float attenuation,
    float shadowFactor,
    float useBeckmann)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    float NdotV = saturate(dot(N, V));
    float VdotH = saturate(dot(V, H));

    float3 F0 = ComputeF0(albedo, metallic);
    float D = DistributionNDF(N, H, roughness, useBeckmann);
    float G = GeometrySmithNDF(N, V, L, roughness, useBeckmann);
    float3 F = FresnelSchlick(VdotH, F0);

    float3 numerator = D * G * F;
    float denom = 4.0f * NdotV * NdotL + 1e-4f;
    float3 specular = numerator / denom;

    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 diffuse = kD * albedo / PBR_PI;

    return (diffuse + specular) * lightColor * NdotL * attenuation * shadowFactor;
}

AmbientIbl ComputeAmbientIBLEx(
    TextureCube irradianceMap,
    TextureCube prefilterMap,
    Texture2D integrationMap,
    SamplerState linearClampSampler,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness,
    float maxReflectionLod)
{
    float3 F0 = ComputeF0(albedo, metallic);
    float NdotV = saturate(dot(N, V));
    float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);

    float3 irradiance = irradianceMap.Sample(linearClampSampler, N).rgb;
    float3 diffuse = irradiance * albedo * kD;

    float3 R = reflect(-V, N);
    const float reflectionLod = saturate(roughness) * max(maxReflectionLod, 0.0f);
    float3 prefilteredColor = prefilterMap.SampleLevel(
        linearClampSampler,
        R,
        reflectionLod).rgb;
    float2 envBRDF = integrationMap.Sample(linearClampSampler, float2(NdotV, roughness)).rg;
    float3 specular = prefilteredColor * (F0 * envBRDF.x + envBRDF.y);

    AmbientIbl result;
    result.Diffuse = diffuse;
    result.Specular = specular;
    return result;
}

float3 ComputeAmbientIBL(
    TextureCube irradianceMap,
    TextureCube prefilterMap,
    Texture2D integrationMap,
    SamplerState linearClampSampler,
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness)
{
    AmbientIbl ibl = ComputeAmbientIBLEx(
        irradianceMap,
        prefilterMap,
        integrationMap,
        linearClampSampler,
        N,
        V,
        albedo,
        metallic,
        roughness,
        PBR_DEFAULT_REFLECTION_LOD);
    return ibl.Diffuse + ibl.Specular;
}

#endif
