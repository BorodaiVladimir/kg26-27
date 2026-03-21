#include "../Common/d3dApp.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "Lights.h"
#include "FrameResource.h"
#include "RenderingSystem.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <string>
#include <sstream>
#include <map>
#include <array>
#include <DirectXTex.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")
#pragma comment(lib, "DirectXTex.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

const int gNumFrameResources = 3;

struct RenderItem
{
    RenderItem() = default;

    XMFLOAT4X4 World = MathHelper::Identity4x4();
    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
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
    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void LoadOBJModels();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void CreateBoxGeometry();

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

    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*> mOpaqueRitems;

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
    mTheta = 1.3f * XM_PI;
    mPhi = 0.4f * XM_PI;
    mRadius = 8.0f;

    mDirectionalLights[0].Direction = { 0.45f, -0.72f, 0.52f };
    mDirectionalLights[0].Strength = { 0.20f, 0.35f, 1.40f };

    mPointLights[0].Position = { -14.0f, 7.0f, -4.0f };
    mPointLights[0].Strength = { 0.20f, 1.80f, 0.20f };
    mPointLights[0].FalloffStart = 2.0f;
    mPointLights[0].FalloffEnd = 24.0f;

    mPointLights[1].Position = { 14.0f, 7.0f, -4.0f };
    mPointLights[1].Strength = { 0.20f, 1.80f, 0.20f };
    mPointLights[1].FalloffStart = 2.0f;
    mPointLights[1].FalloffEnd = 24.0f;

    mPointLights[2].Position = { -10.0f, 6.0f, 16.0f };
    mPointLights[2].Strength = { 0.20f, 1.80f, 0.20f };
    mPointLights[2].FalloffStart = 2.0f;
    mPointLights[2].FalloffEnd = 22.0f;

    mPointLights[3].Position = { 10.0f, 6.0f, 16.0f };
    mPointLights[3].Strength = { 0.20f, 1.80f, 0.20f };
    mPointLights[3].FalloffStart = 2.0f;
    mPointLights[3].FalloffEnd = 22.0f;

    mSpotLights[0].Position = { 0.0f, 12.0f, -30.0f };
    mSpotLights[0].Direction = { 0.0f, -0.35f, 1.0f };
    mSpotLights[0].Strength = { 1.90f, 0.15f, 0.15f };
    mSpotLights[0].FalloffStart = 4.0f;
    mSpotLights[0].FalloffEnd = 65.0f;
    mSpotLights[0].SpotPower = 28.0f;

    mSpotLights[1].Position = { 0.0f, 0.0f, 0.0f };
    mSpotLights[1].Direction = { 0.0f, -0.4f, 1.0f };
    mSpotLights[1].Strength = { 1.70f, 0.12f, 0.12f };
    mSpotLights[1].FalloffStart = 1.5f;
    mSpotLights[1].FalloffEnd = 70.0f;
    mSpotLights[1].SpotPower = 34.0f;
}

CrateApp::~CrateApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();
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

    auto passCB = mCurrFrameResource->PassCB->Resource();
    auto passAddress = passCB->GetGPUVirtualAddress();

    mRenderingSystem->BeginGeometryPass(mCommandList.Get(), DepthStencilView(), passAddress);

    DrawRenderItems(mCommandList.Get(), mOpaqueRitems);

    mRenderingSystem->EndGeometryPass(mCommandList.Get());

    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &transition);
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);

    auto lightCB = mCurrFrameResource->DeferredLightCB->Resource();
    mRenderingSystem->ExecuteLightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        passAddress,
        lightCB->GetGPUVirtualAddress());

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

void CrateApp::LoadModelTexture(const std::string& texturePath, const std::string& texName, int heapIndex)
{
    char msg[256];
    sprintf_s(msg, "Loading texture: %s as %s at index %d\n", texturePath.c_str(), texName.c_str(), heapIndex);
    OutputDebugStringA(msg);

    std::wstring wTexturePath(texturePath.begin(), texturePath.end());

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
        hr = LoadFromWICFile(wTexturePath.c_str(), WIC_FLAGS_NONE, &metadata, image);
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

                sprintf_s(msg, "Texture loaded successfully: %s\n", texturePath.c_str());
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

void CrateApp::LoadOBJModels()
{
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Loading Sponza model...\n");
    OutputDebugStringA("========================================\n");

    Assimp::Importer importer;

    const aiScene* scene = importer.ReadFile("../Models/sponza/sponza.obj",
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
        CreateBoxGeometry();
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
            std::string baseDir = "../Models/sponza/";
            if (!path.empty() && path[0] != '/' && path[0] != '\\' && path.find(":") == std::string::npos)
            {
                path = baseDir + path;
            }
            std::replace(path.begin(), path.end(), '\\', '/');
            materialToTexture[name] = path;
        }
    }

    int nextHeapIndex = 1;
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
    srvHeapDesc.NumDescriptors = 50;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    auto woodCrateTex = mTextures["woodCrateTex"]->Resource;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = woodCrateTex->GetDesc().Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = woodCrateTex->GetDesc().MipLevels;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    md3dDevice->CreateShaderResourceView(woodCrateTex.Get(), &srvDesc, hDescriptor);
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
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), (UINT)mMaterials.size()));
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
    mMaterials["woodCrate"] = std::move(woodCrate);

    for (const auto& entry : mMaterialToHeapIndex)
    {
        const std::string& matName = entry.first;
        int heapIndex = entry.second;

        auto material = std::make_unique<Material>();
        material->Name = matName;
        material->MatCBIndex = (int)mMaterials.size();
        material->DiffuseSrvHeapIndex = heapIndex;
        material->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        material->FresnelR0 = XMFLOAT3(0.05f, 0.05f, 0.05f);
        material->Roughness = 0.2f;

        mMaterials[matName] = std::move(material);

        char msg[256];
        sprintf_s(msg, "Created material %s with texture index %d\n", matName.c_str(), heapIndex);
        OutputDebugStringA(msg);
    }

    OutputDebugStringA("Materials built\n");
}

void CrateApp::BuildRenderItems()
{
    OutputDebugStringA("\n========================================\n");
    OutputDebugStringA("Building render items...\n");
    OutputDebugStringA("========================================\n");

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
        renderItem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
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

        mAllRitems.push_back(std::move(renderItem));
    }

    for (auto& e : mAllRitems)
        mOpaqueRitems.push_back(e.get());

    sprintf_s(msg, "Total render items: %zu\n", mAllRitems.size());
    OutputDebugStringA(msg);
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Render items built\n");
    OutputDebugStringA("========================================\n\n");
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

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootDescriptorTable(0, tex);
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

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));

            currObjectCB->CopyData(e->ObjCBIndex, objConstants);
            e->NumFramesDirty--;
        }
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

    DeferredLightConstants lightConstants = {};
    for (UINT i = 0; i < kDeferredDirectionalLightCount; ++i)
    {
        lightConstants.DirectionalLights[i] = mDirectionalLights[i];
    }
    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        lightConstants.PointLights[i] = mPointLights[i];
    }
    for (UINT i = 0; i < kDeferredSpotLightCount; ++i)
    {
        lightConstants.SpotLights[i] = mSpotLights[i];
    }

    mCurrFrameResource->DeferredLightCB->CopyData(0, lightConstants);
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