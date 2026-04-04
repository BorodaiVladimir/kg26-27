#include "KdTree.h"
#include <algorithm>
#include <cstddef>

using namespace DirectX;

BoundingBox KdTree::MergeBounds(const std::vector<BoundingBox>& worldBounds, const std::vector<int>& ids)
{
    BoundingBox out = worldBounds[static_cast<size_t>(ids[0])];
    for (size_t i = 1; i < ids.size(); ++i)
    {
        BoundingBox merged;
        BoundingBox::CreateMerged(merged, out, worldBounds[static_cast<size_t>(ids[i])]);
        out = merged;
    }
    return out;
}

static float CenterAxis(const BoundingBox& b, int axis)
{
    switch (axis)
    {
    case 0:
        return b.Center.x;
    case 1:
        return b.Center.y;
    default:
        return b.Center.z;
    }
}

std::unique_ptr<KdTree::Node> KdTree::BuildRecursive(
    const std::vector<BoundingBox>& worldBounds,
    std::vector<int>&& objectIndices,
    int depth)
{
    if (objectIndices.empty())
        return nullptr;

    auto node = std::make_unique<Node>();
    node->bounds = MergeBounds(worldBounds, objectIndices);

    if ((int)objectIndices.size() <= kMaxObjectsPerLeaf || depth >= kMaxDepth)
    {
        node->indices = std::move(objectIndices);
        return node;
    }

    const int axis = depth % 3;
    const size_t mid = objectIndices.size() / 2;
    const auto cmp = [&](int a, int b) {
        return CenterAxis(worldBounds[static_cast<size_t>(a)], axis) <
            CenterAxis(worldBounds[static_cast<size_t>(b)], axis);
    };

    const auto midIt = objectIndices.begin() + static_cast<std::ptrdiff_t>(mid);
    std::nth_element(objectIndices.begin(), midIt, objectIndices.end(), cmp);

    std::vector<int> left(objectIndices.begin(), midIt);
    std::vector<int> right(midIt, objectIndices.end());

    if (left.empty() || right.empty())
    {
        node->indices = std::move(objectIndices);
        return node;
    }

    node->left = BuildRecursive(worldBounds, std::move(left), depth + 1);
    node->right = BuildRecursive(worldBounds, std::move(right), depth + 1);
    return node;
}

void KdTree::Build(const std::vector<BoundingBox>& worldBounds)
{
    mRoot.reset();
    if (worldBounds.empty())
        return;

    std::vector<int> all;
    all.reserve(worldBounds.size());
    for (size_t i = 0; i < worldBounds.size(); ++i)
        all.push_back(static_cast<int>(i));

    mRoot = BuildRecursive(worldBounds, std::move(all), 0);
}

bool FrustumContainsOrIntersectsAABB(
    const BoundingFrustum& frustumViewSpace,
    FXMMATRIX viewWorld,
    const BoundingBox& worldAABB)
{
    BoundingBox viewBox;
    worldAABB.Transform(viewBox, viewWorld);
    return frustumViewSpace.Intersects(viewBox);
}

void KdTree::QueryNode(
    const std::vector<BoundingBox>& worldBounds,
    const Node* node,
    const BoundingFrustum& frustumViewSpace,
    FXMMATRIX viewWorld,
    std::vector<int>& outObjectIndices) const
{
    if (!node)
        return;

    BoundingBox boundsView;
    node->bounds.Transform(boundsView, viewWorld);
    if (!frustumViewSpace.Intersects(boundsView))
        return;

    if (!node->indices.empty())
    {
        for (int id : node->indices)
        {
            if (id >= 0 && static_cast<size_t>(id) < worldBounds.size() &&
                FrustumContainsOrIntersectsAABB(
                    frustumViewSpace, viewWorld, worldBounds[static_cast<size_t>(id)]))
            {
                outObjectIndices.push_back(id);
            }
        }
        return;
    }

    QueryNode(worldBounds, node->left.get(), frustumViewSpace, viewWorld, outObjectIndices);
    QueryNode(worldBounds, node->right.get(), frustumViewSpace, viewWorld, outObjectIndices);
}

void KdTree::QueryVisible(
    const std::vector<BoundingBox>& worldBounds,
    const BoundingFrustum& frustumViewSpace,
    FXMMATRIX viewWorld,
    std::vector<int>& outObjectIndices) const
{
    outObjectIndices.clear();
    if (!mRoot)
        return;
    QueryNode(worldBounds, mRoot.get(), frustumViewSpace, viewWorld, outObjectIndices);
}
