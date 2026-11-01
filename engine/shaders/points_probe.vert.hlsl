// One small square per instance, centred on a clip-space point read from a
// buffer. A test probe for indirect draws: the instance count comes from a
// buffer a dispatch wrote, so which squares appear shows the count was used.
// Pairs with solid.frag.hlsl.

ByteAddressBuffer points : register(t0);

static const float2 corners[6] = {
    float2(-1.0F, -1.0F), float2(1.0F, -1.0F), float2(1.0F, 1.0F),
    float2(-1.0F, -1.0F), float2(1.0F, 1.0F), float2(-1.0F, 1.0F),
};

float4 main(uint vertex : SV_VertexID, uint instance : SV_InstanceID) : SV_Position {
    const float2 centre = asfloat(points.Load2(instance * 8U));
    return float4(centre + corners[vertex] * 0.1F, 0.0F, 1.0F);
}
