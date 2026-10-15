#pragma once

#include <mediaserver/doc/ids.hpp>

#include <string>
#include <vector>

namespace mediaserver::editor {

// Panels request a resource; only the live shell opens a native dialog.
enum class FilePickerKind { IesProfile, ShowLut, EnvironmentMap, BakeFolder };

struct FilePickerRequest final {
  FilePickerKind kind{};
  std::string current;
  std::vector<doc::NodeId> targets;
};

} // namespace mediaserver::editor
