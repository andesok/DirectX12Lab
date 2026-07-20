struct VSInput
{
    float3 Pos : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
};

cbuffer cbObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    float4 worldPos = mul(float4(input.Pos, 1.0f), gWorld);
    output.PosW = worldPos.xyz;
    output.PosH = mul(float4(input.Pos, 1.0f), gWorldViewProj);
    output.NormalW = mul(input.Normal, (float3x3) gWorld);
    output.TexCoord = input.TexCoord;

    return output;
}