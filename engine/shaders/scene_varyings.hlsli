// Keep the full vertex/pixel signature, including unused lanes: D3D12 links
// interpolator registers as well as semantics. Coverage needs the late lanes.
#ifndef MEDIASERVER_SCENE_VARYINGS_HLSLI
#define MEDIASERVER_SCENE_VARYINGS_HLSLI
struct Varyings {
    float4 position : SV_Position;
    float3 world_position : TEXCOORD0;
    float3 world_normal : TEXCOORD1;
    // Constant across the triangle: the material belongs to the instance, not
    // to the vertex, and interpolating it would be both wasteful and wrong at
    // a shared edge.
    nointerpolation float4 base_color_roughness : TEXCOORD2;
    nointerpolation float4 emissive_metallic : TEXCOORD3;
    // The vertex's primary texture coordinate, consumed by both imported PBR
    // material maps and the media fragment stage.
    float2 uv : TEXCOORD4;
    nointerpolation float4 layer_style : TEXCOORD5;
    nointerpolation uint4 effect_range : TEXCOORD6;
    nointerpolation uint object_id : TEXCOORD7;
    float4 current_clip : TEXCOORD8;
    float4 previous_clip : TEXCOORD9;
    float4 world_tangent : TEXCOORD10;
    nointerpolation float4 material_properties : TEXCOORD11;
    nointerpolation float4 coat : TEXCOORD12;
    nointerpolation float4 sheen : TEXCOORD13;
    nointerpolation float4 specular : TEXCOORD14;
    nointerpolation float4 anisotropy : TEXCOORD15;
    nointerpolation float4 alpha : TEXCOORD16;
    nointerpolation float motion_valid : TEXCOORD17;
    float clip_distance : SV_ClipDistance0;
#if defined(MODEL_MOTION)
    nointerpolation float4 motion_raster0 : TEXCOORD18;
    nointerpolation float4 motion_raster1 : TEXCOORD19;
    nointerpolation float4 motion_raster2 : TEXCOORD20;
    nointerpolation float4 motion_current0 : TEXCOORD21;
    nointerpolation float4 motion_current1 : TEXCOORD22;
    nointerpolation float4 motion_current2 : TEXCOORD23;
    nointerpolation float4 motion_previous0 : TEXCOORD24;
    nointerpolation float4 motion_previous1 : TEXCOORD25;
    nointerpolation float4 motion_previous2 : TEXCOORD26;
    nointerpolation float4 motion_viewport : TEXCOORD27;
#endif

};
#endif
