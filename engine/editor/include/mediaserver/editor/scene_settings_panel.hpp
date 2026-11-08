#pragma once

// Scene settings shown in the Inspector: Renderer and Environment.

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/file_picker.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/project_settings_panel.hpp>
#include <mediaserver/editor/scene_panel.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mediaserver::editor {

struct EnvironmentKeyEdit final {
  scene::PropertyChannel channel{};
  double value{};
  bool erase{};
};

struct SceneSettingsFrame final {
  std::optional<FilePickerRequest> file_picker;
  ProjectSettingsFrame project;
  std::vector<EnvironmentKeyEdit> keys;
};

// Persistent scene controls share normal Inspector layout and author each
// gesture immediately. Resource/topology policies remain discrete settings;
// environment values use the same channels and diamonds as object properties.
class SceneSettingsPanel final {
public:
  void set_bake_destination(std::string path) { bake_destination_ = std::move(path); }
  [[nodiscard]] SceneSettingsFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
      SceneSettingsTarget target, const ProjectSettingsState& state,
      std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
      std::span<const ElementInfo> elements = {}, bool auto_key = true);
private:
  NumberField fields_;
  SceneSettingsTarget target_{SceneSettingsTarget::None};
  doc::NodeId stage_{};
  gui::ScrollRegion scroll_;
  std::array<bool, 10> sections_{{true,false,false,false,false,false,true,false,false,false}};
  std::string bake_destination_{"lighting"};
};

} // namespace mediaserver::editor
