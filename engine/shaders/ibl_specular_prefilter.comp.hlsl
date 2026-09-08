// One mip of one face of the prefiltered specular environment.
//
// Each mip is the environment convolved with the GGX lobe at a fixed
// roughness, so a rough metal samples a blurred mip and a mirror samples mip
// zero. This is the pass that gives metals something to reflect - without it
// there is no specular ambient term at all and every chrome surface in a shot
// reads as flat grey.
#include "ibl_common.hlsli"

cbuffer PrefilterParameters : register(b0, space1) {
    uint2 mip_extent;
    uint face_index;
    uint sample_count;
    float roughness;
    float source_face_size;
    uint prefilter_padding0;
    uint prefilter_padding1;
};

TextureCube<float4> environment : register(t0);
SamplerState environment_sampler : register(s0);
RWTexture2D<float4> mip_target : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    if (thread.x >= mip_extent.x || thread.y >= mip_extent.y) {
        return;
    }
    const float2 uv = (float2(thread.xy) + 0.5F) / float2(mip_extent);
    const float3 normal = ibl_face_direction(face_index, uv);
    // The usual split-sum simplification: the view direction is assumed to be
    // the normal. It is wrong at grazing angles and it is what makes a single
    // prefiltered chain usable for every view, which is the trade every
    // real-time implementation makes.
    const float3 view = normal;

    float3 accumulated = float3(0.0F, 0.0F, 0.0F);
    float total_weight = 0.0F;
    for (uint index = 0U; index < sample_count; ++index) {
        const float2 sample_point = ibl_hammersley(index, sample_count);
        const float3 half_vector =
            ibl_importance_sample_ggx(sample_point, normal, roughness);
        const float3 light =
            normalize(2.0F * dot(view, half_vector) * half_vector - view);
        const float n_dot_l = dot(normal, light);
        if (n_dot_l <= 0.0F) {
            continue;
        }
        // Sampling a lower mip of the source in proportion to the sample's
        // solid angle is what keeps a rough mip from aliasing into fireflies -
        // bright single texels that survive the convolution and then flicker.
        const float n_dot_h = max(dot(normal, half_vector), 0.0F);
        const float v_dot_h = max(dot(view, half_vector), 0.0F);
        const float a = roughness * roughness;
        const float d = (n_dot_h * a - n_dot_h) * n_dot_h + 1.0F;
        const float distribution = (a * a) / max(kPi * d * d, 1.0e-8F);
        const float pdf =
            (distribution * n_dot_h / max(4.0F * v_dot_h, 1.0e-8F)) + 1.0e-4F;
        const float texel_solid_angle =
            4.0F * kPi / (6.0F * source_face_size * source_face_size);
        const float sample_solid_angle =
            1.0F / (float(sample_count) * pdf);
        const float level = roughness <= 0.0F
            ? 0.0F
            : 0.5F * log2(sample_solid_angle / texel_solid_angle);

        accumulated += environment.SampleLevel(
                           environment_sampler, light, max(level, 0.0F)).rgb
            * n_dot_l;
        total_weight += n_dot_l;
    }
    const float3 filtered = total_weight > 0.0F
        ? accumulated / total_weight
        : environment.SampleLevel(environment_sampler, normal, 0.0F).rgb;
    mip_target[thread.xy] = float4(filtered, 1.0F);
}
