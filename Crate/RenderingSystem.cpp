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
    BuildBillboardRootSignature();
    BuildLightingRootSignature();
    BuildPostProcessRootSignature();
    BuildShadersAndInputLayout();
    BuildBillboardShadersAndLayout();
    BuildPSOs();
    BuildPostProcessResources();
}

void RenderingSystem::OnResize(UINT width, UINT height)
{
    mWidth = width;
    mHeight = height;
    mGBuffer.OnResize(width, height);
    BuildPostProcessResources();
}

void RenderingSystem::SetLightingResources(
    ID3D12Resource* shadowMapResource,
    UINT shadowCascadeCount,
    ID3D12Resource* shadowOverlayResource)
{
    mShadowMapForLighting = shadowMapResource;
    mShadowCascadeCountForLighting = shadowCascadeCount;
    mShadowOverlayForLighting = shadowOverlayResource;
}

void RenderingSystem::CreateLightingSrvs()
{
    if (!mDevice)
        return;

    if (mShadowMapForLighting && mShadowCascadeCountForLighting > 0)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC shadowSrvDesc = {};
        shadowSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowSrvDesc.Texture2DArray.MostDetailedMip = 0;
        shadowSrvDesc.Texture2DArray.MipLevels = 1;
        shadowSrvDesc.Texture2DArray.FirstArraySlice = 0;
        shadowSrvDesc.Texture2DArray.ArraySize = mShadowCascadeCountForLighting;
        mDevice->CreateShaderResourceView(
            mShadowMapForLighting,
            &shadowSrvDesc,
            mGBuffer.GetSrvCpu(GBuffer::BufferCount + 1));
    }

    if (mShadowOverlayForLighting)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC overlaySrvDesc = {};
        overlaySrvDesc.Format = mShadowOverlayForLighting->GetDesc().Format;
        overlaySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        overlaySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        overlaySrvDesc.Texture2D.MostDetailedMip = 0;
        overlaySrvDesc.Texture2D.MipLevels = mShadowOverlayForLighting->GetDesc().MipLevels;
        mDevice->CreateShaderResourceView(
            mShadowOverlayForLighting,
            &overlaySrvDesc,
            mGBuffer.GetSrvCpu(GBuffer::BufferCount + 2));
    }
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

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(mWidth);
    viewport.Height = static_cast<float>(mHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetPipelineState(wireframe ? mGeometryWireframePSO.Get() : mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(2, passCbAddress);
    cmdList->SetGraphicsRootDescriptorTable(4, checkerTextureHandle);
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    // G-buffer targets must be unbound before RTV -> SRV transition (particles draw into them too).
    cmdList->OMSetRenderTargets(0, nullptr, false, nullptr);
    mGBuffer.TransitionToShaderResources(cmdList);
}

void RenderingSystem::ExecuteLightingPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
    D3D12_GPU_VIRTUAL_ADDRESS lightParamsCbAddress,
    D3D12_GPU_VIRTUAL_ADDRESS shadowLightingCbAddress,
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
    CreateLightingSrvs();

    cmdList->OMSetRenderTargets(1, &backBufferRtv, true, nullptr);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(mWidth);
    viewport.Height = static_cast<float>(mHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSignature.Get());

    ID3D12DescriptorHeap* heap = mGBuffer.GetSrvHeap();
    cmdList->SetDescriptorHeaps(1, &heap);

    cmdList->SetGraphicsRootDescriptorTable(0, mGBuffer.GetSrvGpu(0));
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSrvGpu(GBuffer::BufferCount));
    cmdList->SetGraphicsRootDescriptorTable(2, mGBuffer.GetSrvGpu(GBuffer::BufferCount + 1));
    cmdList->SetGraphicsRootDescriptorTable(3, mGBuffer.GetSrvGpu(GBuffer::BufferCount + 2));
    cmdList->SetGraphicsRootConstantBufferView(4, passCbAddress);
    cmdList->SetGraphicsRootConstantBufferView(5, lightParamsCbAddress);
    cmdList->SetGraphicsRootConstantBufferView(6, shadowLightingCbAddress);
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

void RenderingSystem::BuildBillboardRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE checkerTable;
    checkerTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    CD3DX12_DESCRIPTOR_RANGE texTableB;
    texTableB.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
    CD3DX12_DESCRIPTOR_RANGE instanceTable;
    instanceTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

    CD3DX12_ROOT_PARAMETER slotRootParameter[7];
    slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[3].InitAsConstantBufferView(2, 0, D3D12_SHADER_VISIBILITY_ALL);
    slotRootParameter[4].InitAsDescriptorTable(1, &checkerTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[5].InitAsDescriptorTable(1, &texTableB, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[6].InitAsDescriptorTable(1, &instanceTable, D3D12_SHADER_VISIBILITY_VERTEX);

    CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        7,
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
        IID_PPV_ARGS(mBillboardRootSignature.GetAddressOf())));
}

void RenderingSystem::BuildLightingRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE gbufferSrvs;
    gbufferSrvs.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, GBuffer::BufferCount, 0);
    CD3DX12_DESCRIPTOR_RANGE lightsSrv;
    lightsSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
    CD3DX12_DESCRIPTOR_RANGE shadowSrv;
    shadowSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);
    CD3DX12_DESCRIPTOR_RANGE overlaySrv;
    overlaySrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);

    CD3DX12_ROOT_PARAMETER slotRootParameter[7];
    slotRootParameter[0].InitAsDescriptorTable(1, &gbufferSrvs, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsDescriptorTable(1, &lightsSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[2].InitAsDescriptorTable(1, &shadowSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsDescriptorTable(1, &overlaySrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[4].InitAsConstantBufferView(1);
    slotRootParameter[5].InitAsConstantBufferView(2);
    slotRootParameter[6].InitAsConstantBufferView(3);

    CD3DX12_STATIC_SAMPLER_DESC samplers[3] = {};
    samplers[0].Init(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f,
        1,
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0.0f,
        D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);
    samplers[1].Init(
        1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f,
        1,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f,
        D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);
    samplers[2].Init(
        2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        1,
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
        0.0f,
        D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        _countof(slotRootParameter),
        slotRootParameter,
        _countof(samplers),
        samplers,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

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

    mShaders["postVS"] = d3dUtil::CompileShader(L"Shaders\\PostProcess.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["postEdgePS"] = d3dUtil::CompileShader(L"Shaders\\PostProcess.hlsl", nullptr, "PS_Edge", "ps_5_0");
    mShaders["postVcrPS"] = d3dUtil::CompileShader(L"Shaders\\PostProcess.hlsl", nullptr, "PS_VCR", "ps_5_0");

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

void RenderingSystem::BuildBillboardShadersAndLayout()
{
    mShaders["billboardTreeVS"] = d3dUtil::CompileShader(L"Shaders\\BillboardTree.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["billboardTreePS"] = d3dUtil::CompileShader(L"Shaders\\BillboardTree.hlsl", nullptr, "PS", "ps_5_0");
    mShaders["treeMeshInstancedVS"] = d3dUtil::CompileShader(L"Shaders\\TreeMeshInstanced.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["treeMeshInstancedPS"] = d3dUtil::CompileShader(L"Shaders\\TreeMeshInstanced.hlsl", nullptr, "PS", "ps_5_0");

    mBillboardInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
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
    geometryPsoDesc.RTVFormats[0] = GBuffer::AlbedoFormat;
    geometryPsoDesc.RTVFormats[1] = GBuffer::NormalFormat;
    geometryPsoDesc.RTVFormats[2] = GBuffer::MaterialFormat;
    geometryPsoDesc.RTVFormats[3] = GBuffer::PositionFormat;
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC waterWirePsoDesc = waterPsoDesc;
    waterWirePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    waterWirePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&waterWirePsoDesc, IID_PPV_ARGS(&mWaterTransparentWireframePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC billboardPsoDesc = {};
    billboardPsoDesc.InputLayout = { mBillboardInputLayout.data(), (UINT)mBillboardInputLayout.size() };
    billboardPsoDesc.pRootSignature = mBillboardRootSignature.Get();
    billboardPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["billboardTreeVS"]->GetBufferPointer()),
        mShaders["billboardTreeVS"]->GetBufferSize()
    };
    billboardPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["billboardTreePS"]->GetBufferPointer()),
        mShaders["billboardTreePS"]->GetBufferSize()
    };
    billboardPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    billboardPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    billboardPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    billboardPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    billboardPsoDesc.SampleMask = UINT_MAX;
    billboardPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    billboardPsoDesc.NumRenderTargets = GBuffer::BufferCount;
    billboardPsoDesc.RTVFormats[0] = GBuffer::AlbedoFormat;
    billboardPsoDesc.RTVFormats[1] = GBuffer::NormalFormat;
    billboardPsoDesc.RTVFormats[2] = GBuffer::MaterialFormat;
    billboardPsoDesc.RTVFormats[3] = GBuffer::PositionFormat;
    billboardPsoDesc.SampleDesc.Count = 1;
    billboardPsoDesc.SampleDesc.Quality = 0;
    billboardPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&billboardPsoDesc, IID_PPV_ARGS(&mBillboardTreePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC treeMeshPsoDesc = {};
    treeMeshPsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    treeMeshPsoDesc.pRootSignature = mBillboardRootSignature.Get();
    treeMeshPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["treeMeshInstancedVS"]->GetBufferPointer()),
        mShaders["treeMeshInstancedVS"]->GetBufferSize()
    };
    treeMeshPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["treeMeshInstancedPS"]->GetBufferPointer()),
        mShaders["treeMeshInstancedPS"]->GetBufferSize()
    };
    treeMeshPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    treeMeshPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    treeMeshPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    treeMeshPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    treeMeshPsoDesc.SampleMask = UINT_MAX;
    treeMeshPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    treeMeshPsoDesc.NumRenderTargets = GBuffer::BufferCount;
    treeMeshPsoDesc.RTVFormats[0] = GBuffer::AlbedoFormat;
    treeMeshPsoDesc.RTVFormats[1] = GBuffer::NormalFormat;
    treeMeshPsoDesc.RTVFormats[2] = GBuffer::MaterialFormat;
    treeMeshPsoDesc.RTVFormats[3] = GBuffer::PositionFormat;
    treeMeshPsoDesc.SampleDesc.Count = 1;
    treeMeshPsoDesc.SampleDesc.Quality = 0;
    treeMeshPsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&treeMeshPsoDesc, IID_PPV_ARGS(&mTreeMeshInstancedPSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC postPsoDesc = {};
    postPsoDesc.InputLayout = { nullptr, 0 };
    postPsoDesc.pRootSignature = mPostProcessRootSignature.Get();
    postPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["postVS"]->GetBufferPointer()),
        mShaders["postVS"]->GetBufferSize()
    };
    postPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    postPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    postPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    postPsoDesc.DepthStencilState.DepthEnable = false;
    postPsoDesc.DepthStencilState.StencilEnable = false;
    postPsoDesc.SampleMask = UINT_MAX;
    postPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    postPsoDesc.NumRenderTargets = 1;
    postPsoDesc.RTVFormats[0] = mBackBufferFormat;
    postPsoDesc.SampleDesc.Count = 1;
    postPsoDesc.SampleDesc.Quality = 0;
    postPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

    postPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["postEdgePS"]->GetBufferPointer()),
        mShaders["postEdgePS"]->GetBufferSize()
    };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&postPsoDesc, IID_PPV_ARGS(&mEdgePostPSO)));

    postPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["postVcrPS"]->GetBufferPointer()),
        mShaders["postVcrPS"]->GetBufferSize()
    };
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&postPsoDesc, IID_PPV_ARGS(&mVcrPostPSO)));
}

void RenderingSystem::BuildPostProcessRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE sceneSrv;
    sceneSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_DESCRIPTOR_RANGE normalSrv;
    normalSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    CD3DX12_DESCRIPTOR_RANGE positionSrv;
    positionSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

    CD3DX12_ROOT_PARAMETER slotRootParameter[5];
    slotRootParameter[0].InitAsDescriptorTable(1, &sceneSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsDescriptorTable(1, &normalSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[2].InitAsDescriptorTable(1, &positionSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[3].InitAsConstantBufferView(0);
    slotRootParameter[4].InitAsConstantBufferView(1);

    CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        0,
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(
        _countof(slotRootParameter),
        slotRootParameter,
        1,
        &pointClamp,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

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
        IID_PPV_ARGS(mPostProcessRootSignature.GetAddressOf())));
}

void RenderingSystem::BuildPostProcessResources()
{
    if (!mDevice || mWidth == 0 || mHeight == 0)
        return;

    mPostSceneCopy.Reset();
    mPostTempTarget.Reset();
    mPostRtvHeap.Reset();

    auto createColorTarget = [&](ComPtr<ID3D12Resource>& outTex)
    {
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = mWidth;
        texDesc.Height = mHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = mBackBufferFormat;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        CD3DX12_CLEAR_VALUE clearValue(mBackBufferFormat, clearColor);
        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(mDevice->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&outTex)));
    };

    createColorTarget(mPostSceneCopy);
    createColorTarget(mPostTempTarget);

    mPostRtvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mPostRtvHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(mPostRtvHeap->GetCPUDescriptorHandleForHeapStart());
    mDevice->CreateRenderTargetView(mPostSceneCopy.Get(), nullptr, rtvHandle);
    rtvHandle.Offset(1, mPostRtvDescriptorSize);
    mDevice->CreateRenderTargetView(mPostTempTarget.Get(), nullptr, rtvHandle);

    CreatePostProcessSrvs();
}

void RenderingSystem::CreatePostProcessSrvs()
{
    if (!mDevice || !mPostSceneCopy || !mPostTempTarget)
        return;

    UINT srvDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = mBackBufferFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    mDevice->CreateShaderResourceView(
        mPostSceneCopy.Get(),
        &srvDesc,
        mGBuffer.GetSrvCpu(GBuffer::PostSceneSrvIndex));

    mDevice->CreateShaderResourceView(
        mPostTempTarget.Get(),
        &srvDesc,
        mGBuffer.GetSrvCpu(GBuffer::PostTempSrvIndex));
}

void RenderingSystem::CopyBackBufferToScene(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* backBuffer)
{
    auto toCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        mPostSceneCopy.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto toCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
        backBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER barriers[2] = { toCopyDest, toCopySource };
    cmdList->ResourceBarrier(2, barriers);

    cmdList->CopyResource(mPostSceneCopy.Get(), backBuffer);

    std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
    std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
    cmdList->ResourceBarrier(2, barriers);
}

void RenderingSystem::ExecutePostProcessPasses(
    ID3D12GraphicsCommandList* cmdList,
    ID3D12Resource* backBuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
    D3D12_GPU_VIRTUAL_ADDRESS postCbAddress,
    bool enableEdge,
    bool enableVcr)
{
    if (!enableEdge && !enableVcr)
        return;

    if (!mPostSceneCopy || !mPostTempTarget)
        return;

    CopyBackBufferToScene(cmdList, backBuffer);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(mWidth);
    viewport.Height = static_cast<float>(mHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heap = mGBuffer.GetSrvHeap();
    cmdList->SetDescriptorHeaps(1, &heap);

    cmdList->SetGraphicsRootSignature(mPostProcessRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(3, postCbAddress);
    cmdList->SetGraphicsRootConstantBufferView(4, passCbAddress);
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSrvGpu(1));
    cmdList->SetGraphicsRootDescriptorTable(2, mGBuffer.GetSrvGpu(3));
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    CD3DX12_CPU_DESCRIPTOR_HANDLE postSceneRtv(mPostRtvHeap->GetCPUDescriptorHandleForHeapStart());
    CD3DX12_CPU_DESCRIPTOR_HANDLE postTempRtv(postSceneRtv);
    postTempRtv.Offset(1, mPostRtvDescriptorSize);

    const auto drawFullscreen = [&](
        ID3D12PipelineState* pso,
        D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv,
        ID3D12Resource* renderTarget,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv,
        bool transitionTarget)
    {
        if (transitionTarget)
        {
            auto toRtv = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            cmdList->ResourceBarrier(1, &toRtv);
        }

        cmdList->OMSetRenderTargets(1, &rtv, false, nullptr);
        cmdList->SetPipelineState(pso);
        cmdList->SetGraphicsRootDescriptorTable(0, sceneSrv);
        cmdList->DrawInstanced(3, 1, 0, 0);

        if (transitionTarget)
        {
            auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                renderTarget,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &toSrv);
        }
    };

    D3D12_GPU_DESCRIPTOR_HANDLE sceneSrv = mGBuffer.GetSrvGpu(GBuffer::PostSceneSrvIndex);
    D3D12_GPU_DESCRIPTOR_HANDLE tempSrv = mGBuffer.GetSrvGpu(GBuffer::PostTempSrvIndex);

    if (enableEdge && enableVcr)
    {
        drawFullscreen(mEdgePostPSO.Get(), sceneSrv, mPostTempTarget.Get(), postTempRtv, true);
        drawFullscreen(mVcrPostPSO.Get(), tempSrv, backBuffer, backBufferRtv, false);
    }
    else if (enableEdge)
    {
        drawFullscreen(mEdgePostPSO.Get(), sceneSrv, backBuffer, backBufferRtv, false);
    }
    else
    {
        drawFullscreen(mVcrPostPSO.Get(), sceneSrv, backBuffer, backBufferRtv, false);
    }
}

void RenderingSystem::BeginTransparentWaterPass(
    ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_CPU_DESCRIPTOR_HANDLE dsv,
    D3D12_GPU_VIRTUAL_ADDRESS passCbAddress,
    bool wireframe)
{
    cmdList->OMSetRenderTargets(1, &backBufferRtv, FALSE, &dsv);

    D3D12_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(mWidth);
    viewport.Height = static_cast<float>(mHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(mWidth), static_cast<LONG>(mHeight) };
    cmdList->RSSetViewports(1, &viewport);
    cmdList->RSSetScissorRects(1, &scissor);

    cmdList->SetPipelineState(wireframe ? mWaterTransparentWireframePSO.Get() : mWaterTransparentPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(2, passCbAddress);
}
