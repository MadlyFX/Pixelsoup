#include <mediaserver/assets/content_resolver.hpp>

#include <string>

namespace mediaserver::assets {
namespace {

[[nodiscard]] ContentResolveResult failure(
    ContentResolveErrorCode code) {
    return ContentResolveResult::failure(ContentResolveError{code});
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    std::string_view encoded) {
    std::u8string native;
    native.reserve(encoded.size());
    for (const unsigned char byte : encoded) {
        native.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{native};
}

} // namespace

std::optional<std::string>
portable_content_extension(std::string_view source) {
    const std::size_t slash = source.find_last_of("/\\");
    const std::size_t dot = source.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash) ||
        dot + 1U == source.size()) {
        return std::nullopt;
    }
    const std::string_view candidate = source.substr(dot);
    // Long or punctuation-bearing suffixes are more likely a template/query
    // than a real format extension. Reject them explicitly.
    if (candidate.size() > 16U) {
        return std::nullopt;
    }
    std::string extension;
    extension.reserve(candidate.size());
    extension.push_back('.');
    for (std::size_t index = 1U; index < candidate.size(); ++index) {
        const char character = candidate[index];
        if (character >= 'A' && character <= 'Z') {
            extension.push_back(
                static_cast<char>(character - 'A' + 'a'));
        } else if ((character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9')) {
            extension.push_back(character);
        } else {
            return std::nullopt;
        }
    }
    return extension;
}

bool valid_project_relative_source(std::string_view source) noexcept {
    if (source.empty() || source.front() == '/' || source.back() == '/' ||
        source.find('\\') != std::string_view::npos ||
        source.find(':') != std::string_view::npos ||
        source.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < source.size()) {
        const std::size_t end = source.find('/', begin);
        const std::string_view component = source.substr(
            begin, end == std::string_view::npos ? source.size() - begin
                                                 : end - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

std::optional<std::filesystem::path> shared_store_relative_path(
    const ContentIdentity& identity,
    std::string_view source_hint) {
    const auto extension = portable_content_extension(source_hint);
    if (!extension.has_value()) {
        return std::nullopt;
    }
    const std::string digest = format_sha256(identity.sha256);
    return std::filesystem::path{"sha256"} / digest.substr(0U, 2U) /
        (digest + *extension);
}

std::filesystem::path shared_store_blob_relative_path(
    const ContentIdentity& identity) {
    const std::string digest = format_sha256(identity.sha256);
    return std::filesystem::path{"sha256"} / digest.substr(0U, 2U) /
        (digest + ".blob");
}

ContentResolveResult resolve_content_source(
    std::string_view source,
    const AssetContent& content,
    const ContentResolutionRoots& roots) {
    if (source.empty()) {
        return failure(ContentResolveErrorCode::EmptySource);
    }
    switch (content.policy) {
    case AssetDistributionPolicy::External:
        return ContentResolveResult::success(
            path_from_utf8(source));
    case AssetDistributionPolicy::ProjectRelative:
        if (roots.project_file.empty() || !roots.project_file.is_absolute()) {
            return failure(ContentResolveErrorCode::ProjectFileRequired);
        }
        if (!valid_project_relative_source(source)) {
            return failure(
                ContentResolveErrorCode::UnsafeProjectRelativeSource);
        }
        return ContentResolveResult::success(
            (roots.project_file.parent_path() /
             path_from_utf8(source))
                .lexically_normal());
    case AssetDistributionPolicy::SharedStore:
        if (roots.shared_store.empty() || !roots.shared_store.is_absolute()) {
            return failure(ContentResolveErrorCode::SharedStoreRequired);
        }
        if (!content.identity.has_value()) {
            return failure(ContentResolveErrorCode::ContentIdentityRequired);
        }
        const auto relative =
            shared_store_relative_path(*content.identity, source);
        if (!relative.has_value()) {
            return failure(
                ContentResolveErrorCode::UnsafeSharedStoreExtension);
        }
        return ContentResolveResult::success(
            (roots.shared_store / *relative).lexically_normal());
    }
    return failure(ContentResolveErrorCode::EmptySource);
}

ContentResolveResult resolve_asset_source(
    const Asset& asset,
    const ContentResolutionRoots& roots) {
    return resolve_content_source(asset_source(asset), asset_content(asset),
                                  roots);
}

} // namespace mediaserver::assets
