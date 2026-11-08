// Volumetric fog, scattering stage. One froxel in, one (in-scatter, extinction)
// out.
//
// Every node computes the **whole** grid, not just the froxels its own slice
// covers. That is deliberate: 160x90x64 is under a million cells and cheap, and
// computing the identical volume everywhere removes the coverage bookkeeping
// from the frame path entirely - there is no arithmetic left for two nodes to
// disagree about. `render::plan_froxel_grid` still reports coverage, and
// restricting the dispatch to it is a pure optimisation that cannot change the
// answer.
//
// The lights come from the same storage buffer the lit draw reads, and are
// evaluated with the same falloff, the same cone and the same shadow lookup.
// That is the point: a practical has to light the air the way it lights the
// surfaces, and be occluded by the same wall. Two implementations of one
// falloff would drift, and the drift would read as fog that does not belong to
// the lamp it came from.

#include "fog_common.hlsli"

// Matches `GpuLight` in scene_renderer_internal.hpp and `Light` in scene.frag.hlsl.
#include "light.hlsli"
#include "ies.hlsli"
#include "area_shape.hlsli"

// Matches `GpuLightShadow` / `LightShadow`.
struct FogLightShadow {
    column_major float4x4 view_projection;
    // xy: tile origin in atlas texels. z: tile extent. w: one over the extent.
    float4 tile;
};

// Storage buffers follow the sampled textures in the same space, so with one
// sampled texture declared the first buffer is t1. That ordering is the root
// signature's, not a convention this shader chose.
Texture2D<float> shadow_atlas : register(t0);
SamplerState shadow_atlas_sampler : register(s0);
Texture2D<float4> environment_sh : register(t1);
SamplerState environment_sampler : register(s1);
Texture2D<float> ies_atlas : register(t2);
StructuredBuffer<Light> lights : register(t3);
StructuredBuffer<FogLightShadow> light_shadows : register(t4);
RWTexture2D<float4> scatter : register(u0);

// The same lookup `spot_shadow_visibility` performs for a surface, with one
// difference: a froxel has no normal, so there is no slope term. The bias is
// the constant part alone.
//
// Without this a practical lights the air straight through the wall it is
// mounted on - and unlike the surface case, where the wall at least stays dark,
// the fog would glow *in front of* the wall and give the whole set a haze that
// no lamp in the room could account for.
float fog_spot_shadow(uint row, float3 world_position) {
    const FogLightShadow record = light_shadows[row];
    const float4 clip =
        mul(record.view_projection, float4(world_position, 1.0));
    if (!(clip.w > 0.0)) {
        return 1.0;
    }
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0) {
        return 1.0;
    }

    uint atlas_width;
    uint atlas_height;
    shadow_atlas.GetDimensions(atlas_width, atlas_height);
    const float2 atlas_extent =
        max(float2((float)atlas_width, (float)atlas_height), 1.0);
    const float2 origin = record.tile.xy / atlas_extent;
    const float2 span = record.tile.z / atlas_extent;
    const float receiver = min(ndc.z + fog_shadow.x * record.tile.w, 1.0);

    float visible = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            // Clamped inside the tile, not the atlas: a kernel running off one
            // tile would read its neighbour's map, which is one light casting
            // another's shadow.
            const float2 local = clamp(
                uv + float2((float)x, (float)y) / max(record.tile.z, 1.0),
                0.0, 1.0);
            const float stored = shadow_atlas.SampleLevel(
                shadow_atlas_sampler, origin + local * span, 0.0);
            visible += stored <= receiver ? 1.0 : 0.0;
        }
    }
    return visible / 9.0;
}

[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    const int3 grid = int3(max(fog_grid.xyz, float3(1.0, 1.0, 1.0)));
    // One thread per column and row; the depth axis is a loop, so the whole ray
    // for a pixel is written by one thread and the integration that follows
    // reads it in order.
    const int2 cell = int2(thread.xy);
    if (cell.x >= grid.x || cell.y >= grid.y) {
        return;
    }

    const float2 canvas_uv = (float2(cell) + 0.5) / float2(grid.xy);
    const float3 eye = fog_froxel_world(canvas_uv, fog_planes.x);
    const uint light_count = (uint)max(fog_shadow.z, 0.0);

    for (int slice = 0; slice < grid.z; ++slice) {
        // The centre of the slab, which the integration treats as
        // representative of it.
        const float depth = fog_slice_depth(float(slice) + 0.5);
        const float3 position = fog_froxel_world(canvas_uv, depth);
        const float3 view_direction = normalize(position - eye);

        // The zeroth radiance SH coefficient gives the spherical mean.
        // This isotropic ambient term keeps HDRI-only air from being a pure
        // absorber; directional/local lights retain their phase and shadows.
        float3 inscatter = float3(0.0, 0.0, 0.0);
        if (fog_environment.x > 0.0) {
            inscatter = max(environment_sh.Load(int3(0, 0, 0)).rgb, 0.0)
                * (0.2820947918 * fog_environment.x);
        }
        for (uint index = 0U; index < light_count; ++index) {
            const Light light = lights[index];
            if (light.kind == LIGHT_RECTANGLE || light.kind == LIGHT_SPHERE) {
                if (light.ies.w < -1.5) continue;
                const float fade = area_range_weight(light, position);
                if (fade <= 0.0) continue;
                float scattering = 0.0;
                [loop] for (uint sample_index = 0u; sample_index < 64u; ++sample_index) {
                    float3 direction;
                    const float weight = area_sample(light, position, sample_index, direction);
                    scattering += weight * fog_phase(dot(view_direction, direction), fog_planes.w);
                }
                inscatter += light.color_intensity.rgb * (light.color_intensity.a * scattering * fade);
                continue;
            }
            float3 to_light;
            float attenuation = 1.0;

            if (light.kind != LIGHT_DIRECTIONAL) {
                const float3 offset = light.vector - position;
                const float distance = length(offset);
                const float range = max(light.params.x, 1e-3);
                if (distance >= range) {
                    continue;
                }
                to_light = offset / max(distance, 1e-5);
                // Inverse square windowed to reach zero at the stated range,
                // character for character what the lit draw does.
                const float falloff = saturate(1.0 - (distance / range));
                attenuation =
                    falloff * falloff / max(distance * distance, 1e-4);
                attenuation *= light_ies_weight(ies_atlas, light, -to_light);

                if (light.kind == LIGHT_SPOT) {
                    const float alignment = dot(-to_light, light.spot.xyz);
                    const float cone = light_cone_weight(light, alignment);
                    if (cone <= 0.0) {
                        continue;
                    }
                    attenuation *= cone;
                    const int shadow_row = (int)light.params.w;
                    if (shadow_row >= 0) {
                        attenuation *=
                            fog_spot_shadow((uint)shadow_row, position);
                    }
                }
            } else {
                to_light = -normalize(light.vector);
            }

            const float3 radiance =
                light.color_intensity.rgb * light.color_intensity.a
                * attenuation;
            inscatter += radiance * fog_phase(dot(view_direction, to_light),
                                              fog_planes.w);
        }

        // Extinction at this cell's height: full at and below the authored
        // base, thinning exponentially above it. Per cell, so the slab
        // integration that follows still sees a constant within each froxel.
        const float extinction = fog_extinction_at(position);
        // Albedo multiplies what the air returns; extinction is what it
        // removes. Keeping them separate is why coloured haze and coloured
        // smoke look different rather than being one tint.
        scatter[fog_atlas_texel(int3(cell, slice))] =
            float4(inscatter * fog_albedo.rgb * extinction, extinction);
    }
}
