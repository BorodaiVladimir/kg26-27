#pragma once

#include <DirectXMath.h>
#include <cstdint>

constexpr std::uint32_t kDeferredDirectionalLightCount = 1;
constexpr std::uint32_t kDeferredPointLightCount = 4;
constexpr std::uint32_t kDeferredSpotLightCount = 2;

struct DirectionalLightSource
{
    DirectX::XMFLOAT3 Direction = { 0.57735f, -0.57735f, 0.57735f };
    float Pad0 = 0.0f;
    DirectX::XMFLOAT3 Strength = { 0.8f, 0.8f, 0.8f };
    float Pad1 = 0.0f;
};

struct PointLightSource
{
    DirectX::XMFLOAT3 Position = { 0.0f, 4.0f, 0.0f };
    float FalloffStart = 2.0f;
    DirectX::XMFLOAT3 Strength = { 0.8f, 0.7f, 0.6f };
    float FalloffEnd = 30.0f;
};

struct SpotLightSource
{
    DirectX::XMFLOAT3 Position = { 0.0f, 8.0f, -10.0f };
    float FalloffStart = 2.0f;
    DirectX::XMFLOAT3 Direction = { 0.0f, -0.4f, 1.0f };
    float FalloffEnd = 60.0f;
    DirectX::XMFLOAT3 Strength = { 0.9f, 0.9f, 0.95f };
    float SpotPower = 24.0f;
};

struct DeferredLightConstants
{
    DirectionalLightSource DirectionalLights[kDeferredDirectionalLightCount];
    PointLightSource PointLights[kDeferredPointLightCount];
    SpotLightSource SpotLights[kDeferredSpotLightCount];
};
