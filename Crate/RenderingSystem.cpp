#include "RenderingSystem.h"

using Microsoft::WRL::ComPtr;

void RenderingSystem::Initialize(
    ID3D12Device* device,
    UINT width,
    UINT height,
    DXGI_FORMAT backBufferFormat,
    DXGI_FORMAT depthStencilFormat,
    bool msaaEnabled,
    UINT msaaQuality)
{
    mDevice = device;
    mWidth = width;
    mHeight = height;
    mBackBufferFormat = backBufferFormat;
    mDepthStencilFormat = depthStencilFormat;
    mMsaaEnabled = msaaEnabled;
    mMsaaQuality = msaaQuality;

    mGBuffer.Initialize(device, width, height);
    BuildGeometryRootSignature();
    BuildLightingRootSignature();
    BuildShadersAndInputLayout();
    BuildPSOs();
}

void RenderingSystem::OnResize(UINT width, UINT height)
{
    mWidth = width;
    mHeight = height;
    mGBuffer.OnResize(width, height);
}

void RenderingSystem::BeginGeometryPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
    D3D12_GPU_DESCRIPTOR_HANDLE checkerTextureHandle,
    bool wireframe)
{
    mGBuffer.TransitionToRenderTargets(cmdList);
    mGBuffer.Clear(cmdList);
    cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[GBuffer::BufferCount] =
    {
        mGBuffer.GetRtv(0),
        mGBuffer.GetRtv(1),
        mGBuffer.GetRtv(2),
        mGBuffer.GetRtv(3)
    };
    cmdList->OMSetRenderTargets(GBuffer::BufferCount, rtvs, false, &dsv);

    cmdList->SetPipelineState(wireframe ? mGeometryWireframePSO.Get() : mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(2, passCbAddress);
    cmdList->SetGraphicsRootDescriptorTable(4, checkerTextureHandle);
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    mGBuffer.TransitionToShaderResources(cmdList);
}

void RenderingSystem::ExecuteLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightParamsCbAddress,
    ID3D12Resource* lightBufferResource,
    UINT lightCount,
    UINT lightStrideBytes)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC lightSrvDesc = {};
    lightSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lightSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    lightSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    lightSrvDesc.Buffer.FirstElement = 0;
    lightSrvDesc.Buffer.NumElements = lightCount;
    lightSrvDesc.Buffer.StructureByteStride = lightStrideBytes;
    lightSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    mDevice->CreateShaderResourceView(lightBufferResource, &lightSrvDesc, mGBuffer.GetSrvCpu(GBuffer::BufferCount));

    cmdList->OMSetRenderTargets(1, &backBufferRtv, true, nullptr);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { mGBuffer.GetSrvHeap() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetGraphicsRootDescriptorTable(0, mGBuffer.GetSrvGpu(0));
    cmdList->SetGraphicsRootConstantBufferView(1, passCbAddress);
    cmdList->SetGraphicsRootConstantBufferView(2, lightParamsCbAddress);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::BuildGeometryRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE checkerTable;
    checkerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    CD3DX12_DESCRIPTOR_RANGE texTableB;
    texTableB.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

    CD3DX12_ROOT_PARAMETER slotRootParameter[6];
    slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsConstantBufferView(0);
    slotRootParameter[2].InitAsConstantBufferView(1);
    slotRootParameter[3].InitAsConstantBufferView(2);
    slotRootParameter[4].InitAsDescriptorTable(1, &checkerTable, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[5].InitAsDescriptorTable(1, &texTableB, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        6,
        slotRootParameter,
        1,
        &linearWrap,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf()));

    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mGeometryRootSignature.GetAddressOf())));
}

void RenderingSystem::BuildLightingRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE gbufferTable;
    gbufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::TotalSrvCount, 0);

    CD3DX12_ROOT_PARAMETER slotRootParameter[3];
    slotRootParameter[0].InitAsDescriptorTable(1, &gbufferTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsConstantBufferView(1);
    slotRootParameter[2].InitAsConstantBufferView(2);

    CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        3,
        slotRootParameter,
        1,
        &pointClamp,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(
        &rootSigDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(),
        errorBlob.GetAddressOf()));

    ThrowIfFailed(mDevice->CreateRootSignature(
        0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mLightingRootSignature.GetAddressOf())));
}

void RenderingSystem::BuildShadersAndInputLayout()
{
    mShaders["deferredGeometryVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["deferredGeometryHS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "HS", "hs_5_0");
    mShaders["deferredGeometryDS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "DS", "ds_5_0");
    mShaders["deferredGeometryPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredGeometry.hlsl", nullptr, "PS", "ps_5_0");
    mShaders["deferredLightVS"] = d3dUtil::CompileShader(L"Shaders\\DeferredLighting.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["deferredLightPS"] = d3dUtil::CompileShader(L"Shaders\\DeferredLighting.hlsl", nullptr, "PS", "ps_5_0");

    mShaders["waterTransparentVS"] = d3dUtil::CompileShader(L"Shaders\\WaterTransparent.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["waterTransparentHS"] = d3dUtil::CompileShader(L"Shaders\\WaterTransparent.hlsl", nullptr, "HS", "hs_5_0");
    mShaders["waterTransparentDS"] = d3dUtil::CompileShader(L"Shaders\\WaterTransparent.hlsl", nullptr, "DS", "ds_5_0");
    mShaders["waterTransparentPS"] = d3dUtil::CompileShader(L"Shaders\\WaterTransparent.hlsl", nullptr, "WaterPS", "ps_5_0");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
}

void RenderingSystem::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPsoDesc = {};
    geometryPsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    geometryPsoDesc.pRootSignature = mGeometryRootSignature.Get();
    geometryPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredGeometryVS"]->GetBufferPointer()),
        mShaders["deferredGeometryVS"]->GetBufferSize()
    };
    geometryPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredGeometryPS"]->GetBufferPointer()),
        mShaders["deferredGeometryPS"]->GetBufferSize()
    };
    geometryPsoDesc.HS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredGeometryHS"]->GetBufferPointer()),
        mShaders["deferredGeometryHS"]->GetBufferSize()
    };
    geometryPsoDesc.DS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredGeometryDS"]->GetBufferPointer()),
        mShaders["deferredGeometryDS"]->GetBufferSize()
    };
    geometryPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geometryPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geometryPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geometryPsoDesc.SampleMask = UINT_MAX;
    geometryPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    geometryPsoDesc.NumRenderTargets = GBuffer::BufferCount;
    geometryPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geometryPsoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometryPsoDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    geometryPsoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    geometryPsoDesc.SampleDesc.Count = 1;
    geometryPsoDesc.SampleDesc.Quality = 0;
    geometryPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&geometryPsoDesc, IID_PPV_ARGS(&mGeometryPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryWirePsoDesc = geometryPsoDesc;
    geometryWirePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    geometryWirePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&geometryWirePsoDesc, IID_PPV_ARGS(&mGeometryWireframePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC lightingPsoDesc = {};
    lightingPsoDesc.InputLayout = { nullptr, 0 };
    lightingPsoDesc.pRootSignature = mLightingRootSignature.Get();
    lightingPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredLightVS"]->GetBufferPointer()),
        mShaders["deferredLightVS"]->GetBufferSize()
    };
    lightingPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["deferredLightPS"]->GetBufferPointer()),
        mShaders["deferredLightPS"]->GetBufferSize()
    };
    lightingPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    lightingPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    lightingPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    lightingPsoDesc.DepthStencilState.DepthEnable = false;
    lightingPsoDesc.DepthStencilState.StencilEnable = false;
    lightingPsoDesc.SampleMask = UINT_MAX;
    lightingPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    lightingPsoDesc.NumRenderTargets = 1;
    lightingPsoDesc.RTVFormats[0] = mBackBufferFormat;
    lightingPsoDesc.SampleDesc.Count = 1;
    lightingPsoDesc.SampleDesc.Quality = 0;
    lightingPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&lightingPsoDesc, IID_PPV_ARGS(&mLightingPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC waterPsoDesc = {};
    waterPsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    waterPsoDesc.pRootSignature = mGeometryRootSignature.Get();
    waterPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["waterTransparentVS"]->GetBufferPointer()),
        mShaders["waterTransparentVS"]->GetBufferSize()
    };
    waterPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["waterTransparentPS"]->GetBufferPointer()),
        mShaders["waterTransparentPS"]->GetBufferSize()
    };
    waterPsoDesc.HS =
    {
        reinterpret_cast<BYTE*>(mShaders["waterTransparentHS"]->GetBufferPointer()),
        mShaders["waterTransparentHS"]->GetBufferSize()
    };
    waterPsoDesc.DS =
    {
        reinterpret_cast<BYTE*>(mShaders["waterTransparentDS"]->GetBufferPointer()),
        mShaders["waterTransparentDS"]->GetBufferSize()
    };
    waterPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    waterPsoDesc.BlendState.AlphaToCoverageEnable = FALSE;
    waterPsoDesc.BlendState.IndependentBlendEnable = FALSE;
    waterPsoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    waterPsoDesc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;
    waterPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    waterPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    waterPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    waterPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    waterPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    waterPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    waterPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    waterPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    waterPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    waterPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    waterPsoDesc.SampleMask = UINT_MAX;
    waterPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    waterPsoDesc.NumRenderTargets = 1;
    waterPsoDesc.RTVFormats[0] = mBackBufferFormat;
    waterPsoDesc.SampleDesc.Count = 1;
    waterPsoDesc.SampleDesc.Quality = 0;
    waterPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&waterPsoDesc, IID_PPV_ARGS(&mWaterTransparentPSO)));
}

void RenderingSystem::BeginTransparentWaterPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress)
{
    cmdList->OMSetRenderTargets(1, &backBufferRtv, FALSE, &dsv);
    cmdList->SetPipelineState(mWaterTransparentPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(2, passCbAddress);
}
