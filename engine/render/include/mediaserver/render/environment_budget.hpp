#pragma once
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/model_animation.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <tuple>

namespace mediaserver::render {
struct EnvironmentBudget {
    core::u64 instances{}, parts{}, triangles{}, vertices{}, indices{}, bytes{}, shadow_triangles{}, pose_bindings{};
    std::string diagnostic;
};

namespace detail {
// What one immutable model package contributes to the budget.
struct AssetBudget final {
    core::u64 parts{}, triangles{}, vertices{}, indices{}, bytes{}, bindings{}, clips{};
    bool animated{};
};
// Packages are immutable, so each is decoded and measured once per process:
// every later revision - a placement-only drag of a heavy scene included -
// reads the counts instead of decoding the whole scene again. Keyed by path,
// size and write time, so a file replaced in place is measured again.
inline std::optional<AssetBudget> measured_asset(const std::string& source) {
    namespace fs = std::filesystem;
    static std::mutex mutex;
    static std::map<std::string, std::tuple<std::uintmax_t, fs::file_time_type, AssetBudget>> cache;
    std::error_code error;
    const auto size = fs::file_size(source, error);
    if (error) return std::nullopt;
    const auto written = fs::last_write_time(source, error);
    if (error) return std::nullopt;
    {
        const std::lock_guard lock{mutex};
        if (const auto found = cache.find(source); found != cache.end()
            && std::get<0>(found->second) == size && std::get<1>(found->second) == written) {
            return std::get<2>(found->second);
        }
    }
    ModelAsset asset;
    if (!load_model_package(source, asset)) return std::nullopt;
    AssetBudget budget{};
    budget.bindings = asset.pose_bindings.size();
    budget.clips = asset.animations.size();
    budget.animated = !asset.animation_nodes.empty();
    budget.bytes += asset.animation_nodes.size()*sizeof(ModelAnimationNode)+asset.pose_bindings.size()*sizeof(ModelPoseBinding);
    for (const auto& clip : asset.animations) for (const auto& channel : clip.channels)
        budget.bytes += channel.times.size()*sizeof(float)+channel.values.size()*sizeof(scene::Vec4);
    budget.vertices = asset.mesh.vertices.size();
    budget.indices = asset.mesh.indices.size();
    // Geometry + parallel metadata + conservative per-instance upload.
    budget.bytes += asset.mesh.vertices.size()*128ULL+asset.mesh.indices.size()*4ULL;
    for (const auto& texture : asset.textures) {
        budget.bytes += texture.rgba.size();
        for (const auto& mip : texture.lower_mips) budget.bytes += mip.rgba.size();
    }
    if (asset.lods.empty()) {
        budget.parts = asset.parts.size(); budget.triangles = asset.mesh.indices.size()/3;
    } else for (const auto& lod : asset.lods) {
        core::u64 count{};
        for (core::u32 i=0;i<lod.part_count;++i) count+=asset.parts[lod.first_part+i].index_count/3;
        budget.parts = std::max(budget.parts,static_cast<core::u64>(lod.part_count));
        budget.triangles = std::max(budget.triangles,count);
    }
    const std::lock_guard lock{mutex};
    if (cache.size() >= 16384U) cache.clear();
    cache.insert_or_assign(source, std::tuple{size, written, budget});
    return budget;
}
} // namespace detail
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
            const auto measured=detail::measured_asset(occurrence.source);
            if (!measured) { result.diagnostic="Cannot budget immutable model package: "+occurrence.source; return result; }
            animation.emplace(occurrence.source,AnimationBudget{measured->bindings,measured->clips,measured->animated});
            result.bytes+=measured->bytes;
            result.vertices+=measured->vertices; result.indices+=measured->indices;
            found=assets.emplace(occurrence.source,std::pair{measured->parts,measured->triangles}).first;
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
    // One scene's share of the renderer's device-resident geometry arena
    // (48M vertices / 144M indices), leaving room for the scene it replaces
    // and for refreshes: a ~30M-triangle scene.
    else if (result.vertices > (32ULL<<20) || result.indices > (96ULL<<20)) result.diagnostic="Environment shared geometry/upload budget exceeded (32M vertices / 96M indices)";
    else if (result.bytes > 8ULL*1024*1024*1024) result.diagnostic="Environment working memory/upload budget exceeded (8 GiB)";
    else if (result.triangles > 32'000'000 || result.shadow_triangles > 2'176'000'000ULL)
        result.diagnostic="Environment shadow/triangle budget exceeded (32M triangles / 2176M worst-case shadow triangles)";
    return result;
}
} // namespace mediaserver::render
