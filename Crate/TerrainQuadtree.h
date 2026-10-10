#pragma once

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <vector>

struct TerrainTileSelection
{
    float MinX = 0.0f;
    float MinZ = 0.0f;
    float Size = 0.0f;
    float UvMinX = 0.0f;
    float UvMinZ = 0.0f;
    float UvSize = 0.0f;
    DirectX::BoundingBox Bounds;
};

class TerrainQuadtree
{
public:
    void Build(float worldSize, int maxDepth);

    void SelectTiles(
        const DirectX::XMFLOAT3& eyePos,
        float lodDistanceFactor,
        float minTileSize,
        bool frustumCullEnabled,
        const DirectX::BoundingFrustum& frustumProj,
        DirectX::FXMMATRIX viewWorld,
        const float* heightSamples,
        int heightWidth,
        int heightHeight,
        float heightScale,
        std::vector<TerrainTileSelection>& outTiles) const;

    int MaxLeafCount() const { return mMaxLeafCount; }

    // World Y = sample * heightScale + heightBias.
    void SetHeightBias(float heightBias) { mHeightBias = heightBias; }

    static DirectX::BoundingBox ComputeTileBounds(
        float minX,
        float minZ,
        float size,
        float uvMinX,
        float uvMinZ,
        float uvSize,
        const float* heightSamples,
        int heightWidth,
        int heightHeight,
        float heightScale,
        float heightBias);

private:
    struct Node
    {
        float MinX = 0.0f;
        float MinZ = 0.0f;
        float Size = 0.0f;
        int Depth = 0;
        std::unique_ptr<Node> Children[4];
    };

    void BuildRecursive(Node* node, int maxDepth);
    void SelectRecursive(
        const Node* node,
        const DirectX::XMFLOAT3& eyePos,
        float lodDistanceFactor,
        float minTileSize,
        bool frustumCullEnabled,
        const DirectX::BoundingFrustum& frustumProj,
        DirectX::FXMMATRIX viewWorld,
        const float* heightSamples,
        int heightWidth,
        int heightHeight,
        float heightScale,
        std::vector<TerrainTileSelection>& outTiles) const;

    std::unique_ptr<Node> mRoot;
    float mWorldHalf = 0.0f;
    float mWorldSize = 0.0f;
    float mHeightBias = 0.0f;
    int mMaxLeafCount = 0;
};
