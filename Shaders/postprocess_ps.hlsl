// ============================================
// ФИНАЛЬНЫЙ ПРОХОД ПОСТ-ОБРАБОТКИ
// Вход: G-Buffer (t0..t2), итог lighting pass (t3), размытый bloom (t4)
// Эффекты: хроматическая аберрация, bloom
// ============================================

#include "post_common.hlsli"

// ---- G-Buffer (та же раскладка, что в deferred_ps.hlsl) ----
Texture2D gAlbedoMap : register(t0);     // rgb = цвет поверхности
Texture2D gNormalMap : register(t1);     // xyz = N * 0.5 + 0.5
Texture2D gPositionMap : register(t2);   // xyz = мировая позиция, w = 1 (0 — пусто)

// ---- Результат освещения и размытый bloom ----
Texture2D gSceneColor : register(t3);
Texture2D gBloom : register(t4);

// ============================================
// ЗАГОТОВКА: чтение G-Buffer
// ============================================
struct GBufferData
{
    float3 Albedo;
    float3 Normal;     // уже распакованная, [-1, 1]
    float3 PosW;
    bool Valid;        // false — в пикселе нет геометрии
};

GBufferData DecodeGBuffer(float4 albedo, float4 normal, float4 position)
{
    GBufferData g;
    g.Albedo = albedo.rgb;
    g.Normal = normalize(normal.xyz * 2.0f - 1.0f);
    g.PosW = position.xyz;
    g.Valid = position.w > 0.5f;
    return g;
}

// По UV (для текущего пикселя)
GBufferData SampleGBuffer(float2 uv)
{
    return DecodeGBuffer(
        gAlbedoMap.Sample(gPointClamp, uv),
        gNormalMap.Sample(gPointClamp, uv),
        gPositionMap.Sample(gPointClamp, uv));
}

// По целочисленным координатам текселя (для эффектов, которым нужны соседи)
GBufferData LoadGBuffer(int2 texel)
{
    int3 t = int3(texel, 0);
    return DecodeGBuffer(gAlbedoMap.Load(t), gNormalMap.Load(t), gPositionMap.Load(t));
}

// ============================================
// ЭФФЕКТ 1: ХРОМАТИЧЕСКАЯ АБЕРРАЦИЯ
// Линза преломляет волны разной длины по-разному: красный и синий каналы
// смещаются в разные стороны от центра, сильнее к краям кадра.
// ============================================
float2 ChromaticOffset(float2 uv)
{
    float2 fromCenter = uv - 0.5f;       // 0 в центре, до ±0.5 на краях
    return fromCenter * gChromaticStrength * 2.0f;
}

float3 SampleChromatic(Texture2D tex, float2 uv)
{
    if (gChromaticEnabled == 0)
        return tex.Sample(gLinearClamp, uv).rgb;

    float2 offset = ChromaticOffset(uv);
    float r = tex.Sample(gLinearClamp, uv + offset).r;   // красный — наружу
    float g = tex.Sample(gLinearClamp, uv).g;            // зелёный — на месте
    float b = tex.Sample(gLinearClamp, uv - offset).b;   // синий — к центру
    return float3(r, g, b);
}

// ============================================
// MAIN
// ============================================
float4 main(PSInput input) : SV_Target
{
    float2 uv = input.TexCoord;

    // ---- Отладочный просмотр G-Buffer и bloom ----
    if (gDebugView != 0)
    {
        GBufferData g = SampleGBuffer(uv);

        if (gDebugView == 1) return float4(g.Albedo, 1.0f);
        if (gDebugView == 2) return float4(g.Valid ? g.Normal * 0.5f + 0.5f : float3(0.0f, 0.0f, 0.0f), 1.0f);
        if (gDebugView == 3)
        {
            float d = g.Valid ? saturate(distance(g.PosW, gEyePosW) / gDebugDepthRange) : 1.0f;
            return float4(d.xxx, 1.0f);
        }
        return float4(gBloom.Sample(gLinearClamp, uv).rgb, 1.0f);   // 4: только bloom
    }

    // ЭФФЕКТ 1: хроматическая аберрация — при чтении исходного изображения
    float3 color = SampleChromatic(gSceneColor, uv);

    // ЭФФЕКТ 2: bloom — свечение прибавляется поверх (с той же аберрацией)
    if (gBloomEnabled != 0)
        color += SampleChromatic(gBloom, uv) * gBloomIntensity;

    return float4(saturate(color), 1.0f);
}
