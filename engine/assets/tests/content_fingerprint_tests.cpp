#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>

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
#include <system_error>

namespace {

namespace assets = mediaserver::assets;
namespace fs = std::filesystem;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define FINGERPRINT_CHECK(...)                                                 \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_fingerprint_" + std::to_string(stamp));
        std::error_code error;
        if (!fs::create_directory(path_, error) || error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write fixture"};
    }
}

void known_sha256_answers_match() {
    TemporaryDirectory temporary;
    const fs::path empty = temporary.path() / "empty.bin";
    const fs::path abc = temporary.path() / "abc.bin";
    const fs::path million_a = temporary.path() / "million-a.bin";
    write(empty, "");
    write(abc, "abc");
    write(million_a, std::string(1'000'000U, 'a'));

    const auto empty_result = assets::content_fingerprint(
        assets::asset_source_from_path(empty));
    const auto abc_result = assets::content_fingerprint(
        assets::asset_source_from_path(abc));
    const auto million_a_result = assets::content_fingerprint(
        assets::asset_source_from_path(million_a));
    FINGERPRINT_CHECK(empty_result);
    FINGERPRINT_CHECK(abc_result);
    FINGERPRINT_CHECK(million_a_result);
    FINGERPRINT_CHECK(empty_result.value().byte_length == 0U);
    FINGERPRINT_CHECK(abc_result.value().byte_length == 3U);
    FINGERPRINT_CHECK(
        assets::sha256_hex(empty_result.value().sha256) ==
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855");
    FINGERPRINT_CHECK(
        assets::sha256_hex(abc_result.value().sha256) ==
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad");
    FINGERPRINT_CHECK(million_a_result.value().byte_length == 1'000'000U);
    FINGERPRINT_CHECK(
        assets::sha256_hex(million_a_result.value().sha256) ==
        "cdc76e5c9914fb9281a1c7e284d73e67"
        "f1809a48a497200e046d39ccc7112cd0");
}

void hexadecimal_round_trip_is_canonical_and_strict() {
    constexpr std::string_view encoded =
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad";
    const auto parsed = assets::sha256_from_hex(encoded);
    FINGERPRINT_CHECK(parsed.has_value());
    FINGERPRINT_CHECK(assets::sha256_hex(*parsed) == encoded);
    FINGERPRINT_CHECK(!assets::sha256_from_hex("abc").has_value());
    std::string malformed{encoded};
    malformed[8U] = 'x';
    FINGERPRINT_CHECK(!assets::sha256_from_hex(malformed).has_value());
}

void missing_invalid_and_cancelled_sources_are_distinct() {
    TemporaryDirectory temporary;
    const std::string missing = assets::asset_source_from_path(
        temporary.path() / "missing.bin");
    const auto missing_result = assets::content_fingerprint(missing);
    FINGERPRINT_CHECK(!missing_result);
    FINGERPRINT_CHECK(
        missing_result.error().code ==
        assets::ContentFingerprintErrorCode::SourceMissing);

    const auto invalid = assets::content_fingerprint("relative.bin");
    FINGERPRINT_CHECK(!invalid);
    FINGERPRINT_CHECK(invalid.error().code ==
                      assets::ContentFingerprintErrorCode::InvalidSource);

    const fs::path file = temporary.path() / "ready.bin";
    write(file, "content");
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled = assets::content_fingerprint(
        assets::asset_source_from_path(file), cancellation.get_token());
    FINGERPRINT_CHECK(!cancelled);
    FINGERPRINT_CHECK(cancelled.error().code ==
                      assets::ContentFingerprintErrorCode::Cancelled);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"known SHA-256 answers match", &known_sha256_answers_match},
    {"hexadecimal round trip is canonical and strict",
     &hexadecimal_round_trip_is_canonical_and_strict},
    {"missing invalid and cancelled sources are distinct",
     &missing_invalid_and_cancelled_sources_are_distinct},
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
