#pragma once

#include "document.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Studio scenes on disk: `.ajfxstudio`, JSON in the `ajfx-studio-scene`
// format (scene_io.hpp).
namespace ajfx::studio {

inline constexpr std::string_view document_extension = ".ajfxstudio";

[[nodiscard]] std::string save_document(const Document& document);

struct Loaded final {
    std::optional<Document> document;
    std::string error;
};

// Refuses anything but a scene, with a reason.
[[nodiscard]] Loaded load_document(std::string_view text);

[[nodiscard]] bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                              std::string& error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                                  std::string& error);

} // namespace ajfx::studio
