// Temporal antialiasing, resolve stage.
//
// Runs only for an Advance. Initialize, Reset and Hold never reach this shader
// - they are a copy, a copy, and nothing, decided by a table on the CPU. That
// is deliberate: the disposition is the whole reason the temporal contract
// exists, and burying it in branches here would put the decision in two places.
//
// On a volume the trade this pass makes is not the usual one. Ghosting gets
// integrated into the plate permanently and cannot be graded out, whereas
// aliasing at least reads as texture, so every choice below errs toward
// dropping history rather than keeping it.

Texture2D<float4> current_color : register(t0, space2);
SamplerState current_sampler : register(s0, space2);
Texture2D<float4> velocity : register(t1, space2);
SamplerState velocity_sampler : register(s1, space2);
Texture2D<uint> object_id : register(t2, space2);
SamplerState object_id_sampler : register(s2, space2);
Texture2D<float4> history_color : register(t3, space2);
SamplerState history_sampler : register(s3, space2);
Texture2D<uint> history_object_id : register(t4, space2);
SamplerState history_object_sampler : register(s4, space2);
Texture2D<float4> history_motion_depth : register(t5, space2);
SamplerState history_depth_sampler : register(s5, space2);

cbuffer TaaConstants : register(b0, space3) {
    // xy: target extent in pixels. z: how much of the history survives a
    // blend. w: unused.
    float4 taa_parameters;
    // xy: canvas extent in pixels. The velocity product is canvas-relative, so
    // this is what converts it to pixels - and because the padded slice renders
    // at a 1:1 pitch, a canvas pixel of motion is a target pixel of motion. Not
    // the target extent: that is what differed between one node and two.
    float4 taa_canvas;
};

int2 clamp_pixel(int2 pixel, int2 extent) {
    return clamp(pixel, int2(0, 0), extent - int2(1, 1));
}

float4 main(float4 position : SV_Position) : SV_Target {
    const float2 extent_f = max(taa_parameters.xy, 1.0);
    const int2 extent = int2(extent_f);
    const int2 pixel = clamp_pixel(int2(position.xy), extent);
    const float4 current = current_color.Load(int3(pixel, 0));

    // Velocity is a canvas-relative UV delta from the previous frame to this
    // one, exactly as scene_velocity.frag.hlsl writes it, so the history sits
    // at this pixel's position minus it. Working in pixels from here rather
    // than in this target's UV keeps every term node-independent.
    const float2 motion =
        velocity.Load(int3(pixel, 0)).xy * max(taa_canvas.xy, 1.0);
    const float2 history_point = float2(pixel) + 0.5 - motion;

    // Reprojected off the previous frame entirely. There is nothing behind
    // this pixel to blend with, and an edge-clamped sample would blend it with
    // whatever happened to sit on the border - which on a slice is the apron,
    // and on a wall is the seam.
    if (any(history_point < 0.0) || any(history_point > extent_f)) {
        return current;
    }

    // Validate every bilinear tap. An ID alone cannot detect a newly exposed
    // face of the same mesh. Previous depth belongs to this fragment's previous
    // camera and object transform, rather than its current device-space Z.
    const float expected_depth = velocity.Load(int3(pixel, 0)).w;
    if (expected_depth < 0.0 || expected_depth > 1.0) return current;
    const uint id = object_id.Load(int3(pixel, 0));
    const float tolerance = 1.0e-5 + abs(expected_depth) * 0.002;
    const float2 sample_position = history_point - 0.5;
    const float2 base = floor(sample_position);
    const float2 fraction = sample_position - base;
    float4 history = 0.0;
    float accepted_weight = 0.0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            const int2 tap = clamp_pixel(int2(base) + int2(x, y), extent);
            const float depth = history_motion_depth.Load(int3(tap, 0)).z;
            if (history_object_id.Load(int3(tap, 0)) == id
                && abs(depth - expected_depth) <= tolerance) {
                const float w = (x == 0 ? 1.0 - fraction.x : fraction.x)
                    * (y == 0 ? 1.0 - fraction.y : fraction.y);
                history += history_color.Load(int3(tap, 0)) * w;
                accepted_weight += w;
            }
        }
    }
    if (accepted_weight <= 0.0) return current;
    history /= accepted_weight;

    // The neighbourhood colour box of the current frame, over all four
    // channels. Alpha is included because an isolated compositor layer is
    // premultiplied and its coverage ghosts exactly as its colour does.
    //
    // The 3x3 read is why the slice apron reserves two pixels: one for this
    // radius and one for the sub-pixel jitter that can push it a whole pixel
    // further out. Reading past the padded region would clamp to the edge and
    // pin the box to it, which is a bright line precisely at a node boundary.
    float4 low = current;
    float4 high = current;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float4 tap = current_color.Load(
                int3(clamp_pixel(pixel + int2(x, y), extent), 0));
            low = min(low, tap);
            high = max(high, tap);
        }
    }
    history = clamp(history, low, high);

    // A fixed weight, never adapted to local motion magnitude. An adaptive
    // expression is the standard trick and it is the wrong one here: two nodes
    // would have to evaluate it identically for every pixel of a shared edge,
    // and the first time one of them did not, the two halves of the wall would
    // converge on different images.
    return lerp(current, history, saturate(taa_parameters.z));
}
