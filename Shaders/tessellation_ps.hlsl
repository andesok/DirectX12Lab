struct PSInput
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 TangentW : TANGENT;
};

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

Texture2D gAlbedoMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);
SamplerState gSampler : register(s0);

PSOutput main(PSInput input)
{
    PSOutput output;

    // ============================================
    // ЧИТАЕМ ТЕКСТУРУ (ТОЛЬКО ОДНО ПРИСВОЕНИЕ!)
    // ============================================
    float4 diffuse = gAlbedoMap.Sample(gSampler, input.TexCoord);
    float3 N = normalize(input.NormalW);

    output.Albedo = diffuse; // ← ТОЛЬКО ОДИН РАЗ!
    output.Normal = float4(N, 0.0f);
    output.Position = float4(input.PositionW, 1.0f);

    return output;
}