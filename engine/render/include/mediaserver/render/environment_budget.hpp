#pragma once
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/model_animation.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <map>
#include <set>
#include <stop_token>

namespace mediaserver::render {
struct EnvironmentBudget {
    core::u64 instances{}, parts{}, triangles{}, vertices{}, indices{}, bytes{}, shadow_triangles{}, pose_bindings{};
    std::string diagnostic;
};
// Worker only. Counts the full authored scene before camera/slice culling and
// the most expensive authored LOD. Identical admission for every output node.
inline EnvironmentBudget measure_environment(const doc::Root& root, scene::SceneId target, std::stop_token stop = {}) {
    EnvironmentBudget result;
    const auto validated=scene::read_linked_scene(root);
    if (!validated) { result.diagnostic=validated.error().message; return result; }
    scene::SceneModel model; model.reset(root);
    if (model.linked_error()) { result.diagnostic=model.linked_error()->message; return result; }
    const auto evaluated=model.evaluate(target,{});
    std::map<std::string,std::pair<core::u64,core::u64>> assets;
    struct AnimationBudget { core::u64 bindings{}, clips{}; bool animated{}; };
    std::map<std::string,AnimationBudget> animation;
    result.instances=evaluated.models.size()+evaluated.instances.size()+evaluated.media.size();
    result.parts=evaluated.instances.size()+evaluated.media.size();
    std::map<scene::MeshKey,core::u64> primitives;
    const auto count_primitive=[&](scene::MeshKey key) {
        key=scene::normalized(key);
        auto found=primitives.find(key);
        if (found==primitives.end()) {
            scene::MeshData mesh;
            if (!scene::build_mesh(key,mesh)) { result.diagnostic="Cannot budget primitive geometry"; return; }
            result.vertices+=mesh.vertices.size(); result.indices+=mesh.indices.size();
            result.bytes+=mesh.vertices.size()*128ULL+mesh.indices.size()*4ULL;
            found=primitives.emplace(key,mesh.indices.size()/3).first;
        }
        result.triangles+=found->second;
    };
    for (const auto& occurrence : evaluated.instances) count_primitive(occurrence.mesh);
    for ([[maybe_unused]] const auto& occurrence : evaluated.media) count_primitive({scene::Primitive::Quad,1});
    if (!result.diagnostic.empty()) return result;
    for (const auto& occurrence : evaluated.models) {
        if (occurrence.motion_source.value() && !scene::valid_link_affine(occurrence.world)) {
            result.diagnostic="Scatter group composition exceeds affine transform bounds"; return result;
        }
        if (stop.stop_requested()) { result.diagnostic="Environment admission cancelled"; return result; }
        auto found=assets.find(occurrence.source);
        if (found==assets.end()) {
            ModelAsset asset;
            if (!load_model_package(occurrence.source,asset)) { result.diagnostic="Cannot budget immutable model package: "+occurrence.source; return result; }
            animation.emplace(occurrence.source,AnimationBudget{asset.pose_bindings.size(),asset.animations.size(),!asset.animation_nodes.empty()});
            result.bytes+=asset.animation_nodes.size()*sizeof(ModelAnimationNode)+asset.pose_bindings.size()*sizeof(ModelPoseBinding);
            for (const auto& clip : asset.animations) for (const auto& channel : clip.channels)
                result.bytes+=channel.times.size()*sizeof(float)+channel.values.size()*sizeof(scene::Vec4);
            result.vertices+=asset.mesh.vertices.size(); result.indices+=asset.mesh.indices.size();
            // Geometry + parallel metadata + conservative per-instance upload.
            result.bytes+=asset.mesh.vertices.size()*128ULL+asset.mesh.indices.size()*4ULL;
            for (const auto& texture : asset.textures) {
                result.bytes+=texture.rgba.size();
                for (const auto& mip : texture.lower_mips) result.bytes+=mip.rgba.size();
            }
            core::u64 parts{}, triangles{};
            if (asset.lods.empty()) {
                parts=asset.parts.size(); triangles=asset.mesh.indices.size()/3;
            } else for (const auto& lod : asset.lods) {
                core::u64 count{};
                for (core::u32 i=0;i<lod.part_count;++i) count+=asset.parts[lod.first_part+i].index_count/3;
                parts=std::max(parts,static_cast<core::u64>(lod.part_count)); triangles=std::max(triangles,count);
            }
            found=assets.emplace(occurrence.source,std::pair{parts,triangles}).first;
        }
        result.parts+=found->second.first; result.triangles+=found->second.second;
        const auto& bound=animation.at(occurrence.source);
        result.pose_bindings+=bound.bindings;
        if (const auto linked=validated.value().occurrences.find(occurrence.source_node); linked != validated.value().occurrences.end()
            && linked->second.animated != bound.animated) { result.diagnostic="Animated GLB requires an explicit animation assembly record"; return result; }
        if (occurrence.animation.enabled && bound.clips && occurrence.animation.clip >= bound.clips) {
            result.diagnostic="Animation clip index is outside the published assembly"; return result;
        }
    }
    result.bytes+=result.parts*1024ULL;
    // Upper bound: four directional cascades and all 64 local atlas tiles.
    result.shadow_triangles=result.triangles*68ULL;
    result.bytes+=result.pose_bindings*2ULL*sizeof(scene::Mat4);
    if (result.pose_bindings > max_frame_pose_bindings) result.diagnostic="Environment animation palette budget exceeded (65536 bindings)";
    else if (result.instances > scene::max_scene_models) result.diagnostic="Environment instance budget exceeded (4096 including props)";
    else if (result.parts > 8192) result.diagnostic="Environment material submission budget exceeded (8192)";
    else if (result.vertices > (1ULL<<20) || result.indices > (3ULL<<20)) result.diagnostic="Environment shared geometry/upload budget exceeded (1M vertices / 3M indices)";
    else if (result.bytes > 256ULL*1024*1024) result.diagnostic="Environment working memory/upload budget exceeded (256 MiB)";
    else if (result.triangles > 2'000'000 || result.shadow_triangles > 136'000'000)
        result.diagnostic="Environment shadow/triangle budget exceeded (2M triangles / 136M worst-case shadow triangles)";
    return result;
}
} // namespace mediaserver::render
