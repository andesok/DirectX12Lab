struct PSInput
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

// ============================================
// G-BUFFER ТЕКСТУРЫ
// ============================================
Texture2D gAlbedoMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gPositionMap : register(t2);
SamplerState gSampler : register(s0);

// ============================================
// КОНСТАНТЫ
// ============================================
struct Light
{
    float3 Strength;
    int Type;
    float3 Direction;
    float pad0;
    float3 Position;
    float Range;
    float SpotPower;
    float FalloffStart;
    float FalloffEnd;
    float pad1;
};

StructuredBuffer<Light> gLights : register(t3);

cbuffer cbPass : register(b0)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float3 gEyePosW;
    float pad0;
    float4 gAmbientLight;
    int gNumLights;
    float pad1;
    float pad2;
    float pad3;
};

// ============================================
// КАСКАДНЫЕ ТЕНИ
// ============================================
Texture2DArray<float> gShadowMap : register(t4);          // слой = каскад
SamplerComparisonState gShadowSamplerPCF : register(s1);  // линейное сравнение (аппаратный PCF 2x2)
SamplerComparisonState gShadowSamplerPoint : register(s2); // точечное сравнение (без PCF)

cbuffer cbCascades : register(b1)
{
    float4x4 gLightViewProj[4];
    float4 gSplitDepths;      // дальняя граница каждого каскада (глубина в view-space)
    float4 gTexelWorldSize;   // размер текселя каскада в мировых единицах
    float3 gShadowLightDir;   // нормализованное направление света
    float gShadowTexelUV;     // 1 / размер карты
    int gShadowsEnabled;
    int gPcfRadius;           // 0 = без PCF, 1 = 3x3, 2 = 5x5, 3 = 7x7
    int gShowCascades;
    int gCascadeCount;
};

static const float3 kCascadeColors[4] =
{
    float3(1.0f, 0.35f, 0.35f),
    float3(0.35f, 1.0f, 0.35f),
    float3(0.35f, 0.35f, 1.0f),
    float3(1.0f, 1.0f, 0.35f)
};

float SelectComponent(float4 v, int i)
{
    return i == 0 ? v.x : (i == 1 ? v.y : (i == 2 ? v.z : v.w));
}

// Номер каскада по глубине пикселя в пространстве камеры.
// Считаем, сколько границ пиксель уже перешёл.
int SelectCascade(float3 posW)
{
    float viewDepth = mul(float4(posW, 1.0f), gView).z;
    float4 passed = float4(viewDepth.xxxx > gSplitDepths);
    return (int)dot(passed, float4(1.0f, 1.0f, 1.0f, 1.0f));
}

// PCF: усреднение результатов сравнения по окну (2r+1)x(2r+1).
// Каждая выборка через линейный comparison-сэмплер — это ещё и билинейный 2x2.
float SampleShadow(float2 uv, float depth, int cascade)
{
    if (gPcfRadius <= 0)
        return gShadowMap.SampleCmpLevelZero(gShadowSamplerPoint, float3(uv, cascade), depth);

    float sum = 0.0f;
    float count = 0.0f;
    [loop]
    for (int y = -gPcfRadius; y <= gPcfRadius; ++y)
    {
        [loop]
        for (int x = -gPcfRadius; x <= gPcfRadius; ++x)
        {
            float2 offset = float2(x, y) * gShadowTexelUV;
            sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerPCF, float3(uv + offset, cascade), depth);
            count += 1.0f;
        }
    }
    return sum / count;
}

// 1 = освещено, 0 = в тени
float ComputeShadow(float3 posW, float3 N, int cascade)
{
    if (gShadowsEnabled == 0 || cascade >= gCascadeCount)
        return 1.0f;

    // Normal offset: сдвигаем точку вдоль нормали на ~размер текселя,
    // сильнее на скользящих углах — борьба с shadow acne.
    float ndotl = saturate(dot(N, -gShadowLightDir));
    float texelWorld = SelectComponent(gTexelWorldSize, cascade);
    float3 posOffset = posW + N * texelWorld * (0.5f + 1.5f * (1.0f - ndotl));

    float4 posL = mul(float4(posOffset, 1.0f), gLightViewProj[cascade]);
    posL.xyz /= posL.w;

    // NDC -> UV (ось V направлена вниз)
    float2 uv = float2(posL.x * 0.5f + 0.5f, -posL.y * 0.5f + 0.5f);
    float depth = posL.z;

    if (any(uv < 0.0f) || any(uv > 1.0f) || depth > 1.0f)
        return 1.0f;

    return SampleShadow(uv, depth, cascade);
}

// ============================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================

// Затухание точечного света
float Attenuation(float dist, float range, float falloffStart, float falloffEnd)
{
    if (dist > range)
        return 0.0f;
    
    // Линейное затухание
    float att = 1.0f - saturate((dist - falloffStart) / (falloffEnd - falloffStart));
    return att * att;
}

// ============================================
// ВЫЧИСЛЕНИЕ НАПРАВЛЕННОГО СВЕТА
// ============================================
float3 ComputeDirectionalLight(Light L, float3 normal, float3 toEye, float3 albedo)
{
    float3 lightVec = -L.Direction;
    float ndotl = max(dot(lightVec, normal), 0.0f);
    
    float3 halfVec = normalize(lightVec + toEye);
    float spec = pow(max(dot(normal, halfVec), 0.0f), 64.0f);
    
    float3 diffuse = L.Strength * albedo * ndotl;
    float3 specular = L.Strength * spec * 0.5f;
    
    return diffuse + specular;
}

// ============================================
// ВЫЧИСЛЕНИЕ ТОЧЕЧНОГО СВЕТА
// ============================================
float3 ComputePointLight(Light L, float3 posW, float3 normal, float3 toEye, float3 albedo)
{
    float3 lightVec = L.Position - posW;
    float dist = length(lightVec);
    lightVec /= dist;
    
    float att = Attenuation(dist, L.Range, L.FalloffStart, L.FalloffEnd);
    if (att < 0.001f)
        return 0.0f;
    
    float ndotl = max(dot(lightVec, normal), 0.0f);
    float3 halfVec = normalize(lightVec + toEye);
    float spec = pow(max(dot(normal, halfVec), 0.0f), 64.0f);
    
    float3 diffuse = L.Strength * albedo * ndotl * att;
    float3 specular = L.Strength * spec * 0.5f * att;
    
    return diffuse + specular;
}

// ============================================
// ВЫЧИСЛЕНИЕ ПРОЖЕКТОРА
// ============================================
float3 ComputeSpotLight(Light L, float3 posW, float3 normal, float3 toEye, float3 albedo)
{
    float3 lightVec = L.Position - posW;
    float dist = length(lightVec);
    lightVec /= dist;
    
    float att = Attenuation(dist, L.Range, L.FalloffStart, L.FalloffEnd);
    if (att < 0.001f)
        return 0.0f;
    
    // Конус прожектора
    float spot = pow(max(dot(-lightVec, L.Direction), 0.0f), L.SpotPower);
    if (spot < 0.001f)
        return 0.0f;
    
    float ndotl = max(dot(lightVec, normal), 0.0f);
    float3 halfVec = normalize(lightVec + toEye);
    float spec = pow(max(dot(normal, halfVec), 0.0f), 64.0f);
    
    float3 diffuse = L.Strength * albedo * ndotl * att * spot;
    float3 specular = L.Strength * spec * 0.5f * att * spot;
    
    return diffuse + specular;
}

// ============================================
// ОСНОВНАЯ ФУНКЦИЯ
// ============================================
float4 main(PSInput input) : SV_Target
{
    // ============================================
    // 1. ЧИТАЕМ ИЗ G-BUFFER
    // ============================================
    float4 albedo = gAlbedoMap.Sample(gSampler, input.TexCoord);
    float4 normal = gNormalMap.Sample(gSampler, input.TexCoord);
    float4 position = gPositionMap.Sample(gSampler, input.TexCoord);
    
    // Если альбедо чёрное — пиксель пустой
    if (dot(albedo.rgb, float3(1, 1, 1)) < 0.001f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 N = normalize(normal.xyz * 2.0f - 1.0f);
    float3 posW = position.xyz;
    float3 V = normalize(gEyePosW - posW);
    
    // ============================================
    // 2. AMBIENT (ФОНОВОЕ ОСВЕЩЕНИЕ)
    // ============================================
    float3 result = gAmbientLight.rgb * albedo.rgb;

    // Тень от главного направленного света (gLights[0])
    int cascade = SelectCascade(posW);
    float shadow = ComputeShadow(posW, N, cascade);
    
    // ============================================
    // 3. ПРОХОДИМ ПО ВСЕМ ИСТОЧНИКАМ СВЕТА
    // ============================================
    for (int i = 0; i < gNumLights; i++)
    {
        Light L = gLights[i];
        
        if (L.Type == 0)  // Направленный
        {
            float s = (i == 0) ? shadow : 1.0f;
            result += s * ComputeDirectionalLight(L, N, V, albedo.rgb);
        }
        else if (L.Type == 1)  // Точечный
        {
            result += ComputePointLight(L, posW, N, V, albedo.rgb);
        }
        else if (L.Type == 2)  // Прожектор
        {
            result += ComputeSpotLight(L, posW, N, V, albedo.rgb);
        }
    }
    
    // ============================================
    // 4. TONEMAPPING (HDR -> LDR)
    // ============================================
    result = result / (result + 1.0f);

    // Отладка: подкрашиваем каскады разными цветами
    if (gShowCascades != 0 && cascade < gCascadeCount)
    {
        result *= kCascadeColors[cascade];
    }
    
    return float4(result, 1.0f);
}