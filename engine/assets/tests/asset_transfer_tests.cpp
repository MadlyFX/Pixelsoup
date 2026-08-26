#include <mediaserver/assets/asset_transfer.hpp>

#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace fs = std::filesystem;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define TRANSFER_CHECK(...)                                                    \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::error_code error;
        const fs::path base = fs::temp_directory_path(error);
        if (error) {
            throw std::runtime_error{"could not locate temporary directory"};
        }
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        for (std::size_t attempt = 0U; attempt < 128U; ++attempt) {
            path_ = base /
                ("mediaserver_asset_transfer_" + std::to_string(stamp) + "_" +
                 std::to_string(attempt));
            error.clear();
            if (fs::create_directory(path_, error)) {
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error{"could not create temporary directory"};
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<std::byte>(
            static_cast<unsigned char>(character)));
    }
    return result;
}

void write_bytes(const fs::path& path, std::span<const std::byte> content) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error{"could not open test file"};
    }
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write test file"};
    }
}

[[nodiscard]] std::vector<std::byte> read_bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        throw std::runtime_error{"could not open result file"};
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error{"could not size result file"};
    }
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(result.data()),
               static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
        throw std::runtime_error{"could not read result file"};
    }
    return result;
}

[[nodiscard]] assets::ContentIdentity identity_for(const fs::path& path) {
    auto identity = assets::hash_file(path);
    if (!identity) {
        throw std::runtime_error{"could not hash test file"};
    }
    return identity.value();
}

[[nodiscard]] assets::AssetTransferLimits small_limits(
    std::size_t chunk_bytes = 7U) {
    return assets::AssetTransferLimits{1'024U, chunk_bytes};
}

void a_verified_source_serves_only_bounded_content_ranges() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "registered.mov";
    const auto content = bytes("0123456789abcdefghijklmnop");
    write_bytes(source_path, content);
    const auto identity = identity_for(source_path);

    auto opened = assets::VerifiedAssetSource::open_registered(
        source_path, identity, small_limits(5U));
    TRANSFER_CHECK(opened);
    auto source = std::move(opened).value();
    TRANSFER_CHECK(source.identity() == identity);

    auto first = source.read_chunk(0U, 5U);
    TRANSFER_CHECK(first);
    TRANSFER_CHECK(first.value().offset == 0U);
    TRANSFER_CHECK(first.value().bytes == bytes("01234"));
    TRANSFER_CHECK(!first.value().last);

    auto tail = source.read_chunk(identity.byte_size - 3U, 5U);
    TRANSFER_CHECK(tail);
    TRANSFER_CHECK(tail.value().bytes == bytes("nop"));
    TRANSFER_CHECK(tail.value().last);

    auto end = source.read_chunk(identity.byte_size, 1U);
    TRANSFER_CHECK(end);
    TRANSFER_CHECK(end.value().bytes.empty());
    TRANSFER_CHECK(end.value().last);

    const auto oversized = source.read_chunk(0U, 6U);
    TRANSFER_CHECK(!oversized);
    TRANSFER_CHECK(oversized.error().code ==
                   assets::AssetTransferErrorCode::ChunkTooLarge);
    const auto outside = source.read_chunk(identity.byte_size + 1U, 1U);
    TRANSFER_CHECK(!outside);
    TRANSFER_CHECK(outside.error().code ==
                   assets::AssetTransferErrorCode::InvalidOffset);

    auto wrong_digest = identity;
    wrong_digest.sha256[0U] ^= 0xffU;
    const auto mismatched_hash = assets::VerifiedAssetSource::open_registered(
        source_path, wrong_digest, small_limits());
    TRANSFER_CHECK(!mismatched_hash);
    TRANSFER_CHECK(mismatched_hash.error().code ==
                   assets::AssetTransferErrorCode::HashMismatch);

    auto wrong_size = identity;
    ++wrong_size.byte_size;
    const auto mismatched_size = assets::VerifiedAssetSource::open_registered(
        source_path, wrong_size, small_limits());
    TRANSFER_CHECK(!mismatched_size);
    TRANSFER_CHECK(mismatched_size.error().code ==
                   assets::AssetTransferErrorCode::SizeMismatch);
}

void an_interrupted_transfer_resumes_and_installs_atomically() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "plate.MP4";
    const fs::path cache = temporary.path() / "cache";
    const auto content = bytes("one deterministic media object for transfer");
    write_bytes(source_path, content);
    const auto identity = identity_for(source_path);
    const auto limits = small_limits(7U);

    auto source_result = assets::VerifiedAssetSource::open_registered(
        source_path, identity, limits);
    TRANSFER_CHECK(source_result);
    auto source = std::move(source_result).value();

    {
        auto sink_result = assets::ResumableAssetSink::open(
            cache, identity, R"(D:\Show\Plates\Opening.MP4)", limits);
        TRANSFER_CHECK(sink_result);
        auto sink = std::move(sink_result).value();
        auto first = source.read_chunk(0U, limits.max_chunk_bytes);
        TRANSFER_CHECK(first);
        const auto appended = sink.append(first.value().offset,
                                          first.value().bytes);
        TRANSFER_CHECK(appended);
        TRANSFER_CHECK(appended.value() == 7U);
    }

    auto resumed_result = assets::ResumableAssetSink::open(
        cache, identity, "Opening.MP4", limits);
    TRANSFER_CHECK(resumed_result);
    auto resumed = std::move(resumed_result).value();
    TRANSFER_CHECK(resumed.resume_offset() == 7U);
    while (resumed.resume_offset() < identity.byte_size) {
        auto chunk = source.read_chunk(resumed.resume_offset(),
                                       limits.max_chunk_bytes);
        TRANSFER_CHECK(chunk);
        const auto appended = resumed.append(chunk.value().offset,
                                             chunk.value().bytes);
        TRANSFER_CHECK(appended);
    }
    const auto receipt = resumed.finalize();
    TRANSFER_CHECK(receipt);
    TRANSFER_CHECK(resumed.complete());
    TRANSFER_CHECK(receipt.value().identity == identity);
    TRANSFER_CHECK(receipt.value().local_path.extension() == ".mp4");
    TRANSFER_CHECK(read_bytes(receipt.value().local_path) == content);
    const auto verified =
        assets::verify_file(receipt.value().local_path, identity);
    TRANSFER_CHECK(verified);
    TRANSFER_CHECK(verified.value() == assets::ContentVerification::Verified);

    const auto expected_relative =
        assets::shared_store_relative_path(identity, "Opening.MP4");
    TRANSFER_CHECK(expected_relative.has_value());
    TRANSFER_CHECK(receipt.value().local_path ==
                   fs::canonical(cache) / *expected_relative);
    TRANSFER_CHECK(receipt.value().local_path.string().find("Opening") ==
                   std::string::npos);
    const fs::path blob_path = fs::canonical(cache) /
        assets::shared_store_blob_relative_path(identity);
    TRANSFER_CHECK(fs::is_regular_file(blob_path));
    const auto blob_verified = assets::verify_file(blob_path, identity);
    TRANSFER_CHECK(blob_verified);
    TRANSFER_CHECK(blob_verified.value() ==
                   assets::ContentVerification::Verified);

    // The canonical blob can recreate a loader alias without retransferring
    // bytes, and resolution does not create a resumable partial.
    TRANSFER_CHECK(fs::remove(receipt.value().local_path));
    const auto resolved = assets::resolve_verified_asset(
        cache, identity, "another/path/OPENING.mp4", limits);
    TRANSFER_CHECK(resolved);
    TRANSFER_CHECK(resolved.value().local_path ==
                   receipt.value().local_path);
    TRANSFER_CHECK(read_bytes(resolved.value().local_path) == content);

    auto reopened_result = assets::ResumableAssetSink::open(
        cache, identity, "elsewhere/ALIAS.MP4", limits);
    TRANSFER_CHECK(reopened_result);
    auto reopened = std::move(reopened_result).value();
    TRANSFER_CHECK(reopened.complete());
    TRANSFER_CHECK(reopened.resume_offset() == identity.byte_size);
    const auto repeated = reopened.finalize();
    TRANSFER_CHECK(repeated);
    const auto rejected_append = reopened.append(0U, bytes("x"));
    TRANSFER_CHECK(!rejected_append);
    TRANSFER_CHECK(rejected_append.error().code ==
                   assets::AssetTransferErrorCode::AlreadyComplete);
}

void offsets_sizes_and_extensions_are_strictly_bounded() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "tiny.bin";
    const auto content = bytes("12345678");
    write_bytes(source_path, content);
    const auto identity = identity_for(source_path);

    auto invalid_limits = small_limits();
    invalid_limits.max_chunk_bytes =
        assets::absolute_max_transfer_chunk_bytes + 1U;
    const auto invalid_source = assets::VerifiedAssetSource::open_registered(
        source_path, identity, invalid_limits);
    TRANSFER_CHECK(!invalid_source);
    TRANSFER_CHECK(invalid_source.error().code ==
                   assets::AssetTransferErrorCode::InvalidLimits);

    auto too_small = small_limits();
    too_small.max_asset_bytes = identity.byte_size - 1U;
    const auto oversized = assets::ResumableAssetSink::open(
        temporary.path() / "oversized", identity, "tiny.bin", too_small);
    TRANSFER_CHECK(!oversized);
    TRANSFER_CHECK(oversized.error().code ==
                   assets::AssetTransferErrorCode::SizeLimitExceeded);

    const auto unsafe_extension = assets::ResumableAssetSink::open(
        temporary.path() / "bad_extension", identity,
        "movie.mp4?download=true", small_limits());
    TRANSFER_CHECK(!unsafe_extension);
    TRANSFER_CHECK(unsafe_extension.error().code ==
                   assets::AssetTransferErrorCode::InvalidLogicalSource);

    const fs::path unresolved_cache = temporary.path() / "unresolved";
    const auto unresolved = assets::resolve_verified_asset(
        unresolved_cache, identity, "tiny.bin", small_limits());
    TRANSFER_CHECK(!unresolved);
    TRANSFER_CHECK(unresolved.error().code ==
                   assets::AssetTransferErrorCode::Incomplete);
    fs::path unresolved_part = fs::canonical(unresolved_cache) /
        assets::shared_store_blob_relative_path(identity);
    unresolved_part += ".part";
    TRANSFER_CHECK(!fs::exists(unresolved_part));

    auto sink_result = assets::ResumableAssetSink::open(
        temporary.path() / "bounded", identity, "tiny.bin", small_limits(4U));
    TRANSFER_CHECK(sink_result);
    auto sink = std::move(sink_result).value();

    const auto wrong_offset = sink.append(1U, bytes("1"));
    TRANSFER_CHECK(!wrong_offset);
    TRANSFER_CHECK(wrong_offset.error().code ==
                   assets::AssetTransferErrorCode::InvalidOffset);
    const auto large_chunk = sink.append(0U, bytes("12345"));
    TRANSFER_CHECK(!large_chunk);
    TRANSFER_CHECK(large_chunk.error().code ==
                   assets::AssetTransferErrorCode::ChunkTooLarge);
    const auto incomplete = sink.finalize();
    TRANSFER_CHECK(!incomplete);
    TRANSFER_CHECK(incomplete.error().code ==
                   assets::AssetTransferErrorCode::Incomplete);
    TRANSFER_CHECK(sink.append(0U, bytes("1234")));
    TRANSFER_CHECK(sink.append(4U, bytes("5678")));
    const auto beyond = sink.append(8U, bytes("x"));
    TRANSFER_CHECK(!beyond);
    TRANSFER_CHECK(beyond.error().code ==
                   assets::AssetTransferErrorCode::DataBeyondEnd);
}

void a_hash_mismatch_never_becomes_resolvable_content() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "good.glb";
    const auto good = bytes("GOODGOOD");
    const auto bad = bytes("BAD!BAD!");
    write_bytes(source_path, good);
    const auto identity = identity_for(source_path);
    const fs::path cache = temporary.path() / "cache";

    auto sink_result = assets::ResumableAssetSink::open(
        cache, identity, "models/Hero.GLB", small_limits(16U));
    TRANSFER_CHECK(sink_result);
    auto sink = std::move(sink_result).value();
    TRANSFER_CHECK(sink.append(0U, bad));
    const auto finalized = sink.finalize();
    TRANSFER_CHECK(!finalized);
    TRANSFER_CHECK(finalized.error().code ==
                   assets::AssetTransferErrorCode::HashMismatch);

    const auto relative =
        assets::shared_store_relative_path(identity, "Hero.GLB");
    TRANSFER_CHECK(relative.has_value());
    const fs::path final_path = fs::canonical(cache) / *relative;
    fs::path part_path = fs::canonical(cache) /
        assets::shared_store_blob_relative_path(identity);
    part_path += ".part";
    TRANSFER_CHECK(!fs::exists(final_path));
    TRANSFER_CHECK(!fs::exists(part_path));

    auto restarted = assets::ResumableAssetSink::open(
        cache, identity, "Hero.GLB", small_limits(16U));
    TRANSFER_CHECK(restarted);
    TRANSFER_CHECK(restarted.value().resume_offset() == 0U);
}

void destination_names_ignore_traversal_and_preserve_only_type() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "safe.mov";
    const auto content = bytes("safe destination bytes");
    write_bytes(source_path, content);
    const auto identity = identity_for(source_path);
    const fs::path cache = temporary.path() / "cache";

    auto sink_result = assets::ResumableAssetSink::open(
        cache, identity, R"(..\..\outside\TAKE.MOV)", small_limits(64U));
    TRANSFER_CHECK(sink_result);
    auto sink = std::move(sink_result).value();
    TRANSFER_CHECK(sink.append(0U, content));
    const auto receipt = sink.finalize();
    TRANSFER_CHECK(receipt);
    TRANSFER_CHECK(receipt.value().local_path.extension() == ".mov");
    TRANSFER_CHECK(receipt.value().local_path.parent_path().parent_path()
                       .parent_path() == fs::canonical(cache));
    TRANSFER_CHECK(!fs::exists(temporary.path() / "outside"));

    TemporaryDirectory blocked;
    const fs::path blocked_cache = blocked.path() / "cache";
    TRANSFER_CHECK(fs::create_directories(blocked_cache));
    write_bytes(blocked_cache / "sha256", bytes("not a directory"));
    const auto refused = assets::ResumableAssetSink::open(
        blocked_cache, identity, "safe.mov", small_limits(64U));
    TRANSFER_CHECK(!refused);
    TRANSFER_CHECK(refused.error().code ==
                   assets::AssetTransferErrorCode::UnsafeCacheRoot);

    // A dangling staged-file symlink must not be mistaken for an absent file:
    // opening it for append would otherwise follow the link outside the
    // configured cache root. Some Windows installations disallow creating
    // symlinks for an unprivileged test process, so exercise this assertion
    // whenever the platform permits the fixture.
    TemporaryDirectory linked;
    const fs::path linked_cache = linked.path() / "cache";
    fs::path linked_part = linked_cache /
        assets::shared_store_blob_relative_path(identity);
    linked_part += ".part";
    TRANSFER_CHECK(fs::create_directories(linked_part.parent_path()));
    const fs::path escaped_target = linked.path() / "outside-partial";
    std::error_code link_error;
    fs::create_symlink(escaped_target, linked_part, link_error);
    if (!link_error) {
        const auto dangling = assets::ResumableAssetSink::open(
            linked_cache, identity, "safe.mov", small_limits(64U));
        TRANSFER_CHECK(!dangling);
        TRANSFER_CHECK(dangling.error().code ==
                       assets::AssetTransferErrorCode::UnsafeCacheRoot);
        TRANSFER_CHECK(!fs::exists(escaped_target));
    }
}

void corrupt_completed_and_oversized_partial_files_are_repaired() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "image.EXR";
    const auto content = bytes("verified image bytes");
    write_bytes(source_path, content);
    const auto identity = identity_for(source_path);
    const fs::path cache = temporary.path() / "cache";
    const auto limits = small_limits(64U);

    auto first_result = assets::ResumableAssetSink::open(
        cache, identity, "image.EXR", limits);
    TRANSFER_CHECK(first_result);
    auto first = std::move(first_result).value();
    TRANSFER_CHECK(first.append(0U, content));
    const auto installed = first.finalize();
    TRANSFER_CHECK(installed);
    const fs::path final_path = installed.value().local_path;

    write_bytes(final_path, bytes("corrupted image bytes"));
    {
        auto repair_result = assets::ResumableAssetSink::open(
            cache, identity, "IMAGE.exr", limits);
        TRANSFER_CHECK(repair_result);
        auto repair = std::move(repair_result).value();
        TRANSFER_CHECK(!repair.complete());
        TRANSFER_CHECK(repair.resume_offset() == 0U);
        TRANSFER_CHECK(!fs::exists(final_path));
    }

    // Leave an overlong partial behind. The next open truncates it to the
    // descriptor boundary, rejects its hash, removes it, and starts at zero.
    fs::path part_path = fs::canonical(cache) /
        assets::shared_store_blob_relative_path(identity);
    part_path += ".part";
    write_bytes(part_path, bytes("this partial is much longer than expected"));
    auto recovered = assets::ResumableAssetSink::open(
        cache, identity, "IMAGE.exr", limits);
    TRANSFER_CHECK(recovered);
    TRANSFER_CHECK(recovered.value().resume_offset() == 0U);
    TRANSFER_CHECK(fs::file_size(part_path) == 0U);
}

void pre_cancelled_resolution_does_not_touch_the_store() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source.mov";
    write_bytes(source, bytes("cancelled resolution"));
    const auto identity = identity_for(source);
    const fs::path store = temporary.path() / "store";
    std::stop_source cancelled;
    TRANSFER_CHECK(cancelled.request_stop());
    const auto resolved = assets::resolve_verified_asset(
        store, identity, "source.mov", small_limits(64U),
        cancelled.get_token());
    TRANSFER_CHECK(!resolved);
    TRANSFER_CHECK(resolved.error().code ==
                   assets::AssetTransferErrorCode::Cancelled);
    TRANSFER_CHECK(!fs::exists(store));

    const auto opened_cancelled = assets::ResumableAssetSink::open(
        store, identity, "source.mov", small_limits(64U),
        cancelled.get_token());
    TRANSFER_CHECK(!opened_cancelled);
    TRANSFER_CHECK(opened_cancelled.error().code ==
                   assets::AssetTransferErrorCode::Cancelled);
    TRANSFER_CHECK(!fs::exists(store));

    const auto content = bytes("cancelled resolution");
    auto opened = assets::ResumableAssetSink::open(
        store, identity, "source.mov", small_limits(64U));
    TRANSFER_CHECK(opened);
    auto sink = std::move(opened).value();
    TRANSFER_CHECK(sink.append(0U, content));
    const auto finalized_cancelled = sink.finalize(cancelled.get_token());
    TRANSFER_CHECK(!finalized_cancelled);
    TRANSFER_CHECK(finalized_cancelled.error().code ==
                   assets::AssetTransferErrorCode::Cancelled);
    const auto finalized = sink.finalize();
    TRANSFER_CHECK(finalized);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"a verified source serves only bounded content ranges",
     &a_verified_source_serves_only_bounded_content_ranges},
    {"an interrupted transfer resumes and installs atomically",
     &an_interrupted_transfer_resumes_and_installs_atomically},
    {"offsets sizes and extensions are strictly bounded",
     &offsets_sizes_and_extensions_are_strictly_bounded},
    {"a hash mismatch never becomes resolvable content",
     &a_hash_mismatch_never_becomes_resolvable_content},
    {"destination names ignore traversal and preserve only type",
     &destination_names_ignore_traversal_and_preserve_only_type},
    {"corrupt completed and oversized partial files are repaired",
     &corrupt_completed_and_oversized_partial_files_are_repaired},
    {"pre-cancelled resolution does not touch the store",
     &pre_cancelled_resolution_does_not_touch_the_store},
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
