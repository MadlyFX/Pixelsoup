#pragma once

#include <mediaserver/render/model_loader.hpp>

#include <string>

// Private seams between the format readers that fill one ModelAsset. Each
// reader lives in its own translation unit; this keeps shared post-processing
// (MikkTSpace tangents) in exactly one implementation.
namespace mediaserver::render::detail {

// Generates default MikkTSpace tangents for one triangle run of `asset`,
// against its already-baked positions, normals and UV0.
[[nodiscard]] bool generate_model_tangents(ModelAsset& asset,
                                           core::u32 first_index,
                                           core::u32 index_count) noexcept;

// Reads an Autodesk FBX file (binary or ASCII) as static geometry in the
// renderer's glTF-convention model space: right-handed, +Y up, metres.
[[nodiscard]] bool load_fbx(const std::string& path, ModelAsset& into);

} // namespace mediaserver::render::detail
