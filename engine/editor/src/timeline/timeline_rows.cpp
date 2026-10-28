#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor::timeline {

RowList build_rows(RowsState &state, const RowsInput &input,
                   const gui::Theme &theme) {
  const std::span<const ElementInfo> elements = input.elements;
  const std::span<const scene::TimelineLane> lanes = input.lanes;
  const std::span<const output::ProjectionSurface> surfaces = input.surfaces;
  const std::span<const scene::EffectInstance> effects = input.effects;
  const std::span<const scene::ChannelKeyframes> keyframes = input.keyframes;
  const core::u64 show_frame = input.show_frame;
  const std::string_view search = input.search;
  const doc::NodeId anchor = input.anchor;
  std::vector<Row> rows;
  rows.reserve(elements.size() * 7U + surfaces.size() * 4U);
  const auto append_element = [&](const ElementInfo &element,
                                  std::size_t base_indent = 0U,
                                  bool hierarchy = false,
                                  bool include_context = false) {
    const std::string name = layer_name(element);
    if (!include_context && !matches(name, search) &&
        !matches(kind_name(element), search)) {
      return;
    }
    const bool transformable = animatable(element);
    rows.push_back(Row{Row::Kind::Layer, element.node, 0U, 0U, name,
                       element_kind_color(element), element.kind, base_indent,
                       transformable || hierarchy});
    rows.back().locked = element.locked;
    rows.back().element_visible = element.visible;
    rows.back().hierarchy = hierarchy;
    if (hierarchy) {
      if (!state.expanded_hierarchies.contains(element.node) && search.empty()) {
        return;
      }
      if (transformable) {
        rows.push_back(Row{Row::Kind::PropertiesHeader, element.node, 0U, 0U,
                           "Properties", theme.text_muted, element.kind,
                           ++base_indent, true});
      }
    }
    if (!transformable || state.expanded_layers.count(element.node.value()) == 0U) {
      return;
    }
    if (element.kind == ElementKind::Environment) {
      for (auto index = static_cast<std::size_t>(scene::PropertyChannel::SkyTimeOfDay);
           index <= static_cast<std::size_t>(scene::PropertyChannel::SkyIlluminance); ++index) {
        const auto property = static_cast<scene::PropertyChannel>(index);
        const auto sampled = sample_channel_at(
            channel_for(keyframes, element.node, property), show_frame);
        rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
            std::string{channel_label(property)}, theme.text_muted, element.kind,
            base_indent + 1U, false, property, element.node,
            sampled.has_value() && std::isfinite(*sampled)
                ? *sampled
                : scene::environment_channel_value(element.environment, property)});
      }
      return;
    }
    const auto append_group = [&](core::u8 group) {
      rows.push_back(
          Row{Row::Kind::Group, element.node, group, 0U,
              std::string{group_names[static_cast<std::size_t>(group)]},
              theme.text_muted, element.kind,
              base_indent + 1U, true});
      if (state.expanded_groups.count(group_id(element.node, group)) == 0U) {
        return;
      }
      for (core::u8 component = 0U; component < 3U; ++component) {
        rows.push_back(Row{Row::Kind::Component, element.node, group, component,
                           std::string{component_names[static_cast<std::size_t>(
                               group)][static_cast<std::size_t>(component)]},
                           theme.text_muted, element.kind,
                           base_indent + 2U, false});
      }
    };
    const auto append_property = [&](scene::PropertyChannel property,
                                      std::string label, double authored) {
      const auto sampled = sample_channel_at(
          channel_for(keyframes, element.node, property), show_frame);
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
                         std::move(label), theme.text_muted, element.kind,
                         base_indent + 1U, false, property, element.node,
                         sampled.has_value() && std::isfinite(*sampled)
                             ? *sampled
                             : authored});
    };
    if (is_light(element.kind)) {
      // A light keys where it is (a sun has no position, only a direction),
      // how bright it is, and its colour.
      if (element.kind != ElementKind::DirectionalLight) {
        append_group(0U);
      }
      append_property(scene::PropertyChannel::LightIntensity, "Intensity",
                      element.intensity);
      append_group(color_group);
      if (element.kind != ElementKind::DirectionalLight) {
        append_property(scene::PropertyChannel::LightRange, "Range",
                        element.range);
      }
      return;
    }
    if (element.kind == ElementKind::Camera) {
      // A camera moves and turns like anything else; it has no size.
      append_group(0U);
      append_group(1U);
      append_property(scene::PropertyChannel::CameraFieldOfView,
                      "Field of view", element.field_of_view);
      append_property(scene::PropertyChannel::CameraShutter,
                      "Shutter seconds", element.shutter_seconds);
      append_property(scene::PropertyChannel::CameraFocusDistance,
                      "Focus distance", element.focus_distance_m);
      append_property(scene::PropertyChannel::CameraAperture,
                      "Aperture", element.aperture_f_number);
      return;
    }
    for (core::u8 group = 0U; group < 3U; ++group) {
      append_group(group);
    }
    if (element.kind != ElementKind::Group) {
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U, "Opacity",
                         theme.text_muted, element.kind,
                         base_indent + 1U, false,
                         scene::PropertyChannel::Opacity, element.node,
                         element.style.opacity});
    }
    for (const scene::EffectInstance &effect : effects) {
      if (effect.target != element.node || !effect.enabled) {
        continue;
      }
      scene::PropertyChannel property{};
      std::string_view label;
      switch (effect.kind) {
      case scene::EffectKind::Brightness:
        property = scene::PropertyChannel::Brightness;
        label = "Brightness";
        break;
      case scene::EffectKind::Saturation:
        property = scene::PropertyChannel::Saturation;
        label = "Saturation";
        break;
      case scene::EffectKind::Blur:
        property = scene::PropertyChannel::BlurRadius;
        label = "Gaussian Blur Radius";
        break;
      case scene::EffectKind::BoxBlur:
        property = scene::PropertyChannel::BoxBlurRadius;
        label = "Box Blur Radius";
        break;
      case scene::EffectKind::DirectionalBlur:
        property = scene::PropertyChannel::DirectionalBlurLength;
        label = "Directional Blur Length";
        break;
      case scene::EffectKind::FilmGrain:
        property = scene::PropertyChannel::FilmGrainAmount;
        label = "Film Grain Amount";
        break;
      case scene::EffectKind::ChromaKey:
        property = scene::PropertyChannel::ChromaKeyMix;
        label = "Chroma Key Mix";
        break;
      case scene::EffectKind::LumaKey:
        property = scene::PropertyChannel::LumaKeyMix;
        label = "Luma Key Mix";
        break;
      case scene::EffectKind::Levels:
      case scene::EffectKind::LiftGammaGain:
      case scene::EffectKind::ContrastPivot:
      case scene::EffectKind::ColorMatrix:
      case scene::EffectKind::WhiteBalance:
      case scene::EffectKind::Curves:
      case scene::EffectKind::Count:
        // Structured colour payloads are edited by named schema
        // properties in the Color panel. They do not masquerade as a
        // transform channel until typed keyframe channels exist.
        continue;
      }
      double authored_value = static_cast<double>(effect.value);
      if (effect.kind == scene::EffectKind::BoxBlur) {
        authored_value = static_cast<double>(effect.box_blur.radius_px);
      } else if (effect.kind == scene::EffectKind::DirectionalBlur) {
        authored_value = static_cast<double>(effect.directional_blur.length_px);
      } else if (effect.kind == scene::EffectKind::FilmGrain) {
        authored_value = static_cast<double>(effect.film_grain.amount);
      } else if (effect.kind == scene::EffectKind::ChromaKey) {
        authored_value = static_cast<double>(effect.chroma_key.mix);
      } else if (effect.kind == scene::EffectKind::LumaKey) {
        authored_value = static_cast<double>(effect.luma_key.mix);
      }
      const auto sampled = sample_channel_at(
          channel_for(keyframes, effect.source, property), show_frame);
      const double displayed_value =
          sampled.has_value() && std::isfinite(*sampled) ? *sampled
                                                         : authored_value;
      rows.push_back(Row{Row::Kind::Property, element.node, 0U, 0U,
                         std::string{label}, theme.keyframe, element.kind,
                         base_indent + 1U, false,
                         property, effect.source, displayed_value});
    }
  };

  // Explicit tracks and their clips arrive in compositing order: lowest first,
  // which is the one drawn first and therefore the one at the back. The list
  // shows that stack the way every other tool does - what is in front is at
  // the top - so both the bands and the clips inside them are walked backwards.
  // The implicit lane is only an authoring sentinel for "no track". Consuming
  // it here would make every loose clip display by clip_order even though a
  // loose-clip reorder authors group_order, so those clips deliberately fall
  // through to the ordinary layer stack below.
  std::set<doc::NodeId> emitted_media;
  for (auto held = lanes.rbegin(); held != lanes.rend(); ++held) {
    const scene::TimelineLane &lane = *held;
    if (lane.implicit) {
      continue;
    }
    bool matching_child{};
    for (const ElementInfo &element : elements) {
      if (element.kind == ElementKind::Media &&
          element.timeline_lane == lane.id &&
          (matches(layer_name(element), search) ||
           matches(kind_name(element.kind), search))) {
        matching_child = true;
        break;
      }
    }
    if (!matches(lane.name, search) && !matching_child) {
      continue;
    }
    Row header{Row::Kind::LaneHeader,
               lane.id,
               0U,
               0U,
               lane.name,
               theme.accent,
               ElementKind::Media,
               0U,
               true};
    header.locked = lane.locked;
    header.muted = lane.muted;
    header.sync_locked = lane.sync_locked;
    header.lane_member = true;
    header.lane_owner = lane.id;
    rows.push_back(std::move(header));
    std::vector<const ElementInfo *> lane_clips;
    for (const ElementInfo &element : elements) {
      if (element.kind == ElementKind::Media &&
          element.timeline_lane == lane.id) {
        lane_clips.push_back(&element);
      }
    }
    // Higher clip_order is in front and therefore appears above lower peers.
    // Preserve the projection order for legacy files whose orders are equal.
    std::stable_sort(
        lane_clips.begin(), lane_clips.end(),
        [](const ElementInfo *left, const ElementInfo *right) noexcept {
          return left->clip_order > right->clip_order;
        });
    // A closed track lists only its header, unless a search is looking
    // inside it.
    const bool lane_open =
        !search.empty() || !state.collapsed_lanes.contains(lane.id);
    for (const ElementInfo *element : lane_clips) {
      emitted_media.insert(element->node);
      if (!lane_open) {
        continue;
      }
      const std::size_t first_row = rows.size();
      append_element(*element, 1U);
      // A clip's property rows belong to its lane too, so dropping onto an
      // expanded clip's Opacity row is not a hole in the target band.
      for (std::size_t row = first_row; row < rows.size(); ++row) {
        rows[row].lane_member = true;
        rows[row].lane_owner = lane.id;
      }
    }
  }
  std::vector<const ElementInfo *> ordinary_layers;
  std::vector<const ElementInfo *> unordered_layers;
  for (const ElementInfo &element : elements) {
    if (element.kind == ElementKind::Media &&
        emitted_media.contains(element.node)) {
      continue;
    }
    (orderable_layer(element) ? ordinary_layers : unordered_layers)
        .push_back(&element);
  }
  // group_order is a sibling order, not a hierarchy operation. Equal legacy
  // defaults retain their projected order; an authored reorder gets an
  // unambiguous higher-is-front visual stack.
  std::stable_sort(
      ordinary_layers.begin(), ordinary_layers.end(),
      [](const ElementInfo *left, const ElementInfo *right) noexcept {
        return left->group_order > right->group_order;
      });
  ordinary_layers.insert(ordinary_layers.end(), unordered_layers.begin(),
                         unordered_layers.end());
  std::set<doc::NodeId> layer_nodes;
  for (const ElementInfo *element : ordinary_layers) {
    layer_nodes.insert(element->node);
  }
  std::map<doc::NodeId, std::vector<const ElementInfo *>> children;
  for (const ElementInfo *element : ordinary_layers) {
    const doc::NodeId parent = layer_parent(*element);
    children[parent != element->node && layer_nodes.contains(parent)
                 ? parent : doc::NodeId{}].push_back(element);
  }

  // Parent-first traversal retains the outliner's linked sibling order and
  // applies authored group_order only among peers. Keep disconnected nodes
  // visible too; a missing parent or a malformed cycle must not lose a row.
  constexpr std::size_t no_parent = std::numeric_limits<std::size_t>::max();
  struct TreeEntry final {
    const ElementInfo *element{};
    std::size_t parent{std::numeric_limits<std::size_t>::max()};
    std::size_t depth{};
    bool matching{};
    bool visible{};
  };
  std::vector<TreeEntry> tree;
  tree.reserve(ordinary_layers.size());
  std::set<doc::NodeId> visited;
  const auto append_tree = [&](const ElementInfo *root) {
    std::vector<TreeEntry> pending{TreeEntry{root}};
    while (!pending.empty()) {
      TreeEntry entry = pending.back();
      pending.pop_back();
      if (!visited.insert(entry.element->node).second) {
        continue;
      }
      const std::size_t index = tree.size();
      entry.matching = matches(layer_name(*entry.element), search) ||
                       matches(kind_name(entry.element->kind), search) ||
                       (entry.parent != no_parent && tree[entry.parent].matching);
      tree.push_back(entry);
      if (const auto found = children.find(entry.element->node);
          found != children.end()) {
        for (auto child = found->second.rbegin(); child != found->second.rend();
             ++child) {
          pending.push_back(TreeEntry{*child, index, entry.depth + 1U});
        }
      }
    }
  };
  if (const auto roots = children.find(doc::NodeId{}); roots != children.end()) {
    for (const ElementInfo *root : roots->second) {
      append_tree(root);
    }
  }
  for (const ElementInfo *element : ordinary_layers) {
    if (!visited.contains(element->node)) {
      append_tree(element);
    }
  }
  for (std::size_t index = tree.size(); index-- > 0U;) {
    const TreeEntry &entry = tree[index];
    if (entry.matching && entry.parent != no_parent) {
      tree[entry.parent].matching = true;
    }
  }
  if (anchor != state.hierarchy_selection) {
    state.hierarchy_selection = anchor;
    for (const TreeEntry &entry : tree) {
      if (entry.element->node == anchor) {
        for (std::size_t parent = entry.parent; parent != no_parent;
             parent = tree[parent].parent) {
          state.expanded_hierarchies.insert(tree[parent].element->node);
        }
        break;
      }
    }
  }
  for (std::size_t index = 0U; index < tree.size(); ++index) {
    TreeEntry &entry = tree[index];
    entry.visible = entry.matching &&
        (entry.parent == no_parent ||
         (tree[entry.parent].visible &&
          (!search.empty() || state.expanded_hierarchies.contains(
                                  tree[entry.parent].element->node))));
    if (entry.visible) {
      const bool has_children = index + 1U < tree.size() &&
                                tree[index + 1U].parent == index;
      append_element(*entry.element, entry.depth, has_children, true);
    }
  }
  for (const output::ProjectionSurface &surface : surfaces) {
    if (!matches(surface.name, search) &&
        !matches("LED output physical surface", search)) {
      continue;
    }
    Row surface_row{Row::Kind::Layer,
                    surface.node,
                    0U,
                    0U,
                    surface.name.empty() ? "Surface" : surface.name,
                    theme.accent,
                    ElementKind::Object,
                    0U,
                    true};
    surface_row.surface = true;
    rows.push_back(std::move(surface_row));
    if (state.expanded_layers.count(surface.node.value()) == 0U) {
      continue;
    }

    Row position{
        Row::Kind::Group,       surface.node,        0U, 0U,  "Position",
        theme.text_muted, ElementKind::Object, 1U, true};
    position.surface = true;
    position.component_count = 3U;
    rows.push_back(std::move(position));
    if (state.expanded_groups.count(group_id(surface.node, 0U)) != 0U) {
      for (core::u8 component = 0U; component < 3U; ++component) {
        Row one{Row::Kind::Component,
                surface.node,
                0U,
                component,
                std::string{component_names[0U][component]},
                theme.text_muted,
                ElementKind::Object,
                2U,
                false};
        one.surface = true;
        rows.push_back(std::move(one));
      }
    }

    Row rotation{
        Row::Kind::Group,       surface.node,        1U, 0U,  "Rotation",
        theme.text_muted, ElementKind::Object, 1U, true};
    rotation.surface = true;
    rotation.component_count = 1U;
    rows.push_back(std::move(rotation));
    if (state.expanded_groups.count(group_id(surface.node, 1U)) != 0U) {
      Row yaw{Row::Kind::Component,   surface.node,        1U, 0U,   "Yaw",
              theme.text_muted, ElementKind::Object, 2U, false};
      yaw.surface = true;
      rows.push_back(std::move(yaw));
    }
  }
  for (Row &row : rows) {
    if (row.surface) {
      const output::ProjectionSurface *const surface =
          find_surface(surfaces, row.node);
      row.locked = surface != nullptr && surface->locked;
    } else if (row.kind != Row::Kind::LaneHeader) {
      const ElementInfo *const element = find_element(elements, row.node);
      row.locked = element != nullptr && element->locked;
    }
  }
  std::vector<doc::NodeId> visible_order;
  visible_order.reserve(elements.size());
  for (const Row &row : rows) {
    if ((row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader) &&
        !row.surface && !row.locked &&
        std::find(visible_order.begin(), visible_order.end(), row.node) ==
            visible_order.end()) {
      visible_order.push_back(row.node);
    }
  }
  // A placed clip's row takes the height the operator gave clip rows - its
  // own, or every clip row's - so a filmstrip and waveform can be read.
  // Every other row is one list row high.
  std::vector<doc::NodeId> placed_clips;
  for (const ElementInfo &element : elements) {
    if (element.kind == ElementKind::Media && element.timing.placed()) {
      placed_clips.push_back(element.node);
    }
  }
  std::sort(placed_clips.begin(), placed_clips.end());
  float top = 0.0F;
  for (Row &row : rows) {
    row.top = top;
    row.height = row_height;
    if (row.kind == Row::Kind::Layer && !row.surface &&
        row.element_kind == ElementKind::Media) {
      if (std::binary_search(placed_clips.begin(), placed_clips.end(),
                             row.node)) {
        const auto own = state.clip_row_heights.find(row.node);
        row.height = std::clamp(own != state.clip_row_heights.end()
                                    ? own->second
                                    : state.clip_row_height,
                                row_height, clip_row_height_maximum);
      }
    }
    top += row.height;
  }

  // The tree the list shows. Rows are listed parent first, each child one
  // indent deeper, so a row's parent is the nearest earlier row one indent
  // shallower and its descendants run up to the next row no deeper than it.
  std::vector<std::size_t> open;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    while (!open.empty() && rows[open.back()].indent >= rows[index].indent) {
      rows[open.back()].end = index;
      open.pop_back();
    }
    rows[index].parent = open.empty() ? no_row : open.back();
    rows[index].depth =
        open.empty() ? 0U : rows[rows[index].parent].depth + 1U;
    open.push_back(index);
  }
  for (const std::size_t index : open) {
    rows[index].end = rows.size();
  }

  // What each closed parent stands for: a closed track its clips, a closed
  // hierarchy parent itself and everything below it, and a layer or
  // Properties foldout whose properties are folded its own channels.
  // Rebuilt every frame, so sorted runs rather than a node per entry.
  std::vector<std::pair<core::u64, const scene::ChannelKeyframes *>>
      channels_of;
  channels_of.reserve(keyframes.size());
  for (const scene::ChannelKeyframes &channel : keyframes) {
    channels_of.emplace_back(channel.target.value(), &channel);
  }
  std::ranges::sort(channels_of, {}, &decltype(channels_of)::value_type::first);
  std::vector<std::pair<core::u64, core::u64>> effect_sources;
  for (const scene::EffectInstance &effect : effects) {
    if (effect.enabled) {
      effect_sources.emplace_back(effect.target.value(),
                                  effect.source.value());
    }
  }
  std::ranges::sort(effect_sources);
  std::vector<std::pair<core::u64, const ElementInfo *>> element_of;
  element_of.reserve(elements.size());
  for (const ElementInfo &element : elements) {
    element_of.emplace_back(element.node.value(), &element);
  }
  std::ranges::sort(element_of, {}, &decltype(element_of)::value_type::first);
  const auto run_of = [](const auto &sorted, core::u64 key) {
    return std::ranges::equal_range(sorted, key, {},
                                    [](const auto &entry) { return entry.first; });
  };

  std::vector<RowSummary> summaries;
  std::vector<SummaryKey> summary_keys;
  std::vector<std::pair<core::i64, scene::PropertyChannel>> keyed;
  std::vector<const ElementInfo *> covered;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    Row &row = rows[index];
    if (row.end != index + 1U ||
        (row.kind != Row::Kind::LaneHeader && row.kind != Row::Kind::Layer &&
         row.kind != Row::Kind::PropertiesHeader)) {
      continue;
    }
    covered.clear();
    keyed.clear();
    bool spanned = false;
    core::i64 first = 0;
    core::i64 end = 0;
    const auto take_span = [&](core::i64 from, core::i64 to) {
      first = spanned ? std::min(first, from) : from;
      end = spanned ? std::max(end, to) : to;
      spanned = true;
    };
    const auto take_channels = [&](core::u64 target) {
      for (const auto &[unused, channel] : run_of(channels_of, target)) {
        for (const core::i64 frame : channel->frames) {
          keyed.emplace_back(frame, channel->component);
          take_span(frame, frame);
        }
      }
    };
    bool own_only = false;
    if (row.kind == Row::Kind::LaneHeader) {
      for (const ElementInfo &element : elements) {
        if (element.kind == ElementKind::Media &&
            element.timeline_lane == row.node) {
          covered.push_back(&element);
        }
      }
    } else if (row.surface) {
      take_channels(row.node.value());
    } else if (const auto found = run_of(element_of, row.node.value());
               !found.empty()) {
      covered.push_back(found.front().second);
      own_only = row.kind == Row::Kind::PropertiesHeader;
      if (row.hierarchy) {
        // Everything below a closed parent, at any depth.
        for (std::size_t next = 0U; next < covered.size(); ++next) {
          if (const auto below = children.find(covered[next]->node);
              below != children.end()) {
            for (const ElementInfo *child : below->second) {
              if (std::find(covered.begin(), covered.end(), child) ==
                  covered.end()) {
                covered.push_back(child);
              }
            }
          }
        }
      }
    }
    for (const ElementInfo *element : covered) {
      if (!own_only && element->kind == ElementKind::Media &&
          element->timing.placed()) {
        take_span(element->timing.start,
                  element->timing.start + element->timing.duration);
      }
      take_channels(element->node.value());
      for (const auto &[unused, source] :
           run_of(effect_sources, element->node.value())) {
        take_channels(source);
      }
    }
    if (!spanned) {
      continue;
    }
    std::sort(keyed.begin(), keyed.end());
    RowSummary summary{first, end, summary_keys.size(), 0U};
    for (const auto &[frame, channel] : keyed) {
      if (summary.key_count > 0U && summary_keys.back().frame == frame) {
        SummaryKey &key = summary_keys.back();
        ++key.count;
        key.mixed = key.mixed || key.channel != channel;
      } else {
        summary_keys.push_back(SummaryKey{frame, 1U, channel, false});
        ++summary.key_count;
      }
    }
    row.summary = summaries.size();
    summaries.push_back(summary);
  }
  return RowList{std::move(rows), std::move(visible_order),
                 std::move(summaries), std::move(summary_keys)};
}

} // namespace mediaserver::editor::timeline
