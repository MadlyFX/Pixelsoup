Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
cbuffer MotionConstants : register(b0, space3) { float4 extent; float4 motion; };

// A separable maximum finds motion reaching stationary pixels outside the
// current silhouette. Ties keep the first canvas-ordered sample, independent
// of the node's local origin. The authored radius bounds both scans and apron.
float4 main(float4 position : SV_Position) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const int2 axis = extent.w > 0.5 ? int2(0, 1) : int2(1, 0);
    float2 best = 0.0;
    float best_length = 0.0;
    [loop] for (int offset = -int(extent.z); offset <= int(extent.z); ++offset) {
        const int2 at = pixel + axis * offset;
        if (any(at < 0) || any(at >= int2(extent.xy))) continue;
        float2 vector = source.Load(int3(at, 0)).xy;
        if (extent.w < 0.5) {
            vector *= motion.xy * (0.5 * motion.z);
            const float magnitude = length(vector);
            vector *= magnitude > extent.z ? extent.z / magnitude : 1.0;
        }
        const float magnitude_squared = dot(vector, vector);
        if (magnitude_squared > best_length) {
            best = vector;
            best_length = magnitude_squared;
        }
    }
    return float4(best, 0, 0);
}
