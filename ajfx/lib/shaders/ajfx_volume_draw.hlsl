// The reference volume raymarch: one full-screen triangle per grid volume,
// whose pixels march a fixed number of steps through the grid's box, taking
// the density as absorption and scattering one directional light towards
// the camera through a fixed number of steps of their own. Nothing in it
// depends on the order anything runs in, so a frame is the same frame every
// time. Output is premultiplied for the backend's blend.

cbuffer AjfxVolumeDraw : register(b0, space1) {
    uint vdraw_grid;        // the grid's cells, in words (density; temperature and velocity after)
    uint vdraw_nx;
    uint vdraw_ny;
    uint vdraw_nz;
    float vdraw_origin_x;
    float vdraw_origin_y;
    float vdraw_origin_z;
    float vdraw_cell;
    float vdraw_extinction;
    float vdraw_albedo;
    float vdraw_color_r;
    float vdraw_color_g;
    float vdraw_color_b;
    uint vdraw_steps;       // steps along the view ray
    uint vdraw_light_steps; // steps towards the light from each of them
    uint vdraw_unused;
};

RWByteAddressBuffer ajfx_volumes : register(u0);
// The sprite renderer's camera (ajfx_sprite.hlsl): rows 7 to 10 the light,
// rows 11 to 14 the inverse of the view projection.
RWByteAddressBuffer vdraw_camera : register(u1);

#include "ajfx_volume.hlsli"

struct VolumeVertex {
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

float4 vdraw_row(uint row) {
    return asfloat(vdraw_camera.Load4(row * 16u));
}

VolumeVertex volume_vertex(uint vertex : SV_VertexID) {
    const float2 corners[3] = {float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    VolumeVertex output;
    output.position = float4(corners[vertex], 0.5f, 1.0f);
    output.ndc = corners[vertex];
    return output;
}

// The point at normalised device coordinates (ndc, depth) in the effect's
// space.
float3 vdraw_unproject(float2 ndc, float depth) {
    float4 clip = float4(ndc, depth, 1.0f);
    float4 world = float4(dot(vdraw_row(11u), clip), dot(vdraw_row(12u), clip), dot(vdraw_row(13u), clip),
                          dot(vdraw_row(14u), clip));
    return world.xyz / world.w;
}

// The density at `p`, trilinear between cell centres and clamped to the
// edge; zero outside the box.
float vdraw_density(float3 p) {
    float3 origin = float3(vdraw_origin_x, vdraw_origin_y, vdraw_origin_z);
    float3 u = (p - origin) * (1.0f / vdraw_cell);
    if (!(u.x >= 0.0f && u.y >= 0.0f && u.z >= 0.0f && u.x <= (float)vdraw_nx && u.y <= (float)vdraw_ny &&
          u.z <= (float)vdraw_nz)) {
        return 0.0f;
    }
    AjfxVolumeTaps taps = ajfx_volume_taps(vdraw_nx, vdraw_ny, vdraw_nz, u - 0.5f);
    return max(ajfx_volume_sample_scalar(vdraw_grid, vdraw_nx, vdraw_ny, taps), 0.0f);
}

// Where the ray from `o` along `d` enters and leaves the grid's box; false
// when it misses.
bool vdraw_box(float3 o, float3 d, out float enter, out float leave) {
    float3 lo = float3(vdraw_origin_x, vdraw_origin_y, vdraw_origin_z);
    float3 hi = lo + float3((float)vdraw_nx, (float)vdraw_ny, (float)vdraw_nz) * vdraw_cell;
    enter = 0.0f;
    leave = 3.4e38f;
    [unroll] for (uint axis = 0u; axis < 3u; ++axis) {
        float dir = d[axis];
        if (abs(dir) < 1.0e-12f) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        float inv = 1.0f / dir;
        float t0 = (lo[axis] - o[axis]) * inv;
        float t1 = (hi[axis] - o[axis]) * inv;
        enter = max(enter, min(t0, t1));
        leave = min(leave, max(t0, t1));
    }
    return leave > enter;
}

float4 volume_pixel(VolumeVertex input) : SV_Target {
    float3 near = vdraw_unproject(input.ndc, 0.0f);
    float3 far = vdraw_unproject(input.ndc, 1.0f);
    float3 d = far - near;
    float span = length(d);
    if (!(span > 0.0f)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    d = d / span;
    float enter;
    float leave;
    if (!vdraw_box(near, d, enter, leave)) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 to_light = -normalize(vdraw_row(7u).xyz);
    float3 light_color = vdraw_row(8u).xyz;
    // The hemisphere, taken as what a point sees of it on average.
    float3 ambient = (vdraw_row(9u).xyz + vdraw_row(10u).xyz) * 0.5f;
    float3 tint = float3(vdraw_color_r, vdraw_color_g, vdraw_color_b);
    uint steps = max(vdraw_steps, 1u);
    uint light_steps = max(vdraw_light_steps, 1u);
    float ds = (leave - enter) / (float)steps;
    float transmittance = 1.0f;
    float3 scattered = float3(0.0f, 0.0f, 0.0f);
    for (uint k = 0u; k < steps; ++k) {
        float t = enter + ((float)k + 0.5f) * ds;
        float3 p = near + d * t;
        float density = vdraw_density(p);
        if (density <= 0.0f) {
            continue;
        }
        float through = exp(-density * vdraw_extinction * ds);
        // How much of the light reaches p: the density along the way out of
        // the box towards it.
        float light_enter;
        float light_leave;
        float reaching = 1.0f;
        if (vdraw_box(p, to_light, light_enter, light_leave)) {
            float dl = light_leave / (float)light_steps;
            float depth = 0.0f;
            for (uint m = 0u; m < light_steps; ++m) {
                depth += vdraw_density(p + to_light * (((float)m + 0.5f) * dl)) * vdraw_extinction * dl;
            }
            reaching = exp(-depth);
        }
        float3 light = light_color * reaching + ambient;
        scattered += transmittance * (1.0f - through) * vdraw_albedo * tint * light;
        transmittance *= through;
    }
    return float4(scattered, 1.0f - transmittance);
}
