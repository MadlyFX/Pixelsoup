// Matte-first Chroma Key composition. Spill suppression removes only the
// positive component along the key's Rec.709 chroma direction, after matte
// calculation. Final alpha is always gated by source alpha.

Texture2D<float4> source : register(t0, space2);
SamplerState source_sampler : register(s0, space2);
Texture2D<float4> matte_source : register(t1, space2);
SamplerState matte_sampler : register(s1, space2);

cbuffer ChromaKeyComposeConstants : register(b0, space3) {
    float3 key_colour;
    float spill_suppression;
    float key_mix;
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

float3 rgb_709(float y, float2 chroma) {
    const float red = y + 1.5748 * chroma.y;
    const float blue = y + 1.8556 * chroma.x;
    const float green = (y - 0.2126 * red - 0.0722 * blue) / 0.7152;
    return saturate(float3(red, green, blue));
}

float4 main(Input input) : SV_Target0 {
    const float4 premultiplied = source.Sample(source_sampler, input.uv);
    const float matte = saturate(matte_source.Sample(matte_sampler, input.uv).a);
    float3 rgb = premultiplied.a > 0.0
        ? saturate(premultiplied.rgb / premultiplied.a)
        : 0.0.xxx;

    const float2 key_chroma = cbcr_709(key_colour);
    const float key_length = length(key_chroma);
    if (key_length > 1.0e-8) {
        const float y = luminance_709(rgb);
        float2 chroma = cbcr_709(rgb);
        const float2 key_direction = key_chroma / key_length;
        const float projection = max(dot(chroma, key_direction), 0.0);
        const float amount = spill_suppression * key_mix * (1.0 - matte);
        chroma -= key_direction * projection * amount;
        rgb = rgb_709(y, chroma);
    }

    const float final_alpha = premultiplied.a * lerp(1.0, matte, key_mix);
    return float4(rgb * final_alpha, final_alpha);
}
