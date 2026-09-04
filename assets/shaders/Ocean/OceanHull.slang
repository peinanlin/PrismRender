struct OceanPatchVertex
{
    float3 position : POSITION;
};
struct OceanPatchFactors
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};
OceanPatchFactors OceanPatchConstants(InputPatch<OceanPatchVertex, 3> patch)
{
    OceanPatchFactors factors;
    factors.edge[0] = 1.0f;
    factors.edge[1] = 1.0f;
    factors.edge[2] = 1.0f;
    factors.inside = 1.0f;
    return factors;
}
[shader("hull")]
[domain("tri")]
[outputtopology("triangle_cw")]
[partitioning("fractional_odd")]
[patchconstantfunc("OceanPatchConstants")]
[outputcontrolpoints(3)]
OceanPatchVertex OceanHullMain(InputPatch<OceanPatchVertex, 3> patch,
    uint controlPoint : SV_OutputControlPointID)
{
    return patch[controlPoint];
}
