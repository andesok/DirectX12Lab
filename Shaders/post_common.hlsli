// Общие объявления для всех проходов пост-обработки

struct PSInput
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

SamplerState gPointClamp : register(s0);
SamplerState gLinearClamp : register(s1);

// Совпадает со struct PostConstants в PostProcess.h
cbuffer cbPost : register(b0)
{
    float3 gEyePosW;
    float gDebugDepthRange;

    float gBloomThreshold;
    float gBloomKnee;
    float gBloomIntensity;
    float gChromaticStrength;

    int gDebugView;
    int gBloomEnabled;
    int gChromaticEnabled;
    int postPad0;
};

// b1: параметры конкретного прохода (root constants)
cbuffer cbPass : register(b1)
{
    float2 gBlurDirection;   // (1,0) — по X, (0,1) — по Y
    float2 gTexelSize;       // 1 / размер bloom-текстуры
};

float Luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}
