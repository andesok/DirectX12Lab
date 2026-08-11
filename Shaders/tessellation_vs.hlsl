struct VSInput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    uint TexIndex : TEXINDEX;
};

struct VSOutput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    uint TexIndex : TEXINDEX;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = input.Position;
    output.Normal = input.Normal;
    output.TexCoord = input.TexCoord;
    output.Tangent = input.Tangent;
    output.TexIndex = input.TexIndex;
    return output;
}