#ifndef MEDIASERVER_LIGHT_HLSLI
#define MEDIASERVER_LIGHT_HLSLI

static const uint LIGHT_DIRECTIONAL = 0u;
static const uint LIGHT_POINT = 1u;
static const uint LIGHT_SPOT = 2u;

// Matches the 64-byte GpuLight. Geometry and fog consume the same packed
// photometric emission and the same angular distribution.
struct Light {
    float3 vector; // direction of travel for a sun; position for local lights
    uint kind;
    float4 color_intensity;
    // range, outer cosine, inner cosine, shadow-table row (-1 if absent)
    float4 params;
    // cone axis, then 1 for photometric smoothstep or 0 for legacy squared
    float4 spot;
};

float light_cone_weight(Light light, float alignment) {
    const float interval = light.params.z - light.params.y;
    if (light.spot.w > 0.5 && interval == 0.0)
        return alignment >= light.params.z ? 1.0 : 0.0;
    const float cone = saturate((alignment - light.params.y)
        / (light.spot.w > 0.5 ? interval : max(interval, 1e-4)));
    return light.spot.w > 0.5
        ? cone * cone * (3.0 - 2.0 * cone) : cone * cone;
}
#endif
