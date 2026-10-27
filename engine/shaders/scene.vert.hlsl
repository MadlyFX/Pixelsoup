// Scene geometry, vertex stage. Fully pulled: there is no vertex buffer, no
// index buffer, and no vertex input layout anywhere in the pipeline.
//
// The stage reads its own index out of a storage buffer, then its own vertex,
// then its own instance. That costs one extra dependent load per vertex and
// buys three things worth more than the load:
//
//  * the RHI seam needs no vertex input state, so adding a second kind of
//    geometry later changes a shader rather than an interface;
//  * every mesh in the scene lives in one buffer, so switching mesh means
//    changing an offset rather than rebinding;
//  * it is the shape a bindless, GPU-driven pipeline already wants, which is
//    where this is going.
//
// Register spaces follow the SDL_GPU DXIL convention: a vertex stage's storage
// buffers live at (t[n], space0) and its uniform buffers at (b[n], space1).
//
// Matrices are stored as explicit columns rather than as float4x4 because the
// CPU side stores them column-major and a silent transpose is the kind of bug
// that produces a plausible-looking wrong picture. Written out, the contract
// is visible in both files.

struct Vertex {
    float3 position;
    float3 normal;
    float2 uv;
};

struct Instance {
    // Object to world, four columns.
    float4 world0;
    float4 world1;
    float4 world2;
    float4 world3;
    // Previous object-to-world transform. Equal to current on a history reset.
    float4 previous_world0;
    float4 previous_world1;
    float4 previous_world2;
    float4 previous_world3;
    // The inverse transpose of the world basis, three columns. Correct under
    // non-uniform scale, which the world basis alone is not.
    float4 normal0;
    float4 normal1;
    float4 normal2;
    float4 base_color_roughness;
    float4 emissive_metallic;
    // x: normal scale, y: occlusion strength.
    float4 material_properties;
    float4 coat;
    float4 sheen;
    float4 specular;
    float4 anisotropy;
    float4 alpha;

    // x: opacity, y: horizontal edge blend, z: BlendMode, w: depth-only.
    float4 layer_style;
    // x: first operation, y: operation count, z: DecodedPixelLayout.
    uint4 effect_range;
    uint4 pose;
    float4 wind;
    float4 previous_wind;
};

StructuredBuffer<Vertex> vertices : register(t0, space0);
StructuredBuffer<uint> indices : register(t1, space0);
StructuredBuffer<Instance> instances : register(t2, space0);
StructuredBuffer<float4> tangents : register(t3, space0);
struct Skin { uint4 joints; float4 weights; };
struct Pose {
    float4 current0; float4 current1; float4 current2; float4 current3;
    float4 previous0; float4 previous1; float4 previous2; float4 previous3;
};
StructuredBuffer<Skin> skins : register(t4, space0);
StructuredBuffer<Pose> poses : register(t5, space0);

cbuffer ViewConstants : register(b0, space1) {
    // This slice's projection. Rasterisation only.
    column_major float4x4 view_projection;
    // The motion pair, both canvas-wide rather than slice-relative. See the
    // note where they are used.
    column_major float4x4 previous_view_projection;
    column_major float4x4 canvas_view_projection;
    float4 temporal_parameters; // x: valid previous camera/model sample
    float4 clip_plane;
    float4 motion_viewport;
};

// Where this batch's slice of the pulled buffers begins.
//
// Passed explicitly rather than as the draw call's first-vertex and
// first-instance arguments because those do not mean the same thing on every
// backend: D3D's SV_InstanceID excludes the draw's first instance while
// Vulkan's gl_InstanceIndex includes it. One shader, correct on both.
cbuffer BatchConstants : register(b1, space1) {
    // x: first index, y: first instance, z: nonzero for an indexed draw, in
    // which the input assembler has already read the index and SV_VertexID
    // is the vertex itself. w: nonzero for a blended layer's own depth pass,
    // which draws the layer depth-only and keeps only its opaque pixels.
    uint4 batch_offsets;
};

// How far a depth-only instance is pushed away from the camera, in normalised
// device depth. Such an instance is drawn on behalf of another compositor
// layer purely to occlude this one, and coplanar content is a layering
// question rather than a depth one: two quads on the same plane must still be
// ordered by the composite, not silently cancelled by a reversed-depth test
// that neither of them can win. The bias makes the occluder lose that tie in
// both directions - it neither hides coplanar geometry nor is hidden by it -
// while remaining thousands of float32 steps below anything an operator could
// place deliberately.
static const float depth_only_bias = 1.0e-5;

#include "scene_varyings.hlsli"

Varyings evaluate_vertex(uint vertex_id, uint instance_id) {
    const uint index = batch_offsets.z != 0u
        ? vertex_id : indices[batch_offsets.x + vertex_id];
    Vertex vertex = vertices[index];
    const Instance instance = instances[batch_offsets.y + instance_id];
    float4 tangent = tangents[index];
    float3 previous_position = vertex.position;
    float skin_orientation = 1.0;
    // One show-frame phase per instance, calculated in deterministic CPU
    // arithmetic. Bend weight is local height, so the root stays anchored.
    if (any(instance.wind.xyz != 0.0) || any(instance.previous_wind.xyz != 0.0)) {
        const float height = (vertex.position.y-instance.wind.w)*instance.previous_wind.w;
        const float weight = saturate(height);
        previous_position += instance.previous_wind.xyz*weight;
        vertex.position += instance.wind.xyz*weight;
        if (height >= 0.0 && height <= 1.0) {
            const float3 shear = instance.wind.xyz*instance.previous_wind.w;
            const float determinant = 1.0+shear.y;
            if (abs(determinant)>1e-8) vertex.normal.y -= dot(shear,vertex.normal)/determinant;
            tangent.xyz += shear*tangent.y;
            skin_orientation *= determinant<0.0 ? -1.0 : 1.0;
        }
    }
    if (instance.pose.y != 0) {
        // Skin records are their own arena range: z is the mesh's first
        // record, w its first vertex.
        const Skin skin = skins[instance.pose.z + (index - instance.pose.w)];
        float4 c0=0, c1=0, c2=0, c3=0;
        float4 p0=0, p1=0, p2=0, p3=0;
        [unroll] for (uint joint=0; joint<4; ++joint) {
            const Pose pose=poses[instance.pose.x+skin.joints[joint]];
            const float weight=skin.weights[joint];
            c0+=pose.current0*weight; c1+=pose.current1*weight;
            c2+=pose.current2*weight; c3+=pose.current3*weight;
            p0+=pose.previous0*weight; p1+=pose.previous1*weight;
            p2+=pose.previous2*weight; p3+=pose.previous3*weight;
        }
        vertex.position=(c0*vertex.position.x+c1*vertex.position.y+c2*vertex.position.z+c3).xyz;
        previous_position=(p0*previous_position.x+p1*previous_position.y+p2*previous_position.z+p3).xyz;
        // Inverse transpose of the blended affine basis. This also covers
        // animated nonuniform scale; a rigid-only normal transform does not.
        const float3 a=cross(c1.xyz,c2.xyz), b=cross(c2.xyz,c0.xyz), c=cross(c0.xyz,c1.xyz);
        const float determinant=dot(c0.xyz,a);
        if (abs(determinant)>1e-8) vertex.normal=(a*vertex.normal.x+b*vertex.normal.y+c*vertex.normal.z)/determinant;
        tangent.xyz=c0.xyz*tangent.x+c1.xyz*tangent.y+c2.xyz*tangent.z;
        skin_orientation *= determinant<0.0 ? -1.0 : 1.0;
    }

    const float4 world = instance.world0 * vertex.position.x
        + instance.world1 * vertex.position.y
        + instance.world2 * vertex.position.z + instance.world3;

    const float3 normal = instance.normal0.xyz * vertex.normal.x
        + instance.normal1.xyz * vertex.normal.y
        + instance.normal2.xyz * vertex.normal.z;
    const float3 unit_normal = normalize(normal);
    const float3 raw_tangent = instance.world0.xyz * tangent.x
        + instance.world1.xyz * tangent.y
        + instance.world2.xyz * tangent.z;
    const float3 world_tangent = normalize(
        raw_tangent - unit_normal * dot(unit_normal, raw_tangent));
    const float orientation = dot(
        instance.world0.xyz,
        cross(instance.world1.xyz, instance.world2.xyz)) < 0.0
        ? -1.0
        : 1.0;

    Varyings result = (Varyings)0;
    result.clip_distance = dot(world, clip_plane);
    const float4 previous_world =
        instance.previous_world0 * previous_position.x
        + instance.previous_world1 * previous_position.y
        + instance.previous_world2 * previous_position.z
        + instance.previous_world3;
    // The motion pair is canvas-wide, never this slice's own projection.
    //
    // Velocity has to describe the same displacement on every node, and a
    // slice's projection scales it by that slice's extent: one canvas pixel of
    // motion is 1/128 of a 128-wide target and 1/96 of a 96-wide one. Those
    // agree in exact arithmetic and not in the last bits, and the temporal
    // resolve feeds its own output back in every frame until they do not agree
    // at all. Both endpoints come from matrices every node holds identically,
    // so the delta below is identical too.
    //
    // Ordinary slices also use the canvas projection for rasterisation, with
    // their offset applied by the viewport. Explicit views carry their own.
    result.motion_valid = temporal_parameters.x;
    result.current_clip = mul(canvas_view_projection, world);
    result.previous_clip = mul(previous_view_projection, previous_world);
    result.position = mul(view_projection, world);
    // The blended depth pass takes the same bias, so a layer never hides
    // itself or anything on its own plane when it is drawn for colour.
    const float layer_mode =
        batch_offsets.w != 0u ? 2.0 : instance.layer_style.w;
    if (layer_mode > 0.5) {
        // Reversed depth: the near plane is one, so subtracting recedes.
        result.position.z -= depth_only_bias * result.position.w;
    }
    result.world_position = world.xyz;
    // Left unnormalised here and normalised per pixel, so that the
    // interpolation across a curved surface does not shorten it.
    result.world_normal = normal;
    result.world_tangent = float4(
        world_tangent, tangent.w * orientation * skin_orientation);
    result.base_color_roughness = instance.base_color_roughness;
    result.emissive_metallic = instance.emissive_metallic;
    result.material_properties = instance.material_properties;
    result.coat = instance.coat;
    result.sheen = instance.sheen;
    result.specular = instance.specular;
    result.anisotropy = instance.anisotropy;
    result.alpha = instance.alpha;

    result.uv = vertex.uv;
    result.layer_style = float4(instance.layer_style.xyz, layer_mode);
    result.effect_range = instance.effect_range;
    result.object_id = instance.effect_range.w;
    return result;
}

Varyings main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    Varyings result=evaluate_vertex(vertex_id,instance_id);
#if defined(MODEL_MOTION)
    // Needs the triangle's position in the index stream, so a deforming
    // instance is always drawn pulled (batch_offsets.z == 0).
    const Instance instance=instances[batch_offsets.y+instance_id];
    if (instance.pose.y!=0 || any(instance.wind.xyz!=0) || any(instance.previous_wind.xyz!=0)) {
        const uint first=vertex_id-vertex_id%3;
        const Varyings a=evaluate_vertex(first,instance_id);
        const Varyings b=evaluate_vertex(first+1,instance_id);
        const Varyings c=evaluate_vertex(first+2,instance_id);
        result.motion_raster0=a.position; result.motion_raster1=b.position; result.motion_raster2=c.position;
        result.motion_current0=a.current_clip; result.motion_current1=b.current_clip; result.motion_current2=c.current_clip;
        result.motion_previous0=a.previous_clip; result.motion_previous1=b.previous_clip; result.motion_previous2=c.previous_clip;
        result.motion_viewport=motion_viewport;
    }
#endif
    return result;
}
