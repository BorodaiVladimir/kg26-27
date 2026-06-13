#include "../Common/d3dUtil.h"
#include "FrameResource.h"
#include "../Common/d3dApp.h"
#include "../Common/MathHelper.h"
#include "../Common/UploadBuffer.h"
#include "../Common/GeometryGenerator.h"
#include "Lights.h"
#include "RenderingSystem.h"
#include "ShadowSystem.h"
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
static constexpr float kParticlesDisableCameraRadius = 34.0f;
static constexpr float kShadowDisableCameraRadius = 46.0f;
static constexpr UINT kPbrPointLightIndex = 0u;

int NextSceneTextureSrvIndex(const std::map<std::string, int>& cache, int forestInstanceSrvBase, int reservedHeapEnd)
{
    int index = 2;
    for (const auto& kv : cache)
        index = (std::max)(index, kv.second + 1);

    if (index >= forestInstanceSrvBase && index < reservedHeapEnd)
        index = reservedHeapEnd;
    return index;
}

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

bool TryLoadDdsTexture(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    const char* logicalPath,
    const std::string& texName,
    std::unordered_map<std::string, std::unique_ptr<Texture>>& textures)
{
    const std::string resolved = ResolveMediaPath(logicalPath);
    std::wstring wpath(resolved.begin(), resolved.end());

    auto tex = std::make_unique<Texture>();
    tex->Name = texName;
    tex->Filename = wpath;
    const HRESULT hr = DirectX::CreateDDSTextureFromFile12(
        device, cmdList, wpath.c_str(), tex->Resource, tex->UploadHeap);
    if (FAILED(hr))
        return false;

    textures[texName] = std::move(tex);
    return true;
}

static XMFLOAT3 IblCubeDirection(UINT face, float u, float v)
{
    const float uc = 2.0f * u - 1.0f;
    const float vc = 2.0f * v - 1.0f;
    switch (face)
    {
    case 0: return { 1.0f, -vc, -uc };
    case 1: return { -1.0f, -vc, uc };
    case 2: return { uc, 1.0f, vc };
    case 3: return { uc, -1.0f, -vc };
    case 4: return { uc, -vc, 1.0f };
    default: return { -uc, -vc, -1.0f };
    }
}

static XMFLOAT3 ProceduralSkyColor(const XMFLOAT3& dir)
{
    if (dir.y < -0.05f)
        return { 0.03f, 0.04f, 0.06f };

    const float t = powf(MathHelper::Clamp(dir.y, 0.0f, 1.0f), 0.35f);
    const XMFLOAT3 horizon = { 0.95f, 0.50f, 0.18f };
    const XMFLOAT3 zenith = { 0.10f, 0.35f, 0.95f };
    return {
        horizon.x + (zenith.x - horizon.x) * t,
        horizon.y + (zenith.y - horizon.y) * t,
        horizon.z + (zenith.z - horizon.z) * t
    };
}

static void FillCubeFaceSky(
    UINT face,
    UINT size,
    std::vector<uint8_t>& rgba)
{
    rgba.resize((size_t)size * size * 4);
    for (UINT y = 0; y < size; ++y)
    {
        for (UINT x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) / size;
            const float v = (y + 0.5f) / size;
            XMFLOAT3 dir = IblCubeDirection(face, u, v);
            XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&dir));
            XMStoreFloat3(&dir, n);
            const XMFLOAT3 c = ProceduralSkyColor(dir);
            const size_t i = ((size_t)y * size + x) * 4;
            rgba[i + 0] = (uint8_t)(MathHelper::Clamp(c.x, 0.0f, 1.0f) * 255.0f);
            rgba[i + 1] = (uint8_t)(MathHelper::Clamp(c.y, 0.0f, 1.0f) * 255.0f);
            rgba[i + 2] = (uint8_t)(MathHelper::Clamp(c.z, 0.0f, 1.0f) * 255.0f);
            rgba[i + 3] = 255;
        }
    }
}

static void DownsampleCubeFaces(
    const std::vector<std::vector<uint8_t>>& srcFaces,
    UINT srcSize,
    std::vector<std::vector<uint8_t>>& dstFaces,
    UINT dstSize)
{
    dstFaces.resize(6);
    for (UINT face = 0; face < 6; ++face)
    {
        dstFaces[face].resize((size_t)dstSize * dstSize * 4);
        for (UINT y = 0; y < dstSize; ++y)
        {
            for (UINT x = 0; x < dstSize; ++x)
            {
                float r = 0, g = 0, b = 0;
                for (UINT oy = 0; oy < 2; ++oy)
                {
                    for (UINT ox = 0; ox < 2; ++ox)
                    {
                        const UINT sx = x * 2 + ox;
                        const UINT sy = y * 2 + oy;
                        const size_t si = ((size_t)sy * srcSize + sx) * 4;
                        r += srcFaces[face][si + 0];
                        g += srcFaces[face][si + 1];
                        b += srcFaces[face][si + 2];
                    }
                }
                const size_t di = ((size_t)y * dstSize + x) * 4;
                dstFaces[face][di + 0] = (uint8_t)(r * 0.25f);
                dstFaces[face][di + 1] = (uint8_t)(g * 0.25f);
                dstFaces[face][di + 2] = (uint8_t)(b * 0.25f);
                dstFaces[face][di + 3] = 255;
            }
        }
    }
}

static XMFLOAT2 EnvBrdfApprox(float roughness, float nDotV)
{
    const float c0x = -1.0f, c0y = -0.0275f, c0z = -0.572f, c0w = 0.022f;
    const float c1x = 1.0f, c1y = 0.0425f, c1z = 1.04f, c1w = -0.04f;
    const float rx = roughness * c0x + c1x;
    const float ry = roughness * c0y + c1y;
    const float rz = roughness * c0z + c1z;
    const float rw = roughness * c0w + c1w;
    const float a004 = (std::min)(rx * rx, exp2f(-9.28f * nDotV)) * rx + ry;
    return { (-1.04f) * a004 + rz, 1.04f * a004 + rw };
}

static bool UploadTexture2D(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    const void* pixelData,
    UINT pixelStride,
    ComPtr<ID3D12Resource>& outTex,
    ComPtr<ID3D12Resource>& outUpload)
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const CD3DX12_HEAP_PROPERTIES defaultProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(
        &defaultProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex))))
        return false;

    const UINT64 uploadSize = GetRequiredIntermediateSize(outTex.Get(), 0, 1);
    const CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    if (FAILED(device->CreateCommittedResource(
        &uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUpload))))
        return false;

    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData = pixelData;
    sub.RowPitch = width * pixelStride;
    sub.SlicePitch = sub.RowPitch * height;
    UpdateSubresources(cmdList, outTex.Get(), outUpload.Get(), 0, 0, 1, &sub);
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        outTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    return true;
}

static bool UploadTextureCubeMipChain(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* cmdList,
    UINT baseSize,
    UINT mipLevels,
    const std::vector<std::vector<std::vector<uint8_t>>>& mipsFaces,
    ComPtr<ID3D12Resource>& outTex,
    ComPtr<ID3D12Resource>& outUpload)
{
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = baseSize;
    texDesc.Height = baseSize;
    texDesc.DepthOrArraySize = 6;
    texDesc.MipLevels = mipLevels;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    const CD3DX12_HEAP_PROPERTIES defaultProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(
        &defaultProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outTex))))
        return false;

    const UINT64 uploadSize = GetRequiredIntermediateSize(outTex.Get(), 0, mipLevels * 6);
    const CD3DX12_HEAP_PROPERTIES uploadProps(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    if (FAILED(device->CreateCommittedResource(
        &uploadProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&outUpload))))
        return false;

    std::vector<D3D12_SUBRESOURCE_DATA> subs;
    subs.resize(mipLevels * 6);
    for (UINT mip = 0; mip < mipLevels; ++mip)
    {
        const UINT size = (std::max)(1u, baseSize >> mip);
        for (UINT face = 0; face < 6; ++face)
        {
            const UINT idx = mip * 6 + face;
            subs[idx].pData = mipsFaces[mip][face].data();
            subs[idx].RowPitch = size * 4;
            subs[idx].SlicePitch = subs[idx].RowPitch * size;
        }
    }

    UpdateSubresources(cmdList, outTex.Get(), outUpload.Get(), 0, 0, (UINT)subs.size(), subs.data());
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        outTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &barrier);
    return true;
}

static std::string MakeAbsoluteTexturePath(const std::string& baseDir, const std::string& relPath)
{
    if (relPath.empty())
        return {};
    if (relPath[0] == '/' || relPath[0] == '\\' || relPath.find(':') != std::string::npos)
        return ResolveMediaPath(relPath);

    std::string path = baseDir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
        path += '/';
    path += relPath;
    std::replace(path.begin(), path.end(), '\\', '/');
    return ResolveMediaPath(path);
}

static bool TryGetAssimpTexture(
    aiMaterial* material,
    aiTextureType type,
    const std::string& baseDir,
    std::string& outPath)
{
    aiString texPath;
    if (material->GetTexture(type, 0, &texPath) != AI_SUCCESS)
        return false;

    outPath = MakeAbsoluteTexturePath(baseDir, texPath.C_Str());
    return !outPath.empty();
}

static bool TryResolvePbrFallbackPath(const std::string& baseDir, const char* filename, std::string& outPath)
{
    if (!filename || !filename[0])
        return false;

    const std::string candidates[] = {
        baseDir + filename,
        baseDir + "Textures/" + filename,
        baseDir + "textures/" + filename
    };

    for (const auto& candidate : candidates)
    {
        if (FileExistsA_Local(ResolveMediaPath(candidate).c_str()))
        {
            outPath = candidate;
            return true;
        }
    }
    return false;
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
    void UpdatePostProcessCB();
    void UpdateDeferredLightCB();

    void LoadTextures();
    void LoadIblTextures();
    void CreateProceduralIblTextures();
    void LoadBillboardTreeTexture();
    void CreateFallbackTreeCardTexture();
    bool TryLoadBillboardTreeCard(const std::string& resolvedPath, int heapIndex);
    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void LoadOBJModels();
    void LoadPbrModel();
    bool ImportPbrFbxModel(
        const char* const* tryPaths,
        size_t tryPathCount,
        const char* geometryName,
        const char* fallbackAlbedo,
        const char* fallbackNormal,
        const char* fallbackMetal,
        const char* fallbackRough);
    void AddPbrModelRenderItems(
        const char* geometryName,
        float worldX,
        float worldZ,
        float rotationY,
        float targetHeight,
        int& objIndex,
        XMFLOAT3* outCenter = nullptr);
    void LoadSlendermanModel();
    void LoadTreeLodMesh();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildRenderItems();
    void ComputeSponzaWorldBounds();
    void DrawRenderItems(
        ID3D12GraphicsCommandList* cmdList,
        const std::vector<RenderItem*>& ritems,
        size_t maxCount = SIZE_MAX);
    void DrawRenderItemsShadow(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void CreateBoxGeometry();
    void CreateWaterPlaneGeometry();
    void CreateBillboardForest();
    void UpdateForestLod(UINT frameIndex);
    void DrawBillboardForest(ID3D12GraphicsCommandList* cmdList);
    void BuildStressTestObjects(int& objIndex);
    void UpdateStressVisibility();
    void SetupPbrPointLight();
    void WaitForGpu();

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSamplers();

    bool LoadModelTexture(const std::string& texturePath, const std::string& texName, int heapIndex);

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
    std::unique_ptr<ShadowSystem> mShadowSystem;
    std::unique_ptr<ParticleSystem> mParticleSystem;

    std::vector<std::unique_ptr<RenderItem>> mAllRitems;
    std::vector<RenderItem*> mSponzaOpaqueRitems;
    bool mHasSponzaBounds = false;
    XMFLOAT3 mSponzaBoundsMin = {};
    XMFLOAT3 mSponzaBoundsMax = {};
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
    std::map<std::string, int> mMaterialToMetallicHeapIndex;
    std::map<std::string, int> mMaterialToRoughnessHeapIndex;
    std::map<std::string, XMFLOAT4> mSlendermanMaterialAlbedo;

    bool mHasPbrModel = false;
    bool mHasPbrCerberus = false;
    bool mHasPbrWoodRoot = false;
    XMFLOAT3 mPbrWorldCenter = { 0.0f, 0.0f, 0.0f };
    bool mIblTexturesLoaded = false;
    bool mEnableIbl = true;
    float mIblMaxReflectionLod = 4.0f;
    bool mF8KeyDown = false;
    bool mUseBeckmannDistribution = false;
    bool mF9KeyDown = false;
    bool mHasSlenderman = false;
    XMFLOAT3 mSlendermanWorldCenter = { 0.0f, 0.0f, 0.0f };
    static constexpr float kSlendermanVcrNearDist = 10.0f;
    static constexpr float kSlendermanVcrFarDist = 42.0f;
    static constexpr float kSlendermanVcrMin = 0.35f;
    static constexpr float kSlendermanVcrMax = 1.0f;

    PassConstants mMainPassCB;

    XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
    XMFLOAT3 mCameraTarget = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.3f * XM_PI;
    float mPhi = 0.4f * XM_PI;
    float mRadius = 8.0f;
    float mCameraMoveSpeed = 14.0f;

    POINT mLastMousePos;

    std::array<DirectionalLightSource, kDeferredDirectionalLightCount> mDirectionalLights;
    std::array<PointLightSource, kDeferredPointLightCount> mPointLights;
    std::array<SpotLightSource, kDeferredSpotLightCount> mSpotLights;

    float mSpawnAccumulator = 0.0f;
    std::array<float, kDeferredPointLightCount> mFallingVelY{};
    std::array<bool, kDeferredPointLightCount> mFallingActive{};
    UINT mActivePointLights = 0;

    bool mGeometryWireframe = false;
    std::array<D3D12_RESOURCE_STATES, SwapChainBufferCount> mBackBufferStates = {};
    bool mF3KeyDown = false;
    bool mEdgePostEnabled = true;
    bool mVcrPostEnabled = true;
    bool mF6KeyDown = false;
    bool mF7KeyDown = false;

    std::vector<TreeInstanceGpu> mForestInstancesCpu;
    ComPtr<ID3D12Resource> mForestMeshUpload[gNumFrameResources];
    ComPtr<ID3D12Resource> mForestBillboardUpload[gNumFrameResources];
    BYTE* mForestMeshMapped[gNumFrameResources] = {};
    BYTE* mForestBillboardMapped[gNumFrameResources] = {};
    UINT mForestMeshCount[gNumFrameResources] = {};
    UINT mForestBillboardCount[gNumFrameResources] = {};
    std::vector<uint8_t> mForestLodUsesMesh;
    bool mTreeLodMeshLoaded = false;
    UINT mGpuFrameIndex = 0;
    UINT mTreeMtlDiffuseSrvHeapIndex = 0;
    UINT mFallbackTreeSrvHeapIndex = 0;
    UINT mBillboardForestInstanceCount = 0;
    UINT mBillboardObjectCbIndex = 0;
    UINT mBillboardTreeSrvHeapIndex = 0;
    static constexpr UINT kMaxForestInstances = 512;
    static constexpr UINT kMaxPointLightsForShading = 256u;
    static constexpr float kForestLodMeshNear = 20.0f;
    static constexpr float kForestLodMeshFar = 34.0f;
    static constexpr UINT kForestInstanceSrvBaseIndex = 240;
    static constexpr UINT kForestInstanceSrvCount = 6;
    static constexpr UINT kParticleSrvHeapStartIndex = kForestInstanceSrvBaseIndex + kForestInstanceSrvCount;
    static constexpr UINT kParticleSrvDescriptorCount = 10;
    static constexpr UINT kReservedSrvHeapEnd = kParticleSrvHeapStartIndex + kParticleSrvDescriptorCount;
    static constexpr UINT kShadowSrvHeapStartIndex = 500;
    static constexpr UINT kSrvDescriptorHeapSize = 512;
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
        std::wstring msg = e.ToString();
        if (D3DApp* app = D3DApp::GetApp())
        {
            const HRESULT removed = app->GetDeviceRemovedReason();
            if (FAILED(removed))
            {
                wchar_t removedMsg[96];
                swprintf_s(removedMsg, L"\nGetDeviceRemovedReason: 0x%08X", static_cast<unsigned>(removed));
                msg += removedMsg;
            }
        }
        MessageBox(nullptr, msg.c_str(), L"HR Failed", MB_OK);
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

    // Direction = куда летят лучи света (мировое пространство): сверху-вниз в сцену.
    mDirectionalLights[0].Direction = { 0.25f, -0.90f, 0.35f };
    mDirectionalLights[0].Strength = { 0.55f, 0.75f, 2.2f };

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
    mSpotLights[1].Strength = { 2.8f, 2.6f, 2.4f };
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
        L"   stress draw: %zu / %zu   pbr wood:%s   F4 cull:%s F5 kd:%s   F6 edge:%s F7 vcr:%s F8 ibl:%s F9 ndf:%s",
        mStressVisibleRitems.size(),
        mStressRitems.size(),
        mHasPbrWoodRoot ? L"on" : L"off",
        mFrustumCullEnabled ? L"on" : L"off",
        mKdTreeCullingEnabled ? L"on" : L"off",
        mEdgePostEnabled ? L"on" : L"off",
        mVcrPostEnabled ? L"on" : L"off",
        mIblTexturesLoaded ? (mEnableIbl ? L"on" : L"off") : L"missing",
        mUseBeckmannDistribution ? L"beckmann" : L"ggx");
    return buf;
}

bool CrateApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    LoadTextures();
    LoadIblTextures();
    BuildDescriptorHeaps();
    LoadOBJModels();
    LoadPbrModel();
    LoadSlendermanModel();
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

    mShadowSystem = std::make_unique<ShadowSystem>();
    mShadowSystem->Initialize(
        md3dDevice.Get(),
        mSrvDescriptorHeap.Get(),
        kShadowSrvHeapStartIndex,
        mCbvSrvDescriptorSize);
    mRenderingSystem->SetLightingResources(
        mShadowSystem->GetShadowMapResource(),
        ShadowSystem::kCascadeCount,
        mTextures["checkerTex"]->Resource.Get());
    if (mIblTexturesLoaded)
    {
        mRenderingSystem->SetIblResources(
            mTextures["irradianceMap"]->Resource.Get(),
            mTextures["prefilterEnvMap"]->Resource.Get(),
            mTextures["integrationMap"]->Resource.Get());
    }

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

    mBackBufferStates.fill(D3D12_RESOURCE_STATE_COMMON);

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

    const SHORT f6State = GetAsyncKeyState(VK_F6);
    if ((f6State & 0x8000) != 0)
    {
        if (!mF6KeyDown)
        {
            mEdgePostEnabled = !mEdgePostEnabled;
            mF6KeyDown = true;
            char buf[96];
            sprintf_s(buf, "Edge post-process: %s\n", mEdgePostEnabled ? "ON" : "OFF");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF6KeyDown = false;
    }

    const SHORT f7State = GetAsyncKeyState(VK_F7);
    if ((f7State & 0x8000) != 0)
    {
        if (!mF7KeyDown)
        {
            mVcrPostEnabled = !mVcrPostEnabled;
            mF7KeyDown = true;
            char buf[96];
            sprintf_s(buf, "VCR post-process: %s\n", mVcrPostEnabled ? "ON" : "OFF");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF7KeyDown = false;
    }

    const SHORT f8State = GetAsyncKeyState(VK_F8);
    if ((f8State & 0x8000) != 0)
    {
        if (!mF8KeyDown)
        {
            mEnableIbl = !mEnableIbl;
            mF8KeyDown = true;
            char buf[96];
            sprintf_s(buf, "IBL ambient: %s\n", mEnableIbl ? "ON" : "OFF");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF8KeyDown = false;
    }

    const SHORT f9State = GetAsyncKeyState(VK_F9);
    if ((f9State & 0x8000) != 0)
    {
        if (!mF9KeyDown)
        {
            mUseBeckmannDistribution = !mUseBeckmannDistribution;
            mF9KeyDown = true;
            char buf[96];
            sprintf_s(buf, "NDF distribution: %s\n", mUseBeckmannDistribution ? "BECKMANN" : "GGX");
            OutputDebugStringA(buf);
        }
    }
    else
    {
        mF9KeyDown = false;
    }

    UpdatePostProcessCB();

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

    if (!mFallingActive[0] && !mHasPbrModel)
    {
        mFallingActive[0] = true;
        mFallingVelY[0] = 0.0f;
        mPointLights[0].Position = { 0.0f, 12.0f, 0.0f };
    }

    mSpawnAccumulator += gt.DeltaTime();
    const float spawnInterval = 1.0f;
    if (mRadius <= 40.0f)
    {
        while (mSpawnAccumulator >= spawnInterval)
        {
            mSpawnAccumulator -= spawnInterval;
            UINT activeCount = 0;
            for (UINT i = 0; i < kDeferredPointLightCount; ++i)
            {
                if (mFallingActive[i])
                    ++activeCount;
            }
            if (activeCount >= kMaxPointLightsForShading)
                break;

            for (UINT i = 0; i < kDeferredPointLightCount; ++i)
            {
                if (mHasPbrModel && i == kPbrPointLightIndex)
                    continue;

                if (!mFallingActive[i])
                {
                    mFallingActive[i] = true;
                    mFallingVelY[i] = 0.0f;
                    mPointLights[i].Position = { 0.0f, 12.0f, 0.0f };
                    break;
                }
            }
        }
    }

    constexpr float fallSpeed = 21.0f;
    constexpr float floorY = -1.0f;
    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        if (!mFallingActive[i])
            continue;

        if (mHasPbrModel && i == kPbrPointLightIndex)
            continue;

        auto& L = mPointLights[i];
        if (L.Position.y <= floorY + 1e-3f)
        {
            mFallingActive[i] = false;
            continue;
        }

        mFallingVelY[i] += fallSpeed * gt.DeltaTime();
        L.Position.y -= mFallingVelY[i] * gt.DeltaTime();
    }
    UpdateDeferredLightCB();
}

void CrateApp::WaitForGpu()
{
    if (mFence->GetCompletedValue() < mCurrentFence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, nullptr, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

void CrateApp::Draw(const GameTimer& gt)
{
    if (mShadowSystem)
        mShadowSystem->MarkShadowMapShaderResource();

    UpdateForestLod(mCurrFrameResourceIndex);
    mRenderingSystem->UpdateLightBufferSrv(
        mCurrFrameResourceIndex,
        mCurrFrameResource->DeferredLightBuffer->Resource(),
        kDeferredTotalLightCount,
        sizeof(DeferredLightGpu));

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

    if (mShadowSystem)
    {
        const XMMATRIX view = XMLoadFloat4x4(&mView);
        const XMMATRIX proj = XMLoadFloat4x4(&mProj);
        const XMVECTOR lightDir = XMLoadFloat3(&mDirectionalLights[0].Direction);
        if (mHasSponzaBounds)
        {
            mShadowSystem->SetSceneBounds(
                XMLoadFloat3(&mSponzaBoundsMin),
                XMLoadFloat3(&mSponzaBoundsMax));
        }

        mShadowSystem->UpdateCascades(
            view, proj, lightDir, 1.0f, 1000.0f, ShadowSystem::kDefaultSplitLambda);

        if (mRadius <= kShadowDisableCameraRadius)
        {
            mShadowSystem->BeginPass(mCommandList.Get(), passAddress);
            for (UINT c = 0; c < ShadowSystem::kCascadeCount; ++c)
            {
                mShadowSystem->BeginCascade(mCommandList.Get(), c, mCurrFrameResourceIndex);
                DrawRenderItemsShadow(mCommandList.Get(), mSponzaOpaqueRitems);
            }
            mShadowSystem->EndPass(mCommandList.Get());
        }

        mShadowSystem->UpdateLightingConstants(lightDir, mCurrFrameResourceIndex);
        mCommandList->RSSetViewports(1, &mScreenViewport);
        mCommandList->RSSetScissorRects(1, &mScissorRect);
    }

    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE checkerTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    checkerTex.Offset(1, mCbvSrvDescriptorSize);
    mRenderingSystem->BeginGeometryPass(
        mCommandList.Get(),
        DepthStencilView(),
        passAddress,
        checkerTex,
        mGeometryWireframe);

    DrawRenderItems(mCommandList.Get(), mSponzaOpaqueRitems);
    DrawRenderItems(mCommandList.Get(), mStressVisibleRitems);
    DrawBillboardForest(mCommandList.Get());

    if (mParticleSystem && mRadius <= kParticlesDisableCameraRadius)
    {
        mParticleSystem->SetEmitterPosition(XMFLOAT3(0.0f, 1.2f, 0.0f));
        mParticleSystem->Update(mCommandList.Get(), gt.DeltaTime(), gt.TotalTime());
        mParticleSystem->Render(mCommandList.Get(), passAddress);
    }

    mRenderingSystem->EndGeometryPass(mCommandList.Get());

    if (mShadowSystem)
        mShadowSystem->PrepareForLighting(mCommandList.Get());

    D3D12_RESOURCE_STATES& backBufferState = mBackBufferStates[mCurrBackBuffer];
    auto transition = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), backBufferState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &transition);
    backBufferState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::Black, 0, nullptr);

    mRenderingSystem->ExecuteLightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        passAddress,
        mCurrFrameResource->DeferredLightParamsCB->Resource()->GetGPUVirtualAddress(),
        mShadowSystem ? mShadowSystem->GetLightingConstantBufferAddress(mCurrFrameResourceIndex) : 0,
        mCurrFrameResourceIndex);

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

    if (mEdgePostEnabled || mVcrPostEnabled)
    {
        mRenderingSystem->ExecutePostProcessPasses(
            mCommandList.Get(),
            CurrentBackBuffer(),
            CurrentBackBufferView(),
            passAddress,
            mCurrFrameResource->PostProcessCB->Resource()->GetGPUVirtualAddress(),
            mEdgePostEnabled,
            mVcrPostEnabled,
            backBufferState);
    }

    transition = CD3DX12_RESOURCE_BARRIER::Transition(
        CurrentBackBuffer(), backBufferState, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &transition);
    backBufferState = D3D12_RESOURCE_STATE_PRESENT;

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
    mForestLodUsesMesh.assign(mForestInstancesCpu.size(), 0);
    const UINT instBufBytes = kMaxForestInstances * (UINT)sizeof(TreeInstanceGpu);

    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc = CD3DX12_RESOURCE_DESC::Buffer(instBufBytes);

    for (int fi = 0; fi < gNumFrameResources; ++fi)
    {
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mForestMeshUpload[fi])));
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&mForestBillboardUpload[fi])));

    ThrowIfFailed(mForestMeshUpload[fi]->Map(0, nullptr, reinterpret_cast<void**>(&mForestMeshMapped[fi])));
    ThrowIfFailed(mForestBillboardUpload[fi]->Map(0, nullptr, reinterpret_cast<void**>(&mForestBillboardMapped[fi])));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = kMaxForestInstances;
    srvDesc.Buffer.StructureByteStride = sizeof(TreeInstanceGpu);
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    const UINT meshSrvIndex = kForestInstanceSrvBaseIndex + fi * 2u;
    const UINT billSrvIndex = meshSrvIndex + 1u;

    CD3DX12_CPU_DESCRIPTOR_HANDLE hMesh(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    hMesh.Offset(meshSrvIndex, mCbvSrvDescriptorSize);
    md3dDevice->CreateShaderResourceView(mForestMeshUpload[fi].Get(), &srvDesc, hMesh);

    CD3DX12_CPU_DESCRIPTOR_HANDLE hBill(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    hBill.Offset(billSrvIndex, mCbvSrvDescriptorSize);
    md3dDevice->CreateShaderResourceView(mForestBillboardUpload[fi].Get(), &srvDesc, hBill);
    }

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

bool CrateApp::LoadModelTexture(const std::string& texturePath, const std::string& texName, int heapIndex)
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

                auto modelTex = std::make_unique<Texture>();
                modelTex->Name = texName;
                modelTex->Filename = wTexturePath;
                modelTex->Resource = res;
                modelTex->UploadHeap = upload;
                mTextures[texName] = std::move(modelTex);

                sprintf_s(msg, "Texture loaded successfully: %s\n", resolvedPath.c_str());
                OutputDebugStringA(msg);
                return true;
            }
        }
    }

    sprintf_s(msg, "Failed to load texture: %s\n", texturePath.c_str());
    OutputDebugStringA(msg);
    return false;
}

static bool DXGIFormatHasAlpha(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

bool CrateApp::TryLoadBillboardTreeCard(const std::string& resolvedPath, int heapIndex)
{
    if (!FileExistsA_Local(resolvedPath.c_str()))
        return false;

    std::wstring wPath(resolvedPath.begin(), resolvedPath.end());
    ScratchImage image;
    TexMetadata metadata;
    HRESULT hr = LoadFromDDSFile(wPath.c_str(), DDS_FLAGS_NONE, &metadata, image);
    if (FAILED(hr))
        hr = LoadFromTGAFile(wPath.c_str(), &metadata, image);
    if (FAILED(hr))
        hr = LoadFromWICFile(wPath.c_str(), WIC_FLAGS_FORCE_SRGB, &metadata, image);
    if (FAILED(hr))
        hr = LoadFromWICFile(wPath.c_str(), WIC_FLAGS_NONE, &metadata, image);

    if (FAILED(hr))
        return false;

    if (metadata.width < 16 || metadata.height < 16)
    {
        OutputDebugStringA("Billboard: rejected tree sprite (too small)\n");
        return false;
    }
    if (!DXGIFormatHasAlpha(metadata.format))
    {
        OutputDebugStringA("Billboard: rejected tree sprite (no alpha channel)\n");
        return false;
    }

    mTextures.erase("treeTex");
    if (!LoadModelTexture(resolvedPath, "treeTex", heapIndex))
        return false;

    auto it = mTextures.find("treeTex");
    return it != mTextures.end() && it->second && it->second->Resource;
}

void CrateApp::CreateFallbackTreeCardTexture()
{
    if (mFallbackTreeSrvHeapIndex != 0)
        return;

    constexpr UINT tw = 128;
    constexpr UINT th = 256;
    std::vector<uint8_t> bgra((size_t)tw * th * 4);

    for (UINT y = 0; y < th; ++y)
    {
        const float v = (y + 0.5f) / th;
        for (UINT x = 0; x < tw; ++x)
        {
            const float u = (x + 0.5f) / tw;
            uint8_t r = 0, g = 0, b = 0, a = 0;

            if (v < 0.30f)
            {
                const float du = fabsf(u - 0.5f);
                if (du < 0.09f)
                {
                    a = 255;
                    r = 89;
                    g = 56;
                    b = 31;
                }
            }
            else
            {
                const float du = (u - 0.5f) / 0.44f;
                const float dv = (v - 0.58f) / 0.40f;
                const float d2 = du * du + dv * dv;
                if (d2 < 1.0f)
                {
                    const float edge = (std::max)(0.0f, (std::min)(1.0f, (1.0f - d2) / 0.18f));
                    a = (uint8_t)(255.0f * edge);
                    r = 32;
                    g = (uint8_t)(140 + 40 * (1.0f - d2));
                    b = 38;
                }
            }

            const size_t i = ((size_t)y * tw + x) * 4;
            bgra[i + 0] = b;
            bgra[i + 1] = g;
            bgra[i + 2] = r;
            bgra[i + 3] = a;
        }
    }

    int nextHeap = NextSceneTextureSrvIndex(mTextureCache, (int)kForestInstanceSrvBaseIndex, (int)kReservedSrvHeapEnd);

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = tw;
    texDesc.Height = th;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> defaultHeap;
    const CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&defaultHeap)));

    const UINT64 uploadSize = GetRequiredIntermediateSize(defaultHeap.Get(), 0, 1);
    ComPtr<ID3D12Resource> upload;
    const CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC uploadBufDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&upload)));

    D3D12_SUBRESOURCE_DATA subresource = {};
    subresource.pData = bgra.data();
    subresource.RowPitch = tw * 4;
    subresource.SlicePitch = tw * th * 4;

    UpdateSubresources(mCommandList.Get(), defaultHeap.Get(), upload.Get(), 0, 0, 1, &subresource);
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        defaultHeap.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &barrier);

    CD3DX12_CPU_DESCRIPTOR_HANDLE h(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    h.Offset(nextHeap, mCbvSrvDescriptorSize);

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    md3dDevice->CreateShaderResourceView(defaultHeap.Get(), &srvDesc, h);

    auto tex = std::make_unique<Texture>();
    tex->Name = "fallbackTreeCard";
    tex->Resource = defaultHeap;
    tex->UploadHeap = upload;
    mTextures["fallbackTreeCard"] = std::move(tex);

    mFallbackTreeSrvHeapIndex = static_cast<UINT>(nextHeap);
    mTextureCache["__fallback_tree_card__"] = nextHeap;
    char msg[128];
    sprintf_s(msg, "Billboard: procedural fallback tree card at heap %u\n", mFallbackTreeSrvHeapIndex);
    OutputDebugStringA(msg);
}

void CrateApp::LoadBillboardTreeTexture()
{
    CreateFallbackTreeCardTexture();
    mBillboardTreeSrvHeapIndex = mFallbackTreeSrvHeapIndex;

    int nextHeap = NextSceneTextureSrvIndex(mTextureCache, (int)kForestInstanceSrvBaseIndex, (int)kReservedSrvHeapEnd);

    static const char* kTexDirs[] = {
        "../Textures/",
        "Textures/",
        "../../Textures/",
        "../../../Textures/",
    };
    static const char* kTexNames[] = {
        "tree.dds",
        "tree.png",
        "tree.tga",
        "tree.jpg",
        "tree.jpeg",
    };

    for (const char* dir : kTexDirs)
    {
        for (const char* name : kTexNames)
        {
            const std::string resolved = ResolveMediaPath(std::string(dir) + name);
            if (!TryLoadBillboardTreeCard(resolved, nextHeap))
                continue;

            mTextureCache[resolved] = nextHeap;
            mBillboardTreeSrvHeapIndex = static_cast<UINT>(nextHeap);
            char msg[384];
            sprintf_s(msg, "Billboard: tree sprite OK: %s (heap %u)\n", resolved.c_str(), mBillboardTreeSrvHeapIndex);
            OutputDebugStringA(msg);
            return;
        }
    }

    OutputDebugStringA(
        "Billboard: using built-in tree sprite (add Textures/tree.png with alpha).\n");
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
            if (LoadModelTexture(texPath, texName, nextHeapIndex))
            {
                mTextureCache[texPath] = nextHeapIndex;
                mMaterialToHeapIndex[matName] = nextHeapIndex;
                sprintf_s(msg, "Cached texture for material %s at index %d\n", matName.c_str(), nextHeapIndex);
                OutputDebugStringA(msg);
                nextHeapIndex++;
            }
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
            if (LoadModelTexture(texPath, texName, nextHeapIndex))
            {
                mTextureCache[texPath] = nextHeapIndex;
                mMaterialToBumpHeapIndex[matName] = nextHeapIndex;
                nextHeapIndex++;
            }
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

        const uint32_t startIndex = static_cast<uint32_t>(model.Indices.size());

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
        submesh.Geometry.IndexCount = static_cast<uint32_t>(model.Indices.size()) - startIndex;

        model.Submeshes.push_back(submesh);

        vertexOffset += mesh->mNumVertices;
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

void CrateApp::LoadPbrModel()
{
    mHasPbrModel = false;
    mHasPbrCerberus = false;
    mHasPbrWoodRoot = false;
    mMaterialToMetallicHeapIndex.clear();
    mMaterialToRoughnessHeapIndex.clear();

    const char* cerberusPaths[] = {
        "../Models/PBR models/Cerberus_by_Andrew_Maximov/Cerberus_LP.FBX",
        "../Models/PBR models/Cerberus_by_Andrew_Maximov/Cerberus_LP.fbx",
        "../Models/PBR models/Cerberus_by_Andrew_Maximov/Cerberus.FBX"
    };
    const char* woodRootPaths[] = {
        "../Models/PBR models/wood_root/Aset_wood_root_M_rkswd_LOD0.fbx",
        "../Models/PBR models/wood_root/wood_root.fbx"
    };

    mHasPbrCerberus = ImportPbrFbxModel(
        cerberusPaths, _countof(cerberusPaths), "PbrCerberus",
        "Cerberus_A.jpg", "Cerberus_N.jpg", "Cerberus_M.jpg", "Cerberus_R.jpg");
    mHasPbrWoodRoot = ImportPbrFbxModel(
        woodRootPaths, _countof(woodRootPaths), "PbrWoodRoot",
        "Aset_wood_root_M_rkswd_2K_Albedo.jpg",
        "Aset_wood_root_M_rkswd_2K_Normal_LOD0.jpg",
        nullptr,
        "Aset_wood_root_M_rkswd_2K_Roughness.jpg");

    mHasPbrModel = mHasPbrCerberus || mHasPbrWoodRoot;
    if (!mHasPbrModel)
        OutputDebugStringA("PBR models not found in ../Models/PBR models/ — skipping.\n");
}

bool CrateApp::ImportPbrFbxModel(
    const char* const* tryPaths,
    size_t tryPathCount,
    const char* geometryName,
    const char* fallbackAlbedo,
    const char* fallbackNormal,
    const char* fallbackMetal,
    const char* fallbackRough)
{
    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    std::string loadedPath;
    for (size_t pi = 0; pi < tryPathCount; ++pi)
    {
        const std::string resolved = ResolveMediaPath(tryPaths[pi]);
        scene = importer.ReadFile(resolved.c_str(),
            aiProcess_Triangulate |
            aiProcess_FlipUVs |
            aiProcess_GenNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality |
            aiProcess_CalcTangentSpace);

        if (scene && scene->mRootNode && (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) == 0)
        {
            loadedPath = resolved;
            break;
        }
        scene = nullptr;
        importer.FreeScene();
    }

    if (!scene)
        return false;

    char msg[512];
    sprintf_s(msg, "PBR model loaded (%s): %s\n", geometryName, loadedPath.c_str());
    OutputDebugStringA(msg);

    std::string baseDir = loadedPath;
    const size_t slash = baseDir.find_last_of("/\\");
    if (slash != std::string::npos)
        baseDir = baseDir.substr(0, slash + 1);

    int nextHeapIndex = 2;
    for (const auto& cacheEntry : mTextureCache)
        nextHeapIndex = (std::max)(nextHeapIndex, cacheEntry.second + 1);

    for (unsigned int m = 0; m < scene->mNumMaterials; ++m)
    {
        aiMaterial* material = scene->mMaterials[m];
        aiString matName;
        if (material->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS)
            continue;

        const std::string name = std::string("pbr_") + matName.C_Str();

        auto cacheOrLoad = [&](const std::string& texPath, const std::string& texNamePrefix) -> int
        {
            if (texPath.empty())
                return -1;
            if (mTextureCache.find(texPath) != mTextureCache.end())
                return mTextureCache[texPath];

            const std::string texName = texNamePrefix + matName.C_Str();
            if (!LoadModelTexture(texPath, texName, nextHeapIndex))
                return -1;

            mTextureCache[texPath] = nextHeapIndex;
            return nextHeapIndex++;
        };

        std::string albedoPath;
        if (!TryGetAssimpTexture(material, aiTextureType_BASE_COLOR, baseDir, albedoPath))
            TryGetAssimpTexture(material, aiTextureType_DIFFUSE, baseDir, albedoPath);

        std::string normalPath;
        TryGetAssimpTexture(material, aiTextureType_NORMALS, baseDir, normalPath);
        if (normalPath.empty())
            TryGetAssimpTexture(material, aiTextureType_HEIGHT, baseDir, normalPath);

        std::string metallicPath;
        TryGetAssimpTexture(material, aiTextureType_METALNESS, baseDir, metallicPath);

        std::string roughnessPath;
        TryGetAssimpTexture(material, aiTextureType_DIFFUSE_ROUGHNESS, baseDir, roughnessPath);

        auto assignIfLoaded = [&](const std::string& path, const std::string& prefix, std::map<std::string, int>& outMap)
        {
            if (path.empty())
                return;
            const int idx = cacheOrLoad(path, prefix);
            if (idx >= 0)
                outMap[name] = idx;
        };

        assignIfLoaded(albedoPath, "PBRAlbedo_", mMaterialToHeapIndex);
        assignIfLoaded(normalPath, "PBRNormal_", mMaterialToBumpHeapIndex);
        assignIfLoaded(metallicPath, "PBRMetal_", mMaterialToMetallicHeapIndex);
        assignIfLoaded(roughnessPath, "PBRRough_", mMaterialToRoughnessHeapIndex);

        if (fallbackAlbedo && mMaterialToHeapIndex.find(name) == mMaterialToHeapIndex.end())
        {
            std::string fallbackPath;
            if (TryResolvePbrFallbackPath(baseDir, fallbackAlbedo, fallbackPath))
                assignIfLoaded(fallbackPath, "PBRAlbedo_", mMaterialToHeapIndex);
            if (TryResolvePbrFallbackPath(baseDir, fallbackNormal, fallbackPath))
                assignIfLoaded(fallbackPath, "PBRNormal_", mMaterialToBumpHeapIndex);
            if (TryResolvePbrFallbackPath(baseDir, fallbackMetal, fallbackPath))
                assignIfLoaded(fallbackPath, "PBRMetal_", mMaterialToMetallicHeapIndex);
            if (TryResolvePbrFallbackPath(baseDir, fallbackRough, fallbackPath))
                assignIfLoaded(fallbackPath, "PBRRough_", mMaterialToRoughnessHeapIndex);
        }
    }

    for (unsigned int meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
        aiMesh* mesh = scene->mMeshes[meshIdx];
        if (mesh->mMaterialIndex < 0)
            continue;

        aiString matName;
        if (scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS)
            continue;

        const std::string name = std::string("pbr_") + matName.C_Str();
        if (mMaterialToHeapIndex.find(name) != mMaterialToHeapIndex.end())
            continue;

        auto assignFallback = [&](const char* file, const char* prefix, std::map<std::string, int>& outMap)
        {
            if (!file)
                return;
            std::string path;
            if (!TryResolvePbrFallbackPath(baseDir, file, path))
                return;
            if (mTextureCache.find(path) != mTextureCache.end())
            {
                outMap[name] = mTextureCache[path];
                return;
            }
            const std::string texName = std::string(prefix) + matName.C_Str();
            if (LoadModelTexture(path, texName, nextHeapIndex))
            {
                mTextureCache[path] = nextHeapIndex;
                outMap[name] = nextHeapIndex;
                ++nextHeapIndex;
            }
        };

        assignFallback(fallbackAlbedo, "PBRAlbedo_", mMaterialToHeapIndex);
        assignFallback(fallbackNormal, "PBRNormal_", mMaterialToBumpHeapIndex);
        assignFallback(fallbackMetal, "PBRMetal_", mMaterialToMetallicHeapIndex);
        assignFallback(fallbackRough, "PBRRough_", mMaterialToRoughnessHeapIndex);
    }

    LoadedModel model;
    model.Name = geometryName;

    uint32_t vertexOffset = 0;

    for (unsigned int meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
        aiMesh* mesh = scene->mMeshes[meshIdx];
        const uint32_t startIndex = static_cast<uint32_t>(model.Indices.size());

        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            Vertex v = {};
            v.Pos = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };
            if (mesh->HasNormals())
                v.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };
            if (mesh->HasTextureCoords(0))
            {
                v.TexC.x = mesh->mTextureCoords[0][i].x;
                v.TexC.y = mesh->mTextureCoords[0][i].y;
            }
            model.Vertices.push_back(v);
        }

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
        {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int j = 0; j < face.mNumIndices; ++j)
                model.Indices.push_back(face.mIndices[j] + vertexOffset);
        }

        std::string materialName = "pbr_default";
        if (mesh->mMaterialIndex >= 0)
        {
            aiString matName;
            if (scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS)
                materialName = std::string("pbr_") + matName.C_Str();
        }

        SubmeshData submesh;
        submesh.MaterialName = materialName;
        submesh.Geometry.StartIndexLocation = startIndex;
        submesh.Geometry.IndexCount = static_cast<uint32_t>(model.Indices.size()) - startIndex;
        submesh.Geometry.BaseVertexLocation = 0;
        model.Submeshes.push_back(submesh);

        vertexOffset += mesh->mNumVertices;
    }

    if (model.Vertices.empty() || model.Indices.empty())
    {
        sprintf_s(msg, "PBR model %s has no geometry.\n", geometryName);
        OutputDebugStringA(msg);
        return false;
    }

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
        const std::string submeshName = "submesh_" + std::to_string(i) + "_" + model.Submeshes[i].MaterialName;
        geo->DrawArgs[submeshName] = model.Submeshes[i].Geometry;
    }

    mGeometries[geo->Name] = std::move(geo);
    mLoadedModels.push_back(std::move(model));
    sprintf_s(msg, "PBR geometry prepared: %s\n", geometryName);
    OutputDebugStringA(msg);
    return true;
}

void CrateApp::AddPbrModelRenderItems(
    const char* geometryName,
    float worldX,
    float worldZ,
    float rotationY,
    float targetHeight,
    int& objIndex,
    XMFLOAT3* outCenter)
{
    auto pbrGeo = mGeometries[geometryName].get();
    const LoadedModel* pbrModel = nullptr;
    for (const auto& m : mLoadedModels)
    {
        if (m.Name == geometryName)
        {
            pbrModel = &m;
            break;
        }
    }

    if (!pbrGeo || !pbrModel)
        return;

    float minModelY = pbrModel->Vertices[0].Pos.y;
    float maxModelY = pbrModel->Vertices[0].Pos.y;
    for (const Vertex& v : pbrModel->Vertices)
    {
        minModelY = (std::min)(minModelY, v.Pos.y);
        maxModelY = (std::max)(maxModelY, v.Pos.y);
    }
    const float modelHeight = (std::max)(maxModelY - minModelY, 1e-3f);
    const float pbrScale = targetHeight / modelHeight;
    const float pbrY = -1.0f - minModelY * pbrScale;

    const XMMATRIX pbrWorld =
        XMMatrixScaling(pbrScale, pbrScale, pbrScale) *
        XMMatrixRotationY(rotationY) *
        XMMatrixTranslation(worldX, pbrY, worldZ);

    XMVECTOR center = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
    for (const Vertex& v : pbrModel->Vertices)
        center = XMVectorAdd(center, XMLoadFloat3(&v.Pos));
    center = XMVectorScale(center, 1.0f / (float)pbrModel->Vertices.size());
    center = XMVector3TransformCoord(center, pbrWorld);

    XMFLOAT3 centerStore;
    XMStoreFloat3(&centerStore, center);
    if (outCenter)
        *outCenter = centerStore;

    auto initTexTransforms = [](RenderItem* ri) {
        XMMATRIX scale = XMMatrixScaling(ri->TextureScaleU, ri->TextureScaleV, 1.0f);
        XMMATRIX translation = XMMatrixTranslation(ri->TextureOffsetU, ri->TextureOffsetV, 0.0f);
        XMMATRIX texTransform = scale * translation;
        XMStoreFloat4x4(&ri->TexTransform, texTransform);
        XMStoreFloat4x4(&ri->TexTransformDisp, texTransform);
        ri->NumFramesDirty = gNumFrameResources;
    };

    size_t pbrItemsAdded = 0;
    char msg[256];
    for (size_t i = 0; i < pbrModel->Submeshes.size(); ++i)
    {
        const auto& submesh = pbrModel->Submeshes[i];
        const std::string submeshName = "submesh_" + std::to_string(i) + "_" + submesh.MaterialName;
        auto drawArg = pbrGeo->DrawArgs.find(submeshName);
        if (drawArg == pbrGeo->DrawArgs.end())
            continue;

        auto matIt = mMaterials.find(submesh.MaterialName);
        if (matIt == mMaterials.end())
        {
            sprintf_s(msg, "PBR %s: material %s not found, submesh skipped\n",
                geometryName, submesh.MaterialName.c_str());
            OutputDebugStringA(msg);
            continue;
        }

        auto renderItem = std::make_unique<RenderItem>();
        renderItem->ObjCBIndex = objIndex++;
        renderItem->Mat = matIt->second.get();
        renderItem->Geo = pbrGeo;
        renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        renderItem->IndexCount = drawArg->second.IndexCount;
        renderItem->StartIndexLocation = drawArg->second.StartIndexLocation;
        renderItem->BaseVertexLocation = drawArg->second.BaseVertexLocation;
        XMStoreFloat4x4(&renderItem->World, pbrWorld);
        renderItem->AnimateTexture = false;
        renderItem->TextureScaleU = 1.0f;
        renderItem->TextureScaleV = 1.0f;
        initTexTransforms(renderItem.get());
        mAllRitems.push_back(std::move(renderItem));
        ++pbrItemsAdded;
    }

    sprintf_s(
        msg,
        "PBR %s: %zu items at (%.1f, %.1f, %.1f), scale %.4f\n",
        geometryName,
        pbrItemsAdded,
        centerStore.x,
        centerStore.y,
        centerStore.z,
        pbrScale);
    OutputDebugStringA(msg);
}

void CrateApp::LoadSlendermanModel()
{
    mSlendermanMaterialAlbedo.clear();
    mHasSlenderman = false;

    const char* tryPaths[] = {
        "../Models/slenderman.obj",
        "../Models/slenderman/slenderman.obj"
    };

    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    for (const char* path : tryPaths)
    {
        scene = importer.ReadFile(path,
            aiProcess_Triangulate |
            aiProcess_FlipUVs |
            aiProcess_GenNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality);

        if (scene && scene->mRootNode && (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) == 0)
        {
            OutputDebugStringA("Slenderman model loaded.\n");
            break;
        }
        scene = nullptr;
    }

    if (!scene)
    {
        OutputDebugStringA("Slenderman: model not found, skipping.\n");
        return;
    }

    for (unsigned int m = 0; m < scene->mNumMaterials; ++m)
    {
        aiMaterial* material = scene->mMaterials[m];
        aiString matName;
        if (material->Get(AI_MATKEY_NAME, matName) != AI_SUCCESS)
            continue;

        aiColor3D kd(0.8f, 0.8f, 0.8f);
        material->Get(AI_MATKEY_COLOR_DIFFUSE, kd);

        const std::string key = std::string("slender_") + matName.C_Str();
        mSlendermanMaterialAlbedo[key] = XMFLOAT4(kd.r, kd.g, kd.b, 1.0f);
    }

    LoadedModel model;
    model.Name = "Slenderman";

    uint32_t vertexOffset = 0;

    for (unsigned int meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
        aiMesh* mesh = scene->mMeshes[meshIdx];
        const uint32_t startIndex = static_cast<uint32_t>(model.Indices.size());

        for (unsigned int i = 0; i < mesh->mNumVertices; ++i)
        {
            Vertex v = {};
            v.Pos = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };
            if (mesh->HasNormals())
                v.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };
            if (mesh->HasTextureCoords(0))
                v.TexC = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
            model.Vertices.push_back(v);
        }

        for (unsigned int i = 0; i < mesh->mNumFaces; ++i)
        {
            const aiFace& face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; ++j)
                model.Indices.push_back(face.mIndices[j] + vertexOffset);
        }

        std::string materialName = "slender_default";
        if (mesh->mMaterialIndex >= 0)
        {
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            aiString matName;
            if (material->Get(AI_MATKEY_NAME, matName) == AI_SUCCESS)
                materialName = std::string("slender_") + matName.C_Str();
        }

        SubmeshData submesh;
        submesh.MaterialName = materialName;
        submesh.Geometry.StartIndexLocation = startIndex;
        submesh.Geometry.IndexCount = static_cast<uint32_t>(model.Indices.size()) - startIndex;
        submesh.Geometry.BaseVertexLocation = 0;
        model.Submeshes.push_back(submesh);

        vertexOffset += mesh->mNumVertices;
    }

    if (model.Vertices.empty() || model.Submeshes.empty())
    {
        OutputDebugStringA("Slenderman: empty mesh data.\n");
        return;
    }

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = model.Name;

    const UINT vbByteSize = (UINT)model.Vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)model.Indices.size() * sizeof(uint32_t);

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
        const std::string submeshName = "submesh_" + std::to_string(i) + "_" + model.Submeshes[i].MaterialName;
        geo->DrawArgs[submeshName] = model.Submeshes[i].Geometry;
    }

    mGeometries[geo->Name] = std::move(geo);
    mLoadedModels.push_back(std::move(model));
    mHasSlenderman = true;
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
        return NextSceneTextureSrvIndex(mTextureCache, (int)kForestInstanceSrvBaseIndex, (int)kReservedSrvHeapEnd);
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
        OutputDebugStringA("Tree LOD: bark/leaves не найдены — меш рисуется однотонным зелёным.\n");
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

void CrateApp::LoadIblTextures()
{
    OutputDebugStringA("Loading IBL textures...\n");
    mIblTexturesLoaded = false;

    const char* irradiancePaths[] = {
        "../Models/IrradianceMap_BC6U.dds",
        "../Models/irradiance.dds"
    };
    const char* prefilterPaths[] = {
        "../Models/PreFilteredEnvMap_BC6U.dds",
        "../Models/prefilter.dds"
    };
    const char* integrationPaths[] = {
        "../Models/IntegrationMap.dds",
        "../Models/brdf_lut.dds"
    };

    bool okIrr = false;
    for (const char* p : irradiancePaths)
    {
        if (TryLoadDdsTexture(md3dDevice.Get(), mCommandList.Get(), p, "irradianceMap", mTextures))
        {
            okIrr = true;
            break;
        }
    }
    bool okPre = false;
    for (const char* p : prefilterPaths)
    {
        if (TryLoadDdsTexture(md3dDevice.Get(), mCommandList.Get(), p, "prefilterEnvMap", mTextures))
        {
            okPre = true;
            break;
        }
    }
    bool okBrdf = false;
    for (const char* p : integrationPaths)
    {
        if (TryLoadDdsTexture(md3dDevice.Get(), mCommandList.Get(), p, "integrationMap", mTextures))
        {
            okBrdf = true;
            break;
        }
    }

    mIblTexturesLoaded = okIrr && okPre && okBrdf;
    if (!mIblTexturesLoaded)
        CreateProceduralIblTextures();

    mIblMaxReflectionLod = 4.0f;
    if (mIblTexturesLoaded)
    {
        auto preIt = mTextures.find("prefilterEnvMap");
        if (preIt != mTextures.end() && preIt->second && preIt->second->Resource)
        {
            const UINT mipLevels = preIt->second->Resource->GetDesc().MipLevels;
            mIblMaxReflectionLod = static_cast<float>((std::max)(1u, mipLevels) - 1u);
        }

        char msg[160];
        sprintf_s(
            msg,
            "IBL OK: irradiance + prefilter + BRDF loaded (max reflection LOD = %.0f). F8 toggles IBL.\n",
            mIblMaxReflectionLod);
        OutputDebugStringA(msg);
    }
    else
    {
        char miss[256];
        sprintf_s(
            miss,
            "IBL MISSING: irradiance=%s prefilter=%s brdf=%s — add DDS to ../Models/\n",
            okIrr ? "ok" : "no",
            okPre ? "ok" : "no",
            okBrdf ? "ok" : "no");
        OutputDebugStringA(miss);
    }
}

void CrateApp::CreateProceduralIblTextures()
{
    constexpr UINT kCubeBase = 64;
    constexpr UINT kCubeMips = 5;
    constexpr UINT kBrdfSize = 128;

    std::vector<std::vector<std::vector<uint8_t>>> prefilterMips(kCubeMips);
    std::vector<std::vector<uint8_t>> currentFaces(6);
    for (UINT face = 0; face < 6; ++face)
        FillCubeFaceSky(face, kCubeBase, currentFaces[face]);
    prefilterMips[0] = currentFaces;

    UINT size = kCubeBase;
    for (UINT mip = 1; mip < kCubeMips; ++mip)
    {
        size /= 2;
        DownsampleCubeFaces(prefilterMips[mip - 1], size * 2, prefilterMips[mip], size);
    }

    std::vector<std::vector<uint8_t>> irradianceFaces(6);
    DownsampleCubeFaces(prefilterMips[2], 16, irradianceFaces, 16);

    std::vector<float> brdfPixels((size_t)kBrdfSize * kBrdfSize * 2);
    for (UINT y = 0; y < kBrdfSize; ++y)
    {
        const float roughness = (y + 0.5f) / kBrdfSize;
        for (UINT x = 0; x < kBrdfSize; ++x)
        {
            const float nDotV = (x + 0.5f) / kBrdfSize;
            const XMFLOAT2 ab = EnvBrdfApprox(roughness, nDotV);
            const size_t i = ((size_t)y * kBrdfSize + x) * 2;
            brdfPixels[i + 0] = ab.x;
            brdfPixels[i + 1] = ab.y;
        }
    }

    auto storeCube = [&](const std::string& name, ComPtr<ID3D12Resource>& tex, ComPtr<ID3D12Resource>& upload,
        const std::vector<std::vector<std::vector<uint8_t>>>& mips, UINT mipsCount, UINT base)
    {
        if (!UploadTextureCubeMipChain(md3dDevice.Get(), mCommandList.Get(), base, mipsCount, mips, tex, upload))
            return false;
        auto entry = std::make_unique<Texture>();
        entry->Name = name;
        entry->Resource = tex;
        entry->UploadHeap = upload;
        mTextures[name] = std::move(entry);
        return true;
    };

    ComPtr<ID3D12Resource> preTex, preUpload;
    ComPtr<ID3D12Resource> irrTex, irrUpload;
    ComPtr<ID3D12Resource> brdfTex, brdfUpload;

    const bool okPre = storeCube("prefilterEnvMap", preTex, preUpload, prefilterMips, kCubeMips, kCubeBase);
    const std::vector<std::vector<std::vector<uint8_t>>> irradianceMips = { irradianceFaces };
    const bool okIrr = storeCube("irradianceMap", irrTex, irrUpload, irradianceMips, 1, 16);
    const bool okBrdf = UploadTexture2D(
        md3dDevice.Get(), mCommandList.Get(), kBrdfSize, kBrdfSize,
        DXGI_FORMAT_R32G32_FLOAT, brdfPixels.data(), sizeof(float) * 2, brdfTex, brdfUpload);

    if (okBrdf)
    {
        auto entry = std::make_unique<Texture>();
        entry->Name = "integrationMap";
        entry->Resource = brdfTex;
        entry->UploadHeap = brdfUpload;
        mTextures["integrationMap"] = std::move(entry);
    }

    mIblTexturesLoaded = okPre && okIrr && okBrdf;
    mIblMaxReflectionLod = static_cast<float>(kCubeMips - 1);

    if (mIblTexturesLoaded)
    {
        OutputDebugStringA(
            "IBL PROCEDURAL: generated sky cubemap + BRDF LUT (DDS not found in ../Models/). F8 toggles IBL.\n");
    }
    else
    {
        OutputDebugStringA("IBL PROCEDURAL: generation failed.\n");
    }
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
    srvHeapDesc.NumDescriptors = kSrvDescriptorHeapSize;
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
        if (matName.rfind("pbr_", 0) == 0)
            continue;

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
        material->TessellationParams = XMFLOAT4(0.0f, 0.0f, 80.0f, 1.0f);

        mMaterials[matName] = std::move(material);

        char msg[256];
        sprintf_s(msg, "Created material %s diffuse %d\n", matName.c_str(), heapIndex);
        OutputDebugStringA(msg);
    }

    for (const auto& entry : mMaterialToHeapIndex)
    {
        if (entry.first.rfind("pbr_", 0) != 0)
            continue;

        auto material = std::make_unique<Material>();
        material->Name = entry.first;
        material->MatCBIndex = (int)mMaterials.size();
        material->DiffuseSrvHeapIndex = entry.second;

        auto bumpIt = mMaterialToBumpHeapIndex.find(entry.first);
        material->NormalSrvHeapIndex = bumpIt != mMaterialToBumpHeapIndex.end() ? bumpIt->second : entry.second;

        auto metalIt = mMaterialToMetallicHeapIndex.find(entry.first);
        material->MetallicSrvHeapIndex = metalIt != mMaterialToMetallicHeapIndex.end() ? metalIt->second : -1;

        auto roughIt = mMaterialToRoughnessHeapIndex.find(entry.first);
        material->RoughnessSrvHeapIndex = roughIt != mMaterialToRoughnessHeapIndex.end() ? roughIt->second : -1;

        material->DiffuseSrvHeapIndex2 = -1;
        material->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        material->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
        material->Metallic = 1.0f;
        material->Roughness = 1.0f;
        material->TessellationParams = XMFLOAT4(0.0f, 0.0f, 80.0f, 1.0f);
        const bool hasMetalMap = material->MetallicSrvHeapIndex >= 0;
        const bool hasRoughMap = material->RoughnessSrvHeapIndex >= 0;
        const bool hasPbrMaps = hasMetalMap && hasRoughMap;
        material->ChessboardParams = XMFLOAT4(0.0f, 0.0f, 0.0f, hasPbrMaps ? 1.0f : 0.0f);
        if (!hasMetalMap)
            material->Metallic = 0.0f;
        if (!hasRoughMap)
            material->Roughness = hasMetalMap ? 1.0f : 0.75f;
        mMaterials[entry.first] = std::move(material);
    }

    for (const auto& entry : mSlendermanMaterialAlbedo)
    {
        auto material = std::make_unique<Material>();
        material->Name = entry.first;
        material->MatCBIndex = (int)mMaterials.size();
        material->DiffuseSrvHeapIndex = 0;
        material->DiffuseSrvHeapIndex2 = 0;
        material->NormalSrvHeapIndex = 0;
        material->DiffuseAlbedo = entry.second;
        material->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
        material->Roughness = 0.85f;
        material->TessellationParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        material->ChessboardParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
        mMaterials[entry.first] = std::move(material);
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
    water->ChessboardParams = XMFLOAT4(1.2f, 1.2f, 0.0f, 0.0f);
    mMaterials["water"] = std::move(water);

    const UINT treeSrv = mBillboardTreeSrvHeapIndex != 0 ? mBillboardTreeSrvHeapIndex : mFallbackTreeSrvHeapIndex;

    auto billboardTree = std::make_unique<Material>();
    billboardTree->Name = "billboardTree";
    billboardTree->MatCBIndex = (int)mMaterials.size();
    billboardTree->DiffuseSrvHeapIndex = treeSrv;
    billboardTree->DiffuseSrvHeapIndex2 = treeSrv;
    billboardTree->NormalSrvHeapIndex = treeSrv;
    billboardTree->DiffuseAlbedo = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    billboardTree->FresnelR0 = XMFLOAT3(0.04f, 0.06f, 0.04f);
    billboardTree->Roughness = 0.55f;
    billboardTree->TessellationParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    billboardTree->ChessboardParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    mMaterials["billboardTree"] = std::move(billboardTree);

    const bool meshSolidColor = (mTreeMtlDiffuseSrvHeapIndex == 0);
    const UINT meshTreeSrv = meshSolidColor ? treeSrv : mTreeMtlDiffuseSrvHeapIndex;
    auto treeMeshLod = std::make_unique<Material>();
    treeMeshLod->Name = "treeMeshLod";
    treeMeshLod->MatCBIndex = (int)mMaterials.size();
    treeMeshLod->DiffuseSrvHeapIndex = meshTreeSrv;
    treeMeshLod->DiffuseSrvHeapIndex2 = meshTreeSrv;
    treeMeshLod->NormalSrvHeapIndex = meshTreeSrv;
    treeMeshLod->DiffuseAlbedo = meshSolidColor
        ? XMFLOAT4(0.18f, 0.55f, 0.22f, 1.0f)
        : XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    treeMeshLod->FresnelR0 = XMFLOAT3(0.04f, 0.06f, 0.04f);
    treeMeshLod->Roughness = 0.65f;
    treeMeshLod->TessellationParams = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    treeMeshLod->ChessboardParams = meshSolidColor
        ? XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f)
        : XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    mMaterials["treeMeshLod"] = std::move(treeMeshLod);

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

    if (mHasPbrCerberus)
    {
        AddPbrModelRenderItems("PbrCerberus", 6.0f, -30.0f, 0.85f, 5.0f, objIndex, &mPbrWorldCenter);
        SetupPbrPointLight();
    }

    if (mHasPbrWoodRoot)
    {
        AddPbrModelRenderItems("PbrWoodRoot", 1.5f, -29.0f, 0.25f, 4.0f, objIndex);
    }

    if (mHasSlenderman)
    {
        auto slenderGeo = mGeometries["Slenderman"].get();
        const LoadedModel* slenderModel = nullptr;
        for (const auto& m : mLoadedModels)
        {
            if (m.Name == "Slenderman")
            {
                slenderModel = &m;
                break;
            }
        }

        if (slenderGeo && slenderModel)
        {
            constexpr float slenderScale = 1.05f;
            const float slenderX = kForestPatchCenterX;
            const float slenderZ = kForestPatchCenterZ;
            const float slenderY = -1.0f - (-0.635447f) * slenderScale;

            const XMMATRIX slenderWorld =
                XMMatrixScaling(slenderScale, slenderScale, slenderScale) *
                XMMatrixRotationY(0.0f) *
                XMMatrixTranslation(slenderX, slenderY, slenderZ);

            XMVECTOR center = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
            for (const Vertex& v : slenderModel->Vertices)
                center = XMVectorAdd(center, XMLoadFloat3(&v.Pos));
            center = XMVectorScale(center, 1.0f / (float)slenderModel->Vertices.size());
            center = XMVector3TransformCoord(center, slenderWorld);
            XMStoreFloat3(&mSlendermanWorldCenter, center);

            for (size_t i = 0; i < slenderModel->Submeshes.size(); ++i)
            {
                const auto& submesh = slenderModel->Submeshes[i];
                const std::string submeshName = "submesh_" + std::to_string(i) + "_" + submesh.MaterialName;
                auto drawArg = slenderGeo->DrawArgs.find(submeshName);
                if (drawArg == slenderGeo->DrawArgs.end())
                    continue;

                auto renderItem = std::make_unique<RenderItem>();
                renderItem->ObjCBIndex = objIndex++;

                auto matIt = mMaterials.find(submesh.MaterialName);
                renderItem->Mat = matIt != mMaterials.end() ? matIt->second.get() : mMaterials["woodCrate"].get();

                renderItem->Geo = slenderGeo;
                renderItem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
                renderItem->IndexCount = drawArg->second.IndexCount;
                renderItem->StartIndexLocation = drawArg->second.StartIndexLocation;
                renderItem->BaseVertexLocation = drawArg->second.BaseVertexLocation;

                XMStoreFloat4x4(&renderItem->World, slenderWorld);
                renderItem->AnimateTexture = false;
                renderItem->TextureScaleU = 1.0f;
                renderItem->TextureScaleV = 1.0f;
                initTexTransforms(renderItem.get());
                mAllRitems.push_back(std::move(renderItem));
            }

            char slenderMsg[128];
            sprintf_s(
                slenderMsg,
                "Slenderman placed at center (%.1f, %.1f, %.1f)\n",
                mSlendermanWorldCenter.x,
                mSlendermanWorldCenter.y,
                mSlendermanWorldCenter.z);
            OutputDebugStringA(slenderMsg);
        }
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

    ComputeSponzaWorldBounds();

    sprintf_s(msg, "Total render items: %zu\n", mAllRitems.size());
    OutputDebugStringA(msg);
    OutputDebugStringA("========================================\n");
    OutputDebugStringA("Render items built\n");
    OutputDebugStringA("========================================\n\n");
}

void CrateApp::ComputeSponzaWorldBounds()
{
    mHasSponzaBounds = false;

    for (const LoadedModel& model : mLoadedModels)
    {
        if (model.Name != "Sponza")
            continue;

        const XMMATRIX world =
            XMMatrixScaling(0.02f, 0.02f, 0.02f) * XMMatrixTranslation(0.0f, -1.0f, 0.0f);

        XMVECTOR vmin = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 0.0f);
        XMVECTOR vmax = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 0.0f);
        for (const Vertex& v : model.Vertices)
        {
            const XMVECTOR p = XMVector3TransformCoord(XMLoadFloat3(&v.Pos), world);
            vmin = XMVectorMin(vmin, p);
            vmax = XMVectorMax(vmax, p);
        }

        constexpr float pad = 1.5f;
        const XMVECTOR padV = XMVectorSet(pad, pad, pad, 0.0f);
        XMStoreFloat3(&mSponzaBoundsMin, XMVectorSubtract(vmin, padV));
        XMStoreFloat3(&mSponzaBoundsMax, XMVectorAdd(vmax, padV));
        mHasSponzaBounds = true;

        char msg[256];
        sprintf_s(
            msg,
            "Sponza world AABB: (%.2f,%.2f,%.2f) - (%.2f,%.2f,%.2f)\n",
            mSponzaBoundsMin.x,
            mSponzaBoundsMin.y,
            mSponzaBoundsMin.z,
            mSponzaBoundsMax.x,
            mSponzaBoundsMax.y,
            mSponzaBoundsMax.z);
        OutputDebugStringA(msg);
        return;
    }
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

    constexpr int kStressTestCount = 800;
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

void CrateApp::SetupPbrPointLight()
{
    if (!mHasPbrModel)
        return;

    PointLightSource& L = mPointLights[kPbrPointLightIndex];
    L.Position = {
        mPbrWorldCenter.x + 2.5f,
        mPbrWorldCenter.y + 2.0f,
        mPbrWorldCenter.z + 2.2f
    };
    L.Strength = { 1.6f, 1.25f, 0.85f };
    L.FalloffStart = 0.4f;
    L.FalloffEnd = 16.0f;
    mFallingActive[kPbrPointLightIndex] = true;
    mFallingVelY[kPbrPointLightIndex] = 0.0f;

    char msg[160];
    sprintf_s(
        msg,
        "PBR point light at (%.1f, %.1f, %.1f)\n",
        L.Position.x,
        L.Position.y,
        L.Position.z);
    OutputDebugStringA(msg);
}

void CrateApp::UpdateStressVisibility()
{
    mStressVisibleRitems.clear();
    if (mStressRitems.empty())
        return;

    std::vector<size_t> visibleIndices;
    visibleIndices.reserve(mStressRitems.size());

    if (!mFrustumCullEnabled)
    {
        visibleIndices.resize(mStressRitems.size());
        for (size_t i = 0; i < mStressRitems.size(); ++i)
            visibleIndices[i] = i;
    }
    else
    {
        const XMMATRIX view = XMLoadFloat4x4(&mView);
        const XMMATRIX proj = XMLoadFloat4x4(&mProj);

        BoundingFrustum fr;
        BoundingFrustum::CreateFromMatrix(fr, proj);

        if (!mKdTreeCullingEnabled)
        {
            for (size_t i = 0; i < mStressRitems.size(); ++i)
            {
                if (FrustumContainsOrIntersectsAABB(fr, view, mStressWorldBounds[i]))
                    visibleIndices.push_back(i);
            }
        }
        else
        {
            std::vector<int> visibleIds;
            mKdTree.QueryVisible(mStressWorldBounds, fr, view, visibleIds);
            visibleIndices.reserve(visibleIds.size());
            for (int id : visibleIds)
            {
                if (id >= 0 && static_cast<size_t>(id) < mStressRitems.size())
                    visibleIndices.push_back(static_cast<size_t>(id));
            }
        }
    }

    mStressVisibleRitems.reserve(visibleIndices.size());
    for (size_t index : visibleIndices)
        mStressVisibleRitems.push_back(mStressRitems[index]);
}

void CrateApp::UpdateForestLod(UINT frameIndex)
{
    if (frameIndex >= gNumFrameResources)
        return;
    if (mForestInstancesCpu.empty() || !mForestMeshMapped[frameIndex] || !mForestBillboardMapped[frameIndex])
        return;

    const XMVECTOR eye = XMLoadFloat3(&mEyePos);
    const float nearSq = kForestLodMeshNear * kForestLodMeshNear;
    const float farSq = kForestLodMeshFar * kForestLodMeshFar;

    TreeInstanceGpu meshBuf[kMaxForestInstances];
    TreeInstanceGpu billBuf[kMaxForestInstances];
    UINT nMesh = 0;
    UINT nBill = 0;

    const size_t instanceCount = mForestInstancesCpu.size();
    if (mForestLodUsesMesh.size() != instanceCount)
        mForestLodUsesMesh.assign(instanceCount, 0);

    for (size_t i = 0; i < instanceCount; ++i)
    {
        const TreeInstanceGpu& t = mForestInstancesCpu[i];
        XMVECTOR p = XMLoadFloat3(&t.WorldPos);
        const float d2 = XMVectorGetX(XMVector3LengthSq(XMVectorSubtract(p, eye)));

        bool useMesh = mForestLodUsesMesh[i] != 0;
        if (mTreeLodMeshLoaded)
        {
            if (useMesh)
            {
                if (d2 > farSq)
                    useMesh = false;
            }
            else if (d2 < nearSq)
            {
                useMesh = true;
            }
        }
        else
        {
            useMesh = false;
        }
        mForestLodUsesMesh[i] = useMesh ? 1 : 0;

        if (useMesh)
        {
            if (nMesh < kMaxForestInstances)
                meshBuf[nMesh++] = t;
        }
        else if (nBill < kMaxForestInstances)
        {
            billBuf[nBill++] = t;
        }
    }

    mForestMeshCount[frameIndex] = nMesh;
    mForestBillboardCount[frameIndex] = nBill;

    if (nMesh > 0)
        std::memcpy(mForestMeshMapped[frameIndex], meshBuf, (size_t)nMesh * sizeof(TreeInstanceGpu));
    if (nBill > 0)
        std::memcpy(mForestBillboardMapped[frameIndex], billBuf, (size_t)nBill * sizeof(TreeInstanceGpu));
}

void CrateApp::DrawBillboardForest(ID3D12GraphicsCommandList* cmdList)
{
    if (mBillboardForestInstanceCount == 0 || !mRenderingSystem)
        return;
    const UINT meshCount = mForestMeshCount[mCurrFrameResourceIndex];
    const UINT billCount = mForestBillboardCount[mCurrFrameResourceIndex];
    if (meshCount == 0 && billCount == 0)
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

    if (billCount > 0)
    {
        auto geoIt = mGeometries.find("BillboardQuad");
        if (geoIt != mGeometries.end())
        {
            MeshGeometry* geo = geoIt->second.get();
            auto drawArg = geo->DrawArgs.find("tree");
            if (drawArg != geo->DrawArgs.end())
            {
                const UINT billSrvIndex = kForestInstanceSrvBaseIndex + mCurrFrameResourceIndex * 2u + 1u;
                CD3DX12_GPU_DESCRIPTOR_HANDLE inst(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                inst.Offset(billSrvIndex, mCbvSrvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(6, inst);

                cmdList->SetPipelineState(mRenderingSystem->GetBillboardTreePSO());
                auto vbv = geo->VertexBufferView();
                auto ibv = geo->IndexBufferView();
                cmdList->IASetVertexBuffers(0, 1, &vbv);
                cmdList->IASetIndexBuffer(&ibv);
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                cmdList->DrawIndexedInstanced(
                    drawArg->second.IndexCount,
                    billCount,
                    drawArg->second.StartIndexLocation,
                    drawArg->second.BaseVertexLocation,
                    0);
            }
        }
    }

    if (meshCount > 0 && mTreeLodMeshLoaded)
    {
        auto meshMatIt = mMaterials.find("treeMeshLod");
        if (meshMatIt != mMaterials.end())
        {
        Material* meshMat = meshMatIt->second.get();

        CD3DX12_GPU_DESCRIPTOR_HANDLE meshTex(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        meshTex.Offset(meshMat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE meshNrm(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        meshNrm.Offset(meshMat->NormalSrvHeapIndex >= 0 ? meshMat->NormalSrvHeapIndex : meshMat->DiffuseSrvHeapIndex, mCbvSrvDescriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE meshTexB(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        const int meshAltIdx = meshMat->DiffuseSrvHeapIndex2 >= 0 ? meshMat->DiffuseSrvHeapIndex2 : meshMat->DiffuseSrvHeapIndex;
        meshTexB.Offset(meshAltIdx, mCbvSrvDescriptorSize);

        cmdList->SetGraphicsRootDescriptorTable(0, meshTex);
        cmdList->SetGraphicsRootConstantBufferView(3, matCB->GetGPUVirtualAddress() + (UINT64)meshMat->MatCBIndex * matCBByteSize);
        cmdList->SetGraphicsRootDescriptorTable(4, meshNrm);
        cmdList->SetGraphicsRootDescriptorTable(5, meshTexB);

        auto meshIt = mGeometries.find("TreeLodMesh");
        if (meshIt != mGeometries.end())
        {
            MeshGeometry* treeGeo = meshIt->second.get();
            auto meshArg = treeGeo->DrawArgs.find("treeMesh");
            if (meshArg != treeGeo->DrawArgs.end())
            {
                const UINT meshSrvIndex = kForestInstanceSrvBaseIndex + mCurrFrameResourceIndex * 2u;
                CD3DX12_GPU_DESCRIPTOR_HANDLE inst(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
                inst.Offset(meshSrvIndex, mCbvSrvDescriptorSize);
                cmdList->SetGraphicsRootDescriptorTable(6, inst);

                cmdList->SetPipelineState(mRenderingSystem->GetTreeMeshInstancedPSO());
                auto vbv = treeGeo->VertexBufferView();
                auto ibv = treeGeo->IndexBufferView();
                cmdList->IASetVertexBuffers(0, 1, &vbv);
                cmdList->IASetIndexBuffer(&ibv);
                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                cmdList->DrawIndexedInstanced(
                    meshArg->second.IndexCount,
                    meshCount,
                    meshArg->second.StartIndexLocation,
                    meshArg->second.BaseVertexLocation,
                    0);
            }
        }
        }
    }
}

void CrateApp::DrawRenderItemsShadow(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    if (!mShadowSystem || ritems.empty())
        return;

    const UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    const UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));
    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    for (const RenderItem* ri : ritems)
    {
        if (!ri || !ri->Geo || ri->IsStressObject)
            continue;

        auto vbv = ri->Geo->VertexBufferView();
        auto ibv = ri->Geo->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vbv);
        cmdList->IASetIndexBuffer(&ibv);
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        CD3DX12_GPU_DESCRIPTOR_HANDLE heightSrv(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        heightSrv.Offset(
            ri->Mat->NormalSrvHeapIndex >= 0 ? ri->Mat->NormalSrvHeapIndex : ri->Mat->DiffuseSrvHeapIndex,
            mCbvSrvDescriptorSize);

        const D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        const D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootDescriptorTable(4, heightSrv);
        cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(2, matCBAddress);

        const UINT indexCapacity = ri->Geo->IndexBufferByteSize / sizeof(uint32_t);
        if (ri->IndexCount == 0 ||
            ri->StartIndexLocation >= indexCapacity ||
            ri->StartIndexLocation + ri->IndexCount > indexCapacity)
        {
            continue;
        }

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void CrateApp::DrawRenderItems(
    ID3D12GraphicsCommandList* cmdList,
    const std::vector<RenderItem*>& ritems,
    size_t maxCount)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();

    const size_t drawCount = (std::min)(ritems.size(), maxCount);
    for (size_t i = 0; i < drawCount; ++i)
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
        CD3DX12_GPU_DESCRIPTOR_HANDLE metal(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        const int metalIdx = ri->Mat->MetallicSrvHeapIndex >= 0 ? ri->Mat->MetallicSrvHeapIndex : ri->Mat->DiffuseSrvHeapIndex;
        metal.Offset(metalIdx, mCbvSrvDescriptorSize);
        CD3DX12_GPU_DESCRIPTOR_HANDLE rough(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        const int roughIdx = ri->Mat->RoughnessSrvHeapIndex >= 0 ? ri->Mat->RoughnessSrvHeapIndex : ri->Mat->DiffuseSrvHeapIndex;
        rough.Offset(roughIdx, mCbvSrvDescriptorSize);

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootDescriptorTable(0, tex);
        cmdList->SetGraphicsRootDescriptorTable(4, nrm);
        cmdList->SetGraphicsRootDescriptorTable(5, texB);
        cmdList->SetGraphicsRootDescriptorTable(6, metal);
        cmdList->SetGraphicsRootDescriptorTable(7, rough);
        cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);

        const UINT indexCapacity = ri->Geo->IndexBufferByteSize / sizeof(uint32_t);
        if (ri->IndexCount == 0 ||
            ri->StartIndexLocation >= indexCapacity ||
            ri->StartIndexLocation + ri->IndexCount > indexCapacity)
        {
            continue;
        }

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}

void CrateApp::UpdateCamera(const GameTimer& gt)
{
    const float sinPhi = sinf(mPhi);
    const float cosPhi = cosf(mPhi);
    const float sinTheta = sinf(mTheta);
    const float cosTheta = cosf(mTheta);

    XMVECTOR target = XMLoadFloat3(&mCameraTarget);
    XMVECTOR eye = XMVectorAdd(target, XMVectorSet(
        mRadius * sinPhi * cosTheta,
        mRadius * cosPhi,
        mRadius * sinPhi * sinTheta,
        0.0f));

    XMVECTOR forward = XMVectorSubtract(target, eye);
    forward = XMVectorSetY(forward, 0.0f);
    const float forwardLenSq = XMVectorGetX(XMVector3LengthSq(forward));
    if (forwardLenSq > 1e-6f)
        forward = XMVector3Normalize(forward);
    else
        forward = XMVectorSet(sinf(mTheta), 0.0f, cosf(mTheta), 0.0f);

    XMVECTOR right = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    const float move = mCameraMoveSpeed * gt.DeltaTime();

    if ((GetAsyncKeyState('W') & 0x8000) != 0)
        target = XMVectorAdd(target, XMVectorScale(forward, move));
    if ((GetAsyncKeyState('S') & 0x8000) != 0)
        target = XMVectorSubtract(target, XMVectorScale(forward, move));
    if ((GetAsyncKeyState('A') & 0x8000) != 0)
        target = XMVectorSubtract(target, XMVectorScale(right, move));
    if ((GetAsyncKeyState('D') & 0x8000) != 0)
        target = XMVectorAdd(target, XMVectorScale(right, move));
    if ((GetAsyncKeyState('E') & 0x8000) != 0)
        target = XMVectorAdd(target, XMVectorSet(0.0f, move, 0.0f, 0.0f));
    if ((GetAsyncKeyState('Q') & 0x8000) != 0)
        target = XMVectorSubtract(target, XMVectorSet(0.0f, move, 0.0f, 0.0f));

    XMStoreFloat3(&mCameraTarget, target);
    eye = XMVectorAdd(target, XMVectorSet(
        mRadius * sinPhi * cosTheta,
        mRadius * cosPhi,
        mRadius * sinPhi * sinTheta,
        0.0f));
    XMStoreFloat3(&mEyePos, eye);

    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
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
            matConstants.Metallic = mat->Metallic;
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
    mMainPassCB.AmbientLight = { 0.32f, 0.32f, 0.42f, 1.0f };
    mMainPassCB.Lights[0].Direction = { 0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[0].Strength = { 0.6f, 0.6f, 0.6f };
    mMainPassCB.Lights[1].Direction = { -0.57735f, -0.57735f, 0.57735f };
    mMainPassCB.Lights[1].Strength = { 0.3f, 0.3f, 0.3f };
    mMainPassCB.Lights[2].Direction = { 0.0f, -0.707f, -0.707f };
    mMainPassCB.Lights[2].Strength = { 0.15f, 0.15f, 0.15f };

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);
}

void CrateApp::UpdatePostProcessCB()
{
    PostProcessConstants post = {};
    float vcrIntensity = 0.65f;
    float vignetteStrength = 1.0f;

    if (mHasSlenderman && mVcrPostEnabled)
    {
        const float dx = mEyePos.x - mSlendermanWorldCenter.x;
        const float dy = mEyePos.y - mSlendermanWorldCenter.y;
        const float dz = mEyePos.z - mSlendermanWorldCenter.z;
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        const float range = kSlendermanVcrFarDist - kSlendermanVcrNearDist;
        const float proximity = (range > 1e-3f)
            ? (kSlendermanVcrFarDist - dist) / range
            : 0.0f;
        const float t = (std::max)(0.0f, (std::min)(proximity, 1.0f));

        vcrIntensity = kSlendermanVcrMin + (kSlendermanVcrMax - kSlendermanVcrMin) * t;
        vignetteStrength = 0.55f + 0.45f * t;
    }

    post.EdgeAndPost = { 0.95f, 0.10f, vcrIntensity, vignetteStrength };
    post.EnableFlags = {
        mEdgePostEnabled ? 1.0f : 0.0f,
        mVcrPostEnabled ? 1.0f : 0.0f,
        0.0f,
        0.0f };
    mCurrFrameResource->PostProcessCB->CopyData(0, post);
}

void CrateApp::UpdateDeferredLightCB()
{
    XMVECTOR eyePos = XMLoadFloat3(&mEyePos);
    XMVECTOR lookAt = XMLoadFloat3(&mCameraTarget);
    XMVECTOR viewDir = XMVector3Normalize(XMVectorSubtract(lookAt, eyePos));

    mSpotLights[1].Position = mEyePos;
    XMStoreFloat3(&mSpotLights[1].Direction, viewDir);

    UINT activePointLights = 0;
    for (UINT i = 0; i < kDeferredPointLightCount; ++i)
    {
        if (mFallingActive[i])
            ++activePointLights;
    }
    UINT maxPointLights = kMaxPointLightsForShading;
    if (mRadius > 45.0f)
        maxPointLights = 8u;
    else if (mRadius > 35.0f)
        maxPointLights = 24u;
    else if (mRadius > 25.0f)
        maxPointLights = 64u;

    mActivePointLights = (std::min)(activePointLights, maxPointLights);

    DeferredLightParams params = {};
    params.ActivePointLightCount = mActivePointLights;
    params.EnableIbl = (mIblTexturesLoaded && mEnableIbl) ? 1.0f : 0.0f;
    params.IblMaxReflectionLod = mIblMaxReflectionLod;
    params.UseBeckmannDistribution = mUseBeckmannDistribution ? 1.0f : 0.0f;
    mCurrFrameResource->DeferredLightParamsCB->CopyData(0, params);

    UINT dst = 0;
    for (UINT i = 0; i < kDeferredDirectionalLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 0.0f;
        // Мировое направление: в шейдере lightVec = -Direction (вектор к источнику).
        XMStoreFloat3(&l.Direction, XMLoadFloat3(&mDirectionalLights[i].Direction));
        l.Strength = mDirectionalLights[i].Strength;
        mCurrFrameResource->DeferredLightBuffer->CopyData(dst, l);
    }
    for (UINT i = 0; i < kDeferredPointLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 1.0f;
        l.Position = mPointLights[i].Position;
        l.Strength = mPointLights[i].Strength;
        l.FalloffStart = mPointLights[i].FalloffStart;
        l.FalloffEnd = mPointLights[i].FalloffEnd;
        mCurrFrameResource->DeferredLightBuffer->CopyData(dst, l);
    }
    for (UINT i = 0; i < kDeferredSpotLightCount; ++i, ++dst)
    {
        DeferredLightGpu l = {};
        l.Type = 2.0f;
        l.Position = mSpotLights[i].Position;
        l.Direction = mSpotLights[i].Direction;
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
        mRadius = MathHelper::Clamp(mRadius, 5.0f, 85.0f);
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