#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <DirectXCollision.h>
#include "Frustum.h"

// Статистика одного запроса (выводится в заголовок окна)
struct CullStats
{
    uint32_t NodeTests = 0;     // сколько узлов дерева проверено
    uint32_t ObjectTests = 0;   // сколько отдельных объектов проверено
    uint32_t Visible = 0;       // сколько объектов прошло
};

// Окто-дерево для статической сцены.
// Объект кладётся в самый глубокий узел, который содержит его ЦЕЛИКОМ.
// Объекты, пересекающие границу между детьми, остаются в родителе.
class Octree
{
public:
    void Build(const std::vector<DirectX::BoundingBox>& objectBounds,
        int maxDepth = 8, int maxObjectsPerNode = 16)
    {
        mNodes.clear();
        mObjectBounds = objectBounds;
        mMaxDepth = maxDepth;
        mMaxObjectsPerNode = maxObjectsPerNode;

        if (objectBounds.empty())
            return;

        // Корень: объединение всех AABB, приведённое к кубу
        DirectX::BoundingBox root = objectBounds[0];
        for (size_t i = 1; i < objectBounds.size(); ++i)
            DirectX::BoundingBox::CreateMerged(root, root, objectBounds[i]);

        const float half = (std::max)({ root.Extents.x, root.Extents.y, root.Extents.z }) * 1.001f;
        root.Extents = { half, half, half };

        std::vector<uint32_t> all(objectBounds.size());
        for (uint32_t i = 0; i < (uint32_t)all.size(); ++i) all[i] = i;

        mNodes.push_back(Node{});
        mNodes[0].Cell = root;
        BuildNode(0, std::move(all), 0);
        ComputeTightBounds(0);
    }

    // Отсечение: возвращает индексы видимых объектов
    void Query(const Frustum& frustum, std::vector<uint32_t>& outVisible, CullStats& stats) const
    {
        if (!mNodes.empty())
            QueryNode(0, frustum, outVisible, stats);
    }

    size_t NodeCount() const { return mNodes.size(); }

private:
    struct Node
    {
        DirectX::BoundingBox Cell;   // ячейка разбиения (куб)
        DirectX::BoundingBox Tight;  // плотный AABB всего содержимого поддерева
        int Children[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        std::vector<uint32_t> Objects;   // объекты, лежащие именно в этом узле
        bool HasContent = false;
    };

    static bool Contains(const DirectX::BoundingBox& outer, const DirectX::BoundingBox& inner)
    {
        return outer.Contains(inner) == DirectX::CONTAINS;
    }

    void BuildNode(int nodeIndex, std::vector<uint32_t> objects, int depth)
    {
        if ((int)objects.size() <= mMaxObjectsPerNode || depth >= mMaxDepth)
        {
            mNodes[nodeIndex].Objects = std::move(objects);
            return;
        }

        // 8 дочерних ячеек
        const DirectX::BoundingBox cell = mNodes[nodeIndex].Cell;
        const DirectX::XMFLOAT3 h = { cell.Extents.x * 0.5f, cell.Extents.y * 0.5f, cell.Extents.z * 0.5f };

        DirectX::BoundingBox childCells[8];
        std::vector<uint32_t> childObjects[8];
        for (int i = 0; i < 8; ++i)
        {
            childCells[i].Center = {
                cell.Center.x + ((i & 1) ? h.x : -h.x),
                cell.Center.y + ((i & 2) ? h.y : -h.y),
                cell.Center.z + ((i & 4) ? h.z : -h.z) };
            childCells[i].Extents = h;
        }

        std::vector<uint32_t> stay;
        for (uint32_t obj : objects)
        {
            int target = -1;
            for (int i = 0; i < 8; ++i)
            {
                if (Contains(childCells[i], mObjectBounds[obj])) { target = i; break; }
            }
            if (target >= 0) childObjects[target].push_back(obj);
            else stay.push_back(obj);   // пересекает границу детей
        }

        mNodes[nodeIndex].Objects = std::move(stay);

        for (int i = 0; i < 8; ++i)
        {
            if (childObjects[i].empty())
                continue;

            const int childIndex = (int)mNodes.size();
            mNodes.push_back(Node{});           // ссылки на mNodes здесь инвалидируются,
            mNodes[childIndex].Cell = childCells[i];
            mNodes[nodeIndex].Children[i] = childIndex; // поэтому работаем только по индексам
            BuildNode(childIndex, std::move(childObjects[i]), depth + 1);
        }
    }

    // Плотные границы снизу вверх: так узлы отсекаются раньше
    bool ComputeTightBounds(int nodeIndex)
    {
        bool has = false;
        DirectX::BoundingBox tight;

        for (uint32_t obj : mNodes[nodeIndex].Objects)
        {
            if (!has) { tight = mObjectBounds[obj]; has = true; }
            else DirectX::BoundingBox::CreateMerged(tight, tight, mObjectBounds[obj]);
        }
        for (int i = 0; i < 8; ++i)
        {
            const int c = mNodes[nodeIndex].Children[i];
            if (c < 0 || !ComputeTightBounds(c)) continue;
            if (!has) { tight = mNodes[c].Tight; has = true; }
            else DirectX::BoundingBox::CreateMerged(tight, tight, mNodes[c].Tight);
        }

        mNodes[nodeIndex].Tight = tight;
        mNodes[nodeIndex].HasContent = has;
        return has;
    }

    void CollectAll(int nodeIndex, std::vector<uint32_t>& out, CullStats& stats) const
    {
        const Node& n = mNodes[nodeIndex];
        out.insert(out.end(), n.Objects.begin(), n.Objects.end());
        stats.Visible += (uint32_t)n.Objects.size();
        for (int c : n.Children)
            if (c >= 0) CollectAll(c, out, stats);
    }

    void QueryNode(int nodeIndex, const Frustum& frustum, std::vector<uint32_t>& out, CullStats& stats) const
    {
        const Node& n = mNodes[nodeIndex];
        if (!n.HasContent) return;

        ++stats.NodeTests;
        const FrustumTest t = frustum.TestAABB(n.Tight);

        if (t == FrustumTest::Outside)
            return;                          // весь узел со всеми детьми невидим

        if (t == FrustumTest::Inside)
        {
            CollectAll(nodeIndex, out, stats); // весь узел видим — без проверок
            return;
        }

        // Пересечение: проверяем объекты узла по одному и спускаемся ниже
        for (uint32_t obj : n.Objects)
        {
            ++stats.ObjectTests;
            if (frustum.IsVisible(mObjectBounds[obj]))
            {
                out.push_back(obj);
                ++stats.Visible;
            }
        }
        for (int c : n.Children)
            if (c >= 0) QueryNode(c, frustum, out, stats);
    }

    std::vector<Node> mNodes;
    std::vector<DirectX::BoundingBox> mObjectBounds;
    int mMaxDepth = 8;
    int mMaxObjectsPerNode = 16;
};