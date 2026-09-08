Texture2D<float4> color_texture : register(t0, space2);
SamplerState color_sampler : register(s0, space2);
Texture2D<float4> motion_texture : register(t1, space2);
SamplerState motion_sampler : register(s1, space2);
Texture2D<float4> neighborhood_texture : register(t2, space2);
SamplerState neighborhood_sampler : register(s2, space2);
cbuffer MotionConstants : register(b0, space3) { float4 extent; float4 motion; };

float2 shutter_vector(float2 velocity) {
    float2 vector = velocity * motion.xy * (0.5 * motion.z);
    const float magnitude = length(vector);
    return vector * (magnitude > extent.z ? extent.z / magnitude : 1.0);
}

float4 main(float4 position : SV_Position) : SV_Target0 {
    const int2 pixel = int2(position.xy);
    const float4 center = color_texture.Load(int3(pixel, 0));
    const float4 center_motion = motion_texture.Load(int3(pixel, 0));
    const float2 vector = neighborhood_texture.Load(int3(pixel, 0)).xy;
    if (dot(vector, vector) < 0.25) return center;
    const bool moving = dot(shutter_vector(center_motion.xy), shutter_vector(center_motion.xy)) >= 0.25;
    float4 sum = 0.0;
    // Fixed 33 midpoint samples of a centred rectangular shutter. Integer
    // texel offsets preserve identical weights on independently padded nodes.
    // Visible source reconstruction cannot reveal geometry hidden this frame.
    [unroll] for (int tap = 0; tap < 33; ++tap) {
        const float time = (float(tap) + 0.5) * (2.0 / 33.0) - 1.0;
        const int2 delta = int2(round(vector * time));
        const int2 at = pixel + delta;
        if (any(at < 0) || any(at >= int2(extent.xy))) continue;
        const float4 sample_motion = motion_texture.Load(int3(at, 0));
        const float2 sample_vector = shutter_vector(sample_motion.xy);
        const float length_squared = dot(sample_vector, sample_vector);
        const float along = dot(float2(delta), sample_vector) / max(length_squared, 1.0e-6);
        const float2 perpendicular = float2(delta) - along * sample_vector;
        const bool reaches = length_squared >= 0.25 && abs(along) <= 1.0
            && dot(perpendicular, perpendicular) <= 0.5;
        // A static foreground is a real occluder. A moving foreground can mix
        // with visible background; background motion never crosses nearer depth.
        const bool in_front = sample_motion.z >= center_motion.z - 1.0e-6;
        const bool accept = (reaches && in_front)
            || (moving && sample_motion.z <= center_motion.z + 1.0e-6);
        if (accept) sum += color_texture.Load(int3(at, 0)) - center;
    }
    return center + sum / 33.0;
}
