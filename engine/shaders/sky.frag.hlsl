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
    // xy: target extent. z: environment intensity. w: exposure, applied the
    // same way the lit pass applies it so the sky and the scene agree.
    float4 sky_parameters;
};

TextureCube<float4> environment : register(t0, space2);
SamplerState environment_sampler : register(s0, space2);

float4 main(float4 position : SV_Position) : SV_Target {
    // Pixel centre to normalised device coordinates. The y flip is the usual
    // one between a top-left pixel origin and a bottom-up clip space.
    const float2 uv = position.xy / max(sky_parameters.xy, 1.0);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    // Reversed depth puts the far plane at zero, which is where the sky is.
    const float4 far = mul(inverse_view_projection, float4(ndc, 0.0, 1.0));
    const float4 near = mul(inverse_view_projection, float4(ndc, 1.0, 1.0));
    const float3 direction = normalize(
        (far.xyz / far.w) - (near.xyz / near.w));

    const float3 radiance =
        environment.SampleLevel(environment_sampler, direction, 0.0).rgb
        * sky_parameters.z;
    // Scene-linear, premultiplied, exposed - exactly what the lit pass writes.
    //
    // Not tone mapped and not encoded: the Advanced target is a linear RGBA16F
    // working buffer and the display transform belongs to the output stage. A
    // sky that tone mapped here would be transformed twice, which reads as a
    // background that has gone nearly black while every lit surface beside it
    // is correct.
    return float4(max(radiance * sky_parameters.w, 0.0), 1.0);
}
