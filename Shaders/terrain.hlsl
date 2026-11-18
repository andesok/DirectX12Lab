// =====================================================================
// terrain.hlsl — ландшафт (ДЗ Terrain)
//   * одна общая сетка (GridDim x GridDim квадов) для ВСЕХ узлов квадродерева;
//   * высота читается из карты высот в вершинном шейдере;
//   * CDLOD-морфинг: у границы уровня вершины плавно «съезжают» на сетку
//     вдвое реже => нет трещин и «прыжков» при смене LOD;
//   * результат пишется в тот же G-Buffer, что и Sponza (альбедо / нормаль / позиция).
// =====================================================================

cbuffer cbTerrain : register(b0)
{
    float4x4 gViewProj;

    float3   gEyePosW;          // позиция, от которой выбирались LOD (при заморозке F — замороженная)
    float    gHeightScale;

    float2   gOrigin;           // мировые XZ текселя (0,0)
    float    gBaseY;            // мировая Y для высоты 0
    float    gTexelWorld;       // расстояние между текселями карты высот в мире

    float2   gExtent;           // размер ландшафта в мире (X, Z)
    float2   gInvHeightmapSize; // 1 / (ширина, высота) карты высот

    float    gGridDim;          // квадов в стороне сетки (32)
    float    gHalfGridDim;      // gGridDim / 2
    float    gTwoOverGridDim;   // 2 / gGridDim
    int      gDebugLod;         // 1 — раскрасить уровни LOD

    int      gHasColorMap;      // 1 — есть цветовая карта из генератора
    float3   gPad;
};

// Корневые константы — свои для каждого узла
cbuffer cbNode : register(b1)
{
    float2 gNodeOffset;         // мировой угол узла (min X, min Z)
    float  gNodeSize;           // сторона узла
    float  gNodeLod;            // уровень (0 — самый детальный)
    float2 gMorphConsts;        // (end / (end - start), 1 / (end - start))
    float2 gNodePad;
};

Texture2D<float>  gHeightMap : register(t0);
Texture2D<float4> gNormalMap : register(t1);
Texture2D<float4> gColorMap  : register(t2);

SamplerState gLinearClamp : register(s0);

struct VertexIn
{
    float2 Grid : POSITION;     // [0,1] x [0,1] внутри узла
};

struct VertexOut
{
    float4 PosH   : SV_POSITION;
    float3 PosW   : POSITION;
    float2 UV     : TEXCOORD0;
    nointerpolation float Lod : TEXCOORD1;
    float  Morph  : TEXCOORD2;
};

// Мировые XZ -> UV карты высот. Тексель i лежит ровно в gOrigin + i * gTexelWorld,
// поэтому +0.5 попадает в центр текселя и вершины разных уровней совпадают.
float2 HeightUV(float2 worldXZ)
{
    return ((worldXZ - gOrigin) / gTexelWorld + 0.5f) * gInvHeightmapSize;
}

float SampleHeight(float2 worldXZ)
{
    // Всегда mip 0: соседние уровни должны видеть одинаковые высоты на общих вершинах
    return gBaseY + gHeightMap.SampleLevel(gLinearClamp, HeightUV(worldXZ), 0.0f) * gHeightScale;
}

// Сдвиг вершины на сетку вдвое реже: нечётные вершины съезжают к чётным
float2 MorphVertex(float2 grid, float morphK)
{
    float2 fracPart = frac(grid * gHalfGridDim) * gTwoOverGridDim;
    return grid - fracPart * morphK;
}

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    // 1. Позиция без морфинга — только чтобы узнать расстояние до камеры
    float2 worldXZ = gNodeOffset + vin.Grid * gNodeSize;
    float  height  = SampleHeight(worldXZ);
    float  dist    = distance(gEyePosW, float3(worldXZ.x, height, worldXZ.y));

    // 2. morphK: 0 — до начала зоны морфинга, 1 — на дальней границе уровня
    float morphK = 1.0f - saturate(gMorphConsts.x - dist * gMorphConsts.y);

    // 3. Итоговая позиция
    float2 grid = MorphVertex(vin.Grid, morphK);
    worldXZ = gNodeOffset + grid * gNodeSize;
    worldXZ = clamp(worldXZ, gOrigin, gOrigin + gExtent);   // узлы у края карты частично «за краем»
    height  = SampleHeight(worldXZ);

    float3 posW = float3(worldXZ.x, height, worldXZ.y);

    vout.PosW  = posW;
    vout.PosH  = mul(float4(posW, 1.0f), gViewProj);
    vout.UV    = HeightUV(worldXZ);
    vout.Lod   = gNodeLod;
    vout.Morph = morphK;
    return vout;
}

// ---------------------------------------------------------------------
// Пиксельный шейдер -> G-Buffer
// ---------------------------------------------------------------------
struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

static const float3 kLodColors[8] =
{
    float3(1.0f, 0.2f, 0.2f),
    float3(1.0f, 0.6f, 0.1f),
    float3(1.0f, 1.0f, 0.2f),
    float3(0.2f, 1.0f, 0.2f),
    float3(0.2f, 1.0f, 1.0f),
    float3(0.2f, 0.4f, 1.0f),
    float3(0.7f, 0.3f, 1.0f),
    float3(1.0f, 0.3f, 0.8f)
};

// Цвет, если генератор не дал цветовую карту: песок / трава / камень / снег
float3 ProceduralColor(float h01, float slopeY)
{
    const float3 sand  = float3(0.62f, 0.56f, 0.40f);
    const float3 grass = float3(0.20f, 0.33f, 0.11f);
    const float3 rock  = float3(0.36f, 0.33f, 0.30f);
    const float3 snow  = float3(0.92f, 0.93f, 0.96f);

    float3 c = lerp(sand, grass, smoothstep(0.03f, 0.08f, h01));
    c = lerp(c, rock, 1.0f - smoothstep(0.62f, 0.80f, slopeY));          // крутой склон
    c = lerp(c, snow, smoothstep(0.62f, 0.72f, h01) * smoothstep(0.55f, 0.75f, slopeY));
    return c;
}

GBufferOut PS(VertexOut pin)
{
    // Карта нормалей хранит мировую нормаль в [0,1]
    float3 n = normalize(gNormalMap.Sample(gLinearClamp, pin.UV).xyz * 2.0f - 1.0f);

    float h01 = saturate((pin.PosW.y - gBaseY) / gHeightScale);
    float3 albedo = gHasColorMap != 0
        ? gColorMap.Sample(gLinearClamp, pin.UV).rgb
        : ProceduralColor(h01, n.y);

    if (gDebugLod != 0)
    {
        uint lod = (uint)pin.Lod;
        float3 lc = kLodColors[lod % 8];
        // Зона морфинга видна как переход к цвету следующего уровня
        float3 next = kLodColors[(lod + 1) % 8];
        albedo = lerp(albedo, lerp(lc, next, pin.Morph * 0.5f), 0.65f);
    }

    GBufferOut o;
    o.Albedo   = float4(albedo, 1.0f);
    o.Normal   = float4(n, 1.0f);       // если твой G-Buffer хранит нормаль как n*0.5+0.5 — поменяй здесь
    o.Position = float4(pin.PosW, 1.0f);
    return o;
}
