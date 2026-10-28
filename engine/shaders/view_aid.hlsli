// An editor view's surface aids, shared by the lit and clip stages. Each
// returns the colour the screen should show, display-referred (sRGB-encoded),
// so the caller linearises it for a scene-linear target exactly as it would a
// picture. view_mode.x selects them: 4 UV checker, 5 world normals.

#ifndef MEDIASERVER_VIEW_AID_HLSLI
#define MEDIASERVER_VIEW_AID_HLSLI

static const float view_aid_uv_checker = 4.0;
static const float view_aid_world_normals = 5.0;

// An eight-by-eight checker over each unit of UV, red rising with U and green
// with V, so a stretched, flipped or tiled mapping reads at a glance. The
// dark squares keep the same tint at a third of the level.
float3 view_aid_checker(float2 uv) {
    const float2 unit = frac(uv);
    const float2 cell = floor(unit * 8.0);
    const float odd = fmod(cell.x + cell.y, 2.0);
    const float3 light = float3(0.25 + 0.75 * unit.x, 0.25 + 0.75 * unit.y, 0.85);
    return lerp(light, light * 0.35, odd);
}

// x, y and z from -1..1 to 0..1.
float3 view_aid_normal(float3 normal) {
    return saturate(normalize(normal) * 0.5 + 0.5);
}

// Whether view_mode.x names one of these aids.
bool view_aid_active(float mode) {
    return mode > 3.5;
}

float3 view_aid_colour(float mode, float2 uv, float3 normal) {
    return mode > 4.5 ? view_aid_normal(normal) : view_aid_checker(uv);
}

#endif
