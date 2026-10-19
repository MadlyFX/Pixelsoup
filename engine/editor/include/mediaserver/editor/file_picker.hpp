#pragma once

#include <mediaserver/doc/ids.hpp>

#include <string>
#include <vector>

namespace mediaserver::editor {

// Panels request a resource; only the live shell opens a native dialog.
enum class FilePickerKind { IesProfile, ShowLut, EnvironmentMap, BakeFolder, ModelTexture, GeneratorTexture };

struct FilePickerRequest final {
  FilePickerKind kind{};
  std::string current;
  std::vector<doc::NodeId> targets;
  std::string model_source;
  core::u32 material_part{}, texture_role{};
  bool clear_texture{};
};

} // namespace mediaserver::editor
