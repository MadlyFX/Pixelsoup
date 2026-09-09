#ifndef MEDIASERVER_IES_HLSLI
#define MEDIASERVER_IES_HLSLI

float light_ies_weight(Texture2D<float> atlas, Light light, float3 from_light) {
    if (light.ies.w == -1.0) return 1.0;
    if (light.ies.w < 0.0) return 0.0;
    const float3 axis = light.ies_axis.xyz;
    const float3 tangent = light.ies.xyz;
    const float theta = acos(clamp(dot(from_light, axis), -1.0, 1.0)) * 57.295779513;
    const float x = dot(from_light, tangent);
    const float y = dot(from_light, cross(axis, tangent));
    // atan2(0,0) is undefined at the poles; Type C has no azimuth there.
    float phi = (abs(x) + abs(y) > 1e-8) ? atan2(y, x) * 57.295779513 : 0.0;
    if (phi < 0.0) phi += 360.0;
    const float2 coordinates = clamp(float2(theta, phi), 0.0, float2(180.0, 360.0));
    const int2 cell = min(int2(floor(coordinates)), int2(179, 359));
    const float2 fraction = coordinates - float2(cell);
    const int2 a = cell + int2(0, (int)light.ies.w);
    const float low = lerp(atlas.Load(int3(a, 0)), atlas.Load(int3(a + int2(1, 0), 0)), fraction.x);
    const float high = lerp(atlas.Load(int3(a + int2(0, 1), 0)), atlas.Load(int3(a + int2(1, 1), 0)), fraction.x);
    return lerp(low, high, fraction.y);
}
#endif
