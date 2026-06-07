cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float gTime;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float2 TexCoord : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    
    float3 posL = vin.PosL;
    float distanceFromCenter = length(vin.PosL.xz);
    float waveAmount = sin(gTime * 2.0f + distanceFromCenter * 0.5f) * 2.0f;
    posL.y += waveAmount;

    vout.PosH = mul(float4(posL, 1.0f), gWorldViewProj);
    
    vout.TexCoord = vin.TexCoord;
    return vout;
}
