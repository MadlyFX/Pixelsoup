#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Asset sources are absolute machine paths.
//
// Every machine in a show sees the same path for the same file: a clip the
// operator opens from D:\Media\clip.mov is opened from D:\Media\clip.mov by
// every render node. Nothing is copied, hashed, cached, or content-addressed,
// so importing a 40 GB clip costs one probe and adds one document node.
//
// Readiness is therefore a single question - can this machine open that path -
// and it is asked per machine, never resolved centrally.
namespace mediaserver::assets {

// A usable authored source is non-empty, absolute, and carries no '.' or '..'
// component, so it can never depend on a process working directory. Existence
// is deliberately not checked: a path on a share that is not mounted yet is
// still a valid authored source, and the readiness report is what says so.
[[nodiscard]] bool valid_asset_source(std::string_view source) noexcept;

// UTF-8 document text to a native path and back. Document sources stay UTF-8
// on every platform; only the filesystem call sites use native encoding.
[[nodiscard]] std::filesystem::path
asset_path_from_source(std::string_view source);
[[nodiscard]] std::string
asset_source_from_path(const std::filesystem::path& path);

// Whether this machine can open the source right now. A path that names a
// directory, a dangling link, or nothing at all is not present. This is the
// whole of asset readiness in a path-only model.
[[nodiscard]] bool asset_source_present(std::string_view source) noexcept;

} // namespace mediaserver::assets
