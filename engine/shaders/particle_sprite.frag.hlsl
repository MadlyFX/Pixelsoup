// ajfx particle sprites, fragment stage: the emitter's texture (or flipbook
// cell) times the particle's premultiplied colour, or a soft disc when the
// emitter has no texture. Blended by the pipeline (premultiplied over, or
// additive) and depth-tested against the scene without writing depth.
//
// sRGB textures are decoded after filtering, as the scene's model textures
// are: this RHI samples eight-bit textures as stored.

Texture2D<float4> sprite_texture : register(t0, space2);
SamplerState sprite_sampler : register(s0, space2);

float3 decode_srgb(float3 encoded) {
    const float3 low = encoded / 12.92;
    const float3 high = pow(max((encoded + 0.055) / 1.055, 0.0), 2.4);
    return lerp(low, high, step(0.04045, encoded));
}

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 cells : TEXCOORD1;
    float blend : TEXCOORD2;
    nointerpolation uint flags : TEXCOORD3;
    float4 color : COLOR0;
};

float4 main(Input input) : SV_Target {
    if ((input.flags & 1U) != 0U) {
        float4 texel = lerp(sprite_texture.Sample(sprite_sampler, input.cells.xy),
                            sprite_texture.Sample(sprite_sampler, input.cells.zw), input.blend);
        if ((input.flags & 4U) != 0U) {
            texel.rgb = decode_srgb(texel.rgb);
        }
        return input.color * float4(texel.rgb * texel.a, texel.a);
    }
    const float coverage = saturate(1.0F - dot(input.uv, input.uv));
    return input.color * coverage;
}
