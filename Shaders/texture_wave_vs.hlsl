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

VertexOut main(VertexIn vin)
{
    VertexOut vout;
    
    float3 posL = vin.PosL;
    
    float wave = sin(gTime * 5.0f + vin.PosL.x * 0.5f) * 4.0f;
    posL.y = posL.y + wave;
    
    vout.PosH = mul(float4(posL, 1.0f), gWorldViewProj);
    vout.TexCoord = vin.TexCoord;
    return vout;
}
