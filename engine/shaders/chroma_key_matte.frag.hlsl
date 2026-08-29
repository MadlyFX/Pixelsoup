// Chroma-key matte, calculated from the unpremultiplied Rec.709 colour before
// spill suppression. Cb/Cr distance is Euclidean in the conventional
// (B-Y)/1.8556, (R-Y)/1.5748 coordinates.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);

cbuffer ChromaKeyMatteConstants : register(b0, space3) {
    float3 key_colour;
    float tolerance;
    float softness;
    float3 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float luminance_709(float3 rgb) {
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float2 cbcr_709(float3 rgb) {
    const float y = luminance_709(rgb);
    return float2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748);
}

float4 main(Input input) : SV_Target0 {
    const float4 premultiplied = source.Sample(source_sampler, input.uv);
    if (premultiplied.a <= 0.0) {
        return 0.0.xxxx;
    }
    const float3 rgb = saturate(premultiplied.rgb / premultiplied.a);
    const float distance_to_key = length(cbcr_709(rgb) - cbcr_709(key_colour));
    const float matte = softness == 0.0
        ? (distance_to_key <= tolerance ? 0.0 : 1.0)
        : smoothstep(tolerance, tolerance + softness, distance_to_key);
    return matte.xxxx;
}
