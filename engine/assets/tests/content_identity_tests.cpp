#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = mediaserver::assets;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define CONTENT_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        const auto nonce = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
            ("mediaserver-content-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path write(std::string_view name,
                                              std::string_view bytes) const {
        const auto result = path_ / std::filesystem::path{name};
        std::ofstream output{result, std::ios::binary | std::ios::trunc};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        CONTENT_CHECK(output.good());
        return result;
    }

  private:
    std::filesystem::path path_;
};

void hexadecimal_is_strict_and_round_trips() {
    constexpr std::string_view expected =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    const auto parsed = assets::parse_sha256(expected);
    CONTENT_CHECK(parsed.has_value());
    CONTENT_CHECK(assets::format_sha256(*parsed) == expected);
    CONTENT_CHECK(!assets::parse_sha256("ba78").has_value());
    std::string uppercase{expected};
    uppercase[0U] = 'B';
    CONTENT_CHECK(!assets::parse_sha256(uppercase).has_value());
    std::string punctuation{expected};
    punctuation[10U] = '-';
    CONTENT_CHECK(!assets::parse_sha256(punctuation).has_value());
}

void file_hash_and_integrity_match_known_sha256() {
    TemporaryDirectory temporary;
    const auto file = temporary.write("sample.bin", "abc");
    auto identity = assets::hash_file(file);
    CONTENT_CHECK(identity);
    CONTENT_CHECK(identity.value().byte_size == 3U);
    CONTENT_CHECK(assets::format_sha256(identity.value().sha256) ==
                  "ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad");

    auto verified = assets::verify_file(file, identity.value());
    CONTENT_CHECK(verified);
    CONTENT_CHECK(verified.value() == assets::ContentVerification::Verified);

    static_cast<void>(temporary.write("sample.bin", "abd"));
    auto digest_mismatch = assets::verify_file(file, identity.value());
    CONTENT_CHECK(digest_mismatch);
    CONTENT_CHECK(digest_mismatch.value() ==
                  assets::ContentVerification::DigestMismatch);

    static_cast<void>(temporary.write("sample.bin", "longer"));
    auto size_mismatch = assets::verify_file(file, identity.value());
    CONTENT_CHECK(size_mismatch);
    CONTENT_CHECK(size_mismatch.value() ==
                  assets::ContentVerification::SizeMismatch);
}

void invalid_inputs_fail_without_inventing_an_identity() {
    TemporaryDirectory temporary;
    auto empty = assets::hash_file({});
    CONTENT_CHECK(!empty);
    CONTENT_CHECK(empty.error().code ==
                  assets::ContentIntegrityErrorCode::InvalidPath);

    auto directory = assets::hash_file(temporary.path());
    CONTENT_CHECK(!directory);
    CONTENT_CHECK(directory.error().code ==
                  assets::ContentIntegrityErrorCode::NotRegularFile);

    auto missing = assets::hash_file(temporary.path() / "missing.bin");
    CONTENT_CHECK(!missing);
    CONTENT_CHECK(missing.error().code ==
                      assets::ContentIntegrityErrorCode::OpenFailed ||
                  missing.error().code ==
                      assets::ContentIntegrityErrorCode::NotRegularFile);
}

[[nodiscard]] assets::ContentIdentity representative_identity() {
    const auto digest = assets::parse_sha256(
        "0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef");
    CONTENT_CHECK(digest.has_value());
    return assets::ContentIdentity{*digest, 42U};
}

void portable_paths_cannot_escape_the_project() {
    CONTENT_CHECK(assets::valid_project_relative_source("media/plate.mov"));
    CONTENT_CHECK(!assets::valid_project_relative_source("../plate.mov"));
    CONTENT_CHECK(!assets::valid_project_relative_source("media//plate.mov"));
    CONTENT_CHECK(!assets::valid_project_relative_source("C:/plate.mov"));
    CONTENT_CHECK(!assets::valid_project_relative_source("media\\plate.mov"));

    assets::AssetContent content{};
    content.policy = assets::AssetDistributionPolicy::ProjectRelative;
    content.identity = representative_identity();
    const auto project =
        std::filesystem::absolute("show/project.mstage").lexically_normal();
    auto resolved = assets::resolve_content_source(
        "media/plate.mov", content,
        assets::ContentResolutionRoots{project, {}});
    CONTENT_CHECK(resolved);
    CONTENT_CHECK(resolved.value() ==
                  (project.parent_path() / "media/plate.mov").lexically_normal());
}

void shared_store_keys_retain_only_a_safe_format_extension() {
    const assets::ContentIdentity identity = representative_identity();
    CONTENT_CHECK(assets::portable_content_extension("C:\\Show\\PLATE.MP4") ==
                  std::optional<std::string>{".mp4"});
    CONTENT_CHECK(!assets::portable_content_extension("plate").has_value());
    CONTENT_CHECK(!assets::portable_content_extension("plate.bad-ext")
                       .has_value());
    const auto relative =
        assets::shared_store_relative_path(identity, "C:\\Show\\PLATE.MP4");
    CONTENT_CHECK(relative.has_value());
    CONTENT_CHECK(relative->generic_string() ==
                  "sha256/01/"
                  "0123456789abcdef0123456789abcdef"
                  "0123456789abcdef0123456789abcdef.mp4");
    CONTENT_CHECK(assets::shared_store_blob_relative_path(identity)
                      .generic_string() ==
                  "sha256/01/"
                  "0123456789abcdef0123456789abcdef"
                  "0123456789abcdef0123456789abcdef.blob");

    assets::AssetContent content{};
    content.policy = assets::AssetDistributionPolicy::SharedStore;
    content.identity = identity;
    const auto store = std::filesystem::absolute("asset-store");
    auto resolved = assets::resolve_content_source(
        "plate.MP4", content, assets::ContentResolutionRoots{{}, store});
    CONTENT_CHECK(resolved);
    CONTENT_CHECK(resolved.value() ==
                  (store / *relative).lexically_normal());

    auto unsafe = assets::resolve_content_source(
        "no-extension", content,
        assets::ContentResolutionRoots{{}, store});
    CONTENT_CHECK(!unsafe);
    CONTENT_CHECK(unsafe.error().code ==
                  assets::ContentResolveErrorCode::UnsafeSharedStoreExtension);
}

void utf8_authored_paths_cross_the_filesystem_boundary_losslessly() {
    constexpr std::u8string_view encoded = u8"\u5a92\u9ad4/\u821e\u53f0.mov";
    std::string authored;
    authored.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        authored.push_back(static_cast<char>(byte));
    }

    assets::AssetContent external{};
    external.policy = assets::AssetDistributionPolicy::External;
    const auto resolved = assets::resolve_content_source(
        authored, external, assets::ContentResolutionRoots{});
    CONTENT_CHECK(resolved);
    CONTENT_CHECK(resolved.value() == std::filesystem::path{encoded});

    assets::AssetContent relative{};
    relative.policy = assets::AssetDistributionPolicy::ProjectRelative;
    relative.identity = representative_identity();
    const auto project = std::filesystem::absolute("show/project.mstage");
    const auto project_source = assets::resolve_content_source(
        authored, relative, assets::ContentResolutionRoots{project, {}});
    CONTENT_CHECK(project_source);
    CONTENT_CHECK(project_source.value() ==
                  (project.parent_path() / std::filesystem::path{encoded})
                      .lexically_normal());
}

void pre_cancelled_integrity_work_never_reads_the_file() {
    TemporaryDirectory temporary;
    const auto file = temporary.write("cancel.bin", "cancel me");
    const auto identity = assets::hash_file(file);
    CONTENT_CHECK(identity);

    std::stop_source cancelled;
    CONTENT_CHECK(cancelled.request_stop());
    const auto hashed = assets::hash_file(file, cancelled.get_token());
    CONTENT_CHECK(!hashed);
    CONTENT_CHECK(hashed.error().code ==
                  assets::ContentIntegrityErrorCode::Cancelled);
    const auto verified = assets::verify_file(
        file, identity.value(), cancelled.get_token());
    CONTENT_CHECK(!verified);
    CONTENT_CHECK(verified.error().code ==
                  assets::ContentIntegrityErrorCode::Cancelled);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"hexadecimal is strict and round trips",
     &hexadecimal_is_strict_and_round_trips},
    {"file hash and integrity match known SHA-256",
     &file_hash_and_integrity_match_known_sha256},
    {"invalid inputs fail without identity",
     &invalid_inputs_fail_without_inventing_an_identity},
    {"portable paths cannot escape the project",
     &portable_paths_cannot_escape_the_project},
    {"shared store keys retain safe format extension",
     &shared_store_keys_retain_only_a_safe_format_extension},
    {"UTF-8 authored paths cross the filesystem boundary losslessly",
     &utf8_authored_paths_cross_the_filesystem_boundary_losslessly},
    {"pre-cancelled integrity work never reads the file",
     &pre_cancelled_integrity_work_never_reads_the_file},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
