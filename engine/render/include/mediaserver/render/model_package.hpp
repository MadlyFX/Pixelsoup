#pragma once

#include <mediaserver/render/model_loader.hpp>

namespace mediaserver::render {
// Worker-only immutable ingest. A package contains baked geometry, exact material
// values, and role-filtered mip payloads; render nodes never reinterpret glTF.
struct ModelIngestResult final {
    std::string source;
    std::string diagnostic;
    core::u64 vertex_count{};
    core::u64 triangle_count{};
    core::u32 material_count{};
    ModelBounds bounds{};
};

[[nodiscard]] ModelIngestResult ingest_model(const std::string& source);
[[nodiscard]] bool load_model_package(const std::string& source, ModelAsset& into);
// Also exposed for tools/tests. Writes a new immutable, content-addressed sibling
// of source, never replaces an existing asset or package.
[[nodiscard]] std::string write_model_package(const std::string& source, const ModelAsset& asset);
} // namespace mediaserver::render
