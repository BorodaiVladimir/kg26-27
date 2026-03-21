#pragma once

#include "../Common/d3dUtil.h"

class GBuffer
{
public:
    static constexpr UINT BufferCount = 4;

public:
    void Initialize(ID3D12Device* device, UINT width, UINT height);
    void OnResize(UINT width, UINT height);

    void TransitionToRenderTargets(ID3D12GraphicsCommandList* cmdList);
    void TransitionToShaderResources(ID3D12GraphicsCommandList* cmdList);
    void Clear(ID3D12GraphicsCommandList* cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu(UINT index) const;
    ID3D12DescriptorHeap* GetSrvHeap() const { return mSrvHeap.Get(); }

private:
    void BuildResources();
    void BuildDescriptors();

private:
    ID3D12Device* mDevice = nullptr;
    UINT mWidth = 1;
    UINT mHeight = 1;

    DXGI_FORMAT mFormats[BufferCount] =
    {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mSrvHeap = nullptr;
    UINT mRtvDescriptorSize = 0;
    UINT mSrvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mBuffers[BufferCount];
};
