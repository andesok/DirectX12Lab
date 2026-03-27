Texture2D gDiffuseMap : register(t0);
SamplerState gSampler : register(s0);

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

float4 PS(VertexOut pin) : SV_Target
{
    return gDiffuseMap.Sample(gSampler, pin.TexCoord);
}