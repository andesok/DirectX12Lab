struct PSInput
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 TangentW : TANGENT;
    uint TexIndex : TEXINDEX;
};

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

Texture2DArray gAlbedoMaps : register(t0);
Texture2DArray gNormalMaps : register(t1);
Texture2DArray gHeightMaps : register(t2);
SamplerState gSampler : register(s0);

PSOutput main(PSInput input)
{
    PSOutput output;

    uint index = input.TexIndex;
    
    float4 albedo = gAlbedoMaps.Sample(gSampler, float3(input.TexCoord, index));
    
    float3 normalMapSample = gNormalMaps.Sample(gSampler, float3(input.TexCoord, index)).rgb;
    float3 normalMap = normalMapSample * 2.0f - 1.0f;

    float3 N = normalize(input.NormalW);
    float3 T = normalize(input.TangentW - dot(input.TangentW, N) * N);
    float3 B = cross(N, T);

    float3x3 TBN = float3x3(T, B, N);
    float3 finalNormal = normalize(mul(normalMap, TBN));

    output.Albedo = albedo;
    output.Normal = float4(finalNormal, 0.0f);
    output.Position = float4(input.PositionW, 1.0f);

    return output;
}