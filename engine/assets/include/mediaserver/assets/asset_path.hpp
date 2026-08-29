#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Asset sources are absolute machine paths.
//
// Every machine in a show sees the same path for the same file: a clip the
// operator opens from D:\Media\clip.mov is opened from D:\Media\clip.mov by
// every render node. Media bytes are not copied into the project; optional
// expected length/SHA-256 metadata verifies that the path resolves to the same
// content revision on each machine.
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
// presence-only part of asset readiness. Fingerprinted assets additionally use
// content_fingerprint.hpp to verify the expected revision.
[[nodiscard]] bool asset_source_present(std::string_view source) noexcept;

} // namespace mediaserver::assets
