#include "particles_common.hlsli"

// ============================================
// КОНСТАНТЫ И ДАННЫЕ
// ============================================
cbuffer cbParticleDraw : register(b0)
{
    float4x4 gViewProj;
    float3 gCameraRight;
    float pad0;
    float3 gCameraUp;
    float pad1;
    float3 gCameraLook;
    float pad2;
};

StructuredBuffer<Particle> gParticles : register(t0);

// ============================================
// VS: вершинного буфера нет — читаем частицу по номеру вершины
// ============================================
struct VSOut
{
    float3 Center : POSITION;
    float Size : SIZE;
    float3 Color : COLOR;
};

VSOut VS(uint vertexID : SV_VertexID)
{
    Particle p = gParticles[vertexID];

    VSOut o;
    o.Center = p.Position;
    o.Color = p.Color;

    // Частица уменьшается в последние 0.5 с жизни, а не исчезает резко
    float fade = saturate((p.Lifetime - p.Age) / 0.5f);
    o.Size = p.Size * fade;
    return o;
}

// ============================================
// GS: точка -> билборд (квадрат, повёрнутый к камере)
// ============================================
struct GSOut
{
    float4 PosH : SV_POSITION;
    float2 UV : TEXCOORD0;
    nointerpolation float3 Center : CENTER;
    nointerpolation float Size : SIZE;
    nointerpolation float3 Color : COLOR;
};

[maxvertexcount(4)]
void GS(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    if (input[0].Size <= 0.0f)
        return;

    float3 right = gCameraRight * input[0].Size;
    float3 up = gCameraUp * input[0].Size;

    // Порядок для triangle strip: TL, TR, BL, BR
    const float2 corners[4] =
    {
        float2(-1.0f, 1.0f),
        float2(1.0f, 1.0f),
        float2(-1.0f, -1.0f),
        float2(1.0f, -1.0f)
    };

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        float3 posW = input[0].Center + corners[i].x * right + corners[i].y * up;

        GSOut o;
        o.PosH = mul(float4(posW, 1.0f), gViewProj);
        o.UV = float2(corners[i].x * 0.5f + 0.5f, -corners[i].y * 0.5f + 0.5f);
        o.Center = input[0].Center;
        o.Size = input[0].Size;
        o.Color = input[0].Color;
        stream.Append(o);
    }
}

// ============================================
// PS: непрозрачная частица-«шарик» прямо в G-Buffer
// ============================================
struct PSOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

PSOut PS(GSOut input)
{
    // Координаты на диске [-1, 1]; всё вне круга отбрасываем — частица круглая
    float2 d = input.UV * 2.0f - 1.0f;
    d.y = -d.y;
    float r2 = dot(d, d);
    clip(1.0f - r2);

    // Нормаль сферы-импостора: освещение сделает частицу объёмным шариком
    float z = sqrt(1.0f - r2);
    float3 N = normalize(d.x * gCameraRight + d.y * gCameraUp - z * gCameraLook);

    PSOut o;
    o.Albedo = float4(input.Color, 1.0f);
    o.Normal = float4(N * 0.5f + 0.5f, 0.0f);
    // Точка на поверхности сферы — для корректного освещения и теней
    o.Position = float4(input.Center + N * input.Size, 1.0f);
    return o;
}
