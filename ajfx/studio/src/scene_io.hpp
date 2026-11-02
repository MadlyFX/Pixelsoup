#pragma once
#include "document_io.hpp"
#include "json.hpp"

namespace ajfx::studio {
[[nodiscard]] std::string save_scene(const Document &document);
[[nodiscard]] Loaded load_scene(const json::Value &root);
} // namespace ajfx::studio
