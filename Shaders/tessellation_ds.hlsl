struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

struct HSOutput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    uint TexIndex : TEXINDEX;
};

struct DSOutput
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 TangentW : TANGENT;
    uint TexIndex : TEXINDEX;
};

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2DArray gHeightMaps : register(t2);
SamplerState gSampler : register(s0);

cbuffer cbTessellation : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float3 gEyePosW;
    float gTessellationFactor;
    float gDisplacementScale;
};

[domain("tri")]
DSOutput main(
    PatchTess patchTess,
    float3 bary : SV_DomainLocation,
    const OutputPatch<HSOutput, 3> tri)
{
    DSOutput output;

    float3 position = bary.x * tri[0].Position +
                       bary.y * tri[1].Position +
                       bary.z * tri[2].Position;

    float3 normal = normalize(
        bary.x * tri[0].Normal +
        bary.y * tri[1].Normal +
        bary.z * tri[2].Normal);

    float2 texCoord = bary.x * tri[0].TexCoord +
                       bary.y * tri[1].TexCoord +
                       bary.z * tri[2].TexCoord;

    float3 tangent = normalize(
        bary.x * tri[0].Tangent +
        bary.y * tri[1].Tangent +
        bary.z * tri[2].Tangent);
    
    uint index = tri[0].TexIndex;
    float height = gHeightMaps.SampleLevel(gSampler, float3(texCoord, tri[0].TexIndex), 0.0f).r;
    float displacement = (height - 0.5f) * gDisplacementScale;
    position += normal * displacement;

    float4 worldPos = mul(float4(position, 1.0f), gWorld);
    output.PositionW = worldPos.xyz;
    output.PositionH = mul(worldPos, gWorldViewProj);

    output.NormalW = normalize(mul(normal, (float3x3) gWorldInvTranspose));
    output.TangentW = normalize(mul(tangent, (float3x3) gWorld));
    output.TexCoord = texCoord;
    output.TexIndex = index;
    
    return output;
}