#pragma once

#include <DirectXCollision.h>
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <vector>

class KdTree
{
public:
    void Build(const std::vector<DirectX::BoundingBox>& worldBounds);

    void QueryVisible(
        const std::vector<DirectX::BoundingBox>& worldBounds,
        const DirectX::BoundingFrustum& frustumViewSpace,
        DirectX::FXMMATRIX viewWorld,
        std::vector<int>& outObjectIndices) const;

private:
    struct Node
    {
        DirectX::BoundingBox bounds;
        std::vector<int> indices;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;
    };

    static DirectX::BoundingBox MergeBounds(
        const std::vector<DirectX::BoundingBox>& worldBounds,
        const std::vector<int>& ids);

    static std::unique_ptr<Node> BuildRecursive(
        const std::vector<DirectX::BoundingBox>& worldBounds,
        std::vector<int>&& objectIndices,
        int depth);

    void QueryNode(
        const std::vector<DirectX::BoundingBox>& worldBounds,
        const Node* node,
        const DirectX::BoundingFrustum& frustumViewSpace,
        DirectX::FXMMATRIX viewWorld,
        std::vector<int>& outObjectIndices) const;

    std::unique_ptr<Node> mRoot;

    static constexpr int kMaxObjectsPerLeaf = 24;
    static constexpr int kMaxDepth = 16;
};

bool FrustumContainsOrIntersectsAABB(
    const DirectX::BoundingFrustum& frustumViewSpace,
    DirectX::FXMMATRIX viewWorld,
    const DirectX::BoundingBox& worldAABB);
