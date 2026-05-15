cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float gTime; // Получаем время из C++
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
    vout.PosH = mul(float4(vin.PosL, 1.0f), gWorldViewProj);

    // ТАЙЛИНГ
    float2 tex = vin.TexCoord * 1.0f;

    vout.TexCoord = tex;
    return vout;
}