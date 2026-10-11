// A fixed 25-tap tent kernel, with manual bilinear comparison. Every fetch and
// weight is explicit, including at atlas edges; no hardware comparison filter.
// Receiver-plane correction prevents a wide kernel shadowing an empty slope.
float2 shadow_receiver_gradient(float4x4 projection, float3 position, float3 normal) {
    // Differentiate the projective receiver plane analytically. Unlike screen
    // derivatives this works at silhouettes and inside cascade branches, and
    // unlike an orthographic approximation it also handles spot projections.
    const float3 tangent = normalize(cross(normal, abs(normal.z) < 0.9
        ? float3(0, 0, 1) : float3(0, 1, 0)));
    const float3 bitangent = cross(normal, tangent);
    const float4 p = mul(projection, float4(position, 1.0));
    const float4 t = mul(projection, float4(tangent, 0.0));
    const float4 b = mul(projection, float4(bitangent, 0.0));
    const float3 dt = (t.xyz * p.w - p.xyz * t.w) * float3(0.5, -0.5, 1.0);
    const float3 db = (b.xyz * p.w - p.xyz * b.w) * float3(0.5, -0.5, 1.0);
    const float determinant = dt.x * db.y - dt.y * db.x;
    return abs(determinant) > 1e-12
        ? float2(dt.z * db.y - db.z * dt.y, dt.x * db.z - db.x * dt.z) / determinant : 0.0;
}

float shadow_compare(Texture2D<float> map, int2 pixel, int2 origin, int2 extent,
                     float receiver) {
    // Missing samples are lit. Never read the next atlas tile or clamp a
    // border caster into an otherwise empty part of the light projection.
    if (any(pixel < origin) || any(pixel >= origin + extent)) return 1.0;
    return map.Load(int3(pixel, 0)) <= receiver ? 1.0 : 0.0;
}

float shadow_bilinear(Texture2D<float> map, float2 uv, float depth, float bias,
                      float2 gradient, float2 receiver_uv, int2 origin, int2 extent) {
    const float2 coord = uv * float2(extent) - 0.5;
    const int2 pixel = int2(floor(coord));
    const float2 fraction = frac(coord);
    float values[4];
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const int2 local = pixel + int2(i & 1u, i >> 1u);
        const float2 tap_uv = (float2(local) + 0.5) / float2(extent);
        const float receiver = min(depth + bias + dot(gradient, tap_uv - receiver_uv), 1.0);
        values[i] = shadow_compare(map, origin + local, origin, extent, receiver);
    }
    return lerp(lerp(values[0], values[1], fraction.x),
                lerp(values[2], values[3], fraction.x), fraction.y);
}

float shadow_kernel(Texture2D<float> map, float2 uv, float depth, float bias,
                    float2 gradient, float radius, int2 origin, int2 extent) {
    float visible = 0.0;
    // Rows stay a loop: unrolling all 25 taps of every cascade made the lit
    // stage too large to keep enough pixels in flight, for the same sum in
    // the same order.
    [loop] for (int y = -2; y <= 2; ++y) {
        [unroll] for (int x = -2; x <= 2; ++x) {
            const float weight = float((3 - abs(x)) * (3 - abs(y)));
            visible += weight * shadow_bilinear(map,
                uv + float2(x, y) * (radius * 0.5) / float2(extent),
                depth, bias, gradient, uv, origin, extent);
        }
    }
    return visible / 81.0;
}

// Nine blocker taps, always the same count. Sun size turns light-space
// receiver/blocker separation into a PCSS-shaped radius, capped at 16 texels.
float shadow_penumbra(Texture2D<float> map, float2 uv, float depth, float bias,
                      float2 gradient, float texel_depth, int2 extent) {
    float separation = 0.0, blockers = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 tap = int2(floor(uv * float2(extent))) + int2(x, y) * 8;
            if (any(tap < 0) || any(tap >= extent)) continue;
            const float stored = map.Load(int3(tap, 0));
            const float plane = depth + dot(gradient, (float2(tap) + 0.5) / float2(extent) - uv);
            if (stored > plane + bias) {
                separation += stored - plane;
                blockers += 1.0;
            }
        }
    }
    const float radius = separation / max(blockers, 1.0)
        * shadow_quality.z / max(texel_depth, 1e-8);
    return clamp(radius, shadow_quality.y, 16.0);
}

float filtered_cascade(Texture2D<float> map, uint index, float2 uv, float depth,
                       float3 position, float3 normal, float bias) {
    uint w, h; map.GetDimensions(w, h);
    const int2 extent = int2(w, h);
    const float2 gradient = shadow_receiver_gradient(shadow_view_projection[index], position, normal);
    float radius = shadow_quality.y;
    if (shadow_quality.z > 0.0) radius = shadow_penumbra(map, uv, depth, bias,
        gradient, shadow_cascade_texel_depth[index], extent);
    return shadow_kernel(map, uv, depth, bias, gradient, radius, int2(0, 0), extent);
}

// How much of cascade `index` this point takes: zero outside the map or its
// guard band, one well inside it, a smooth blend across its edge band. The
// last cascade, the full-scene fit, always takes all of it.
float cascade_weight(uint index, uint cascades, float3 position,
                     out float2 uv, out float depth) {
    uv = 0.0;
    depth = 0.0;
    const float4 clip = mul(shadow_view_projection[index], float4(position, 1.0));
    if (clip.w <= 0.0) return 0.0;
    const float3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    depth = ndc.z;
    if (any(uv <= 0.0) || any(uv >= 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) return 0.0;
    const float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    const float guard = (shadow_quality.z > 0.0 ? 16.5 : shadow_quality.y + 0.5) * shadow_parameters.x;
    const float blend = shadow_quality.x > 0.0
        ? smoothstep(guard, guard + shadow_quality.x, edge) : (edge > guard ? 1.0 : 0.0);
    return index + 1u == cascades ? 1.0 : blend;
}

float directional_shadow_visibility(float3 position, float3 normal, float3 direction) {
    const uint cascades = min(counts.z, 4u);
    const float facing = saturate(dot(normal, -normalize(direction)));
    const float texels = shadow_parameters.z + shadow_parameters.w * (1.0 - facing);
    // Composition runs far to near, including a next-map gap as lit. Both
    // lookups and the blend weight depend on this one world position, never
    // slice depth.
    //
    // The nearest map this point takes with full weight replaces everything
    // composed before it whenever it reads fully lit or fully shadowed:
    // lerp(v, 0, 1) and lerp(v, 1, 1) are exactly 0 and 1 for every v in
    // [0, 1], however the blend is rounded. So the maps are filtered near to
    // far, and the farther ones only for a penumbra there, which then
    // composes exactly as it always has. Each map keeps one static filtering
    // site, so a warp steps through the maps together.
    float weights[4] = {0.0, 0.0, 0.0, 0.0};
    float filtered[4] = {1.0, 1.0, 1.0, 1.0};
    bool found = false;
    bool penumbra = false;
    int first = int(cascades) - 1;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        if (i < cascades) {
            float2 uv;
            float depth;
            weights[i] = cascade_weight(i, cascades, position, uv, depth);
            if (weights[i] > 0.0 && (!found || penumbra)) {
                const float bias = texels * shadow_cascade_texel_depth[i];
                if (i == 0u) filtered[i] = filtered_cascade(shadow_cascade0_texture, 0u, uv, depth, position, normal, bias);
                else if (i == 1u) filtered[i] = filtered_cascade(shadow_cascade1_texture, 1u, uv, depth, position, normal, bias);
                else if (i == 2u) filtered[i] = filtered_cascade(shadow_cascade2_texture, 2u, uv, depth, position, normal, bias);
                else filtered[i] = filtered_cascade(shadow_cascade3_texture, 3u, uv, depth, position, normal, bias);
            }
            if (!found && (weights[i] >= 1.0 || i + 1u == cascades)) {
                found = true;
                first = int(i);
                penumbra = weights[i] > 0.0 && filtered[i] > 0.0 && filtered[i] < 1.0;
            }
        }
    }
    float visible = 1.0;
    [unroll] for (int i = 3; i >= 0; --i) {
        if (i < int(cascades) && weights[i] > 0.0 && (i <= first || penumbra)) {
            visible = lerp(visible, filtered[i], weights[i]);
        }
    }
    return visible;
}

float spot_shadow_visibility(uint row, float3 position, float3 normal, float facing) {
    const LightShadow record = light_shadows[row];
    const float4 clip = mul(record.view_projection, float4(position, 1.0));
    if (clip.w <= 0.0) return 1.0;
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv <= 0.0) || any(uv >= 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) return 1.0;
    const float bias = (shadow_parameters.z + shadow_parameters.w * (1.0 - facing)) * record.tile.w;
    return shadow_kernel(shadow_atlas_texture, uv, ndc.z, bias,
        shadow_receiver_gradient(record.view_projection, position, normal),
        shadow_quality.y, int2(record.tile.xy), int2(record.tile.zz));
}

float3 contact_world(float2 canvas_pixel, float depth) {
    const float2 ndc = canvas_pixel / lighting_canvas.xy * float2(2.0, -2.0) + float2(-1.0, 1.0);
    const float4 world = mul(lighting_inverse_view_projection, float4(ndc, depth, 1.0));
    return world.xyz / world.w;
}

// Twelve samples per eligible light, bounded in world distance AND pixels.
// The ray uses the full-canvas projection, then translates to the local depth
// image only for Load. There is no history, random seed or camera-ranked work.
float contact_shadow_visibility(float3 position, float3 normal, float3 to_light, float ray_length) {
    const float4 origin_clip = mul(lighting_view_projection, float4(position, 1.0));
    const float2 origin_pixel = (origin_clip.xy / origin_clip.w * float2(0.5, -0.5) + 0.5) * lighting_canvas.xy;
    const float2 pitch = float2(grain_extent.xy) / max(float2(grain_extent.zw), 1.0);
    float occluded = 0.0;
    [unroll] for (uint i = 1u; i <= 12u; ++i) {
        const float distance = ray_length * (float(i) / 12.0);
        const float3 ray = position + to_light * distance;
        const float4 clip = mul(lighting_view_projection, float4(ray, 1.0));
        if (clip.w <= 0.0) continue;
        const float2 canvas_pixel = (clip.xy / clip.w * float2(0.5, -0.5) + 0.5) * lighting_canvas.xy;
        if (max(abs(canvas_pixel.x - origin_pixel.x) / pitch.x,
                abs(canvas_pixel.y - origin_pixel.y) / pitch.y) > contact_shadow.z - 1.0) continue;
        const int2 pixel = int2(floor((canvas_pixel - lighting_canvas.zw) / pitch));
        if (any(pixel < 0) || any(pixel >= int2(grain_extent.zw))) continue;
        const float depth = contact_depth_texture.Load(int3(pixel, 0));
        if (depth <= 0.0) continue;
        const float3 hit = contact_world((float2(pixel) + 0.5) * pitch + lighting_canvas.zw, depth);
        const float separation = dot(ray - hit, normalize(hit - eye_position.xyz));
        // A coplanar receiver cannot occlude itself, regardless of ray slope.
        if (dot(hit - position, normal) > 0.001 && separation > 0.001
            && separation < contact_shadow.y) {
            occluded = max(occluded, 1.0 - smoothstep(0.75, 1.0, float(i) / 12.0));
        }
    }
    return 1.0 - occluded;
}
