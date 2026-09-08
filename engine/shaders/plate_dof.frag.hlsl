// Step 28: deterministic, fixed 113-tap disk gather in premultiplied linear HDR.
// Matrix, physical lens, and wall all come from the replicated full plate.
Texture2D<float4> color_image : register(t0, space2);
SamplerState color_sampler : register(s0, space2);
Texture2D<float> depth_image : register(t1, space2);
SamplerState depth_sampler : register(s1, space2);
cbuffer PlateDofConstants : register(b0, space3) {
    column_major float4x4 inverse_view_projection;
    float4 extent; // xy target, z authored radius bound
    float4 optics; // f metres, focus metres, thin-lens sensor radius factor
    float4 eye, lens_forward, lens_right, lens_up;
    float4 wall_right, wall_up; // w: visible plate pixels per wall metre
    float4 wall_normal; // w: dot(normal, center - eye)
};

float3 ray_at(int2 p, float depth) {
    const float2 ndc = (float2(p) + 0.5) / extent.xy * float2(2, -2) + float2(-1, 1);
    const float4 world = mul(inverse_view_projection, float4(ndc, depth, 1));
    return world.xyz / world.w - eye.xyz;
}

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    const int2 p = int2(position.xy);
    const float4 center = color_image.Load(int3(p, 0));
    const float depth = depth_image.Load(int3(p, 0));
    // Ray through the near plane remains finite even for an empty/infinite sky.
    const float3 ray = ray_at(p, 1.0);
    const float denominator = dot(wall_normal.xyz, ray);
    const float wall_z = dot(ray * (wall_normal.w / denominator), lens_forward.xyz);
    const float virtual_z = depth > 0.0 ? dot(ray_at(p, depth), lens_forward.xyz) : 1.0e30;
    if (denominator >= -1.0e-8 || wall_z <= 0.0 || virtual_z <= 0.0) return center;
    const float coc = optics.z * max(abs(1.0 - optics.y / virtual_z) - abs(1.0 - optics.y / wall_z), 0.0);
    // Jacobian from a sensor disk to a tilted wall. Preserve the ellipse and
    // cap its major singular value so every tap stays inside the authored guard.
    const float3 dx = (lens_right.xyz - ray * (dot(wall_normal.xyz, lens_right.xyz) / denominator)) * (wall_z / optics.x);
    const float3 dy = (lens_up.xyz - ray * (dot(wall_normal.xyz, lens_up.xyz) / denominator)) * (wall_z / optics.x);
    float2 a = float2(dot(dx, wall_right.xyz) * wall_right.w, -dot(dx, wall_up.xyz) * wall_up.w) * coc;
    float2 b = float2(dot(dy, wall_right.xyz) * wall_right.w, -dot(dy, wall_up.xyz) * wall_up.w) * coc;
    const float xx = a.x * a.x + b.x * b.x, yy = a.y * a.y + b.y * b.y;
    const float xy = a.x * a.y + b.x * b.y;
    const float major = sqrt(max(0.5 * (xx + yy + sqrt((xx - yy) * (xx - yy) + 4.0 * xy * xy)), 0.0));
    // Exact identity, including alpha, for zero and sub-texel support.
    if (major < 0.5) return center;
    const float cap = min(extent.z / major, 1.0);
    a *= cap; b *= cap;
    float4 delta = 0.0;
    float count = 0.0;
    [loop] for (int y = -6; y <= 6; ++y) {
        [loop] for (int x = -6; x <= 6; ++x) {
            if (x * x + y * y > 36) continue;
            const int2 offset = int2(round((a * float(x) + b * float(y)) / 6.0));
            const int2 q = clamp(p + offset, int2(0, 0), int2(extent.xy) - 1);
            const float neighbor = depth_image.Load(int3(q, 0));
            // Keep nearer occluders out of the background kernel. This single
            // visible-depth gather cannot reveal hidden surfaces at silhouettes.
            if (neighbor > depth + max(0.000001, depth * 0.01)) continue;
            delta += color_image.Load(int3(q, 0)) - center;
            count += 1.0;
        }
    }
    return center + delta / max(count, 1.0);
}
