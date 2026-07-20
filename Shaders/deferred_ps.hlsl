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
    
    float3 N = normalize(normal.xyz);
    float3 posW = position.xyz;
    float3 V = normalize(gEyePosW - posW);
    
    // ============================================
    // 2. AMBIENT (ФОНОВОЕ ОСВЕЩЕНИЕ)
    // ============================================
    float3 result = gAmbientLight.rgb * albedo.rgb;
    
    // ============================================
    // 3. ПРОХОДИМ ПО ВСЕМ ИСТОЧНИКАМ СВЕТА
    // ============================================
    for (int i = 0; i < gNumLights; i++)
    {
        Light L = gLights[i];
        
        if (L.Type == 0)  // Направленный
        {
            result += ComputeDirectionalLight(L, N, V, albedo.rgb);
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
    
    return float4(result, 1.0f);
}