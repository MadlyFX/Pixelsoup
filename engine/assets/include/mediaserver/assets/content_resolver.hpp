#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// Pure path resolution for the three manifest distribution policies. This
// module decides where content ought to be; transfer/store population and the
// final integrity check remain explicit separate operations.
namespace mediaserver::assets {

struct ContentResolutionRoots final {
    // Absolute path to the saved .mstage project. Only its parent is used.
    std::filesystem::path project_file;
    // Absolute directory containing the verified shared content store.
    std::filesystem::path shared_store;

    bool operator==(const ContentResolutionRoots&) const = default;
};

enum class ContentResolveErrorCode : core::u8 {
    EmptySource,
    ProjectFileRequired,
    SharedStoreRequired,
    ContentIdentityRequired,
    UnsafeProjectRelativeSource,
    UnsafeSharedStoreExtension,
};

struct ContentResolveError final {
    ContentResolveErrorCode code{ContentResolveErrorCode::EmptySource};

    bool operator==(const ContentResolveError&) const = default;
};

using ContentResolveResult =
    core::Result<std::filesystem::path, ContentResolveError>;

// Portable project-relative paths use '/' separators and contain no root,
// drive/ADS colon, empty, '.', or '..' component. This keeps one authored path
// inside the project directory on every supported host platform.
[[nodiscard]] bool
valid_project_relative_source(std::string_view source) noexcept;

// Stable store key. The source hint contributes only a conservative lowercase
// extension so current extension-routed decoders/loaders still recognise the
// content; identity and sharding are entirely digest-derived.
[[nodiscard]] std::optional<std::string>
portable_content_extension(std::string_view logical_source);

// Canonical extension-independent object path used by digest-only transfer and
// readiness checks. Loader-facing aliases below retain a safe source extension
// because current media/model dispatch is extension-routed.
[[nodiscard]] std::filesystem::path shared_store_blob_relative_path(
    const ContentIdentity& identity);

[[nodiscard]] std::optional<std::filesystem::path> shared_store_relative_path(
    const ContentIdentity& identity,
    std::string_view source_hint = {});

[[nodiscard]] ContentResolveResult resolve_content_source(
    std::string_view source,
    const AssetContent& content,
    const ContentResolutionRoots& roots);

[[nodiscard]] ContentResolveResult resolve_asset_source(
    const Asset& asset,
    const ContentResolutionRoots& roots);

} // namespace mediaserver::assets
