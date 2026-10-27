// One visibility equation shared by media colour and every Advanced raster
// product. Keeping it here prevents alpha/opacity/edge semantics from drifting
// between independently compiled fragment stages.

static const float media_depth_only_coverage_cutoff = 0.5;

// A blended layer's own depth pass (layer_style.w == 2) keeps only the pixels
// that hide what is behind them completely: anything an 8-bit alpha would
// still store as fully opaque. Lit layers apply the same cutoff.
static const float blended_depth_coverage_cutoff = 1.0 - 0.5 / 255.0;

float media_composed_coverage(
    float sampled_alpha, float2 uv, float4 layer_style) {
    const float edge_distance = min(uv.x, 1.0 - uv.x);
    const float horizontal_coverage = layer_style.y > 0.0
        ? smoothstep(0.0, layer_style.y, edge_distance)
        : 1.0;
    return saturate(
        layer_style.x * sampled_alpha * horizontal_coverage);
}
