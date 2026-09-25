#pragma once

#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <cmath>

// Результат проверки объёма относительно пирамиды видимости
enum class FrustumTest
{
    Outside,    // полностью снаружи  -> отбрасываем
    Intersect,  // пересекает границу -> нужна детальная проверка
    Inside      // полностью внутри   -> всё содержимое видно
};

// Пирамида видимости из 6 плоскостей.
// Плоскость хранится как (n.x, n.y, n.z, d), нормаль смотрит ВНУТРЬ:
// точка p внутри, если dot(n, p) + d >= 0.
class Frustum
{
public:
    // Метод Gribb/Hartmann: плоскости извлекаются прямо из матрицы View*Proj.
    // Конвенция DirectX: вектор-строка (clip = p * M), z_clip в [0, w].
    void ExtractFromViewProj(DirectX::FXMMATRIX viewProj)
    {
        using namespace DirectX;

        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, viewProj);

        // Столбцы матрицы: col_j = (m._1j, m._2j, m._3j, m._4j)
        const XMFLOAT4 c1(m._11, m._21, m._31, m._41);
        const XMFLOAT4 c2(m._12, m._22, m._32, m._42);
        const XMFLOAT4 c3(m._13, m._23, m._33, m._43);
        const XMFLOAT4 c4(m._14, m._24, m._34, m._44);

        auto add = [](const XMFLOAT4& a, const XMFLOAT4& b) { return XMFLOAT4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); };
        auto sub = [](const XMFLOAT4& a, const XMFLOAT4& b) { return XMFLOAT4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); };

        mPlanes[0] = add(c4, c1); // Left:   -w <= x
        mPlanes[1] = sub(c4, c1); // Right:   x <= w
        mPlanes[2] = add(c4, c2); // Bottom: -w <= y
        mPlanes[3] = sub(c4, c2); // Top:     y <= w
        mPlanes[4] = c3;          // Near:    0 <= z
        mPlanes[5] = sub(c4, c3); // Far:     z <= w

        for (XMFLOAT4& p : mPlanes)
        {
            const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if (len > 1e-6f)
            {
                p.x /= len; p.y /= len; p.z /= len; p.w /= len;
            }
        }
    }

    // Тест AABB (центр + полуразмеры) против 6 плоскостей.
    FrustumTest TestAABB(const DirectX::BoundingBox& box) const
    {
        const DirectX::XMFLOAT3& c = box.Center;
        const DirectX::XMFLOAT3& e = box.Extents;

        FrustumTest result = FrustumTest::Inside;
        for (const DirectX::XMFLOAT4& p : mPlanes)
        {
            // Расстояние от центра до плоскости
            const float dist = p.x * c.x + p.y * c.y + p.z * c.z + p.w;
            // Проекция полуразмеров на нормаль ("радиус" коробки вдоль нормали)
            const float radius = e.x * std::fabs(p.x) + e.y * std::fabs(p.y) + e.z * std::fabs(p.z);

            if (dist < -radius)
                return FrustumTest::Outside;   // целиком за плоскостью
            if (dist < radius)
                result = FrustumTest::Intersect;
        }
        return result;
    }

    bool IsVisible(const DirectX::BoundingBox& box) const
    {
        return TestAABB(box) != FrustumTest::Outside;
    }

private:
    DirectX::XMFLOAT4 mPlanes[6] = {};
};