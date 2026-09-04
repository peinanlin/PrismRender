struct OceanPatchVertex
{
    float3 position : POSITION;
};
struct OceanPatchFactors
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};
[shader("domain")]
[domain("tri")]
[partitioning("fractional_odd")]
float4 OceanDomainMain(OceanPatchFactors factors,
    const OutputPatch<OceanPatchVertex, 3> patch,
    float3 barycentric : SV_DomainLocation) : SV_Position
{
    float3 position = patch[0].position * barycentric.x
        + patch[1].position * barycentric.y
        + patch[2].position * barycentric.z;
    return float4(position, 1.0f);
}
