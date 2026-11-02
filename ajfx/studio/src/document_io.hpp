#pragma once

#include "document.hpp"
#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Studio pages on disk: `.ajfxstudio`, JSON. A page keeps only the values an
// artist changed, so it stays readable and survives new defaults.
namespace ajfx::studio {

inline constexpr std::string_view document_extension = ".ajfxstudio";
// 3: Pile and Prop became one Object block (and so did the model, size and
//    material each of them carried). A page saved at 2 or earlier is
//    converted as it loads, each Object keeping the uid of the block it came
//    from so that module ids and seeds -- and therefore the particles -- stay
//    where they were.
// 4: Appearance's "debris" material became "model", since it is the one
//    that draws a 3D model and "Solid" beside it read as a near-synonym.
inline constexpr int document_version = 4;

[[nodiscard]] std::string save_document(const Document& document);

// One block as the page stores it, and back. Detaching keeps the block it
// detached as this text, to put it back together later.
[[nodiscard]] json::Value block_to_json(const Block& block);
[[nodiscard]] std::optional<Block> block_from_json(const json::Value& value, std::string& error);
[[nodiscard]] std::string block_text(const Block& block);
[[nodiscard]] std::optional<Block> block_from_text(std::string_view text, std::string& error);

struct Loaded final {
    std::optional<Document> document;
    std::string error;
};

// Refuses other formats and newer versions with a reason. Values a control no
// longer accepts are put back in range; unknown controls are dropped.
[[nodiscard]] Loaded load_document(std::string_view text);

[[nodiscard]] bool write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                              std::string& error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                                                  std::string& error);

} // namespace ajfx::studio
