// Inner frustum over outer frustum, with a feather between them.
//
// The outer frustum is the whole slice this node already rendered from the
// authored camera. The inner frustum is a second render of the same scene from
// the tracked camera's real viewpoint, covering only the rectangle the physical
// camera can see. This pass puts the second over the first.
//
// The transition is the delicate part. A hard edge between two renders of the
// same geometry from two viewpoints is a visible line on the wall, and a camera
// pointed at it records that line permanently. So the inner render fades in
// across `feather` pixels inside its own rectangle, where the two views are
// closest to agreeing.
//
// Every quantity here arrives in absolute canvas pixels and is converted using
// this node's slice. Two nodes sharing the inner rectangle's boundary therefore
// evaluate the same canvas coordinate for the same wall pixel, and the same
// weight for it: the feather is continuous across the seam because it does not
// know the seam exists.
//
// Note this is a feather *within* one canvas between two render regions. It is
// not the node-to-node edge blend a projector wall needs, which this product
// deliberately does not have: adjacent LED nodes abut pixel-exact.

Texture2D<float4> outer_scene : register(t0, space2);
SamplerState outer_sampler : register(s0, space2);
Texture2D<float4> inner_scene : register(t1, space2);
SamplerState inner_sampler : register(s1, space2);

cbuffer IcvfxConstants : register(b0, space3) {
    // The top-left of this node's slice, in absolute canvas pixels.
    float2 slice_origin;
    // Canvas pixels per target pixel: the slice's extent over the target's.
    // Exactly one in the ordinary case where a node renders its slice at its
    // slice resolution, which is what makes the canvas coordinate below an
    // exact reconstruction rather than a nearly-exact one.
    float2 canvas_per_pixel;
    // The inner-frustum rectangle, in the same absolute canvas pixels.
    float2 inner_origin;
    float2 inner_size;
    // Feather width in canvas pixels, measured inward from the inner edge.
    float feather;
    // Zero disables the inner frustum entirely; the pass is then a copy of the
    // outer render, which keeps one pipeline instead of two code paths.
    float enabled;
    float2 padding;
};

struct Input {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Distance from `value` to the nearer end of [low, high]. Negative outside.
float edge_distance(float value, float low, float high) {
    return min(value - low, high - value);
}

float4 main(Input input) : SV_Target0 {
    const float4 outer = outer_scene.Sample(outer_sampler, input.uv);
    if (enabled < 0.5) {
        return outer;
    }

    // This pixel's absolute canvas position.
    //
    // From SV_Position rather than from the interpolated uv, and that is the
    // whole reason the seam holds. SV_Position.xy is exactly `pixel + 0.5` in
    // target space, so with canvas_per_pixel at one this is an exact integer
    // offset from the slice origin. Going through uv would divide by the
    // target width and multiply by the slice width, which round-trips exactly
    // only when the width is a power of two - and a wall is not obliged to be
    // laid out in powers of two.
    const float2 canvas = slice_origin + input.position.xy * canvas_per_pixel;
    const float2 inner_end = inner_origin + inner_size;
    const float distance_x =
        edge_distance(canvas.x, inner_origin.x, inner_end.x);
    const float distance_y =
        edge_distance(canvas.y, inner_origin.y, inner_end.y);
    const float inside = min(distance_x, distance_y);
    if (inside <= 0.0) {
        return outer;
    }

    // Full strength one feather width in; smoothstep rather than a linear ramp
    // because a linear ramp leaves a first-derivative discontinuity at both
    // ends of the transition, and a camera integrating the wall sees it.
    const float weight = feather > 0.0
        ? smoothstep(0.0, 1.0, saturate(inside / feather))
        : 1.0;

    // The inner render's own uv: where this canvas pixel sits inside the inner
    // rectangle, which is what the inner target was rendered to cover.
    const float2 inner_uv = (canvas - inner_origin) / max(inner_size, 1.0);
    const float4 inner = inner_scene.Sample(inner_sampler, inner_uv);
    return lerp(outer, inner, weight);
}
