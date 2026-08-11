struct VSOutput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    uint TexIndex : TEXINDEX;
};

struct HSOutput
{
    float3 Position : POSITION;
    float3 Normal : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 Tangent : TANGENT;
    uint TexIndex : TEXINDEX;
};

struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

cbuffer cbTessellation : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float3 gEyePosW;
    float gTessellationFactor;
    float gDisplacementScale;
};

PatchTess ConstantHS(InputPatch<VSOutput, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess output;

    float3 centerL = (patch[0].Position + patch[1].Position + patch[2].Position) / 3.0f;
    float3 centerW = mul(float4(centerL, 1.0f), gWorld).xyz;
    float dist = distance(centerW, gEyePosW);

    float minDist = 0.5f;
    float maxDist = 50.0f;
    float maxTess = 64.0f;
    float minTess = 1.0f;

    float tess = maxTess - (maxTess - minTess) * saturate((dist - minDist) / (maxDist - minDist));
    tess = clamp(round(tess), minTess, maxTess);

    output.EdgeTess[0] = tess;
    output.EdgeTess[1] = tess;
    output.EdgeTess[2] = tess;
    output.InsideTess = tess;

    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
HSOutput main(
    InputPatch<VSOutput, 3> patch,
    uint i : SV_OutputControlPointID,
    uint patchID : SV_PrimitiveID)
{
    HSOutput output;
    output.Position = patch[i].Position;
    output.Normal = patch[i].Normal;
    output.TexCoord = patch[i].TexCoord;
    output.Tangent = patch[i].Tangent;
    output.TexIndex = patch[i].TexIndex;
    return output;
}