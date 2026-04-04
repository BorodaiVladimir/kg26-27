#pragma once

#include "../Common/d3dUtil.h"
#include "GBuffer.h"

class RenderingSystem
{
public:
    void Initialize(
        ID3D12Device* device,
        UINT width,
        UINT height,
        DXGI_FORMAT backBufferFormat,
        DXGI_FORMAT depthStencilFormat,
        bool msaaEnabled,
        UINT msaaQuality);

    void OnResize(UINT width, UINT height);

    void BeginGeometryPass(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
        D3D12_GPU_DESCRIPTOR_HANDLE checkerTextureHandle,
        bool wireframe);

    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    void ExecuteLightingPass(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
        D3D12_GPU_VIRTUAL_ADDRESS lightParamsCbAddress,
        ID3D12Resource* lightBufferResource,
        UINT lightCount,
        UINT lightStrideBytes);

    void BeginTransparentWaterPass(
        ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
        D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
        bool wireframe);

private:
    void BuildGeometryRootSignature();
    void BuildLightingRootSignature();
    void BuildShadersAndInputLayout();
    void BuildPSOs();

private:
    ID3D12Device* mDevice = nullptr;

    UINT mWidth = 1;
    UINT mHeight = 1;
    DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    bool mMsaaEnabled = false;
    UINT mMsaaQuality = 0;

    GBuffer mGBuffer;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> mShaders;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mGeometryRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mLightingRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mGeometryWireframePSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mLightingPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWaterTransparentPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mWaterTransparentWireframePSO = nullptr;
};
