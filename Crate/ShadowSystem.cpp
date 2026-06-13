#include "ShadowSystem.h"
#include <Windows.h>
#include <algorithm>
#include <cfloat>
#include <cstring>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
struct FrustumCorner
{
    XMFLOAT3 Corners[8];
};

FrustumCorner BuildFrustumCornersWorld(FXMMATRIX invViewProj)
{
    FrustumCorner frustum = {};
    const XMFLOAT3 cornersNdc[8] =
    {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f },
        { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f }
    };

    for (int i = 0; i < 8; ++i)
    {
        XMVECTOR p = XMVectorSet(cornersNdc[i].x, cornersNdc[i].y, cornersNdc[i].z, 1.0f);
        p = XMVector3TransformCoord(p, invViewProj);
        XMStoreFloat3(&frustum.Corners[i], p);
    }
    return frustum;
}

XMVECTOR ComputeFrustumCenter(const FrustumCorner& frustum)
{
    XMVECTOR center = XMVectorZero();
    for (int i = 0; i < 8; ++i)
        center = XMVectorAdd(center, XMLoadFloat3(&frustum.Corners[i]));
    return XMVectorScale(center, 1.0f / 8.0f);
}

XMMATRIX BuildLightViewMatrix(FXMVECTOR lightDirectionTowardScene, FXMVECTOR focusWorld)
{
    // lightDirectionTowardScene = куда летят лучи (мир). Камера смотит туда же.
    const XMVECTOR forward = XMVector3Normalize(lightDirectionTowardScene);
    XMVECTOR upRef = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetX(XMVector3Dot(upRef, forward))) > 0.99f)
        upRef = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);

    const XMVECTOR lightPos = XMVectorSubtract(focusWorld, XMVectorScale(forward, 300.0f));
    return XMMatrixLookAtLH(lightPos, focusWorld, upRef);
}

void ExpandSceneZInLightView(
    FXMMATRIX lightView,
    FXMVECTOR sceneMinW,
    FXMVECTOR sceneMaxW,
    float& minZ,
    float& maxZ)
{
    XMFLOAT3 minF;
    XMFLOAT3 maxF;
    XMStoreFloat3(&minF, sceneMinW);
    XMStoreFloat3(&maxF, sceneMaxW);

    const XMFLOAT3 corners[8] =
    {
        { minF.x, minF.y, minF.z }, { maxF.x, minF.y, minF.z },
        { maxF.x, maxF.y, minF.z }, { minF.x, maxF.y, minF.z },
        { minF.x, minF.y, maxF.z }, { maxF.x, minF.y, maxF.z },
        { maxF.x, maxF.y, maxF.z }, { minF.x, maxF.y, maxF.z },
    };

    for (const XMFLOAT3& c : corners)
    {
        const float z = XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&c), lightView));
        minZ = (std::min)(minZ, z);
        maxZ = (std::max)(maxZ, z);
    }
}

void ExpandSceneXYInLightView(
    FXMMATRIX lightView,
    FXMVECTOR sceneMinW,
    FXMVECTOR sceneMaxW,
    float& minX,
    float& maxX,
    float& minY,
    float& maxY)
{
    XMFLOAT3 minF;
    XMFLOAT3 maxF;
    XMStoreFloat3(&minF, sceneMinW);
    XMStoreFloat3(&maxF, sceneMaxW);

    const XMFLOAT3 corners[8] =
    {
        { minF.x, minF.y, minF.z }, { maxF.x, minF.y, minF.z },
        { maxF.x, maxF.y, minF.z }, { minF.x, maxF.y, minF.z },
        { minF.x, minF.y, maxF.z }, { maxF.x, minF.y, maxF.z },
        { maxF.x, maxF.y, maxF.z }, { minF.x, maxF.y, maxF.z },
    };

    for (const XMFLOAT3& c : corners)
    {
        const XMVECTOR lightP = XMVector3TransformCoord(XMLoadFloat3(&c), lightView);
        minX = (std::min)(minX, XMVectorGetX(lightP));
        maxX = (std::max)(maxX, XMVectorGetX(lightP));
        minY = (std::min)(minY, XMVectorGetY(lightP));
        maxY = (std::max)(maxY, XMVectorGetY(lightP));
    }
}

void SnapOrthoBoundsToTexelGrid(
    float& minX,
    float& maxX,
    float& minY,
    float& maxY,
    float shadowMapSize)
{
    const float sizeX = maxX - minX;
    const float sizeY = maxY - minY;
    float extent = (std::max)(sizeX, sizeY);
    extent = (std::max)(extent, 1e-3f);

    const float texelWorld = extent / shadowMapSize;
    minX = floorf(minX / texelWorld) * texelWorld;
    minY = floorf(minY / texelWorld) * texelWorld;
    maxX = minX + extent;
    maxY = minY + extent;
}
}

ShadowSystem::ShadowSystem()
    : mShadowMapState(ShadowMapState::Common)
    , mDevice(nullptr)
    , mSrvHeap(nullptr)
    , mSrvHeapStartIndex(0)
    , mDescriptorSize(0)
    , mDsvDescriptorSize(0)
    , mView(DirectX::XMMatrixIdentity())
    , mProj(DirectX::XMMatrixIdentity())
    , mCameraNearZ(1.0f)
    , mCameraFarZ(1000.0f)
    , mHasSceneBounds(false)
    , mUsePerCascadeMatrices(false)
    , mStableOrthoValid(false)
    , mLastLightDir(0.0f, -1.0f, 0.0f)
    , mStableOrthoMinX(0.0f)
    , mStableOrthoMaxX(0.0f)
    , mStableOrthoMinY(0.0f)
    , mStableOrthoMaxY(0.0f)
    , mStableOrthoMinZ(0.0f)
    , mStableOrthoMaxZ(0.0f)
{
    for (UINT i = 0; i < kCascadeCount; ++i)
        mCascadeSplitsViewZ[i] = 0.0f;
    for (UINT i = 0; i < kCascadeCount; ++i)
        mCascadeLightViewProj[i] = MathHelper::Identity4x4();
    mShadowViewport = {};
    mShadowScissorRect = {};
    mSceneMin = mSceneMax = mSceneCenter = {};
}

ShadowSystem::~ShadowSystem()
{
    for (UINT i = 0; i < kShadowFrameResourceCount; ++i)
    {
        delete mPassCB[i];
        mPassCB[i] = nullptr;
        delete mLightingCB[i];
        mLightingCB[i] = nullptr;
    }
}

void ShadowSystem::Initialize(
    ID3D12Device* device,
    ID3D12DescriptorHeap* srvHeap,
    UINT srvHeapStartIndex,
    UINT descriptorSize)
{
    mDevice = device;
    mSrvHeap = srvHeap;
    mSrvHeapStartIndex = srvHeapStartIndex;
    mDescriptorSize = descriptorSize;
    mDsvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (UINT i = 0; i < kShadowFrameResourceCount; ++i)
    {
        delete mPassCB[i];
        delete mLightingCB[i];
        mPassCB[i] = new UploadBuffer<ShadowPassConstants>(device, 1, true);
        mLightingCB[i] = new UploadBuffer<ShadowLightingConstants>(device, 1, true);
    }
    mShadowViewport = { 0.0f, 0.0f, (float)kShadowMapSize, (float)kShadowMapSize, 0.0f, 1.0f };
    mShadowScissorRect = { 0, 0, (LONG)kShadowMapSize, (LONG)kShadowMapSize };

    BuildRootSignature();
    BuildResources();
    BuildPSO();
}

void ShadowSystem::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE heightMap;
    heightMap.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER params[5];
    params[0].InitAsConstantBufferView(0);
    params[1].InitAsConstantBufferView(1);
    params[2].InitAsConstantBufferView(2);
    params[3].InitAsConstantBufferView(3);
    params[4].InitAsDescriptorTable(1, &heightMap, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC desc(
        _countof(params),
        params,
        1,
        &linearClamp,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serialized = nullptr;
    ComPtr<ID3DBlob> errors = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.GetAddressOf(),
        errors.GetAddressOf()));

    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serialized->GetBufferPointer(),
        serialized->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void ShadowSystem::BuildResources()
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kShadowMapSize;
    texDesc.Height = kShadowMapSize;
    texDesc.DepthOrArraySize = kCascadeCount;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COMMON,
        &clearValue,
        IID_PPV_ARGS(mShadowMap.GetAddressOf())));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = kCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < kCascadeCount; ++i)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        mDevice->CreateDepthStencilView(mShadowMap.Get(), &dsvDesc, dsvHandle);
        dsvHandle.Offset(1, mDsvDescriptorSize);
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE srvCpu(mSrvHeap->GetCPUDescriptorHandleForHeapStart(), mSrvHeapStartIndex, mDescriptorSize);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = kCascadeCount;
    mDevice->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, srvCpu);
}

void ShadowSystem::BuildPSO()
{
    ComPtr<ID3DBlob> vs = d3dUtil::CompileShader(L"Shaders\\ShadowDepth.hlsl", nullptr, "VS", "vs_5_0");
    ComPtr<ID3DBlob> hs = d3dUtil::CompileShader(L"Shaders\\ShadowDepth.hlsl", nullptr, "HS", "hs_5_0");
    ComPtr<ID3DBlob> ds = d3dUtil::CompileShader(L"Shaders\\ShadowDepth.hlsl", nullptr, "DS", "ds_5_0");

    static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    psoDesc.HS = { hs->GetBufferPointer(), hs->GetBufferSize() };
    psoDesc.DS = { ds->GetBufferPointer(), ds->GetBufferSize() };
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    psoDesc.NumRenderTargets = 0;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mShadowPSO)));
}

void ShadowSystem::SetSceneBounds(FXMVECTOR minW, FXMVECTOR maxW)
{
    XMStoreFloat3(&mSceneMin, minW);
    XMStoreFloat3(&mSceneMax, maxW);
    const XMVECTOR center = XMVectorScale(XMVectorAdd(minW, maxW), 0.5f);
    XMStoreFloat3(&mSceneCenter, center);
    mHasSceneBounds = true;
    mStableOrthoValid = false;
}

void ShadowSystem::SetUsePerCascadeMatrices(bool enabled)
{
    if (mUsePerCascadeMatrices == enabled)
        return;

    mUsePerCascadeMatrices = enabled;
    mStableOrthoValid = false;
}

void ShadowSystem::UpdateStableSceneOrthoBounds(FXMVECTOR lightDirectionW)
{
    if (!mHasSceneBounds)
        return;

    const XMVECTOR lightDir = XMVector3Normalize(lightDirectionW);
    XMVECTOR lastDir = XMLoadFloat3(&mLastLightDir);
    const float dirDelta = XMVectorGetX(XMVector3Length(XMVectorSubtract(lightDir, lastDir)));
    if (mStableOrthoValid && dirDelta < 1e-4f)
        return;

    const XMMATRIX lightView = BuildLightViewMatrix(lightDir, XMLoadFloat3(&mSceneCenter));

    float minX = FLT_MAX, maxX = -FLT_MAX;
    float minY = FLT_MAX, maxY = -FLT_MAX;
    ExpandSceneXYInLightView(
        lightView,
        XMLoadFloat3(&mSceneMin),
        XMLoadFloat3(&mSceneMax),
        minX,
        maxX,
        minY,
        maxY);

    SnapOrthoBoundsToTexelGrid(minX, maxX, minY, maxY, static_cast<float>(kShadowMapSize));

    float minZ = FLT_MAX;
    float maxZ = -FLT_MAX;
    ExpandSceneZInLightView(
        lightView,
        XMLoadFloat3(&mSceneMin),
        XMLoadFloat3(&mSceneMax),
        minZ,
        maxZ);

    constexpr float zPad = 100.0f;
    mStableOrthoMinX = minX;
    mStableOrthoMaxX = maxX;
    mStableOrthoMinY = minY;
    mStableOrthoMaxY = maxY;
    mStableOrthoMinZ = minZ - zPad;
    mStableOrthoMaxZ = maxZ + zPad;
    mStableOrthoValid = true;
    XMStoreFloat3(&mLastLightDir, lightDir);
}

void ShadowSystem::ComputeCascadeSplits(float nearZ, float farZ, float lambda)
{
    const float ratio = farZ / nearZ;
    for (UINT i = 0; i < kCascadeCount; ++i)
    {
        const float p = (i + 1) / static_cast<float>(kCascadeCount);
        const float logSplit = nearZ * powf(ratio, p);
        const float uniformSplit = nearZ + (farZ - nearZ) * p;
        mCascadeSplitsViewZ[i] = logSplit * lambda + uniformSplit * (1.0f - lambda);
    }
}

void ShadowSystem::BuildCascadeMatrices(FXMVECTOR lightDirectionW)
{
    const XMVECTOR lightDir = XMVector3Normalize(lightDirectionW);
    UpdateStableSceneOrthoBounds(lightDir);

    if (!mUsePerCascadeMatrices && mStableOrthoValid)
    {
        const XMMATRIX lightView = BuildLightViewMatrix(lightDir, XMLoadFloat3(&mSceneCenter));
        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            mStableOrthoMinX,
            mStableOrthoMaxX,
            mStableOrthoMinY,
            mStableOrthoMaxY,
            mStableOrthoMinZ,
            mStableOrthoMaxZ);
        const XMMATRIX lightViewProj = XMMatrixMultiply(lightView, lightProj);
        XMFLOAT4X4 stored = {};
        XMStoreFloat4x4(&stored, XMMatrixTranspose(lightViewProj));
        for (UINT cascade = 0; cascade < kCascadeCount; ++cascade)
            mCascadeLightViewProj[cascade] = stored;
        return;
    }

    const XMMATRIX invViewProj = XMMatrixInverse(nullptr, XMMatrixMultiply(mView, mProj));
    const FrustumCorner fullFrustum = BuildFrustumCornersWorld(invViewProj);
    const float invDepthRange = 1.0f / (mCameraFarZ - mCameraNearZ);
    const XMVECTOR lightFocus = ComputeFrustumCenter(fullFrustum);
    const XMMATRIX lightView = mHasSceneBounds
        ? BuildLightViewMatrix(lightDir, XMLoadFloat3(&mSceneCenter))
        : BuildLightViewMatrix(lightDir, lightFocus);
    const bool useStableSceneZ = mUsePerCascadeMatrices && mStableOrthoValid;

    for (UINT cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        const float nearSplit = (cascade == 0) ? mCameraNearZ : mCascadeSplitsViewZ[cascade - 1];
        const float farSplit = mCascadeSplitsViewZ[cascade];
        const float nearT = (nearSplit - mCameraNearZ) * invDepthRange;
        const float farT = (farSplit - mCameraNearZ) * invDepthRange;

        FrustumCorner frustum = fullFrustum;
        for (int i = 0; i < 4; ++i)
        {
            const XMVECTOR cNear = XMLoadFloat3(&frustum.Corners[i]);
            const XMVECTOR cFar = XMLoadFloat3(&frustum.Corners[i + 4]);
            const XMVECTOR ray = XMVectorSubtract(cFar, cNear);
            XMStoreFloat3(&frustum.Corners[i], XMVectorMultiplyAdd(ray, XMVectorReplicate(nearT), cNear));
            XMStoreFloat3(&frustum.Corners[i + 4], XMVectorMultiplyAdd(ray, XMVectorReplicate(farT), cNear));
        }

        float minX = FLT_MAX, maxX = -FLT_MAX;
        float minY = FLT_MAX, maxY = -FLT_MAX;
        float minZ = FLT_MAX, maxZ = -FLT_MAX;

        for (int i = 0; i < 8; ++i)
        {
            const XMVECTOR p = XMVector3TransformCoord(XMLoadFloat3(&frustum.Corners[i]), lightView);
            minX = (std::min)(minX, XMVectorGetX(p));
            maxX = (std::max)(maxX, XMVectorGetX(p));
            minY = (std::min)(minY, XMVectorGetY(p));
            maxY = (std::max)(maxY, XMVectorGetY(p));
            if (!useStableSceneZ)
            {
                minZ = (std::min)(minZ, XMVectorGetZ(p));
                maxZ = (std::max)(maxZ, XMVectorGetZ(p));
            }
        }

        SnapOrthoBoundsToTexelGrid(minX, maxX, minY, maxY, static_cast<float>(kShadowMapSize));

        constexpr float kMinOrthoExtent = 0.25f;
        if (maxX - minX < kMinOrthoExtent)
        {
            const float midX = 0.5f * (minX + maxX);
            minX = midX - kMinOrthoExtent * 0.5f;
            maxX = midX + kMinOrthoExtent * 0.5f;
        }
        if (maxY - minY < kMinOrthoExtent)
        {
            const float midY = 0.5f * (minY + maxY);
            minY = midY - kMinOrthoExtent * 0.5f;
            maxY = midY + kMinOrthoExtent * 0.5f;
        }

        if (useStableSceneZ)
        {
            minZ = mStableOrthoMinZ;
            maxZ = mStableOrthoMaxZ;
        }
        else
        {
            constexpr float zPadding = 80.0f;
            minZ -= zPadding;
            maxZ += zPadding;
        }

        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);
        const XMMATRIX lightViewProj = XMMatrixMultiply(lightView, lightProj);
        XMStoreFloat4x4(&mCascadeLightViewProj[cascade], XMMatrixTranspose(lightViewProj));
    }
}

void ShadowSystem::UpdateCascades(
    FXMMATRIX view,
    FXMMATRIX proj,
    FXMVECTOR lightDirectionW,
    float cameraNearZ,
    float cameraFarZ,
    float splitLambda)
{
    mView = view;
    mProj = proj;
    mCameraNearZ = cameraNearZ;
    mCameraFarZ = cameraFarZ;
    ComputeCascadeSplits(cameraNearZ, cameraFarZ, splitLambda);
    BuildCascadeMatrices(lightDirectionW);
}

void ShadowSystem::TransitionShadowMap(ID3D12GraphicsCommandList* cmdList, ShadowMapState newState)
{
    if (mShadowMapState == newState)
        return;

    D3D12_RESOURCE_STATES stateBefore = D3D12_RESOURCE_STATE_COMMON;
    switch (mShadowMapState)
    {
    case ShadowMapState::Common:
        stateBefore = D3D12_RESOURCE_STATE_COMMON;
        break;
    case ShadowMapState::DepthWrite:
        stateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        break;
    case ShadowMapState::ShaderResource:
        stateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        break;
    default:
        return;
    }

    D3D12_RESOURCE_STATES stateAfter = D3D12_RESOURCE_STATE_COMMON;
    switch (newState)
    {
    case ShadowMapState::Common:
        stateAfter = D3D12_RESOURCE_STATE_COMMON;
        break;
    case ShadowMapState::DepthWrite:
        stateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        break;
    case ShadowMapState::ShaderResource:
        stateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        break;
    default:
        return;
    }

    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(mShadowMap.Get(), stateBefore, stateAfter);
    cmdList->ResourceBarrier(1, &barrier);
    mShadowMapState = newState;
}

void ShadowSystem::BeginPass(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS passCbAddress)
{
    TransitionShadowMap(cmdList, ShadowMapState::DepthWrite);

    cmdList->SetPipelineState(mShadowPSO.Get());
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(3, passCbAddress);
    cmdList->RSSetViewports(1, &mShadowViewport);
    cmdList->RSSetScissorRects(1, &mShadowScissorRect);
}

void ShadowSystem::BeginCascade(ID3D12GraphicsCommandList* cmdList, UINT cascadeIndex, UINT frameIndex)
{
    const UINT slot = (std::min)(frameIndex, kShadowFrameResourceCount - 1u);
    CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(mDsvHeap->GetCPUDescriptorHandleForHeapStart(), cascadeIndex, mDsvDescriptorSize);
    cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    ShadowPassConstants cb = {};
    cb.LightViewProj = mCascadeLightViewProj[cascadeIndex];
    mPassCB[slot]->CopyData(0, cb);
    cmdList->SetGraphicsRootConstantBufferView(1, mPassCB[slot]->Resource()->GetGPUVirtualAddress());
}

void ShadowSystem::EndPass(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->OMSetRenderTargets(0, nullptr, false, nullptr);
    TransitionShadowMap(cmdList, ShadowMapState::ShaderResource);
}

void ShadowSystem::MarkShadowMapShaderResource()
{
    mShadowMapState = ShadowMapState::ShaderResource;
}

void ShadowSystem::PrepareForLighting(ID3D12GraphicsCommandList* cmdList)
{
    if (!cmdList)
        return;

    TransitionShadowMap(cmdList, ShadowMapState::ShaderResource);
}

D3D12_GPU_DESCRIPTOR_HANDLE ShadowSystem::GetShadowMapSrvGpu() const
{
    CD3DX12_GPU_DESCRIPTOR_HANDLE h(mSrvHeap->GetGPUDescriptorHandleForHeapStart(), mSrvHeapStartIndex, mDescriptorSize);
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE ShadowSystem::GetShadowMapSrvCpu() const
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE h(mSrvHeap->GetCPUDescriptorHandleForHeapStart(), mSrvHeapStartIndex, mDescriptorSize);
    return h;
}

void ShadowSystem::UpdateLightingConstants(FXMVECTOR lightDirectionW, UINT frameIndex)
{
    const UINT slot = (std::min)(frameIndex, kShadowFrameResourceCount - 1u);
    ShadowLightingConstants cb = {};
    for (UINT i = 0; i < kCascadeCount; ++i)
        cb.LightViewProj[i] = mCascadeLightViewProj[i];

    cb.CascadeSplits = XMFLOAT4(
        mCascadeSplitsViewZ[0],
        mCascadeSplitsViewZ[1],
        mCascadeSplitsViewZ[2],
        mCascadeSplitsViewZ[3]);

    XMStoreFloat3(&cb.LightDirectionW, XMVector3Normalize(lightDirectionW));
    cb.ShadowMapInvSize = { 1.0f / kShadowMapSize, 1.0f / kShadowMapSize };
    cb.CameraNearZ = mCameraNearZ;
    cb.CameraFarZ = mCameraFarZ;
    mLightingCB[slot]->CopyData(0, cb);
}

D3D12_GPU_VIRTUAL_ADDRESS ShadowSystem::GetLightingConstantBufferAddress(UINT frameIndex) const
{
    const UINT slot = (std::min)(frameIndex, kShadowFrameResourceCount - 1u);
    return mLightingCB[slot] ? mLightingCB[slot]->Resource()->GetGPUVirtualAddress() : 0;
}
