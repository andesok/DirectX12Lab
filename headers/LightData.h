#pragma once
#include <DirectXMath.h>

enum class LightType : int
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

struct Light
{
    DirectX::XMFLOAT3 Position;   // Point, Spot
    float             Padding0;
    DirectX::XMFLOAT3 Direction;  // Directional, Spot
    float             Padding1;
    DirectX::XMFLOAT3 Color;
    float             Intensity;
    float             Range;      // Point, Spot
    float             SpotInner;  // Spot — косинус внутреннего угла
    float             SpotOuter;  // Spot — косинус внешнего угла
    int               Type;       // LightType
};

struct LightConstants
{
    Light   Lights[16];
    int     LightCount;
    DirectX::XMFLOAT3 CameraPos;
    DirectX::XMFLOAT3 Padding;
    float   Padding2;
};