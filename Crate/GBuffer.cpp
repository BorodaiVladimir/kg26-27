#include "GBuffer.h"

using Microsoft::WRL::ComPtr;

void GBuffer::Initialize(ID3D12Device* device, UINT width, UINT height)
{
    mDevice = device;
    mWidth = width;
    mHeight = height;

    mRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mSrvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = BufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap)));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = TotalSrvCount;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvHeap)));

    BuildResources();
    BuildDescriptors();
}

void GBuffer::OnResize(UINT width, UINT height)
{
    if (width == mWidth && height == mHeight)
    {
        return;
    }

    mWidth = width;
    mHeight = height;

    BuildResources();
    BuildDescriptors();
}

void GBuffer::TransitionToRenderTargets(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER barriers[BufferCount];
    for (UINT i = 0; i < BufferCount; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    cmdList->ResourceBarrier(BufferCount, barriers);
}

void GBuffer::TransitionToShaderResources(ID3D12GraphicsCommandList* cmdList)
{
    CD3DX12_RESOURCE_BARRIER barriers[BufferCount];
    for (UINT i = 0; i < BufferCount; ++i)
    {
        barriers[i] = CD3DX12_RESOURCE_BARRIER::Transition(
            mBuffers[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    cmdList->ResourceBarrier(BufferCount, barriers);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (UINT i = 0; i < BufferCount; ++i)
    {
        cmdList->ClearRenderTargetView(GetRtv(i), black, 0, nullptr);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetRtv(UINT index) const
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(static_cast<INT>(index), mRtvDescriptorSize);
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE GBuffer::GetSrvGpu(UINT index) const
{
    CD3DX12_GPU_DESCRIPTOR_HANDLE handle(mSrvHeap->GetGPUDescriptorHandleForHeapStart());
    handle.Offset(static_cast<INT>(index), mSrvDescriptorSize);
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetSrvCpu(UINT index) const
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
    handle.Offset(static_cast<INT>(index), mSrvDescriptorSize);
    return handle;
}

void GBuffer::BuildResources()
{
    for (UINT i = 0; i < BufferCount; ++i)
    {
        mBuffers[i].Reset();

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = mWidth;
        texDesc.Height = mHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = mFormats[i];
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CD3DX12_CLEAR_VALUE clearValue(mFormats[i], clearColor);

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&mBuffers[i])));
    }
}

void GBuffer::BuildDescriptors()
{
    for (UINT i = 0; i < BufferCount; ++i)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
        rtvHandle.Offset(static_cast<INT>(i), mRtvDescriptorSize);
        mDevice->CreateRenderTargetView(mBuffers[i].Get(), nullptr, rtvHandle);

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(mSrvHeap->GetCPUDescriptorHandleForHeapStart());
        srvHandle.Offset(static_cast<INT>(i), mSrvDescriptorSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = mFormats[i];
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        mDevice->CreateShaderResourceView(mBuffers[i].Get(), &srvDesc, srvHandle);
    }
}
