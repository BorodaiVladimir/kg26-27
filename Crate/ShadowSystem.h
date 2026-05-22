#pragma once

#include "../Common/d3dUtil.h"
#include "../Common/UploadBuffer.h"

struct ShadowPassConstants
{
    DirectX::XMFLOAT4X4 LightViewProj = MathHelper::Identity4x4();
};

struct ShadowLightingConstants
{
    DirectX::XMFLOAT4X4 LightViewProj[4];
    DirectX::XMFLOAT4 CascadeSplits = {};
    DirectX::XMFLOAT3 LightDirectionW = { 0.0f, -1.0f, 0.0f };
    float CameraNearZ = 1.0f;
    DirectX::XMFLOAT2 ShadowMapInvSize = { 0.0f, 0.0f };
    float CameraFarZ = 1000.0f;
    float Pad0 = 0.0f;
};

class ShadowSystem
{
public:
    static const UINT kCascadeCount = 4;
    static const UINT kShadowMapSize = 2048;
    static constexpr float kDefaultSplitLambda = 0.5f;

    ShadowSystem();
    ~ShadowSystem();

    ShadowSystem(const ShadowSystem&) = delete;
    ShadowSystem& operator=(const ShadowSystem&) = delete;

    void Initialize(
        ID3D12Device* device,
        ID3D12DescriptorHeap* srvHeap,
        UINT srvHeapStartIndex,
        UINT descriptorSize);

    void SetSceneBounds(DirectX::FXMVECTOR minW, DirectX::FXMVECTOR maxW);

    void SetUsePerCascadeMatrices(bool enabled);
    bool UsePerCascadeMatrices() const { return mUsePerCascadeMatrices; }

    void UpdateCascades(
        DirectX::FXMMATRIX view,
        DirectX::FXMMATRIX proj,
        DirectX::FXMVECTOR lightDirectionW,
        float cameraNearZ,
        float cameraFarZ,
        float splitLambda);

    void BeginPass(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS passCbAddress);
    void BeginCascade(ID3D12GraphicsCommandList* cmdList, UINT cascadeIndex);
    void EndPass(ID3D12GraphicsCommandList* cmdList);
    void PrepareForLighting(ID3D12GraphicsCommandList* cmdList);

    ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
    ID3D12PipelineState* GetPipelineState() const { return mShadowPSO.Get(); }

    D3D12_GPU_DESCRIPTOR_HANDLE GetShadowMapSrvGpu() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetShadowMapSrvCpu() const;
    ID3D12Resource* GetShadowMapResource() const { return mShadowMap.Get(); }

    void UpdateLightingConstants(DirectX::FXMVECTOR lightDirectionW);
    D3D12_GPU_VIRTUAL_ADDRESS GetLightingConstantBufferAddress() const;

private:
    enum class ShadowMapState
    {
        Common,
        DepthWrite,
        ShaderResource
    };

    void BuildRootSignature();
    void BuildResources();
    void BuildPSO();
    void ComputeCascadeSplits(float nearZ, float farZ, float lambda);
    void BuildCascadeMatrices(DirectX::FXMVECTOR lightDirectionW);
    void UpdateStableSceneOrthoBounds(DirectX::FXMVECTOR lightDirectionW);
    void TransitionShadowMap(ID3D12GraphicsCommandList* cmdList, ShadowMapState newState);

    ShadowMapState mShadowMapState;
    ID3D12Device* mDevice;
    ID3D12DescriptorHeap* mSrvHeap;
    UINT mSrvHeapStartIndex;
    UINT mDescriptorSize;
    UINT mDsvDescriptorSize;
    float mCascadeSplitsViewZ[4];
    DirectX::XMFLOAT4X4 mCascadeLightViewProj[4];
    DirectX::XMMATRIX mView;
    DirectX::XMMATRIX mProj;
    float mCameraNearZ;
    float mCameraFarZ;
    bool mHasSceneBounds;
    bool mUsePerCascadeMatrices;
    DirectX::XMFLOAT3 mSceneMin;
    DirectX::XMFLOAT3 mSceneMax;
    DirectX::XMFLOAT3 mSceneCenter;
    bool mStableOrthoValid;
    DirectX::XMFLOAT3 mLastLightDir;
    float mStableOrthoMinX;
    float mStableOrthoMaxX;
    float mStableOrthoMinY;
    float mStableOrthoMaxY;
    float mStableOrthoMinZ;
    float mStableOrthoMaxZ;
    D3D12_VIEWPORT mShadowViewport;
    D3D12_RECT mShadowScissorRect;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mShadowPSO;
    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    UploadBuffer<ShadowPassConstants>* mPassCB;
    UploadBuffer<ShadowLightingConstants>* mLightingCB;
};
