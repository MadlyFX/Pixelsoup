#pragma once

#include <mediaserver/scene/geometry.hpp>

#include <string>

// Loads an imported 3D model file into the renderer's mesh format.
//
// Behind the same idea as the media decode seam: the scene carries only a path,
// and this turns it into geometry the renderer uploads and caches. glTF and OBJ
// are supported, chosen by extension. Only base geometry is read - positions,
// normals (generated flat if absent), and UVs; materials and textures are a
// later step. Pure with respect to the file, so two nodes with the same file
// build the same mesh.
namespace mediaserver::render {

// Fills `into` (cleared first) with the model's geometry, node transforms baked
// in. Returns false on any failure (unreadable file, unknown extension, empty
// result), which the caller draws as nothing rather than a failed frame.
[[nodiscard]] bool load_model(const std::string& source, scene::MeshData& into);

} // namespace mediaserver::render
