// One visibility equation shared by media colour and every Advanced raster
// product. Keeping it here prevents alpha/opacity/edge semantics from drifting
// between independently compiled fragment stages.

static const float media_depth_only_coverage_cutoff = 0.5;

float media_composed_coverage(
    float sampled_alpha, float2 uv, float4 layer_style) {
    const float edge_distance = min(uv.x, 1.0 - uv.x);
    const float horizontal_coverage = layer_style.y > 0.0
        ? smoothstep(0.0, layer_style.y, edge_distance)
        : 1.0;
    return saturate(
        layer_style.x * sampled_alpha * horizontal_coverage);
}
