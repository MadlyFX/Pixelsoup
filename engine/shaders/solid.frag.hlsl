// Opaque white, and nothing else.
//
// The companion to solid.vert.hlsl: a draw whose output is a single known
// value, so a test can say exactly which pixels the rasterizer touched. Any
// pixel that is not this colour was not covered, which is the whole question a
// viewport test asks.
float4 main() : SV_Target {
    return float4(1.0F, 1.0F, 1.0F, 1.0F);
}
