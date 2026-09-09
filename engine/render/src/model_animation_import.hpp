// Private worker-only glTF adapter. Included after cgltf by model_loader.cpp.
#pragma once
#include <mediaserver/render/model_animation.hpp>

namespace mediaserver::render {
inline bool import_animation_nodes(const cgltf_data &data, ModelAsset &asset,
                                   std::vector<core::u32> &remap) {
  if (data.nodes_count > max_model_animation_nodes ||
      data.animations_count > 64U)
    return false;
  remap.resize(data.nodes_count, 0xFFFFFFFFU);
  while (asset.animation_nodes.size() < data.nodes_count) {
    bool progress = false;
    for (cgltf_size i = 0; i < data.nodes_count; ++i) {
      if (remap[i] != 0xFFFFFFFFU)
        continue;
      const auto &source = data.nodes[i];
      const auto parent = source.parent ? cgltf_node_index(&data, source.parent)
                                        : data.nodes_count;
      if (parent < data.nodes_count && remap[parent] == 0xFFFFFFFFU)
        continue;
      ModelAnimationNode node{};
      node.parent = parent < data.nodes_count ? remap[parent] : 0xFFFFFFFFU;
      if (source.has_translation)
        node.translation = {source.translation[0], source.translation[1],
                            source.translation[2]};
      if (source.has_rotation)
        node.rotation = {source.rotation[0], source.rotation[1],
                         source.rotation[2], source.rotation[3]};
      if (source.has_scale)
        node.scale = {source.scale[0], source.scale[1], source.scale[2]};
      node.has_matrix = source.has_matrix != 0;
      if (node.has_matrix)
        std::copy_n(source.matrix, 16, node.matrix.m.begin());
      remap[i] = static_cast<core::u32>(asset.animation_nodes.size());
      asset.animation_nodes.push_back(node);
      progress = true;
    }
    if (!progress)
      return false;
  }
  std::size_t total_keys = 0;
  for (cgltf_size i = 0; i < data.animations_count; ++i) {
    const auto &source = data.animations[i];
    if (source.channels_count > max_model_animation_nodes * 3U)
      return false;
    ModelAnimationClip clip{};
    if (source.name)
      clip.name = source.name;
    for (cgltf_size j = 0; j < source.channels_count; ++j) {
      const auto &channel = source.channels[j];
      if (!channel.target_node)
        continue;
      if (!channel.sampler || !channel.sampler->input ||
          !channel.sampler->output)
        return false;
      const auto &sampler = *channel.sampler;
      ModelAnimationChannel c{};
      c.node = remap[cgltf_node_index(&data, channel.target_node)];
      switch (channel.target_path) {
      case cgltf_animation_path_type_translation:
        c.path = ModelAnimationPath::Translation;
        break;
      case cgltf_animation_path_type_rotation:
        c.path = ModelAnimationPath::Rotation;
        break;
      case cgltf_animation_path_type_scale:
        c.path = ModelAnimationPath::Scale;
        break;
      default:
        asset.diagnostic =
            "Morph animation is not supported; export node or skin animation";
        return false;
      }
      switch (sampler.interpolation) {
      case cgltf_interpolation_type_linear:
        c.interpolation = ModelInterpolation::Linear;
        break;
      case cgltf_interpolation_type_step:
        c.interpolation = ModelInterpolation::Step;
        break;
      case cgltf_interpolation_type_cubic_spline:
        c.interpolation = ModelInterpolation::CubicSpline;
        break;
      default:
        return false;
      }
      total_keys += sampler.input->count;
      const auto components = c.path == ModelAnimationPath::Rotation ? 4U : 3U;
      if (total_keys > 1000000U || sampler.input->count == 0 ||
          sampler.input->type != cgltf_type_scalar ||
          sampler.input->component_type != cgltf_component_type_r_32f ||
          sampler.output->type !=
              (components == 4 ? cgltf_type_vec4 : cgltf_type_vec3) ||
          sampler.output->count !=
              sampler.input->count *
                  (c.interpolation == ModelInterpolation::CubicSpline ? 3U
                                                                      : 1U))
        return false;
      c.times.resize(sampler.input->count);
      for (cgltf_size k = 0; k < sampler.input->count; ++k) {
        if (!cgltf_accessor_read_float(sampler.input, k, &c.times[k], 1))
          return false;
        clip.duration = std::max(clip.duration, c.times[k]);
      }
      c.values.resize(sampler.output->count);
      for (cgltf_size k = 0; k < sampler.output->count; ++k) {
        float value[4]{};
        if (!cgltf_accessor_read_float(sampler.output, k, value, components))
          return false;
        c.values[k] = {value[0], value[1], value[2], value[3]};
      }
      clip.channels.push_back(std::move(c));
    }
    asset.animations.push_back(std::move(clip));
  }
  return true;
}
} // namespace mediaserver::render
