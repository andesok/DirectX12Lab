// Проверка CPU-части: покрытие без дыр/перекрытий, соседи отличаются <= 1 уровня,
// и на границе уровней вершины мелкого узла полностью доморфированы (morphK = 1).
#include "../headers/TerrainQuadtree.h"
#include <cstdio>
#include <random>
#include <cassert>

int main()
{
    const uint32_t W = 1025, H = 769;     // нарочно не квадрат и не степень двойки
    std::vector<float> h((size_t)W * H);
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(0, 1);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            h[(size_t)y * W + x] = 0.5f + 0.25f * std::sin(x * 0.01f) * std::cos(y * 0.013f) + 0.02f * u(rng);

    const float texel = 6.0f, hs = 3000.0f, ox = -3000, oz = -2000, by = -100;
    const uint32_t leafQ = 32;
    TerrainQuadtree qt;
    qt.Build(h.data(), W, H, texel, hs, ox, oz, by, leafQ);
    printf("levels=%u leafSize=%.1f\n", qt.LevelCount(), qt.NodeSize(0));

    const uint32_t leaves = qt.NodesPerSide(0);
    int failures = 0;
    for (int trial = 0; trial < 300; ++trial)
    {
        for (float k : {3.0f, 4.0f, 6.0f})
        {
            qt.SetLodDistance(k);
            float ex = ox + u(rng) * 7000 - 500, ez = oz + u(rng) * 5500 - 500, ey = by + u(rng) * 5000;
            std::vector<TerrainDrawNode> nodes;
            TerrainSelectStats st;
            qt.Select(ex, ey, ez, nullptr, nodes, st);

            // Растр уровней на сетке листьев
            std::vector<int> lvl((size_t)leaves * leaves, -1), cnt((size_t)leaves * leaves, 0);
            for (auto& n : nodes)
            {
                uint32_t span = 1u << n.Level;
                uint32_t nx = (uint32_t)std::lround((n.OffsetX - ox) / qt.NodeSize(0));
                uint32_t nz = (uint32_t)std::lround((n.OffsetZ - oz) / qt.NodeSize(0));
                uint32_t x0 = nx, z0 = nz, sx = span, sz = span;
                if (n.Quarter >= 0)
                {
                    sx = sz = span / 2;
                    x0 += (n.Quarter & 1) * sx;
                    z0 += (n.Quarter >> 1) * sz;
                }
                for (uint32_t z = z0; z < z0 + sz; ++z)
                    for (uint32_t x = x0; x < x0 + sx; ++x)
                    {
                        if (x >= leaves || z >= leaves) continue;
                        lvl[(size_t)z * leaves + x] = (int)n.Level;
                        cnt[(size_t)z * leaves + x]++;
                    }
            }
            // Покрытие внутри карты
            for (uint32_t z = 0; z < leaves; ++z)
                for (uint32_t x = 0; x < leaves; ++x)
                {
                    bool inside = x * leafQ < W - 1 && z * leafQ < H - 1;
                    int c = cnt[(size_t)z * leaves + x];
                    if (inside && c != 1) { if (failures++ < 5) printf("coverage %d at %u,%u\n", c, x, z); }
                }
            // Соседи и морфинг
            auto heightAt = [&](float wx, float wz) {
                int tx = (int)std::lround((wx - ox) / texel), tz = (int)std::lround((wz - oz) / texel);
                tx = std::clamp(tx, 0, (int)W - 1); tz = std::clamp(tz, 0, (int)H - 1);
                return by + h[(size_t)tz * W + tx] * hs;
            };
            for (uint32_t z = 0; z < leaves; ++z)
                for (uint32_t x = 0; x < leaves; ++x)
                {
                    int a = lvl[(size_t)z * leaves + x];
                    if (a < 0) continue;
                    const int dxs[2] = { 1, 0 }, dzs[2] = { 0, 1 };
                    for (int d = 0; d < 2; ++d)
                    {
                        uint32_t x2 = x + dxs[d], z2 = z + dzs[d];
                        if (x2 >= leaves || z2 >= leaves) continue;
                        int b = lvl[(size_t)z2 * leaves + x2];
                        if (b < 0) continue;
                        if (std::abs(a - b) > 1) { if (failures++ < 5) printf("level jump %d vs %d (k=%.0f)\n", a, b, k); }
                        if (a != b)
                        {
                            int fine = std::min(a, b);
                            // общая граница: вершины мелкого уровня с шагом его сетки
                            float step = qt.NodeSize(fine) / leafQ;
                            float L = qt.NodeSize(0);
                            float bx0 = ox + x2 * L, bz0 = oz + z2 * L;   // начало общего ребра
                            for (int i = 0; i <= (int)(L / step + 0.5f); ++i)
                            {
                                float wx = d == 0 ? bx0 : bx0 + i * step;
                                float wz = d == 0 ? bz0 + i * step : bz0;
                                wx = std::min(wx, ox + qt.ExtentX()); wz = std::min(wz, oz + qt.ExtentZ());
                                float wy = heightAt(wx, wz);
                                double dist = std::sqrt(double(wx - ex) * (wx - ex) + double(wy - ey) * (wy - ey) + double(wz - ez) * (wz - ez));
                                const float* m = qt.MorphConsts(fine);
                                double mk = 1.0 - std::clamp(m[0] - dist * m[1], 0.0, 1.0);
                                if (mk < 0.999) { if (failures++ < 5) printf("morph %.3f at boundary L%d/L%d\n", mk, a, b); }
                            }
                        }
                    }
                }
        }
    }
    printf(failures ? "FAILED: %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
