struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexCoord : TEXCOORD;
};

VertexOut main(uint vertexID : SV_VertexID)
{
    VertexOut output;
    
    float2 texCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.TexCoord = float2(texCoord.x, 1.0f - texCoord.y);
    output.PosH = float4(texCoord * 2.0f - 1.0f, 0.0f, 1.0f);
    
    return output;
}