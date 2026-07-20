#pragma once

#include <DirectXMath.h>
#include "../framework/MathHelper.h"
#include <vector>

enum LightType : int
{
    LIGHT_TYPE_DIRECTIONAL = 0,  // Направленный
    LIGHT_TYPE_POINT = 1,        // Точечный
    LIGHT_TYPE_SPOT = 2          // Прожектор
};

struct Light
{
    DirectX::XMFLOAT3 Strength = { 0.8f, 0.8f, 0.8f };
    int Type = LIGHT_TYPE_DIRECTIONAL;

    DirectX::XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f };
    float pad0 = 0.0f;

    DirectX::XMFLOAT3 Position = { 0.0f, 10.0f, 0.0f };
    float Range = 50.0f;

    float SpotPower = 64.0f;     // Угол конуса
    float FalloffStart = 0.0f;   // Начало затухания
    float FalloffEnd = 1.0f;     // Конец затухания
    float pad1 = 0.0f;           // Выравнивание
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float pad0 = 0.0f;
    DirectX::XMFLOAT4 AmbientLight = { 0.05f, 0.05f, 0.05f, 1.0f };
    int NumLights = 0;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
    float pad3 = 0.0f;
    //Light Lights[16];
};