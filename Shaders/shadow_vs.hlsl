// Проход теней: только глубина с точки зрения света (пиксельного шейдера нет)

cbuffer cbShadowObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbShadowPass : register(b1)
{
    float4x4 gLightViewProj;   // ViewProj текущего каскада
};

struct VSInput
{
    float3 Pos : POSITION;
};

float4 main(VSInput input) : SV_POSITION
{
    float4 posW = mul(float4(input.Pos, 1.0f), gWorld);
    return mul(posW, gLightViewProj);
}
