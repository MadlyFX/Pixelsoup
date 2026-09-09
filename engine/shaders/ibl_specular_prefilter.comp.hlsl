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
    float4 sun_direction_radius;
    float4 sun_irradiance;
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
    float3 filtered = total_weight > 0.0F
        ? accumulated / total_weight
        : environment.SampleLevel(environment_sampler, normal, 0.0F).rgb;
    if (sun_direction_radius.w > 0 && any(sun_irradiance.rgb > 0)) {
        const float radius = sun_direction_radius.w;
        const float3 axis = sun_direction_radius.xyz;
        if (roughness <= 0.0) {
            const float angle = atan2(length(cross(normal,axis)),dot(normal,axis));
            const float footprint = 2.0 / float(mip_extent.x);
            const float coverage = saturate((radius-angle)/footprint+0.5)
                * min(1.0,kPi*radius*radius/(footprint*footprint));
            filtered += sun_irradiance.rgb * coverage / max(kPi*sin(radius)*sin(radius),1e-12);
        } else {
            // Integrate the small bright source directly. Importance sampling
            // the whole hemisphere can miss a sub-texel sun completely.
            const float3 up = abs(axis.y) < 0.99 ? float3(0,1,0) : float3(1,0,0);
            const float3 tangent = normalize(cross(up,axis));
            const float3 bitangent = cross(axis,tangent);
            float integral = 0;
            [loop] for (uint i=0; i<32; ++i) {
                float2 q = ibl_hammersley(i,32);
                float c = 1.0-q.x*(1.0-cos(radius));
                float s = sqrt(max(1.0-c*c,0.0));
                float3 l = axis*c+(tangent*cos(2*kPi*q.y)+bitangent*sin(2*kPi*q.y))*s;
                float nl = max(dot(normal,l),0.0);
                float nh = sqrt(max((1.0+dot(normal,l))*0.5,0.0));
                float a = roughness*roughness;
                float denominator = nh*nh*(a*a-1.0)+1.0;
                float distribution = a*a/max(kPi*denominator*denominator,1e-12);
                integral += distribution*nl*0.25;
            }
            // Same normalized N.L convolution as the environment samples.
            float normalization = max(total_weight/float(sample_count),1e-6);
            filtered += sun_irradiance.rgb * (2.0/(1.0+cos(radius))) * integral/(32.0*normalization);
        }
    }
    mip_target[thread.xy] = float4(sun_direction_radius.w > 0 ? clamp(filtered,0.0,65504.0) : filtered, 1.0F);
}
