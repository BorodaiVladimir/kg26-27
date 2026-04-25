#include "ParticleSystem.h"
#include "FrameResource.h"
#include <cstddef>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
constexpr UINT kRenderSrvTable = 0;   // [pool SRV, sort SRV]
constexpr UINT kUavTableAB = 2;       // [pool, aliveA, aliveB]
constexpr UINT kUavTableBA = 5;       // [pool, aliveB, aliveA]
constexpr UINT kUavDeadSort = 7;      // [dead, sort]
}

void ParticleSystem::Initialize(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap,
    UINT descriptorSize,
    UINT descriptorStartIndex)
{
    mDevice = device;
    mSrvHeap = srvHeap;
    mDescriptorSize = descriptorSize;
    mDescriptorStartIndex = descriptorStartIndex;

    BuildRootSignatures();
    BuildPSOs();
    BuildCommandSignature();
    BuildBuffersAndDescriptors(cmdList);
}

void ParticleSystem::BuildRootSignatures()
{
    CD3DX12_ROOT_PARAMETER computeParams[3];
    CD3DX12_DESCRIPTOR_RANGE uavMainRange;
    uavMainRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);
    CD3DX12_DESCRIPTOR_RANGE uavAuxRange;
    uavAuxRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 3);
    computeParams[0].InitAsConstantBufferView(0);
    computeParams[1].InitAsDescriptorTable(1, &uavMainRange);
    computeParams[2].InitAsDescriptorTable(1, &uavAuxRange);

    CD3DX12_ROOT_SIGNATURE_DESC computeRootDesc(
        _countof(computeParams), computeParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_NONE);

    ComPtr<ID3DBlob> sig = nullptr;
    ComPtr<ID3DBlob> err = nullptr;
    ThrowIfFailed(D3D12SerializeRootSignature(&computeRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        sig.GetAddressOf(), err.GetAddressOf()));
    ThrowIfFailed(mDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&mComputeRootSignature)));

    CD3DX12_ROOT_PARAMETER renderParams[2];
    CD3DX12_DESCRIPTOR_RANGE renderSrv;
    renderSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
    renderParams[0].InitAsDescriptorTable(1, &renderSrv, D3D12_SHADER_VISIBILITY_ALL);
    renderParams[1].InitAsConstantBufferView(1);

    CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_ROOT_SIGNATURE_DESC renderRootDesc(
        _countof(renderParams), renderParams, 1, &linearWrap,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    sig.Reset();
    err.Reset();
    ThrowIfFailed(D3D12SerializeRootSignature(&renderRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        sig.GetAddressOf(), err.GetAddressOf()));
    ThrowIfFailed(mDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        IID_PPV_ARGS(&mRenderRootSignature)));
}

void ParticleSystem::BuildPSOs()
{
    auto emitCs = d3dUtil::CompileShader(L"Shaders\\ParticlesEmit.hlsl", nullptr, "CSMain", "cs_5_0");
    auto simCs = d3dUtil::CompileShader(L"Shaders\\ParticlesSimulate.hlsl", nullptr, "CSMain", "cs_5_0");
    auto renderVs = d3dUtil::CompileShader(L"Shaders\\ParticlesRender.hlsl", nullptr, "VSMain", "vs_5_0");
    auto renderGs = d3dUtil::CompileShader(L"Shaders\\ParticlesRender.hlsl", nullptr, "GSMain", "gs_5_0");
    auto renderPs = d3dUtil::CompileShader(L"Shaders\\ParticlesRender.hlsl", nullptr, "PSMain", "ps_5_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC emitDesc = {};
    emitDesc.pRootSignature = mComputeRootSignature.Get();
    emitDesc.CS = { reinterpret_cast<BYTE*>(emitCs->GetBufferPointer()), emitCs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateComputePipelineState(&emitDesc, IID_PPV_ARGS(&mEmitPSO)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC simDesc = {};
    simDesc.pRootSignature = mComputeRootSignature.Get();
    simDesc.CS = { reinterpret_cast<BYTE*>(simCs->GetBufferPointer()), simCs->GetBufferSize() };
    ThrowIfFailed(mDevice->CreateComputePipelineState(&simDesc, IID_PPV_ARGS(&mSimulatePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawDesc = {};
    drawDesc.InputLayout = { nullptr, 0 };
    drawDesc.pRootSignature = mRenderRootSignature.Get();
    drawDesc.VS = { reinterpret_cast<BYTE*>(renderVs->GetBufferPointer()), renderVs->GetBufferSize() };
    drawDesc.GS = { reinterpret_cast<BYTE*>(renderGs->GetBufferPointer()), renderGs->GetBufferSize() };
    drawDesc.PS = { reinterpret_cast<BYTE*>(renderPs->GetBufferPointer()), renderPs->GetBufferSize() };
    drawDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    drawDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    drawDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    drawDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    drawDesc.SampleMask = UINT_MAX;
    drawDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    drawDesc.NumRenderTargets = 4;
    drawDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    drawDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    drawDesc.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
    drawDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    drawDesc.SampleDesc.Count = 1;
    drawDesc.SampleDesc.Quality = 0;
    drawDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&drawDesc, IID_PPV_ARGS(&mRenderPSO)));
}

void ParticleSystem::BuildCommandSignature()
{
    D3D12_INDIRECT_ARGUMENT_DESC arg = {};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC desc = {};
    desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    ThrowIfFailed(mDevice->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&mDrawIndirectSignature)));
}

void ParticleSystem::BuildBuffersAndDescriptors(ID3D12GraphicsCommandList* cmdList)
{
    const UINT particleStride = sizeof(GpuParticle);
    const UINT64 particleBytes = static_cast<UINT64>(particleStride) * kMaxParticles;
    const UINT64 indexBytes = static_cast<UINT64>(sizeof(UINT)) * kMaxParticles;
    auto poolDesc = CD3DX12_RESOURCE_DESC::Buffer(particleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto indexDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto counterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &poolDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mParticlePool)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mAliveListA)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mAliveListB)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mDeadList)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &indexDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mSortList)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mAliveCounterA)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mAliveCounterB)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mDeadCounter)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mSortCounter)));

    auto oneUintDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT));
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &oneUintDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mCounterResetUpload)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &oneUintDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mCounterMaxUpload)));
    auto deadInitDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBytes);
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &deadInitDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mDeadInitUpload)));
    ThrowIfFailed(mDevice->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &oneUintDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mAliveCountReadback)));

    UINT* resetPtr = nullptr;
    ThrowIfFailed(mCounterResetUpload->Map(0, nullptr, reinterpret_cast<void**>(&resetPtr)));
    *resetPtr = 0;
    mCounterResetUpload->Unmap(0, nullptr);
    UINT* maxPtr = nullptr;
    ThrowIfFailed(mCounterMaxUpload->Map(0, nullptr, reinterpret_cast<void**>(&maxPtr)));
    *maxPtr = kMaxParticles;
    mCounterMaxUpload->Unmap(0, nullptr);
    UINT* deadInit = nullptr;
    ThrowIfFailed(mDeadInitUpload->Map(0, nullptr, reinterpret_cast<void**>(&deadInit)));
    for (UINT i = 0; i < kMaxParticles; ++i)
        deadInit[i] = i;
    mDeadInitUpload->Unmap(0, nullptr);
    ThrowIfFailed(mAliveCountReadback->Map(0, nullptr, reinterpret_cast<void**>(&mMappedAliveCount)));
    *mMappedAliveCount = 0;

    auto cbSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ParticleSystemConstants));
    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mParticleConstants)));
    ThrowIfFailed(mParticleConstants->Map(0, nullptr, reinterpret_cast<void**>(&mMappedConstants)));
    ZeroMemory(mMappedConstants, sizeof(ParticleSystemConstants));

    auto argsDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(D3D12_DRAW_ARGUMENTS));
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &argsDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mIndirectArgsUpload)));
    ThrowIfFailed(mIndirectArgsUpload->Map(0, nullptr, reinterpret_cast<void**>(&mMappedIndirectArgs)));
    mMappedIndirectArgs->VertexCountPerInstance = 1;
    mMappedIndirectArgs->InstanceCount = 0;
    mMappedIndirectArgs->StartVertexLocation = 0;
    mMappedIndirectArgs->StartInstanceLocation = 0;
    ThrowIfFailed(mDevice->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &argsDesc,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, nullptr, IID_PPV_ARGS(&mIndirectArgs)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hCpu(mSrvHeap->GetCPUDescriptorHandleForHeapStart(), mDescriptorStartIndex, mDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC poolSrv = {};
    poolSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    poolSrv.Format = DXGI_FORMAT_UNKNOWN;
    poolSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    poolSrv.Buffer.FirstElement = 0;
    poolSrv.Buffer.NumElements = kMaxParticles;
    poolSrv.Buffer.StructureByteStride = particleStride;
    poolSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    mDevice->CreateShaderResourceView(mParticlePool.Get(), &poolSrv, hCpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC sortSrv = {};
    sortSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sortSrv.Format = DXGI_FORMAT_UNKNOWN;
    sortSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sortSrv.Buffer.FirstElement = 0;
    sortSrv.Buffer.NumElements = kMaxParticles;
    sortSrv.Buffer.StructureByteStride = sizeof(UINT);
    sortSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateShaderResourceView(mSortList.Get(), &sortSrv, hCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC poolUav = {};
    poolUav.Format = DXGI_FORMAT_UNKNOWN;
    poolUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    poolUav.Buffer.FirstElement = 0;
    poolUav.Buffer.NumElements = kMaxParticles;
    poolUav.Buffer.StructureByteStride = particleStride;
    poolUav.Buffer.CounterOffsetInBytes = 0;
    poolUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mParticlePool.Get(), nullptr, &poolUav, hCpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC idxUav = {};
    idxUav.Format = DXGI_FORMAT_UNKNOWN;
    idxUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    idxUav.Buffer.FirstElement = 0;
    idxUav.Buffer.NumElements = kMaxParticles;
    idxUav.Buffer.StructureByteStride = sizeof(UINT);
    idxUav.Buffer.CounterOffsetInBytes = 0;
    idxUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mAliveListA.Get(), mAliveCounterA.Get(), &idxUav, hCpu);
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mAliveListB.Get(), mAliveCounterB.Get(), &idxUav, hCpu);
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mAliveListB.Get(), mAliveCounterB.Get(), &idxUav, hCpu);
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mAliveListA.Get(), mAliveCounterA.Get(), &idxUav, hCpu);
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mDeadList.Get(), mDeadCounter.Get(), &idxUav, hCpu);
    hCpu.Offset(1, mDescriptorSize);
    mDevice->CreateUnorderedAccessView(mSortList.Get(), mSortCounter.Get(), &idxUav, hCpu);

    if (!cmdList)
        return;

    auto toCopyPool = CD3DX12_RESOURCE_BARRIER::Transition(mParticlePool.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto toCopyAliveA = CD3DX12_RESOURCE_BARRIER::Transition(mAliveCounterA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toCopyAliveB = CD3DX12_RESOURCE_BARRIER::Transition(mAliveCounterB.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toCopyDeadList = CD3DX12_RESOURCE_BARRIER::Transition(mDeadList.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toCopyDeadCtr = CD3DX12_RESOURCE_BARRIER::Transition(mDeadCounter.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toCopySort = CD3DX12_RESOURCE_BARRIER::Transition(mSortCounter.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    auto toUavAliveA = CD3DX12_RESOURCE_BARRIER::Transition(mAliveListA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto toUavAliveB = CD3DX12_RESOURCE_BARRIER::Transition(mAliveListB.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto toUavSortList = CD3DX12_RESOURCE_BARRIER::Transition(mSortList.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &toCopyPool);
    cmdList->ResourceBarrier(1, &toCopyAliveA);
    cmdList->ResourceBarrier(1, &toCopyAliveB);
    cmdList->ResourceBarrier(1, &toCopyDeadList);
    cmdList->ResourceBarrier(1, &toCopyDeadCtr);
    cmdList->ResourceBarrier(1, &toCopySort);
    cmdList->ResourceBarrier(1, &toUavAliveA);
    cmdList->ResourceBarrier(1, &toUavAliveB);
    cmdList->ResourceBarrier(1, &toUavSortList);

    cmdList->CopyBufferRegion(mAliveCounterA.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mAliveCounterB.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mSortCounter.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mDeadCounter.Get(), 0, mCounterMaxUpload.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mDeadList.Get(), 0, mDeadInitUpload.Get(), 0, indexBytes);

    auto aliveAToUav = CD3DX12_RESOURCE_BARRIER::Transition(mAliveCounterA.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto aliveBToUav = CD3DX12_RESOURCE_BARRIER::Transition(mAliveCounterB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto deadToUav = CD3DX12_RESOURCE_BARRIER::Transition(mDeadCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto sortToUav = CD3DX12_RESOURCE_BARRIER::Transition(mSortCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto deadListToUav = CD3DX12_RESOURCE_BARRIER::Transition(mDeadList.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &aliveAToUav);
    cmdList->ResourceBarrier(1, &aliveBToUav);
    cmdList->ResourceBarrier(1, &deadToUav);
    cmdList->ResourceBarrier(1, &sortToUav);
    cmdList->ResourceBarrier(1, &deadListToUav);

    mResourcesPrimed = true;
}

void ParticleSystem::ResetAliveAndSortCounters(ID3D12GraphicsCommandList* cmdList)
{
    ID3D12Resource* outCounter = mCurrentIsA ? mAliveCounterB.Get() : mAliveCounterA.Get();
    auto outToCopy = CD3DX12_RESOURCE_BARRIER::Transition(outCounter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    auto sortToCopy = CD3DX12_RESOURCE_BARRIER::Transition(mSortCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &outToCopy);
    cmdList->ResourceBarrier(1, &sortToCopy);
    cmdList->CopyBufferRegion(outCounter, 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mSortCounter.Get(), 0, mCounterResetUpload.Get(), 0, sizeof(UINT));
    auto outToUav = CD3DX12_RESOURCE_BARRIER::Transition(outCounter, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto sortToUav = CD3DX12_RESOURCE_BARRIER::Transition(mSortCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &outToUav);
    cmdList->ResourceBarrier(1, &sortToUav);
}

void ParticleSystem::SwapBuffers()
{
    mCurrentIsA = !mCurrentIsA;
}

void ParticleSystem::Update(ID3D12GraphicsCommandList* cmdList, float dt, float totalTime)
{
    if (!cmdList || !mResourcesPrimed)
        return;

    mAliveCount = (std::min)(*mMappedAliveCount, kMaxParticles);
    const UINT freeSlots = kMaxParticles - mAliveCount;

    mEmitAccumulator += dt * 260.0f;
    UINT emitCount = static_cast<UINT>(mEmitAccumulator);
    mEmitAccumulator -= static_cast<float>(emitCount);
    emitCount = (std::min)(emitCount, freeSlots);

    ResetAliveAndSortCounters(cmdList);

    mMappedConstants->DeltaTime = dt;
    mMappedConstants->TotalTime = totalTime;
    mMappedConstants->EmitRate = 260.0f;
    mMappedConstants->Gravity = -9.8f;
    mMappedConstants->EmitterPos = mEmitterPos;
    mMappedConstants->MaxLife = 6.0f;
    mMappedConstants->ConsumeCount = mAliveCount;
    mMappedConstants->EmitCount = emitCount;
    mMappedConstants->MaxParticles = kMaxParticles;

    CD3DX12_GPU_DESCRIPTOR_HANDLE uavMain(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mDescriptorStartIndex + (mCurrentIsA ? kUavTableAB : kUavTableBA), mDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavAux(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mDescriptorStartIndex + kUavDeadSort, mDescriptorSize);

    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, mParticleConstants->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(1, uavMain);
    cmdList->SetComputeRootDescriptorTable(2, uavAux);

    if (emitCount > 0)
    {
        cmdList->SetPipelineState(mEmitPSO.Get());
        const UINT groups = (emitCount + kThreadGroupSize - 1) / kThreadGroupSize;
        cmdList->Dispatch((std::max)(groups, 1u), 1, 1);
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    if (mAliveCount > 0)
    {
        cmdList->SetPipelineState(mSimulatePSO.Get());
        const UINT groups = (mAliveCount + kThreadGroupSize - 1) / kThreadGroupSize;
        cmdList->Dispatch((std::max)(groups, 1u), 1, 1);
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        cmdList->ResourceBarrier(1, &uavBarrier);
    }

    ID3D12Resource* outAliveCounter = mCurrentIsA ? mAliveCounterB.Get() : mAliveCounterA.Get();
    auto aliveToCopy = CD3DX12_RESOURCE_BARRIER::Transition(outAliveCounter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->ResourceBarrier(1, &aliveToCopy);
    cmdList->CopyBufferRegion(mAliveCountReadback.Get(), 0, outAliveCounter, 0, sizeof(UINT));
    auto aliveToUav = CD3DX12_RESOURCE_BARRIER::Transition(outAliveCounter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &aliveToUav);

    mMappedIndirectArgs->VertexCountPerInstance = 1;
    mMappedIndirectArgs->InstanceCount = 0;
    mMappedIndirectArgs->StartVertexLocation = 0;
    mMappedIndirectArgs->StartInstanceLocation = 0;
    auto argsToCopy = CD3DX12_RESOURCE_BARRIER::Transition(mIndirectArgs.Get(),
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &argsToCopy);
    cmdList->CopyBufferRegion(mIndirectArgs.Get(), 0, mIndirectArgsUpload.Get(), 0, sizeof(D3D12_DRAW_ARGUMENTS));
    cmdList->CopyBufferRegion(mIndirectArgs.Get(), static_cast<UINT>(offsetof(D3D12_DRAW_ARGUMENTS, InstanceCount)),
        mSortCounter.Get(), 0, sizeof(UINT));
    auto argsToIndirect = CD3DX12_RESOURCE_BARRIER::Transition(mIndirectArgs.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cmdList->ResourceBarrier(1, &argsToIndirect);

    SwapBuffers();
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* cmdList, D3D12_GPU_VIRTUAL_ADDRESS passCbAddress)
{
    if (!cmdList)
        return;

    auto poolToSrv = CD3DX12_RESOURCE_BARRIER::Transition(mParticlePool.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto sortToSrv = CD3DX12_RESOURCE_BARRIER::Transition(mSortList.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &poolToSrv);
    cmdList->ResourceBarrier(1, &sortToSrv);

    CD3DX12_GPU_DESCRIPTOR_HANDLE renderSrv(mSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mDescriptorStartIndex + kRenderSrvTable, mDescriptorSize);
    cmdList->SetGraphicsRootSignature(mRenderRootSignature.Get());
    cmdList->SetPipelineState(mRenderPSO.Get());
    cmdList->SetGraphicsRootDescriptorTable(0, renderSrv);
    cmdList->SetGraphicsRootConstantBufferView(1, passCbAddress);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmdList->ExecuteIndirect(mDrawIndirectSignature.Get(), 1, mIndirectArgs.Get(), 0, nullptr, 0);

    auto poolBack = CD3DX12_RESOURCE_BARRIER::Transition(mParticlePool.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto sortBack = CD3DX12_RESOURCE_BARRIER::Transition(mSortList.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmdList->ResourceBarrier(1, &poolBack);
    cmdList->ResourceBarrier(1, &sortBack);
}
