Texture2D gAlbedoMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gPositionMap : register(t2);
SamplerState gSampler : register(s0);

struct PixelIn
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

float4 main(PixelIn pin) : SV_Target
{
    float4 albedo = gAlbedoMap.Sample(gSampler, pin.TexCoord);
    
    return albedo;

    float4 normalData = gNormalMap.Sample(gSampler, pin.TexCoord);
    float4 positionData = gPositionMap.Sample(gSampler, pin.TexCoord);

    float3 normal = normalize(normalData.rgb * 2.0f - 1.0f);
    
    float3 lightDir = normalize(float3(1.0f, 1.0f, -1.0f));
    float3 lightColor = float3(1.0f, 1.0f, 1.0f);
    float3 ambient = float3(0.1f, 0.1f, 0.1f);
    
    float diffuse = max(dot(normal, lightDir), 0.0f);
    float3 finalColor = albedo.rgb * (ambient + diffuse * lightColor);
    
    return float4(finalColor, 1.0f);
}