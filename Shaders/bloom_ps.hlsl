// ============================================
// BLOOM: два прохода
//   Bright — выделение ярких областей SceneColor с уменьшением до 1/4 разрешения
//   Blur   — размытие Гаусса по одной оси (вызывается по X, потом по Y)
// ============================================
#include "post_common.hlsli"

Texture2D gInput : register(t3);   // вход прохода: SceneColor или BloomA/BloomB

// ---- 1. BRIGHT PASS ----
float4 Bright(PSInput input) : SV_Target
{
    // Один пиксель 1/4-текстуры покрывает 4x4 пикселя исходной.
    // 4 билинейные выборки со сдвигом ±1 тексель = усреднение всего блока 4x4
    // (без этого мелкие яркие детали мерцали бы при движении камеры)
    uint w, h;
    gInput.GetDimensions(w, h);
    float2 srcTexel = 1.0f / float2(w, h);

    float2 uv = input.TexCoord;
    float3 c =
        gInput.Sample(gLinearClamp, uv + srcTexel * float2(-1.0f, -1.0f)).rgb +
        gInput.Sample(gLinearClamp, uv + srcTexel * float2( 1.0f, -1.0f)).rgb +
        gInput.Sample(gLinearClamp, uv + srcTexel * float2(-1.0f,  1.0f)).rgb +
        gInput.Sample(gLinearClamp, uv + srcTexel * float2( 1.0f,  1.0f)).rgb;
    c *= 0.25f;

    // Порог с мягким «коленом»: ниже (threshold - knee) — 0,
    // выше threshold — линейно, между ними — плавная парабола (без резкой границы)
    float lum = Luminance(c);
    float soft = clamp(lum - gBloomThreshold + gBloomKnee, 0.0f, 2.0f * gBloomKnee);
    soft = soft * soft / (4.0f * gBloomKnee + 1e-5f);
    float contribution = max(soft, lum - gBloomThreshold) / max(lum, 1e-5f);

    return float4(c * contribution, 1.0f);
}

// ---- 2. РАЗМЫТИЕ ГАУССА (раздельное) ----
// 9 выборок: центр + по 4 в каждую сторону. Сумма весов = 1.
static const float kWeights[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };

float4 Blur(PSInput input) : SV_Target
{
    float2 uv = input.TexCoord;
    float2 texelStep = gBlurDirection * gTexelSize;

    float3 result = gInput.Sample(gLinearClamp, uv).rgb * kWeights[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        result += gInput.Sample(gLinearClamp, uv + texelStep * i).rgb * kWeights[i];
        result += gInput.Sample(gLinearClamp, uv - texelStep * i).rgb * kWeights[i];
    }
    return float4(result, 1.0f);
}
