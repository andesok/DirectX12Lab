#pragma once

// =====================================================================
// ДЗ (Terrain) — квадродерево ландшафта и выбор LOD (CDLOD)
//
// Чистый C++ без D3D: здесь только CPU-логика.
//   * Карта высот покрыта сеткой «листьев» (по LeafQuads квадов на сторону).
//   * Уровень 0 — листья (самые детальные), уровень L-1 — корень.
//   * Узел уровня l в 2^l раз больше листа, но рисуется ТОЙ ЖЕ сеткой
//     (общие вершинный и индексный буферы), поэтому вдвое реже на каждом уровне.
//   * Для каждого узла заранее посчитаны min/max высоты => точный AABB
//     для frustum culling и для проверки дистанции.
//   * Выбор LOD — Strugar, "Continuous Distance-Dependent Level of Detail":
//     у уровня l есть дальность ranges[l] = K * leafSize * 2^l.
//     Узел, который не пересекает сферу ranges[l-1], рисуется целиком;
//     иначе спускаемся к детям, а дети, слишком далёкие для уровня l-1,
//     рисуются четвертью родителя на уровне l.
//   * Морфинг вершин в VS убирает трещины и «прыжки» между уровнями.
// =====================================================================

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <functional>
#include <cfloat>

struct TerrainAabb
{
    float MinX, MinY, MinZ;
    float MaxX, MaxY, MaxZ;
};

enum class TerrainCull { Outside = 0, Intersect = 1, Inside = 2 };

// Что рисовать: узел целиком (Quarter = -1) или одну его четверть (0..3 = dx + 2*dz)
struct TerrainDrawNode
{
    float    OffsetX = 0.0f;   // мировой угол узла (min x)
    float    OffsetZ = 0.0f;   // мировой угол узла (min z)
    float    Size = 0.0f;      // сторона узла в мировых единицах
    uint32_t Level = 0;        // 0 = самый детальный
    int      Quarter = -1;
};

struct TerrainSelectStats
{
    static constexpr uint32_t MaxLevels = 16;
    uint32_t NodesVisited = 0;
    uint32_t NodesCulled = 0;
    uint32_t DrawCalls = 0;
    uint32_t PerLevel[MaxLevels] = {};
};

class TerrainQuadtree
{
public:
    static constexpr uint32_t MaxLevels = TerrainSelectStats::MaxLevels;

    // heights   — нормализованные высоты [0,1], width*height значений, строка = ось Z
    // texelWorld — расстояние между соседними текселями в мире
    // leafQuads — сколько квадов в стороне общей сетки (чётное, например 32)
    void Build(const float* heights, uint32_t width, uint32_t height,
        float texelWorld, float heightScale, float originX, float originZ, float baseY,
        uint32_t leafQuads)
    {
        mWidth = width;
        mHeight = height;
        mTexelWorld = texelWorld;
        mHeightScale = heightScale;
        mOriginX = originX;
        mOriginZ = originZ;
        mBaseY = baseY;
        mLeafQuads = leafQuads;
        mLeafSize = texelWorld * (float)leafQuads;
        mExtentX = texelWorld * (float)(width - 1);
        mExtentZ = texelWorld * (float)(height - 1);

        // Сколько листьев нужно по каждой оси, корень — ближайшая степень двойки
        const uint32_t quads = (std::max)(width - 1, height - 1);
        const uint32_t leavesNeeded = (quads + leafQuads - 1) / leafQuads;
        uint32_t leaves = 1;
        mLevelCount = 1;
        while (leaves < leavesNeeded) { leaves *= 2; ++mLevelCount; }
        if (mLevelCount > MaxLevels) mLevelCount = MaxLevels;

        mLevels.assign(mLevelCount, Level{});

        // ---- Листья: min/max по текселям (включая общую границу с соседом) ----
        Level& leaf = mLevels[0];
        leaf.Count = leaves;
        leaf.Min.assign((size_t)leaves * leaves, FLT_MAX);
        leaf.Max.assign((size_t)leaves * leaves, -FLT_MAX);

        for (uint32_t ly = 0; ly < leaves; ++ly)
        {
            const uint32_t y0 = ly * leafQuads;
            if (y0 >= height - 1) continue;                 // целиком за краем карты
            const uint32_t y1 = (std::min)(y0 + leafQuads, height - 1);
            for (uint32_t lx = 0; lx < leaves; ++lx)
            {
                const uint32_t x0 = lx * leafQuads;
                if (x0 >= width - 1) continue;
                const uint32_t x1 = (std::min)(x0 + leafQuads, width - 1);

                float mn = FLT_MAX;
                float mx = -FLT_MAX;
                for (uint32_t y = y0; y <= y1; ++y)
                {
                    const float* row = heights + (size_t)y * width;
                    for (uint32_t x = x0; x <= x1; ++x)
                    {
                        mn = (std::min)(mn, row[x]);
                        mx = (std::max)(mx, row[x]);
                    }
                }
                leaf.Min[(size_t)ly * leaves + lx] = mn;
                leaf.Max[(size_t)ly * leaves + lx] = mx;
            }
        }

        // ---- Родители: min/max четырёх детей ----
        for (uint32_t l = 1; l < mLevelCount; ++l)
        {
            const Level& child = mLevels[l - 1];
            Level& lev = mLevels[l];
            lev.Count = child.Count / 2;
            lev.Min.assign((size_t)lev.Count * lev.Count, FLT_MAX);
            lev.Max.assign((size_t)lev.Count * lev.Count, -FLT_MAX);
            for (uint32_t y = 0; y < lev.Count; ++y)
                for (uint32_t x = 0; x < lev.Count; ++x)
                {
                    float mn = FLT_MAX;
                    float mx = -FLT_MAX;
                    for (uint32_t q = 0; q < 4; ++q)
                    {
                        const size_t ci = (size_t)(2 * y + (q >> 1)) * child.Count + (2 * x + (q & 1));
                        mn = (std::min)(mn, child.Min[ci]);
                        mx = (std::max)(mx, child.Max[ci]);
                    }
                    lev.Min[(size_t)y * lev.Count + x] = mn;
                    lev.Max[(size_t)y * lev.Count + x] = mx;
                }
        }

        SetLodDistance(mLodDistance);
    }

    // K: ranges[l] = K * leafSize * 2^l. Меньше K => грубее и быстрее.
    // K >= 3 гарантирует, что соседние узлы отличаются не больше чем на 1 уровень.
    void SetLodDistance(float k)
    {
        mLodDistance = (std::max)(3.0f, k);
        double prev = 0.0;
        for (uint32_t l = 0; l < mLevelCount; ++l)
        {
            double end = (double)mLodDistance * (double)mLeafSize * std::pow(2.0, (double)l);
            if (l + 1 == mLevelCount) end = 1e12;            // корень виден всегда, без морфинга
            const double start = prev + (end - prev) * kMorphStartRatio;
            mRanges[l] = end;
            mMorph[l][0] = (float)(end / (end - start));
            mMorph[l][1] = (float)(1.0 / (end - start));
            prev = end;
        }
    }

    // cull: вернуть Outside / Intersect / Inside для AABB (nullptr => без отсечения)
    void Select(float eyeX, float eyeY, float eyeZ,
        const std::function<TerrainCull(const TerrainAabb&)>& cull,
        std::vector<TerrainDrawNode>& out, TerrainSelectStats& stats) const
    {
        out.clear();
        stats = {};
        if (mLevelCount == 0) return;

        Ctx ctx{ eyeX, eyeY, eyeZ, cull, out, stats };
        const uint32_t root = mLevelCount - 1;
        if (!SelectNode(ctx, root, 0, 0, false))
            AddNode(ctx, root, 0, 0, -1);   // недостижимо при «бесконечной» дальности корня
    }

    TerrainAabb NodeBox(uint32_t level, uint32_t x, uint32_t y) const
    {
        const float size = NodeSize(level);
        const size_t i = (size_t)y * mLevels[level].Count + x;
        TerrainAabb b;
        b.MinX = mOriginX + x * size;
        b.MinZ = mOriginZ + y * size;
        b.MaxX = (std::min)(b.MinX + size, mOriginX + mExtentX);   // вершины за краем зажимаются в VS
        b.MaxZ = (std::min)(b.MinZ + size, mOriginZ + mExtentZ);
        b.MinY = mBaseY + mLevels[level].Min[i] * mHeightScale;
        b.MaxY = mBaseY + mLevels[level].Max[i] * mHeightScale;
        return b;
    }

    float NodeSize(uint32_t level) const { return mLeafSize * (float)(1u << level); }
    uint32_t LevelCount() const { return mLevelCount; }
    uint32_t NodesPerSide(uint32_t level) const { return mLevels[level].Count; }
    float LodDistance() const { return mLodDistance; }
    double Range(uint32_t level) const { return mRanges[level]; }
    const float* MorphConsts(uint32_t level) const { return mMorph[level]; }
    float ExtentX() const { return mExtentX; }
    float ExtentZ() const { return mExtentZ; }

private:
    static constexpr double kMorphStartRatio = 0.66;

    struct Level
    {
        uint32_t Count = 0;          // узлов на сторону
        std::vector<float> Min, Max; // нормализованные высоты
    };

    struct Ctx
    {
        float ex, ey, ez;
        const std::function<TerrainCull(const TerrainAabb&)>& cull;
        std::vector<TerrainDrawNode>& out;
        TerrainSelectStats& stats;
    };

    bool IsEmpty(uint32_t level, uint32_t x, uint32_t y) const
    {
        const Level& lev = mLevels[level];
        const size_t i = (size_t)y * lev.Count + x;
        return lev.Min[i] > lev.Max[i];
    }

    static bool SphereIntersects(const Ctx& c, double r, const TerrainAabb& b)
    {
        const double dx = (std::max)({ (double)b.MinX - c.ex, 0.0, (double)c.ex - b.MaxX });
        const double dy = (std::max)({ (double)b.MinY - c.ey, 0.0, (double)c.ey - b.MaxY });
        const double dz = (std::max)({ (double)b.MinZ - c.ez, 0.0, (double)c.ez - b.MaxZ });
        return dx * dx + dy * dy + dz * dz <= r * r;
    }

    void AddNode(Ctx& c, uint32_t level, uint32_t x, uint32_t y, int quarter) const
    {
        TerrainDrawNode n;
        n.Size = NodeSize(level);
        n.OffsetX = mOriginX + x * n.Size;
        n.OffsetZ = mOriginZ + y * n.Size;
        n.Level = level;
        n.Quarter = quarter;
        c.out.push_back(n);
        ++c.stats.DrawCalls;
        ++c.stats.PerLevel[level];
    }

    // true  — область узла обработана (нарисована или отсечена)
    // false — узел слишком далеко для своего уровня, его должен нарисовать родитель
    bool SelectNode(Ctx& c, uint32_t level, uint32_t x, uint32_t y, bool parentInside) const
    {
        if (IsEmpty(level, x, y)) return true;      // за краем карты — рисовать нечего

        ++c.stats.NodesVisited;
        const TerrainAabb box = NodeBox(level, x, y);

        if (!SphereIntersects(c, mRanges[level], box))
            return false;

        TerrainCull vis = TerrainCull::Inside;
        if (!parentInside && c.cull)
            vis = c.cull(box);
        if (vis == TerrainCull::Outside)
        {
            ++c.stats.NodesCulled;
            return true;
        }

        if (level == 0 || !SphereIntersects(c, mRanges[level - 1], box))
        {
            AddNode(c, level, x, y, -1);
            return true;
        }

        // Часть узла ближе дальности следующего уровня — спускаемся к детям
        const bool inside = (vis == TerrainCull::Inside);
        bool done[4];
        for (uint32_t q = 0; q < 4; ++q)
            done[q] = SelectNode(c, level - 1, 2 * x + (q & 1), 2 * y + (q >> 1), inside);

        if (!done[0] && !done[1] && !done[2] && !done[3])
        {
            AddNode(c, level, x, y, -1);   // все четыре четверти — одним вызовом
            return true;
        }
        for (int q = 0; q < 4; ++q)
            if (!done[q]) AddNode(c, level, x, y, q);
        return true;
    }

    std::vector<Level> mLevels;
    uint32_t mLevelCount = 0;
    uint32_t mWidth = 0, mHeight = 0;
    uint32_t mLeafQuads = 32;
    float mLeafSize = 1.0f;
    float mTexelWorld = 1.0f;
    float mHeightScale = 1.0f;
    float mOriginX = 0.0f, mOriginZ = 0.0f, mBaseY = 0.0f;
    float mExtentX = 0.0f, mExtentZ = 0.0f;
    float mLodDistance = 4.0f;
    double mRanges[MaxLevels] = {};
    float mMorph[MaxLevels][2] = {};
};
