#include "TerrainQuadtree.h"

#include "KdTree.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    float HorizontalDistance(float x0, float z0, float x1, float z1)
    {
        const float dx = x1 - x0;
        const float dz = z1 - z0;
        return std::sqrt(dx * dx + dz * dz);
    }
}

void TerrainQuadtree::Build(float worldSize, int maxDepth)
{
    mWorldSize = worldSize;
    mWorldHalf = worldSize * 0.5f;
    mRoot = std::make_unique<Node>();
    mRoot->MinX = -mWorldHalf;
    mRoot->MinZ = -mWorldHalf;
    mRoot->Size = worldSize;
    mRoot->Depth = 0;
    mMaxLeafCount = 0;
    BuildRecursive(mRoot.get(), maxDepth);
}

void TerrainQuadtree::BuildRecursive(Node* node, int maxDepth)
{
    if (!node)
        return;

    if (node->Depth >= maxDepth)
    {
        ++mMaxLeafCount;
        return;
    }

    const float half = node->Size * 0.5f;
    const float x0 = node->MinX;
    const float z0 = node->MinZ;
    const float x1 = x0 + half;
    const float z1 = z0 + half;
    const int childDepth = node->Depth + 1;

    node->Children[0] = std::make_unique<Node>(); // NW
    node->Children[0]->MinX = x0;
    node->Children[0]->MinZ = z0;
    node->Children[0]->Size = half;
    node->Children[0]->Depth = childDepth;

    node->Children[1] = std::make_unique<Node>(); // NE
    node->Children[1]->MinX = x1;
    node->Children[1]->MinZ = z0;
    node->Children[1]->Size = half;
    node->Children[1]->Depth = childDepth;

    node->Children[2] = std::make_unique<Node>(); // SW
    node->Children[2]->MinX = x0;
    node->Children[2]->MinZ = z1;
    node->Children[2]->Size = half;
    node->Children[2]->Depth = childDepth;

    node->Children[3] = std::make_unique<Node>(); // SE
    node->Children[3]->MinX = x1;
    node->Children[3]->MinZ = z1;
    node->Children[3]->Size = half;
    node->Children[3]->Depth = childDepth;

    for (const auto& child : node->Children)
        BuildRecursive(child.get(), maxDepth);
}

BoundingBox TerrainQuadtree::ComputeTileBounds(
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
    float heightBias)
{
    float minY = 0.0f;
    float maxY = heightScale;

    if (heightSamples && heightWidth > 1 && heightHeight > 1)
    {
        const int x0 = (std::max)(0, (int)std::floor(uvMinX * (heightWidth - 1)));
        const int y0 = (std::max)(0, (int)std::floor(uvMinZ * (heightHeight - 1)));
        const int x1 = (std::min)(heightWidth - 1, (int)std::ceil((uvMinX + uvSize) * (heightWidth - 1)));
        const int y1 = (std::min)(heightHeight - 1, (int)std::ceil((uvMinZ + uvSize) * (heightHeight - 1)));

        minY = heightScale;
        maxY = 0.0f;
        for (int y = y0; y <= y1; ++y)
        {
            for (int x = x0; x <= x1; ++x)
            {
                const float h = heightSamples[y * heightWidth + x];
                minY = (std::min)(minY, h * heightScale);
                maxY = (std::max)(maxY, h * heightScale);
            }
        }
        maxY = (std::max)(maxY, minY + 0.5f);
    }

    const XMFLOAT3 minimum(minX, minY + heightBias, minZ);
    const XMFLOAT3 maximum(minX + size, maxY + heightBias, minZ + size);
    BoundingBox box;
    BoundingBox::CreateFromPoints(box, XMLoadFloat3(&minimum), XMLoadFloat3(&maximum));
    return box;
}

void TerrainQuadtree::SelectTiles(
    const XMFLOAT3& eyePos,
    float lodDistanceFactor,
    float minTileSize,
    bool frustumCullEnabled,
    const BoundingFrustum& frustumProj,
    FXMMATRIX viewWorld,
    const float* heightSamples,
    int heightWidth,
    int heightHeight,
    float heightScale,
    std::vector<TerrainTileSelection>& outTiles) const
{
    outTiles.clear();
    if (!mRoot)
        return;
    SelectRecursive(
        mRoot.get(),
        eyePos,
        lodDistanceFactor,
        minTileSize,
        frustumCullEnabled,
        frustumProj,
        viewWorld,
        heightSamples,
        heightWidth,
        heightHeight,
        heightScale,
        outTiles);
}

void TerrainQuadtree::SelectRecursive(
    const Node* node,
    const XMFLOAT3& eyePos,
    float lodDistanceFactor,
    float minTileSize,
    bool frustumCullEnabled,
    const BoundingFrustum& frustumProj,
    FXMMATRIX viewWorld,
    const float* heightSamples,
    int heightWidth,
    int heightHeight,
    float heightScale,
    std::vector<TerrainTileSelection>& outTiles) const
{
    if (!node)
        return;

    const float centerX = node->MinX + node->Size * 0.5f;
    const float centerZ = node->MinZ + node->Size * 0.5f;
    const float dist = HorizontalDistance(eyePos.x, eyePos.z, centerX, centerZ);

    const bool canSplit = node->Size > minTileSize + 1e-3f && node->Children[0] != nullptr;
    const bool shouldSplit = canSplit && dist < node->Size * lodDistanceFactor;

    if (shouldSplit)
    {
        for (const auto& child : node->Children)
            SelectRecursive(
                child.get(),
                eyePos,
                lodDistanceFactor,
                minTileSize,
                frustumCullEnabled,
                frustumProj,
                viewWorld,
                heightSamples,
                heightWidth,
                heightHeight,
                heightScale,
                outTiles);
        return;
    }

    const float uvMinX = (node->MinX + mWorldHalf) / mWorldSize;
    // Grid V grows toward -Z, so the tile's V origin is at its max-Z edge.
    const float uvMinZ = (mWorldHalf - (node->MinZ + node->Size)) / mWorldSize;
    const float uvSize = node->Size / mWorldSize;

    TerrainTileSelection tile;
    tile.MinX = node->MinX;
    tile.MinZ = node->MinZ;
    tile.Size = node->Size;
    tile.UvMinX = uvMinX;
    tile.UvMinZ = uvMinZ;
    tile.UvSize = uvSize;
    tile.Bounds = ComputeTileBounds(
        node->MinX,
        node->MinZ,
        node->Size,
        uvMinX,
        uvMinZ,
        uvSize,
        heightSamples,
        heightWidth,
        heightHeight,
        heightScale,
        mHeightBias);

    if (frustumCullEnabled &&
        !FrustumContainsOrIntersectsAABB(frustumProj, viewWorld, tile.Bounds))
    {
        return;
    }

    outTiles.push_back(tile);
}
