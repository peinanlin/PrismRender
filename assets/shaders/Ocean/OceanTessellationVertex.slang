struct OceanTessVertex
{
    float3 position : POSITION;
};

[shader("vertex")]
OceanTessVertex OceanTessellationVS(uint vertexId : SV_VertexID)
{
    static const float3 positions[3] = {
        float3(-0.65f, -0.55f, 0.0f),
        float3(0.65f, -0.55f, 0.0f),
        float3(0.0f, 0.65f, 0.0f)};
    OceanTessVertex output;
    output.position = positions[vertexId % 3u];
    return output;
}
