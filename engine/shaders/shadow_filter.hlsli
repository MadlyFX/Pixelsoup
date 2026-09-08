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
    [unroll] for (int y = -2; y <= 2; ++y) {
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

float directional_shadow_visibility(float3 position, float3 normal, float3 direction) {
    const uint cascades = min(counts.z, 4u);
    const float facing = saturate(dot(normal, -normalize(direction)));
    const float texels = shadow_parameters.z + shadow_parameters.w * (1.0 - facing);
    float visible = 1.0;
    // Compose far to near, including a next-map gap as lit. Both lookups and
    // the blend weight depend on this one world position, never slice depth.
    for (int i = int(cascades) - 1; i >= 0; --i) {
        const float4 clip = mul(shadow_view_projection[i], float4(position, 1.0));
        if (clip.w <= 0.0) continue;
        const float3 ndc = clip.xyz / clip.w;
        const float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(uv <= 0.0) || any(uv >= 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) continue;
        const float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
        const float guard = (shadow_quality.z > 0.0 ? 16.5 : shadow_quality.y + 0.5) * shadow_parameters.x;
        const float blend = shadow_quality.x > 0.0
            ? smoothstep(guard, guard + shadow_quality.x, edge) : (edge > guard ? 1.0 : 0.0);
        const float weight = i + 1 == int(cascades) ? 1.0 : blend;
        if (weight <= 0.0) continue;
        const float bias = texels * shadow_cascade_texel_depth[i];
        float filtered;
        if (i == 0) filtered = filtered_cascade(shadow_cascade0_texture, 0u, uv, ndc.z, position, normal, bias);
        else if (i == 1) filtered = filtered_cascade(shadow_cascade1_texture, 1u, uv, ndc.z, position, normal, bias);
        else if (i == 2) filtered = filtered_cascade(shadow_cascade2_texture, 2u, uv, ndc.z, position, normal, bias);
        else filtered = filtered_cascade(shadow_cascade3_texture, 3u, uv, ndc.z, position, normal, bias);
        visible = lerp(visible, filtered, weight);
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
