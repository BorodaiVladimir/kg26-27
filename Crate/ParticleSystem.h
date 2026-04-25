#pragma once

#include "../Common/d3dUtil.h"

struct ParticleSystemConstants
{
    float DeltaTime = 0.0f;
    float TotalTime = 0.0f;
    float EmitRate = 200.0f;
    float Gravity = -9.8f;
    DirectX::XMFLOAT3 EmitterPos = { 0.0f, 3.0f, -10.0f };
    float MaxLife = 6.0f;
    UINT ConsumeCount = 0;
    UINT EmitCount = 0;
    UINT MaxParticles = 0;
    UINT Padding = 0;
};

class ParticleSystem
{
public:
    void Initialize(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* srvHeap,
        UINT descriptorSize,
        UINT descriptorStartIndex);

    void Update(ID3D12GraphicsCommandList* cmdList, float dt, float totalTime);
    void Render(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS passCbAddress);
    void SetEmitterPosition(const DirectX::XMFLOAT3& pos) { mEmitterPos = pos; }

private:
    void BuildRootSignatures();
    void BuildPSOs();
    void BuildCommandSignature();
    void BuildBuffersAndDescriptors(ID3D12GraphicsCommandList* cmdList);
    void ResetAliveAndSortCounters(ID3D12GraphicsCommandList* cmdList);
    void SwapBuffers();

private:
    struct GpuParticle
    {
        DirectX::XMFLOAT3 Pos;
        float Age;
        DirectX::XMFLOAT3 Vel;
        float Life;
        DirectX::XMFLOAT4 Color;
        float Size;
        DirectX::XMFLOAT3 Pad;
    };

    static constexpr UINT kThreadGroupSize = 256;
    static constexpr UINT kMaxParticles = 1u << 15; // 32768

    ID3D12Device* mDevice = nullptr;
    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT mDescriptorSize = 0;
    UINT mDescriptorStartIndex = 0;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRenderRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEmitPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSimulatePSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mRenderPSO = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mDrawIndirectSignature = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mParticlePool = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveListA = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveListB = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveCounterA = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveCounterB = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadList = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSortList = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadCounter = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mSortCounter = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterResetUpload = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterMaxUpload = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadInitUpload = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveCountReadback = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mParticleConstants = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgs = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgsUpload = nullptr;

    ParticleSystemConstants* mMappedConstants = nullptr;
    D3D12_DRAW_ARGUMENTS* mMappedIndirectArgs = nullptr;
    UINT* mMappedAliveCount = nullptr;

    UINT mAliveCount = 0;
    bool mCurrentIsA = true;
    float mEmitAccumulator = 0.0f;
    bool mResourcesPrimed = false;
    DirectX::XMFLOAT3 mEmitterPos = { 0.0f, 1.2f, 0.0f };
};
