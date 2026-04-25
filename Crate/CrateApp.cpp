#include "../Common/d3dUtil.h"
#include "FrameResource.h"
#include "../Common/d3dApp.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "Lights.h"
#include "RenderingSystem.h"
#include "KdTree.h"
#include "ParticleSystem.h"
#include <DirectXCollision.h>
#include <memory>
#include <random>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <string>
#include <sstream>
#include <map>
#include <array>
#include <algorithm>
#include <cstring>
#include <DirectXTex.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "DirectXTex.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace
{
struct BillboardVertex
{
    XMFLOAT3 Pos;
    XMFLOAT2 TexC;
};

struct TreeInstanceGpu
{
    XMFLOAT3 WorldPos;
    float Pad;
};

constexpr float kForestPatchHalfExtent = 24.0f;
constexpr float kForestPatchCenterX = 0.0f;
constexpr float kForestPatchCenterZ = -63.0f;

bool FileExistsA_Local(const char* p)
{
    return p && ::GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

std::string GetExeDirA_Local()
{
    char buf[MAX_PATH]{};
    if (::GetModuleFileNameA(nullptr, buf, MAX_PATH) == 0)
        return {};
    std::string s(buf);
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos)
        s.resize(slash + 1);
    return s;
}

std::string ResolveMediaPath(const std::string& path)
{
    if (FileExistsA_Local(path.c_str()))
        return path;

    std::string p = path;
    std::replace(p.begin(), p.end(), '/', '\\');

    std::string fname = p;
    const size_t fs = fname.find_last_of("\\/");
    if (fs != std::string::npos)
        fname = fname.substr(fs + 1);

    const std::string exe = GetExeDirA_Local();

    const std::string tryPaths[] = {
        p,
        path,
        exe + p,
        exe + "..\\" + p,
        exe + "..\\..\\" + p,
        exe + "..\\..\\..\\" + p,
        std::string("..\\") + p,
        std::string("..\\..\\") + p,
        std::string("..\\..\\..\\") + p,
        exe + "..\\..\\Models\\" + fname,
        exe + "..\\Models\\" + fname,
        std::string("..\\..\\Models\\") + fname,
        std::string("..\\Models\\") + fname,
        std::string("Models\\") + fname,
        exe + "Models\\" + fname,
    };

    for (const auto& t : tryPaths)
    {
        if (!t.empty() && FileExistsA_Local(t.c_str()))
            return t;
    }
    return path;
}
}

const int gNumFrameResources = 3;

struct RenderItem
{
    RenderItem() = default;

    XMFLOAT4X4 World = MathHelper::Identity4x4();
    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
    XMFLOAT4X4 TexTransformDisp = MathHelper::Identity4x4();
    int NumFramesDirty = gNumFrameResources;
    UINT ObjCBIndex = -1;
    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
    float TextureOffsetU = 0.0f;
    float TextureOffsetV = 0.0f;
    float TextureScaleU = 1.0f;
    float TextureScaleV = 1.0f;
    bool AnimateTexture = false;
    XMFLOAT2 AnimationSpeed = { 0.0f, 0.0f };
    bool IsStressObject = false;
};

struct SubmeshData
{
    std::string MaterialName;
    SubmeshGeometry Geometry;
};

class CrateApp : public D3DApp
{
public:
    CrateApp(HINSTANCE hInstance);
    CrateApp(const CrateApp& rhs) = delete;
    CrateApp& operator=(const CrateApp& rhs) = delete;
    ~CrateApp();

    virtual bool Initialize() override;

private:
    std::wstring GetFrameStatsExtra() const override;

    virtual void OnResize() override;
    virtual void Update(const GameTimer& gt) override;
    virtual void Draw(const GameTimer& gt) override;
    virtual void OnMouseDown(WPARAM btnState, int x, int y) override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y) override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y) override;

    void UpdateCamera(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMaterialCBs(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateDeferredLightCB();

    void LoadTextures();
    void LoadBillboardTreeTexture();
    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void LoadOBJModels();
    void LoadTreeLodMesh();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void CreateBoxGeometry();
    void CreateWaterPlaneGeometry();
    void CreateBillboardForest();
    void UpdateForestLod();
    void DrawBillboardForest(ID3D12GraphicsCommandList* cmdList);
    void BuildStressTestObjects(int& objIndex);
    void UpdateStressVisibility();

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

    void LoadModelTexture(const std::string& texturePath, const std::string& texName, int heapIndex);

private:
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;
    std::unordered_map<std::string, std::unique_ptr<Texture>> mTextures;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;
    ComPtr<ID3D12PipelineState> mOpaquePSO = nullptr;
    std::unique_ptr<RenderingSystem> mRenderingSystem;
    std::unique_ptr<ParticleSystem> mParticleSystem;

    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*> mSponzaOpaqueRitems;
    std::vector<RenderItem*> mStressRitems;
    std::vector<DirectX::BoundingBox> mStressWorldBounds;
    std::vector<RenderItem*> mStressVisibleRitems;
    KdTree mKdTree;

    std::vector<RenderItem*> mWaterRitems;

    bool mFrustumCullEnabled = true;
    bool mKdTreeCullingEnabled = true;
    bool mF4KeyDown = false;
    bool mF5KeyDown = false;

    struct LoadedModel
    {
        std::string Name;
        std::vector<Vertex> Vertices;
        std::vector<uint32_t> Indices;
        std::vector<SubmeshData> Submeshes;
    };
    std::vector<LoadedModel> mLoadedModels;

    std::map<std::string, int> mTextureCache;
    std::map<std::string, int> mMaterialToHeapIndex;
    std::map<std::string, int> mMaterialToBumpHeapIndex;

    PassConstants mMainPassCB;

    XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.3f * XM_PI;
    float mPhi = 0.4f * XM_PI;
    float mRadius = 8.0f;

    POINT mLastMousePos;

    std::array<DirectionalLightSource, kDeferredDirectionalLightCount> mDirectionalLights;
    std::array<PointLightSource, kDeferredPointLightCount> mPointLights;
    std::array<SpotLightSource, kDeferredSpotLightCount> mSpotLights;

    float mSpawnAccumulator = 0.0f;
    std::array<float, kDeferredPointLightCount> mFallingVelY{};
    std::array<bool, kDeferredPointLightCount> mFallingActive{};
    UINT mActivePointLights = 0;

    bool mGeometryWireframe = false;
    bool mF3KeyDown = false;

    std::vector<TreeInstanceGpu> mForestInstancesCpu;
    ComPtr<ID3D12Resource> mForestMeshUpload;
    ComPtr<ID3D12Resource> mForestBillboardUpload;
    BYTE* mForestMeshMapped = nullptr;
    BYTE* mForestBillboardMapped = nullptr;
    UINT mForestMeshCount = 0;
    UINT mForestBillboardCount = 0;
    bool mTreeLodMeshLoaded = false;
    UINT mTreeMtlDiffuseSrvHeapIndex = 0;
    UINT mBillboardForestInstanceCount = 0;
    UINT mBillboardObjectCbIndex = 0;
    UINT mBillboardTreeSrvHeapIndex = 0;
    static constexpr UINT kMaxForestInstances = 512;
    static constexpr float kForestLodMeshDistance = 26.0f;
    static constexpr UINT kTreeInstanceMeshSrvHeapIndex = 240;
    static constexpr UINT kTreeInstanceBillboardSrvHeapIndex = 241;
    static constexpr UINT kParticleSrvHeapStartIndex = 244;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance, PSTR cmdLine, int showCmd)
{
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        CrateApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;
        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

CrateApp::CrateApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
    mMainWndCaption = L"Crate DX12";
    mTheta = 1.3f * XM_PI;
    mPhi = 0.4f * XM_PI;
    mRadius = 8.0f;

    mDirectionalLights[0].Direction = { 0.45f, -0.72f, 0.52f };
    mDirectionalLights[0].Strength = { 0.20f, 0.35f, 1.40f };

    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        mPointLights[i].Strength = { 0.20f, 1.20f, 0.20f };
        mPointLights[i].FalloffStart = 0.6f;
        mPointLights[i].FalloffEnd = 8.0f;
        mPointLights[i].Position = { 0.0f, 10000.0f, 0.0f };
        mFallingActive[i] = false;
        mFallingVelY[i] = 0.0f;
    }

    mSpotLights[0].Position = { 0.0f, 12.0f, -30.0f };
    mSpotLights[0].Direction = { 0.0f, -0.35f, 1.0f };
    mSpotLights[0].Strength = { 4.0f, 0.35f, 0.35f };
    mSpotLights[0].FalloffStart = 4.0f;
    mSpotLights[0].FalloffEnd = 65.0f;
    mSpotLights[0].SpotPower = 28.0f;

    mSpotLights[1].Position = { 0.0f, 0.0f, 0.0f };
    mSpotLights[1].Direction = { 0.0f, -0.4f, 1.0f };
    mSpotLights[1].Strength = { 3.6f, 0.28f, 0.28f };
    mSpotLights[1].FalloffStart = 1.5f;
    mSpotLights[1].FalloffEnd = 70.0f;
    mSpotLights[1].SpotPower = 96.0f;
}

CrateApp::~CrateApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
}

std::wstring CrateApp::GetFrameStatsExtra() const
{
    wchar_t buf[320];
    swprintf_s(
        buf,
        L"   stress draw: %zu / %zu   F4 cull:%s F5 kd:%s",
        mStressVisibleRitems.size(),
        mStressRitems.size(),
        mFrustumCullEnabled ? L"on" : L"off",
        mKdTreeCullingEnabled ? L"on" : L"off");
    return buf;
}

bool CrateApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LoadTextures();
    BuildDescriptorHeaps();
    LoadOBJModels();
    LoadTreeLodMesh();
    LoadBillboardTreeTexture();
    CreateWaterPlaneGeometry();
    CreateBoxGeometry();
    CreateBillboardForest();
    BuildMaterials();
    BuildRenderItems();
    BuildFrameResources();

    mRenderingSystem = std::make_unique<RenderingSystem>();
    mRenderingSystem->Initialize(
        md3dDevice.Get(),
        mClientWidth,
        mClientHeight,
        mBackBufferFormat,
        mDepthStencilFormat,
        m4xMsaaState,
        m4xMsaaQuality);

    mParticleSystem = std::make_unique<ParticleSystem>();
    mParticleSystem->Initialize(
        md3dDevice.Get(),
        mCommandList.Get(),
        mSrvDescriptorHeap.Get(),
        mCbvSrvDescriptorSize,
        kParticleSrvHeapStartIndex);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    FlushCommandQueue();

    return true;
}

void CrateApp::OnResize()
{
    D3DApp::OnResize();

    if (mRenderingSystem)
    {
        mRenderingSystem->OnResize(mClientWidth, mClientHeight);
    }

    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void CrateApp::Update(const GameTimer& gt)
{
    UpdateCamera(gt);
    UpdateForestLod();

    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    AnimateMaterials(gt);
    UpdateObjectCBs(gt);
    UpdateMaterialCBs(gt);
    UpdateMainPassCB(gt);

    const SHORT f3State = GetAsyncKeyState(VK_F3);
    if ((f3State & 0x8000) != 0)
    {
        if (!mF3KeyDown)
        {
            mGeometryWireframe = !mGeometryWireframe;
            mF3KeyDown = true;
            OutputDebugStringA(mGeometryWireframe ? "Geometry + water: WIREFRAME (tessellation debug)\n" : "Geometry + water: SOLID\n");
        }
    }
    else
    {
        mF3KeyDown = false;
    }

    const SHORT f4State = GetAsyncKeyState(VK_F4);
    if ((f4State & 0x8000) != 0)
    {
        if (!mF4KeyDown)
        {
            mFrustumCullEnabled = !mFrustumCullEnabled;
            mF4KeyDown = true;
            char buf[128];
            sprintf_s(buf, "Frustum culling: %s\n", mFrustumCullEnabled ? "ON" : "OFF");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF4KeyDown = false;
    }

    const SHORT f5State = GetAsyncKeyState(VK_F5);
    if ((f5State & 0x8000) != 0)
    {
        if (!mF5KeyDown)
        {
            mKdTreeCullingEnabled = !mKdTreeCullingEnabled;
            mF5KeyDown = true;
            char buf[128];
            sprintf_s(buf, "KD-tree for culling: %s\n", mKdTreeCullingEnabled ? "ON" : "OFF");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF5KeyDown = false;
    }

    if (!mFallingActive[0])
    {
        mFallingActive[0] = true;
        mFallingVelY[0] = 0.0f;
        mPointLights[0].Position = { 6.0f, 20.0f, 0.0f };
    }

    mSpawnAccumulator += gt.DeltaTime();
    while (mSpawnAccumulator >= 1.0f)
    {
        mSpawnAccumulator -= 1.0f;
        for (UINT i = 0; i < kDeferredPointLightCount; ++i)
        {
            if (!mFallingActive[i])
            {
                mFallingActive[i] = true;
                mFallingVelY[i] = 0.0f;
                mPointLights[i].Position = { 6.0f, 20.0f, 0.0f };
                break;
            }
        }
    }

    constexpr float fallSpeed = 21.0f;
    constexpr float floorY = -1.0f;
    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        if (!mFallingActive[i])
            continue;

        auto& L = mPointLights[i];
        if (L.Position.y <= floorY + 1e-3f)
            continue;

        mFallingVelY[i] = -fallSpeed;
        L.Position.y += mFallingVelY[i] * gt.DeltaTime();
        if (L.Position.y <= floorY)
        {
            L.Position.y = floorY;
            mFallingVelY[i] = 0.0f;
        }
    }
    UpdateDeferredLightCB();
}

void CrateApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    ThrowIfFailed(cmdListAlloc->Reset());
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    UpdateStressVisibility();

    auto passCB = mCurrFrameResource->PassCB->Resource();
    auto passAddress = passCB->GetGPUVirtualAddress();

    CD3DX12_GPU_DESCRIPTOR_HANDLE checkerTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    checkerTex.Offset(1, mCbvSrvDescriptorSize);
    mRenderingSystem->BeginGeometryPass(mCommandList.Get(), DepthStencilView(), passAddress, checkerTex, mGeometryWireframe);

    DrawRenderItems(mCommandList.Get(), mSponzaOpaqueRitems);
    DrawRenderItems(mCommandList.Get(), mStressVisibleRitems);
    DrawBillboardForest(mCommandList.Get());
    if (mParticleSystem)
    {
        mParticleSystem->SetEmitterPosition(XMFLOAT3(0.0f, 1.2f, 0.0f));
        mParticleSystem->Update(mCommandList.Get(), gt.DeltaTime(), gt.TotalTime());
        mParticleSystem->Render(mCommandList.Get(), passAddress);
    }

    mRenderingSystem->EndGeometryPass(mCommandList.Get());

    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &transition);
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);

    mRenderingSystem->ExecuteLightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        passAddress,
        mCurrFrameResource->DeferredLightParamsCB->Resource()->GetGPUVirtualAddress(),
        mCurrFrameResource->DeferredLightBuffer->Resource(),
        kDeferredTotalLightCount,
        sizeof(DeferredLightGpu));

    if (!mWaterRitems.empty())
    {
        ID3D12DescriptorHeap* srvHeap[] = { mSrvDescriptorHeap.Get() };
        mCommandList->SetDescriptorHeaps(1, srvHeap);

        mRenderingSystem->BeginTransparentWaterPass(
            mCommandList.Get(),
            CurrentBackBufferView(),
            DepthStencilView(),
            passAddress,
            mGeometryWireframe);
        DrawRenderItems(mCommandList.Get(), mWaterRitems);
    }

    transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &transition);

    ThrowIfFailed(mCommandList->Close());

    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CrateApp::CreateBoxGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);

    std::vector<Vertex> vertices(box.Vertices.size());
    for (size_t i = 0; i < box.Vertices.size(); ++i)
    {
        vertices[i].Pos = box.Vertices[i].Position;
        vertices[i].Normal = box.Vertices[i].Normal;
        vertices[i].TexC = box.Vertices[i].TexC;
    }

    std::vector<std::uint32_t> indices = box.Indices32;

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "BoxGeo";

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["box"] = submesh;

    mGeometries[geo->Name] = std::move(geo);
}

void CrateApp::CreateBillboardForest()
{
    std::vector<BillboardVertex> verts = {
        { { -0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
        { { 0.5f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
        { { 0.5f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
        { { -0.5f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
    };
    const std::vector<std::uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "BillboardQuad";

    const UINT vbByteSize = (UINT)(verts.size() * sizeof(BillboardVertex));
    const UINT ibByteSize = (UINT)(indices.size() * sizeof(std::uint32_t));

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), verts.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), verts.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(BillboardVertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["tree"] = submesh;

    mGeometries[geo->Name] = std::move(geo);

    mForestInstancesCpu.reserve(kMaxForestInstances);
    std::mt19937 rng(99901u);
    std::uniform_real_distribution<float> distX(
        kForestPatchCenterX - kForestPatchHalfExtent,
        kForestPatchCenterX + kForestPatchHalfExtent);
    std::uniform_real_distribution<float> distZ(
        kForestPatchCenterZ - kForestPatchHalfExtent,
        kForestPatchCenterZ + kForestPatchHalfExtent);
    while (mForestInstancesCpu.size() < kMaxForestInstances)
    {
        const float x = distX(rng);
        const float z = distZ(rng);
        TreeInstanceGpu t{};
        t.WorldPos = XMFLOAT3(x, -1.0f, z);
        t.Pad = 0.0f;
        mForestInstancesCpu.push_back(t);
    }

    mBillboardForestInstanceCount = (UINT)mForestInstancesCpu.size();
    const UINT instBufBytes = kMaxForestInstances * (UINT)sizeof(TreeInstanceGpu);

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(instBufBytes);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mForestMeshUpload)));
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mForestBillboardUpload)));

    ThrowIfFailed(mForestMeshUpload->Map(0, nullptr, reinterpret_cast<void**>(&mForestMeshMapped)));
    ThrowIfFailed(mForestBillboardUpload->Map(0, nullptr, reinterpret_cast<void**>(&mForestBillboardMapped)));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = kMaxForestInstances;
    srvDesc.Buffer.StructureByteStride = sizeof(TreeInstanceGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hMesh(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    hMesh.Offset(kTreeInstanceMeshSrvHeapIndex, mCbvSrvDescriptorSize);
    md3dDevice->CreateShaderResourceView(mForestMeshUpload.Get(), &srvDesc, hMesh);

    CD3DX12_CPU_DESCRIPTOR_HANDLE hBill(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    hBill.Offset(kTreeInstanceBillboardSrvHeapIndex, mCbvSrvDescriptorSize);
    md3dDevice->CreateShaderResourceView(mForestBillboardUpload.Get(), &srvDesc, hBill);

    OutputDebugStringA("Billboard forest geometry + instances created\n");
}

void CrateApp::CreateWaterPlaneGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData grid = geoGen.CreateGrid(120.0f, 120.0f, 56, 56);

    std::vector<Vertex> vertices(grid.Vertices.size());
    for (size_t i = 0; i < grid.Vertices.size(); ++i)
    {
        vertices[i].Pos = grid.Vertices[i].Position;
        vertices[i].Normal = grid.Vertices[i].Normal;
        vertices[i].TexC = grid.Vertices[i].TexC;
    }

    std::vector<std::uint32_t> indices = grid.Indices32;

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "WaterPlane";

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["water"] = submesh;

    mGeometries[geo->Name] = std::move(geo);
    OutputDebugStringA("Water plane geometry created\n");
}

void CrateApp::LoadModelTexture(const std::string& texturePath, const std::string& texName, int heapIndex)
{
    const std::string resolvedPath = ResolveMediaPath(texturePath);

    char msg[512];
    if (resolvedPath != texturePath)
        sprintf_s(msg, "Loading texture: %s (resolved: %s) as %s at index %d\n", texturePath.c_str(),
            resolvedPath.c_str(), texName.c_str(), heapIndex);
    else
        sprintf_s(msg, "Loading texture: %s as %s at index %d\n", texturePath.c_str(), texName.c_str(), heapIndex);
    OutputDebugStringA(msg);

    std::wstring wTexturePath(resolvedPath.begin(), resolvedPath.end());

    auto modelTex = std::make_unique<Texture>();
    modelTex->Name = texName;
    modelTex->Filename = wTexturePath;

    ComPtr<ID3D12Resource> res;
    ComPtr<ID3D12Resource> upload;
    HRESULT hr = E_FAIL;

    ScratchImage image;
    TexMetadata metadata;

    hr = LoadFromTGAFile(wTexturePath.c_str(), &metadata, image);
    if (FAILED(hr))
    {
        hr = LoadFromWICFile(wTexturePath.c_str(), WIC_FLAGS_FORCE_SRGB, &metadata, image);
        if (FAILED(hr))
            hr = LoadFromWICFile(wTexturePath.c_str(), WIC_FLAGS_NONE, &metadata, image);
    }
    if (FAILED(hr))
        hr = LoadFromDDSFile(wTexturePath.c_str(), DDS_FLAGS_NONE, &metadata, image);

    if (SUCCEEDED(hr))
    {
        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = metadata.width;
        textureDesc.Height = (UINT)metadata.height;
        textureDesc.DepthOrArraySize = (UINT16)metadata.arraySize;
        textureDesc.MipLevels = (UINT16)metadata.mipLevels;
        textureDesc.Format = metadata.format;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

        hr = md3dDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&res));

        if (SUCCEEDED(hr))
        {
            const UINT64 uploadBufferSize = GetRequiredIntermediateSize(res.Get(), 0, static_cast<UINT>(image.GetImageCount()));

            CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
            CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

            hr = md3dDevice->CreateCommittedResource(
                &uploadHeapProps,
                D3D12_HEAP_FLAG_NONE,
                &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&upload));

            if (SUCCEEDED(hr))
            {
                std::vector<D3D12_SUBRESOURCE_DATA> subresources(image.GetImageCount());
                for (size_t i = 0; i < image.GetImageCount(); ++i)
                {
                    const auto* img = image.GetImages() + i;
                    subresources[i].pData = img->pixels;
                    subresources[i].RowPitch = img->rowPitch;
                    subresources[i].SlicePitch = img->slicePitch;
                }

                CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    res.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_DEST);
                mCommandList->ResourceBarrier(1, &barrier);

                UpdateSubresources(mCommandList.Get(), res.Get(), upload.Get(),
                    0, 0, (UINT)subresources.size(), subresources.data());

                barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                    res.Get(),
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                mCommandList->ResourceBarrier(1, &barrier);

                modelTex->Resource = res;
                modelTex->UploadHeap = upload;

                CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(
                    mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
                hDescriptor.Offset(heapIndex, mCbvSrvDescriptorSize);

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Format = metadata.format;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MostDetailedMip = 0;
                srvDesc.Texture2D.MipLevels = (UINT)metadata.mipLevels;
                srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

                md3dDevice->CreateShaderResourceView(res.Get(), &srvDesc, hDescriptor);

                sprintf_s(msg, "Texture loaded successfully: %s\n", resolvedPath.c_str());
                OutputDebugStringA(msg);
            }
        }
    }

    if (FAILED(hr))
    {
        sprintf_s(msg, "Failed to load texture: %s\n", texturePath.c_str());
        OutputDebugStringA(msg);
    }

    mTextures[modelTex->Name] = std::move(modelTex);
}

void CrateApp::LoadBillboardTreeTexture()
{
    mBillboardTreeSrvHeapIndex = 0;

    int nextHeap = 2;
    for (const auto& kv : mTextureCache)
        nextHeap = (std::max)(nextHeap, kv.second + 1);
    nextHeap = (std::max)(nextHeap, (int)kTreeInstanceBillboardSrvHeapIndex + 1);

    static const char* kTexNames[] = {
        "tree.dds",
        "tree.png",
        "tree.tga",
        "tree.jpg",
        "tree.jpeg",
    };
    static const char* kTexDirs[] = {
        "../Textures/",
        "Textures/",
        "../../Textures/",
        "../../../Textures/",
    };

    for (const char* dir : kTexDirs)
    {
        for (const char* name : kTexNames)
        {
            const std::string path = std::string(dir) + name;
            const std::string resolved = ResolveMediaPath(path);
            if (!FileExistsA_Local(resolved.c_str()))
                continue;

            mTextures.erase("treeTex");
            LoadModelTexture(resolved, "treeTex", nextHeap);

            auto it = mTextures.find("treeTex");
            if (it != mTextures.end() && it->second && it->second->Resource)
            {
                mBillboardTreeSrvHeapIndex = static_cast<UINT>(nextHeap);
                char msg[384];
                sprintf_s(msg, "Billboard: tree texture OK: %s (heap %u)\n", resolved.c_str(), mBillboardTreeSrvHeapIndex);
                OutputDebugStringA(msg);
                return;
            }
        }
    }

    OutputDebugStringA(
        "Billboard: tree texture not found (place tree.png|dds|... under Textures/ next to exe or ../Textures/). Using wood.\n");
}

void CrateApp::LoadOBJModels()
{
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Loading Sponza model...\n");
    OutputDebugStringA("========================================\n");

    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile("../Models/sponza1/sponza.obj",
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_JoinIdenticalVertices);

    if (!scene || !scene->mRootNode || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)
    {
        std::string error = "Failed to load Sponza: ";
        error += importer.GetErrorString();
        error += "\n";
        OutputDebugStringA(error.c_str());
        return;
    }

    OutputDebugStringA("Sponza loaded successfully!\n");

    LoadedModel model;
    model.Name = "Sponza";

    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;

    char msg[256];
    sprintf_s(msg, "Total meshes found: %d\n", scene->mNumMeshes);
    OutputDebugStringA(msg);

    std::map<std::string, std::string> materialToTexture;
    std::map<std::string, std::string> materialToBump;
    for (unsigned int m = 0; m < scene->mNumMaterials; ++m)
    {
        aiMaterial* material = scene->mMaterials[m];
        aiString matName;
        if (material->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS)
            continue;

        std::string name = matName.C_Str();

        aiString texPath;
        if (material->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
        {
            std::string path = texPath.C_Str();
            std::string baseDir = "../Models/sponza1/";
            if (!path.empty() && path[0] != '/' && path[0] != '\\' && path.find(":") == std::string::npos)
            {
                path = baseDir + path;
            }
            std::replace(path.begin(), path.end(), '\\', '/');
            materialToTexture[name] = path;
        }

        aiString bumpPath;
        if (material->GetTexture(aiTextureType_HEIGHT, 0, &bumpPath) == AI_SUCCESS ||
            material->GetTexture(aiTextureType_NORMALS, 0, &bumpPath) == AI_SUCCESS)
        {
            std::string path = bumpPath.C_Str();
            std::string baseDir = "../Models/sponza1/";
            if (!path.empty() && path[0] != '/' && path[0] != '\\' && path.find(":") == std::string::npos)
            {
                path = baseDir + path;
            }
            std::replace(path.begin(), path.end(), '\\', '/');
            materialToBump[name] = path;
        }
    }

    int nextHeapIndex = 2;
    for (const auto& pair : materialToTexture)
    {
        const std::string& matName = pair.first;
        const std::string& texPath = pair.second;

        if (mTextureCache.find(texPath) == mTextureCache.end())
        {
            std::string texName = "Tex_" + matName;
            LoadModelTexture(texPath, texName, nextHeapIndex);
            mTextureCache[texPath] = nextHeapIndex;
            mMaterialToHeapIndex[matName] = nextHeapIndex;
            sprintf_s(msg, "Cached texture for material %s at index %d\n", matName.c_str(), nextHeapIndex);
            OutputDebugStringA(msg);
            nextHeapIndex++;
        }
        else
        {
            mMaterialToHeapIndex[matName] = mTextureCache[texPath];
        }
    }

    for (const auto& pair : materialToBump)
    {
        const std::string& matName = pair.first;
        const std::string& texPath = pair.second;
        if (mTextureCache.find(texPath) == mTextureCache.end())
        {
            std::string texName = "Bump_" + matName;
            LoadModelTexture(texPath, texName, nextHeapIndex);
            mTextureCache[texPath] = nextHeapIndex;
            mMaterialToBumpHeapIndex[matName] = nextHeapIndex;
            nextHeapIndex++;
        }
        else
        {
            mMaterialToBumpHeapIndex[matName] = mTextureCache[texPath];
        }
    }

    for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
    {
        aiMesh* mesh = scene->mMeshes[m];

        sprintf_s(msg, "  Mesh %d: %d vertices, %d faces\n", m, mesh->mNumVertices, mesh->mNumFaces);
        OutputDebugStringA(msg);

        uint32_t startIndex = indexOffset;

        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            Vertex v;

            v.Pos.x = mesh->mVertices[i].x;
            v.Pos.y = mesh->mVertices[i].y;
            v.Pos.z = mesh->mVertices[i].z;

            if (mesh->HasNormals())
            {
                v.Normal.x = mesh->mNormals[i].x;
                v.Normal.y = mesh->mNormals[i].y;
                v.Normal.z = mesh->mNormals[i].z;
            }

            if (mesh->HasTextureCoords(0))
            {
                v.TexC.x = mesh->mTextureCoords[0][i].x;
                v.TexC.y = mesh->mTextureCoords[0][i].y;
            }

            model.Vertices.push_back(v);
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; ++j)
            {
                model.Indices.push_back(face.mIndices[j] + vertexOffset);
            }
        }

        std::string materialName = "default";
        if (mesh->mMaterialIndex >= 0)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString matName;
            if (material->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS)
            {
                materialName = matName.C_Str();
            }
        }

        SubmeshData submesh;
        submesh.MaterialName = materialName;
        submesh.Geometry.BaseVertexLocation = 0;
        submesh.Geometry.StartIndexLocation = startIndex;
        submesh.Geometry.IndexCount = mesh->mNumFaces * 3;

        model.Submeshes.push_back(submesh);

        vertexOffset += mesh->mNumVertices;
        indexOffset += mesh->mNumFaces * 3;
    }

    sprintf_s(msg, "\nTotal vertices: %zu, indices: %zu, submeshes: %zu\n",
        model.Vertices.size(), model.Indices.size(), model.Submeshes.size());
    OutputDebugStringA(msg);

    OutputDebugStringA("Creating MeshGeometry...\n");
    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = model.Name;

    const UINT vbByteSize = (UINT)model.Vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)model.Indices.size() * sizeof(uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), model.Vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), model.Indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), model.Vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), model.Indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    for (size_t i = 0; i < model.Submeshes.size(); ++i)
    {
        std::string submeshName = "submesh_" + std::to_string(i) + "_" + model.Submeshes[i].MaterialName;
        geo->DrawArgs[submeshName] = model.Submeshes[i].Geometry;
    }

    mGeometries[geo->Name] = std::move(geo);
    mLoadedModels.push_back(std::move(model));

    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Sponza geometry and textures prepared\n");
    OutputDebugStringA("========================================\n\n");
}

void CrateApp::LoadTreeLodMesh()
{
    mTreeLodMeshLoaded = false;
    mTreeMtlDiffuseSrvHeapIndex = 0;

    const char* tryPaths[] = {
        "../Models/tree-branched/tree-branched.obj",
        "../Models/tree-branched.obj"
    };

    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    const char* usedPath = nullptr;

    for (const char* path : tryPaths)
    {
        scene = importer.ReadFile(path,
            aiProcess_Triangulate |
            aiProcess_FlipUVs |
            aiProcess_GenNormals |
            aiProcess_JoinIdenticalVertices);

        if (scene && scene->mRootNode && (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) == 0)
        {
            usedPath = path;
            break;
        }
    }

    if (!scene || !usedPath)
    {
        OutputDebugStringA(
            "Tree LOD: ../Models/tree/tree.obj (или ../Models/tree.obj) не найден — LOD0: только крест и билборд.\n");
        return;
    }

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t vertexOffset = 0;

    for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
    {
        aiMesh* mesh = scene->mMeshes[m];

        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            Vertex v{};
            v.Pos.x = mesh->mVertices[i].x;
            v.Pos.y = mesh->mVertices[i].y;
            v.Pos.z = mesh->mVertices[i].z;
            if (mesh->HasNormals())
            {
                v.Normal.x = mesh->mNormals[i].x;
                v.Normal.y = mesh->mNormals[i].y;
                v.Normal.z = mesh->mNormals[i].z;
            }
            if (mesh->HasTextureCoords(0))
            {
                v.TexC.x = mesh->mTextureCoords[0][i].x;
                v.TexC.y = mesh->mTextureCoords[0][i].y;
            }
            vertices.push_back(v);
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            const aiFace& face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; ++j)
                indices.push_back(face.mIndices[j] + vertexOffset);
        }

        vertexOffset += mesh->mNumVertices;
    }

    if (vertices.empty() || indices.empty())
    {
        OutputDebugStringA("Tree LOD: пустая геометрия.\n");
        return;
    }

    float minY = vertices[0].Pos.y;
    float maxY = vertices[0].Pos.y;
    for (const Vertex& v : vertices)
    {
        minY = (std::min)(minY, v.Pos.y);
        maxY = (std::max)(maxY, v.Pos.y);
    }
    for (Vertex& v : vertices)
        v.Pos.y -= minY;

    const float h = (std::max)(maxY - minY, 1e-4f);
    const float targetHeight = 2.35f;
    const float s = targetHeight / h;
    for (Vertex& v : vertices)
    {
        v.Pos.x *= s;
        v.Pos.y *= s;
        v.Pos.z *= s;
    }

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "TreeLodMesh";

    const UINT vbByteSize = (UINT)(vertices.size() * sizeof(Vertex));
    const UINT ibByteSize = (UINT)(indices.size() * sizeof(uint32_t));

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);
    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);
    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh{};
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    geo->DrawArgs["treeMesh"] = submesh;

    mGeometries[geo->Name] = std::move(geo);
    mTreeLodMeshLoaded = true;

    char msg[280];
    sprintf_s(msg, "Tree LOD: загружен %s, вершин=%zu, индексов=%zu\n", usedPath, vertices.size(), indices.size());
    OutputDebugStringA(msg);

    std::string objPathStr(usedPath);
    size_t lastSlash = objPathStr.find_last_of("/\\");
    const std::string baseDir = (lastSlash != std::string::npos) ? objPathStr.substr(0, lastSlash + 1) : std::string();

    std::string diffuseRel;
    for (unsigned int mi = 0; mi < scene->mNumMeshes; ++mi)
    {
        aiMesh* mesh = scene->mMeshes[mi];
        if (mesh->mMaterialIndex < 0)
            continue;
        aiMaterial* amat = scene->mMaterials[mesh->mMaterialIndex];
        aiString tp;
        if (amat->GetTexture(aiTextureType_DIFFUSE, 0, &tp) == AI_SUCCESS ||
            amat->GetTexture(aiTextureType_BASE_COLOR, 0, &tp) == AI_SUCCESS)
        {
            diffuseRel = tp.C_Str();
            break;
        }
    }

    auto allocNextTreeHeap = [&]() -> int {
        int nh = 2;
        for (const auto& kv : mTextureCache)
            nh = (std::max)(nh, kv.second + 1);
        nh = (std::max)(nh, (int)kTreeInstanceBillboardSrvHeapIndex + 1);
        return nh;
    };

    auto tryLoadTreeDiffusePath = [&](const std::string& fullTexPath, const char* reasonTag) -> bool {
        const std::string resolved = ResolveMediaPath(fullTexPath);
        auto cached = mTextureCache.find(resolved);
        if (cached != mTextureCache.end())
        {
            mTreeMtlDiffuseSrvHeapIndex = static_cast<UINT>(cached->second);
            sprintf_s(msg, "Tree LOD: %s (кэш): %s heap %u\n", reasonTag, resolved.c_str(), mTreeMtlDiffuseSrvHeapIndex);
            OutputDebugStringA(msg);
            return true;
        }

        const int nextHeap = allocNextTreeHeap();
        mTextures.erase("treeMtlDiffuse");
        LoadModelTexture(resolved, "treeMtlDiffuse", nextHeap);

        auto it = mTextures.find("treeMtlDiffuse");
        if (it != mTextures.end() && it->second && it->second->Resource)
        {
            mTextureCache[resolved] = nextHeap;
            mTreeMtlDiffuseSrvHeapIndex = static_cast<UINT>(nextHeap);
            sprintf_s(msg, "Tree LOD: %s: %s heap %u\n", reasonTag, resolved.c_str(), mTreeMtlDiffuseSrvHeapIndex);
            OutputDebugStringA(msg);
            return true;
        }

        sprintf_s(msg, "Tree LOD: не загрузилось: %s\n", resolved.c_str());
        OutputDebugStringA(msg);
        return false;
    };

    if (!diffuseRel.empty())
    {
        std::replace(diffuseRel.begin(), diffuseRel.end(), '\\', '/');
        std::string fullTexPath;
        if (!diffuseRel.empty() && diffuseRel[0] != '/' && diffuseRel.find(':') == std::string::npos)
            fullTexPath = baseDir + diffuseRel;
        else
            fullTexPath = diffuseRel;

        tryLoadTreeDiffusePath(fullTexPath, "diffuse из MTL");
    }
    else
        OutputDebugStringA("Tree LOD: в материалах OBJ нет пути к diffuse (проверьте mtllib / map_Kd).\n");

    if (mTreeMtlDiffuseSrvHeapIndex == 0)
    {
        static const char* kFolderGuess[] = {
            "bark.png",
            "bark.jpg",
            "leaves.png",
            "leaves.jpg",
            "tree.png",
            "tree.jpg",
            "tree.jpeg",
            "tree.dds",
            "tree.tga",
            "diffuse.png",
            "albedo.png",
            "texture.png",
            "color.png",
        };
        for (const char* guess : kFolderGuess)
        {
            const std::string fullTexPath = baseDir + guess;
            if (tryLoadTreeDiffusePath(fullTexPath, "diffuse рядом с OBJ"))
                break;
        }
    }

    if (mTreeMtlDiffuseSrvHeapIndex == 0)
        OutputDebugStringA("Tree LOD: нет текстуры для меша — для LOD0 используется та же, что у билборда (../Textures/tree.*).\n");
}

void CrateApp::LoadTextures()
{
    OutputDebugStringA("Loading wood crate texture...\n");
    auto woodCrateTex = std::make_unique<Texture>();
    woodCrateTex->Name = "woodCrateTex";
    woodCrateTex->Filename = L"../Textures/WoodCrate01.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
        mCommandList.Get(), woodCrateTex->Filename.c_str(),
        woodCrateTex->Resource, woodCrateTex->UploadHeap));

    mTextures[woodCrateTex->Name] = std::move(woodCrateTex);

    auto checkerTex = std::make_unique<Texture>();
    checkerTex->Name = "checkerTex";
    checkerTex->Filename = L"../Textures/checkboard.dds";
    ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(md3dDevice.Get(),
        mCommandList.Get(), checkerTex->Filename.c_str(),
        checkerTex->Resource, checkerTex->UploadHeap));
    mTextures[checkerTex->Name] = std::move(checkerTex);
    OutputDebugStringA("Wood crate texture loaded\n");
}

void CrateApp::BuildRootSignature()
{
    OutputDebugStringA("Building root signature...\n");
    CD3DX12_DESCRIPTOR_RANGE texTable;
    texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER slotRootParameter[4];

    slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
    slotRootParameter[1].InitAsConstantBufferView(0);
    slotRootParameter[2].InitAsConstantBufferView(1);
    slotRootParameter[3].InitAsConstantBufferView(2);

    auto staticSamplers = GetStaticSamplers();

    CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
        (UINT)staticSamplers.size(), staticSamplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serializedRootSig = nullptr;
    ComPtr<ID3DBlob> errorBlob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

    if (errorBlob != nullptr)
    {
        ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
    }
    ThrowIfFailed(hr);

    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        serializedRootSig->GetBufferPointer(),
        serializedRootSig->GetBufferSize(),
        IID_PPV_ARGS(mRootSignature.GetAddressOf())));
    OutputDebugStringA("Root signature built\n");
}

void CrateApp::BuildDescriptorHeaps()
{
    OutputDebugStringA("Building descriptor heap...\n");
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 256;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    auto woodCrateTex = mTextures["woodCrateTex"]->Resource;
    auto checkerTex = mTextures["checkerTex"]->Resource;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = woodCrateTex->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = woodCrateTex->GetDesc().MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    md3dDevice->CreateShaderResourceView(woodCrateTex.Get(), &srvDesc, hDescriptor);

    hDescriptor.Offset(1, mCbvSrvDescriptorSize);
    srvDesc.Format = checkerTex->GetDesc().Format;
    srvDesc.Texture2D.MipLevels = checkerTex->GetDesc().MipLevels;
    md3dDevice->CreateShaderResourceView(checkerTex.Get(), &srvDesc, hDescriptor);
    OutputDebugStringA("Descriptor heap built\n");
}

void CrateApp::BuildShadersAndInputLayout()
{
    OutputDebugStringA("Compiling shaders...\n");
    mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
    mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");
    OutputDebugStringA("Shaders compiled\n");

    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void CrateApp::BuildPSOs()
{
    OutputDebugStringA("Building PSO...\n");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc = {};

    ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
    opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
    opaquePsoDesc.pRootSignature = mRootSignature.Get();
    opaquePsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()),
        mShaders["standardVS"]->GetBufferSize()
    };
    opaquePsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
        mShaders["opaquePS"]->GetBufferSize()
    };
    opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    opaquePsoDesc.SampleMask = UINT_MAX;
    opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    opaquePsoDesc.NumRenderTargets = 1;
    opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
    opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
    opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
    opaquePsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mOpaquePSO)));
    OutputDebugStringA("PSO built\n");
}

void CrateApp::BuildFrameResources()
{
    OutputDebugStringA("Building frame resources...\n");
    mBillboardObjectCbIndex = (UINT)mAllRitems.size();
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size() + 1u, (UINT)mMaterials.size()));
    }
    OutputDebugStringA("Frame resources built\n");
}

void CrateApp::BuildMaterials()
{
    OutputDebugStringA("Building materials...\n");

    auto woodCrate = std::make_unique<Material>();
    woodCrate->Name = "woodCrate";
    woodCrate->MatCBIndex = 0;
    woodCrate->DiffuseSrvHeapIndex = 0;
    woodCrate->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    woodCrate->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
    woodCrate->Roughness = 0.2f;
    woodCrate->TessellationParams = XMFLOAT4(0.0f, 0.0f, 80.0f, 0.0f);
    mMaterials["woodCrate"] = std::move(woodCrate);

    for (const auto& entry : mMaterialToHeapIndex)
    {
        const std::string& matName = entry.first;
        int heapIndex = entry.second;

        auto material = std::make_unique<Material>();
        material->Name = matName;
        material->MatCBIndex = (int)mMaterials.size();
        if (matName == "floor")
        {
            material->DiffuseSrvHeapIndex = heapIndex;
            material->DiffuseSrvHeapIndex2 = 1;
            auto bumpIt = mMaterialToBumpHeapIndex.find(matName);
            material->NormalSrvHeapIndex = bumpIt != mMaterialToBumpHeapIndex.end() ? bumpIt->second : heapIndex;
            material->ChessboardParams = XMFLOAT4(6.0f, 6.0f, 1.0f, 0.0f);
        }
        else
        {
            material->DiffuseSrvHeapIndex = heapIndex;
            auto bumpIt = mMaterialToBumpHeapIndex.find(matName);
            material->NormalSrvHeapIndex = bumpIt != mMaterialToBumpHeapIndex.end() ? bumpIt->second : heapIndex;
            material->DiffuseSrvHeapIndex2 = -1;
            material->ChessboardParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        material->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        material->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
        material->Roughness = 0.2f;
        if (matName.find("floor") != std::string::npos ||
            matName.find("bricks") != std::string::npos ||
            matName.find("lion") != std::string::npos)
        {
            material->TessellationParams = XMFLOAT4(0.12f, -0.06f, 90.0f, 1.0f);
        }
        else
        {
            material->TessellationParams = XMFLOAT4(0.0f, 0.0f, 80.0f, 0.0f);
        }

        mMaterials[matName] = std::move(material);

        char msg[256];
        sprintf_s(msg, "Created material %s diffuse %d\n", matName.c_str(), heapIndex);
        OutputDebugStringA(msg);
    }

    auto water = std::make_unique<Material>();
    water->Name = "water";
    water->MatCBIndex = (int)mMaterials.size();
    water->DiffuseSrvHeapIndex = 0;
    water->DiffuseSrvHeapIndex2 = 0;
    water->NormalSrvHeapIndex = 0;
    water->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    water->FresnelR0 = XMFLOAT3(0.06f, 0.1f, 0.14f);
    water->Roughness = 0.04f;
    water->TessellationParams = XMFLOAT4(0.35f, 0.0f, 140.0f, 1.0f);
    water->ChessboardParams = XMFLOAT4(1.2f, 1.2f, 0.0f, 1.0f);
    mMaterials["water"] = std::move(water);

    const UINT treeSrv = mBillboardTreeSrvHeapIndex != 0 ? mBillboardTreeSrvHeapIndex : 0u;

    auto billboardTree = std::make_unique<Material>();
    billboardTree->Name = "billboardTree";
    billboardTree->MatCBIndex = (int)mMaterials.size();
    billboardTree->DiffuseSrvHeapIndex = treeSrv;
    billboardTree->DiffuseSrvHeapIndex2 = treeSrv;
    billboardTree->NormalSrvHeapIndex = treeSrv;
    billboardTree->DiffuseAlbedo =
        mBillboardTreeSrvHeapIndex != 0 ? XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f) : XMFLOAT4(0.85f, 0.95f, 0.78f, 1.0f);
    billboardTree->FresnelR0 = XMFLOAT3(0.04f, 0.06f, 0.04f);
    billboardTree->Roughness = 0.55f;
    billboardTree->TessellationParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    billboardTree->ChessboardParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    mMaterials["billboardTree"] = std::move(billboardTree);

    OutputDebugStringA("Materials built\n");
}

void CrateApp::BuildRenderItems()
{
    OutputDebugStringA("\n========================================\n");
    OutputDebugStringA("Building render items...\n");
    OutputDebugStringA("========================================\n");

    auto initTexTransforms = [](RenderItem* ri)
    {
        XMMATRIX scale = XMMatrixScaling(ri->TextureScaleU, ri->TextureScaleV, 1.0f);
        XMMATRIX translation = XMMatrixTranslation(ri->TextureOffsetU, ri->TextureOffsetV, 0.0f);
        XMMATRIX texTransform = scale * translation;
        XMStoreFloat4x4(&ri->TexTransform, texTransform);
        if (ri->AnimateTexture)
            XMStoreFloat4x4(&ri->TexTransformDisp, scale);
        else
            XMStoreFloat4x4(&ri->TexTransformDisp, texTransform);
        ri->NumFramesDirty = gNumFrameResources;
    };

    auto geo = mGeometries["Sponza"].get();
    if (!geo)
    {
        OutputDebugStringA("ERROR: Sponza geometry not found!\n");
        return;
    }

    const LoadedModel* model = nullptr;
    for (const auto& m : mLoadedModels)
    {
        if (m.Name == "Sponza")
        {
            model = &m;
            break;
        }
    }
    if (!model)
    {
        OutputDebugStringA("ERROR: LoadedModel not found!\n");
        return;
    }

    int objIndex = 0;
    char msg[256];

    for (size_t i = 0; i < model->Submeshes.size(); ++i)
    {
        const auto& submesh = model->Submeshes[i];
        std::string submeshName = "submesh_" + std::to_string(i) + "_" + submesh.MaterialName;

        auto drawArg = geo->DrawArgs.find(submeshName);
        if (drawArg == geo->DrawArgs.end())
        {
            sprintf_s(msg, "WARNING: Submesh %s not found in DrawArgs!\n", submeshName.c_str());
            OutputDebugStringA(msg);
            continue;
        }

        auto renderItem = std::make_unique<RenderItem>();
        renderItem->ObjCBIndex = objIndex++;

        auto matIt = mMaterials.find(submesh.MaterialName);
        if (matIt != mMaterials.end())
        {
            renderItem->Mat = matIt->second.get();
        }
        else
        {
            renderItem->Mat = mMaterials["woodCrate"].get();
            sprintf_s(msg, "WARNING: Material %s not found, using woodCrate\n", submesh.MaterialName.c_str());
            OutputDebugStringA(msg);
        }

        renderItem->Geo = geo;
        renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        renderItem->IndexCount = drawArg->second.IndexCount;
        renderItem->StartIndexLocation = drawArg->second.StartIndexLocation;
        renderItem->BaseVertexLocation = drawArg->second.BaseVertexLocation;

        XMMATRIX scale = XMMatrixScaling(0.02f, 0.02f, 0.02f);
        XMMATRIX translation = XMMatrixTranslation(0.0f, -1.0f, 0.0f);
        XMStoreFloat4x4(&renderItem->World, scale * translation);

        renderItem->AnimateTexture = true;
        renderItem->AnimationSpeed = XMFLOAT2(0.1f, 0.05f);
        renderItem->TextureScaleU = 2.0f;
        renderItem->TextureScaleV = 2.0f;

        initTexTransforms(renderItem.get());
        mAllRitems.push_back(std::move(renderItem));
    }

    BuildStressTestObjects(objIndex);

    {
        auto waterGeo = mGeometries["WaterPlane"].get();
        auto waterMatIt = mMaterials.find("water");
        if (waterGeo && waterMatIt != mMaterials.end())
        {
            auto waterRi = std::make_unique<RenderItem>();
            waterRi->ObjCBIndex = objIndex++;
            waterRi->Mat = waterMatIt->second.get();
            waterRi->Geo = waterGeo;
            waterRi->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            auto drawArg = waterGeo->DrawArgs.find("water");
            if (drawArg != waterGeo->DrawArgs.end())
            {
                waterRi->IndexCount = drawArg->second.IndexCount;
                waterRi->StartIndexLocation = drawArg->second.StartIndexLocation;
                waterRi->BaseVertexLocation = drawArg->second.BaseVertexLocation;
            }
            XMMATRIX scale = XMMatrixScaling(1.0f, 1.0f, 1.0f);
            XMMATRIX translation = XMMatrixTranslation(0.0f, -2.5f, 0.0f);
            XMStoreFloat4x4(&waterRi->World, scale * translation);
            waterRi->AnimateTexture = false;
            waterRi->TextureScaleU = 1.0f;
            waterRi->TextureScaleV = 1.0f;
            initTexTransforms(waterRi.get());
            mAllRitems.push_back(std::move(waterRi));
        }
    }

    mSponzaOpaqueRitems.clear();
    mStressRitems.clear();
    mWaterRitems.clear();
    for (auto& e : mAllRitems)
    {
        if (!e->Mat)
            continue;
        if (e->Mat->Name == "water")
            mWaterRitems.push_back(e.get());
        else if (e->IsStressObject)
            mStressRitems.push_back(e.get());
        else
            mSponzaOpaqueRitems.push_back(e.get());
    }

    sprintf_s(msg, "Total render items: %zu\n", mAllRitems.size());
    OutputDebugStringA(msg);
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Render items built\n");
    OutputDebugStringA("========================================\n\n");
}

void CrateApp::BuildStressTestObjects(int& objIndex)
{
    auto boxGeoIt = mGeometries.find("BoxGeo");
    auto matIt = mMaterials.find("woodCrate");
    if (boxGeoIt == mGeometries.end() || matIt == mMaterials.end())
    {
        OutputDebugStringA("BuildStressTestObjects: BoxGeo or woodCrate missing\n");
        return;
    }

    MeshGeometry* boxGeo = boxGeoIt->second.get();
    Material* mat = matIt->second.get();
    auto drawArg = boxGeo->DrawArgs.find("box");
    if (drawArg == boxGeo->DrawArgs.end())
        return;

    constexpr int kStressTestCount = 5000;
    mStressWorldBounds.clear();
    mStressWorldBounds.reserve(kStressTestCount);

    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> distX(-38.0f, 38.0f);
    std::uniform_real_distribution<float> distZ(-38.0f, 38.0f);
    std::uniform_real_distribution<float> distY(0.35f, 8.0f);

    BoundingBox unitBox;
    BoundingBox::CreateFromPoints(
        unitBox,
        XMVectorSet(-0.5f, -0.5f, -0.5f, 1.0f),
        XMVectorSet(0.5f, 0.5f, 0.5f, 1.0f));

    auto initTexTransforms = [](RenderItem* ri) {
        XMMATRIX scale = XMMatrixScaling(ri->TextureScaleU, ri->TextureScaleV, 1.0f);
        XMMATRIX translation = XMMatrixTranslation(ri->TextureOffsetU, ri->TextureOffsetV, 0.0f);
        XMMATRIX texTransform = scale * translation;
        XMStoreFloat4x4(&ri->TexTransform, texTransform);
        XMStoreFloat4x4(&ri->TexTransformDisp, texTransform);
        ri->NumFramesDirty = gNumFrameResources;
    };

    const float boxScale = 0.32f;

    for (int i = 0; i < kStressTestCount; ++i)
    {
        const float x = distX(rng);
        const float y = distY(rng);
        const float z = distZ(rng);
        const XMMATRIX world = XMMatrixScaling(boxScale, boxScale, boxScale) * XMMatrixTranslation(x, y, z);

        XMFLOAT4X4 worldStore;
        XMStoreFloat4x4(&worldStore, world);

        BoundingBox worldBB;
        unitBox.Transform(worldBB, world);

        auto ri = std::make_unique<RenderItem>();
        ri->ObjCBIndex = objIndex++;
        ri->Mat = mat;
        ri->Geo = boxGeo;
        ri->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        ri->IndexCount = drawArg->second.IndexCount;
        ri->StartIndexLocation = drawArg->second.StartIndexLocation;
        ri->BaseVertexLocation = drawArg->second.BaseVertexLocation;
        ri->World = worldStore;
        ri->IsStressObject = true;
        ri->AnimateTexture = false;
        ri->TextureScaleU = 1.0f;
        ri->TextureScaleV = 1.0f;
        initTexTransforms(ri.get());

        mStressWorldBounds.push_back(worldBB);
        mAllRitems.push_back(std::move(ri));
    }

    if (mStressWorldBounds.empty())
        return;

    mKdTree.Build(mStressWorldBounds);

    char msg[256];
    sprintf_s(msg, "Stress test: %d boxes, kd-tree built\n", kStressTestCount);
    OutputDebugStringA(msg);
}

void CrateApp::UpdateStressVisibility()
{
    mStressVisibleRitems.clear();
    if (mStressRitems.empty())
        return;

    if (!mFrustumCullEnabled)
    {
        mStressVisibleRitems = mStressRitems;
        return;
    }

    const XMMATRIX view = XMLoadFloat4x4(&mView);
    const XMMATRIX proj = XMLoadFloat4x4(&mProj);

    BoundingFrustum fr;
    BoundingFrustum::CreateFromMatrix(fr, proj);

    if (!mKdTreeCullingEnabled)
    {
        mStressVisibleRitems.reserve(mStressRitems.size());
        for (size_t i = 0; i < mStressRitems.size(); ++i)
        {
            if (FrustumContainsOrIntersectsAABB(fr, view, mStressWorldBounds[i]))
                mStressVisibleRitems.push_back(mStressRitems[i]);
        }
        return;
    }

    std::vector<int> visibleIds;
    mKdTree.QueryVisible(mStressWorldBounds, fr, view, visibleIds);
    mStressVisibleRitems.reserve(visibleIds.size());
    for (int id : visibleIds)
    {
        if (id >= 0 && static_cast<size_t>(id) < mStressRitems.size())
            mStressVisibleRitems.push_back(mStressRitems[static_cast<size_t>(id)]);
    }
}

void CrateApp::UpdateForestLod()
{
    if (mForestInstancesCpu.empty() || !mForestMeshMapped || !mForestBillboardMapped)
        return;

    const XMVECTOR eye = XMLoadFloat3(&mEyePos);
    const float meshSq = kForestLodMeshDistance * kForestLodMeshDistance;

    TreeInstanceGpu meshBuf[kMaxForestInstances];
    TreeInstanceGpu billBuf[kMaxForestInstances];
    UINT nMesh = 0;
    UINT nBill = 0;

    for (const TreeInstanceGpu& t : mForestInstancesCpu)
    {
        XMVECTOR p = XMLoadFloat3(&t.WorldPos);
        const float d2 = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(p, eye)));

        if (mTreeLodMeshLoaded && d2 < meshSq)
        {
            if (nMesh < kMaxForestInstances)
                meshBuf[nMesh++] = t;
        }
        else
        {
            if (nBill < kMaxForestInstances)
                billBuf[nBill++] = t;
        }
    }

    mForestMeshCount = nMesh;
    mForestBillboardCount = nBill;

    if (nMesh > 0)
        std::memcpy(mForestMeshMapped, meshBuf, (size_t)nMesh * sizeof(TreeInstanceGpu));
    if (nBill > 0)
        std::memcpy(mForestBillboardMapped, billBuf, (size_t)nBill * sizeof(TreeInstanceGpu));
}

void CrateApp::DrawBillboardForest(ID3D12GraphicsCommandList* cmdList)
{
    if (mBillboardForestInstanceCount == 0 || !mRenderingSystem)
        return;
    if (mForestMeshCount == 0 && mForestBillboardCount == 0)
        return;

    auto matIt = mMaterials.find("billboardTree");
    if (matIt == mMaterials.end())
        return;

    Material* mat = matIt->second.get();

    const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    const UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();
    auto passCB = mCurrFrameResource->PassCB->Resource();

    cmdList->SetGraphicsRootSignature(mRenderingSystem->GetBillboardRootSignature());

    CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    tex.Offset(mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE nrm(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    nrm.Offset(mat->NormalSrvHeapIndex >= 0 ? mat->NormalSrvHeapIndex : mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE texB(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    const int altIdx = mat->DiffuseSrvHeapIndex2 >= 0 ? mat->DiffuseSrvHeapIndex2 : mat->DiffuseSrvHeapIndex;
    texB.Offset(altIdx, mCbvSrvDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE checkerTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    checkerTex.Offset(1, mCbvSrvDescriptorSize);

    cmdList->SetGraphicsRootDescriptorTable(0, tex);
    cmdList->SetGraphicsRootConstantBufferView(1, objectCB->GetGPUVirtualAddress() + (UINT64)mBillboardObjectCbIndex * objCBByteSize);
    cmdList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(3, matCB->GetGPUVirtualAddress() + (UINT64)mat->MatCBIndex * matCBByteSize);
    cmdList->SetGraphicsRootDescriptorTable(4, checkerTex);
    cmdList->SetGraphicsRootDescriptorTable(5, texB);

    if (mForestBillboardCount > 0)
    {
        auto geoIt = mGeometries.find("BillboardQuad");
        if (geoIt != mGeometries.end())
        {
            MeshGeometry* geo = geoIt->second.get();
            auto drawArg = geo->DrawArgs.find("tree");
            if (drawArg != geo->DrawArgs.end())
            {
                CD3DX12_GPU_DESCRIPTOR_HANDLE inst(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                inst.Offset(kTreeInstanceBillboardSrvHeapIndex, mCbvSrvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(6, inst);

                cmdList->SetPipelineState(mRenderingSystem->GetBillboardTreePSO());
                auto vbv = geo->VertexBufferView();
                auto ibv = geo->IndexBufferView();
                cmdList->IASetVertexBuffers(0, 1, &vbv);
                cmdList->IASetIndexBuffer(&ibv);
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                cmdList->DrawIndexedInstanced(
                    drawArg->second.IndexCount,
                    mForestBillboardCount,
                    drawArg->second.StartIndexLocation,
                    drawArg->second.BaseVertexLocation,
                    0);
            }
        }
    }

    if (mForestMeshCount > 0 && mTreeLodMeshLoaded)
    {
        auto meshIt = mGeometries.find("TreeLodMesh");
        if (meshIt != mGeometries.end())
        {
            MeshGeometry* treeGeo = meshIt->second.get();
            auto meshArg = treeGeo->DrawArgs.find("treeMesh");
            if (meshArg != treeGeo->DrawArgs.end())
            {
                if (mTreeMtlDiffuseSrvHeapIndex != 0)
                {
                    CD3DX12_GPU_DESCRIPTOR_HANDLE texMtl(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                    texMtl.Offset(mTreeMtlDiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
                    cmdList->SetGraphicsRootDescriptorTable(0, texMtl);
                    cmdList->SetGraphicsRootDescriptorTable(5, texMtl);
                }

                CD3DX12_GPU_DESCRIPTOR_HANDLE inst(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                inst.Offset(kTreeInstanceMeshSrvHeapIndex, mCbvSrvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(6, inst);

                cmdList->SetPipelineState(mRenderingSystem->GetTreeMeshInstancedPSO());
                auto vbv = treeGeo->VertexBufferView();
                auto ibv = treeGeo->IndexBufferView();
                cmdList->IASetVertexBuffers(0, 1, &vbv);
                cmdList->IASetIndexBuffer(&ibv);
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                cmdList->DrawIndexedInstanced(
                    meshArg->second.IndexCount,
                    mForestMeshCount,
                    meshArg->second.StartIndexLocation,
                    meshArg->second.BaseVertexLocation,
                    0);
            }
        }
    }
}

void CrateApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    for (size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        auto vbv = ri->Geo->VertexBufferView();
        auto ibv = ri->Geo->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        CD3DX12_GPU_DESCRIPTOR_HANDLE tex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        tex.Offset(ri->Mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE nrm(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        nrm.Offset(ri->Mat->NormalSrvHeapIndex >= 0 ? ri->Mat->NormalSrvHeapIndex : ri->Mat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE texB(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        const int altIdx = ri->Mat->DiffuseSrvHeapIndex2 >= 0 ? ri->Mat->DiffuseSrvHeapIndex2 : ri->Mat->DiffuseSrvHeapIndex;
        texB.Offset(altIdx, mCbvSrvDescriptorSize);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootDescriptorTable(0, tex);
        cmdList->SetGraphicsRootDescriptorTable(4, nrm);
        cmdList->SetGraphicsRootDescriptorTable(5, texB);
        cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void CrateApp::UpdateCamera(const GameTimer& gt)
{
    mEyePos.x = mRadius * sinf(mPhi) * cosf(mTheta);
    mEyePos.z = mRadius * sinf(mPhi) * sinf(mTheta);
    mEyePos.y = mRadius * cosf(mPhi);

    XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
    XMVECTOR target = XMVectorZero();
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&mView, view);
}

void CrateApp::AnimateMaterials(const GameTimer& gt)
{
    for (auto& e : mAllRitems)
    {
        if (e->AnimateTexture)
        {
            e->TextureOffsetU += e->AnimationSpeed.x * gt.DeltaTime();
            e->TextureOffsetV += e->AnimationSpeed.y * gt.DeltaTime();

            e->TextureOffsetU = fmod(e->TextureOffsetU, 1.0f);
            if (e->TextureOffsetU < 0.0f) e->TextureOffsetU += 1.0f;
            e->TextureOffsetV = fmod(e->TextureOffsetV, 1.0f);
            if (e->TextureOffsetV < 0.0f) e->TextureOffsetV += 1.0f;

            XMMATRIX scale = XMMatrixScaling(e->TextureScaleU, e->TextureScaleV, 1.0f);
            XMMATRIX translation = XMMatrixTranslation(e->TextureOffsetU, e->TextureOffsetV, 0.0f);
            XMMATRIX texTransform = scale * translation;
            XMStoreFloat4x4(&e->TexTransform, texTransform);
            XMStoreFloat4x4(&e->TexTransformDisp, scale);
            e->NumFramesDirty = gNumFrameResources;
        }
    }
}

void CrateApp::UpdateObjectCBs(const GameTimer& gt)
{
    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        if (e->NumFramesDirty > 0)
        {
            XMMATRIX world = XMLoadFloat4x4(&e->World);
            XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);
            XMMATRIX texTransformDisp = XMLoadFloat4x4(&e->TexTransformDisp);

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
            XMStoreFloat4x4(&objConstants.TexTransformDisp, XMMatrixTranspose(texTransformDisp));

            currObjectCB->CopyData(e->ObjCBIndex, objConstants);
            e->NumFramesDirty--;
        }
    }

    {
        ObjectConstants billObj;
        const XMMATRIX I = XMMatrixIdentity();
        XMStoreFloat4x4(&billObj.World, XMMatrixTranspose(I));
        XMStoreFloat4x4(&billObj.TexTransform, XMMatrixTranspose(I));
        XMStoreFloat4x4(&billObj.TexTransformDisp, XMMatrixTranspose(I));
        currObjectCB->CopyData(mBillboardObjectCbIndex, billObj);
    }
}

void CrateApp::UpdateMaterialCBs(const GameTimer& gt)
{
    auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
    for (auto& e : mMaterials)
    {
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialConstants matConstants;
            matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
            matConstants.FresnelR0 = mat->FresnelR0;
            matConstants.Roughness = mat->Roughness;
            XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));
            matConstants.TessellationParams = mat->TessellationParams;
            matConstants.ChessboardParams = mat->ChessboardParams;

            currMaterialCB->CopyData(mat->MatCBIndex, matConstants);
            mat->NumFramesDirty--;
        }
    }
}

void CrateApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(XMMatrixInverse(nullptr, proj)));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(XMMatrixInverse(nullptr, viewProj)));

    mMainPassCB.EyePosW = mEyePos;
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 1000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();
    mMainPassCB.AmbientLight = { 0.25f, 0.25f, 0.35f, 1.0f };
    mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[0].Strength = { 0.6f, 0.6f, 0.6f };
    mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[1].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
    mMainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void CrateApp::UpdateDeferredLightCB()
{
    XMVECTOR eyePos = XMLoadFloat3(&mEyePos);
    XMVECTOR lookAt = XMVectorZero();
    XMVECTOR viewDir = XMVector3Normalize(lookAt - eyePos);

    mSpotLights[1].Position = mEyePos;
    XMStoreFloat3(&mSpotLights[1].Direction, viewDir);

    UINT activePointLights = 0;
    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        if (mFallingActive[i])
            ++activePointLights;
    }
    mActivePointLights = activePointLights;

    DeferredLightParams params = {};
    params.ActivePointLightCount = mActivePointLights;
    mCurrFrameResource->DeferredLightParamsCB->CopyData(0, params);

    UINT dst = 0;
    for (UINT i = 0; i < kDeferredDirectionalLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 0.0f;
        XMVECTOR dirW = XMLoadFloat3(&mDirectionalLights[i].Direction);
        XMVECTOR dirV = XMVector3Normalize(XMVector3TransformNormal(dirW, XMLoadFloat4x4(&mView)));
        XMStoreFloat3(&l.Direction, dirV);
        l.Strength = mDirectionalLights[i].Strength;
        mCurrFrameResource->DeferredLightBuffer->CopyData(dst, l);
    }
    for (UINT i = 0; i < kDeferredPointLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 1.0f;
        XMVECTOR posW = XMLoadFloat3(&mPointLights[i].Position);
        XMVECTOR posV = XMVector3TransformCoord(posW, XMLoadFloat4x4(&mView));
        XMStoreFloat3(&l.Position, posV);
        l.Strength = mPointLights[i].Strength;
        l.FalloffStart = mPointLights[i].FalloffStart;
        l.FalloffEnd = mPointLights[i].FalloffEnd;
        mCurrFrameResource->DeferredLightBuffer->CopyData(dst, l);
    }
    for (UINT i = 0; i < kDeferredSpotLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 2.0f;
        XMVECTOR posW = XMLoadFloat3(&mSpotLights[i].Position);
        XMVECTOR posV = XMVector3TransformCoord(posW, XMLoadFloat4x4(&mView));
        XMStoreFloat3(&l.Position, posV);

        XMVECTOR dirW = XMLoadFloat3(&mSpotLights[i].Direction);
        XMVECTOR dirV = XMVector3Normalize(XMVector3TransformNormal(dirW, XMLoadFloat4x4(&mView)));
        XMStoreFloat3(&l.Direction, dirV);
        l.Strength = mSpotLights[i].Strength;
        l.FalloffStart = mSpotLights[i].FalloffStart;
        l.FalloffEnd = mSpotLights[i].FalloffEnd;
        l.SpotPower = mSpotLights[i].SpotPower;
        mCurrFrameResource->DeferredLightBuffer->CopyData(dst, l);
    }
}

void CrateApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;
    SetCapture(mhMainWnd);
}

void CrateApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void CrateApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if ((btnState & MK_LBUTTON) != 0)
    {
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

        mTheta += dx;
        mPhi += dy;
        mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
    }
    else if ((btnState & MK_RBUTTON) != 0)
    {
        float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
        float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);

        mRadius += dx - dy;
        mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> CrateApp::GetStaticSamplers()
{
    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 8);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 8);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp
    };
}