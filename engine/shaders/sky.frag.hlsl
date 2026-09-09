// The environment drawn as the background.
//
// Not decoration. On a volume the wall behind the subject *is* the light
// hitting it, so a flat sky clear behind a scene lit by an HDRI is a
// contradiction the camera sees: the reflection in a chrome surface shows a
// room that the wall does not.
//
// Runs as a fullscreen triangle before the lit pass, sharing that pass's vertex
// stage, so it costs one draw and no geometry.
cbuffer SkyConstants : register(b0, space3) {
    // Clip to world, for reconstructing a view ray per pixel.
    column_major float4x4 inverse_view_projection;
    // xy: canvas pixels per target pixel. z: environment intensity. w: exposure, applied the
    // same way the lit pass applies it so the sky and the scene agree.
    float4 sky_parameters;
    // xy: complete projection raster; zw: this draw's absolute origin.
    float4 sky_canvas;
    float4 sky_fallback;
    float4 sun_direction_radius[4];
    float4 sun_radiance[4];
};

TextureCube<float4> environment : register(t0, space2);
SamplerState environment_sampler : register(s0, space2);

float3 sky_ray(float2 pixel) {
    // Pixel centre to normalised device coordinates. The y flip is the usual
    // one between a top-left pixel origin and a bottom-up clip space.
    const float2 uv = (pixel * sky_parameters.xy + sky_canvas.zw) / max(sky_canvas.xy, 1.0);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    // Reversed depth puts the far plane at zero, which is where the sky is.
    const float4 far = mul(inverse_view_projection, float4(ndc, 0.0, 1.0));
    const float4 near = mul(inverse_view_projection, float4(ndc, 1.0, 1.0));
    return normalize(
        (far.xyz / far.w) - (near.xyz / near.w));
}

float4 main(float4 position : SV_Position) : SV_Target {
    const float3 direction = sky_ray(position.xy);
    float3 radiance = sky_fallback.w > 0.5
        ? environment.SampleLevel(environment_sampler, direction, 0.0).rgb * sky_parameters.z
        : sky_fallback.rgb;
    // Explicit neighbouring rays make edge coverage independent of quad or
    // slice placement. The same wall pixel performs the same arithmetic.
    const float footprint = max(max(length(sky_ray(position.xy + float2(1, 0)) - direction),
                                     length(sky_ray(position.xy + float2(0, 1)) - direction)), 1e-7);
    [unroll] for (uint i = 0; i < 4; ++i) {
        const float radius = sun_direction_radius[i].w;
        if (radius <= 0.0) continue;
        const float3 axis = sun_direction_radius[i].xyz;
        const float angle = atan2(length(cross(direction, axis)), dot(direction, axis));
        const float coverage = saturate((radius - angle) / footprint + 0.5);
        const float subpixel = min(1.0, 3.14159265359 * radius * radius / (footprint * footprint));
        radiance += sun_radiance[i].rgb * coverage * subpixel;
    }
    // Scene-linear, premultiplied, exposed - exactly what the lit pass writes.
    //
    // Not tone mapped and not encoded: the Advanced target is a linear RGBA16F
    // working buffer and the display transform belongs to the output stage. A
    // sky that tone mapped here would be transformed twice, which reads as a
    // background that has gone nearly black while every lit surface beside it
    // is correct.
    return float4(clamp(radiance * sky_parameters.w, 0.0, 65504.0), 1.0);
}
