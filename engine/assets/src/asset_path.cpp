#include <mediaserver/assets/asset_path.hpp>

#include <system_error>

namespace mediaserver::assets {
namespace {

[[nodiscard]] bool relative_component(std::string_view component) noexcept {
    return component == "." || component == "..";
}

} // namespace

bool valid_asset_source(std::string_view source) noexcept {
    if (source.empty() ||
        source.find('\0') != std::string_view::npos) {
        return false;
    }
    // A '.'/'..' component would make the path depend on whichever directory
    // the process happens to be started in, which differs between the editor
    // and a lifecycle-supervised render child on the same machine.
    std::size_t begin = 0U;
    while (begin <= source.size()) {
        const std::size_t end = source.find_first_of("/\\", begin);
        const std::string_view component = source.substr(
            begin, end == std::string_view::npos ? source.size() - begin
                                                 : end - begin);
        if (relative_component(component)) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    try {
        return asset_path_from_source(source).is_absolute();
    } catch (...) {
        return false;
    }
}

std::filesystem::path asset_path_from_source(std::string_view source) {
    std::u8string native;
    native.reserve(source.size());
    for (const unsigned char byte : source) {
        native.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{std::move(native)};
}

std::string asset_source_from_path(const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

bool asset_source_present(std::string_view source) noexcept {
    try {
        if (!valid_asset_source(source)) {
            return false;
        }
        std::error_code error;
        // status() follows links deliberately: a link whose target is present
        // is a file this machine can open, which is the question being asked.
        const auto status =
            std::filesystem::status(asset_path_from_source(source), error);
        return !error && std::filesystem::is_regular_file(status);
    } catch (...) {
        return false;
    }
}

} // namespace mediaserver::assets
