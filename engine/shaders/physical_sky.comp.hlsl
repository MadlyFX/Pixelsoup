// Item 29: fixed single scattering in a spherical exponential atmosphere.
// Lengths are kilometres. No camera, slice, clock, random seed or adaptive loop.
// Equations: Rayleigh phase, Henyey-Greenstein Mie phase and Beer-Lambert
// transmittance. See 29-physical-sky-and-sun.md for constants and limitations.
#include "ibl_common.hlsli"
cbuffer SkyParameters : register(b0, space1) {
    float4 sun_haze; // direction towards sun, aerosol density
    float4 solar_ground; // scaled solar illuminance, ground albedo, width, height
    float4 direct_irradiance;
};
RWTexture2D<float4> sky_target : register(u0);
static const float earth = 6360.0;
static const float atmosphere = 6460.0;
static const float3 beta = float3(0.005802, 0.013558, 0.033100);
static const float3 observer = float3(0, 6360.001, 0);
float top_distance(float3 p, float3 d) {
    float b = dot(p,d);
    return -b + sqrt(max(b*b-dot(p,p)+atmosphere*atmosphere,0.0));
}
float3 extinction(float r, float m) { return exp(-beta*r-0.00444*sun_haze.w*m); }
float3 sunlight(float3 p) {
    float b = dot(p,sun_haze.xyz);
    if (b < 0 && b*b-dot(p,p)+earth*earth > 0) return 0;
    float step = top_distance(p,sun_haze.xyz)/16.0;
    float r=0, m=0;
    [loop] for (uint i=0; i<16; ++i) {
        float3 q = p+sun_haze.xyz*((float(i)+0.5)*step);
        float h = max(length(q)-earth,0.0);
        r += exp(-h/8.0)*step;
        m += exp(-h/1.2)*step;
    }
    return extinction(r,m);
}
[numthreads(8,8,1)]
void main(uint3 thread : SV_DispatchThreadID) {
    uint2 extent = uint2(solar_ground.zw);
    if (any(thread.xy >= extent)) return;
    float2 uv = (float2(thread.xy)+0.5)/float2(extent);
    float phi = (uv.x-0.5)*2.0*kPi, theta = uv.y*kPi;
    float3 d = float3(cos(phi)*sin(theta),cos(theta),sin(phi)*sin(theta));
    float3 sum = 0;
    if (d.y < 0) {
        sum = direct_irradiance.rgb*(max(sun_haze.y,0.0)*solar_ground.y/kPi);
    } else {
        float step = top_distance(observer,d)/32.0;
        float mu = dot(d,sun_haze.xyz);
        float phase_r = 3.0*(1.0+mu*mu)/(16.0*kPi);
        float denom = max(1.64-1.6*mu,0.001);
        float phase_m = 0.36/(4.0*kPi*denom*sqrt(denom));
        float optical_r=0, optical_m=0;
        [loop] for (uint i=0; i<32; ++i) {
            float3 p = observer+d*((float(i)+0.5)*step);
            float h = max(length(p)-earth,0.0);
            float r=exp(-h/8.0), m=exp(-h/1.2);
            float3 trans = extinction(optical_r+r*step*0.5,optical_m+m*step*0.5)*sunlight(p);
            sum += trans*(beta*(r*phase_r)+0.003996*sun_haze.w*m*phase_m)*step;
            optical_r += r*step; optical_m += m*step;
        }
        sum *= solar_ground.x;
    }
    // The solar disc is integrated separately into the specular chain, and
    // rasterised analytically behind geometry. Diffuse SH excludes direct sun:
    // the directional light supplies that term with shadows.
    sky_target[thread.xy] = float4(clamp(sum,0.0,65504.0),1.0);
}
