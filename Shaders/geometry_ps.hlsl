struct PSInput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct PSOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 Position : SV_Target2;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

PSOutput main(PSInput input)
{
    PSOutput output;

    float4 texColor = gDiffuseMap.Sample(gSampler, input.TexCoord);
    output.Albedo = float4(texColor.rgb, 1.0f);
    output.Normal = float4(normalize(input.NormalW), 0.0f);
    output.Position = float4(input.PosW, 1.0f);

    return output;
}