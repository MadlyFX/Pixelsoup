// Asset Viewer inspection over a finished display image.
//
// Display-only: the picture the wall shows never passes through here. The
// source is already in display code values, so an exposure offset decodes
// them, scales the light, and encodes again; a single channel or luma is
// shown as grey. Same-size input and output with a nearest sampler, so each
// output pixel reads exactly the display pixel under it.

Texture2D<float4> image : register(t0, space2);
SamplerState image_sampler : register(s0, space2);

cbuffer InspectionConstants : register(b0, space3) {
    // 0 RGB, 1 red, 2 green, 3 blue, 4 luma.
    uint channel;
    // Linear-light multiplier: two to the exposure offset in stops.
    float gain;
    float2 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 decode_srgb(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow(max((encoded + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded));
}

float3 encode_srgb(float3 linear_colour) {
    const float3 low = linear_colour * 12.92;
    const float3 high = 1.055 * pow(max(linear_colour, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(low, high, step(0.0031308, linear_colour));
}

float4 main(Input input) : SV_Target0 {
    const float3 shown = gain == 1.0
        ? saturate(image.Sample(image_sampler, input.uv).rgb)
        : encode_srgb(saturate(
              decode_srgb(saturate(image.Sample(image_sampler, input.uv).rgb))
              * gain));
    if (channel == 1u) {
        return float4(shown.rrr, 1.0);
    }
    if (channel == 2u) {
        return float4(shown.ggg, 1.0);
    }
    if (channel == 3u) {
        return float4(shown.bbb, 1.0);
    }
    if (channel == 4u) {
        // Rec. 709 luma over the display code values, as a waveform reads it.
        const float luma = dot(shown, float3(0.2126, 0.7152, 0.0722));
        return float4(luma, luma, luma, 1.0);
    }
    return float4(shown, 1.0);
}
