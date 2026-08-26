#include <mediaserver/persist/project_file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

namespace {

namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace wire = mediaserver::wire;

constexpr std::size_t project_header_size = 32U;
constexpr std::size_t project_version_offset = 8U;
constexpr std::size_t project_payload_size_offset = 16U;
constexpr std::size_t project_header_crc_offset = 28U;
constexpr std::array<std::byte, 8U> project_magic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'P'}, std::byte{'R'},
    std::byte{'O'}, std::byte{'J'}, std::byte{'0'}, std::byte{'1'},
};

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define PROJECT_CHECK(expression)                                              \
  check(static_cast<bool>(expression), #expression, __LINE__)

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("mediaserver-project-file-" + std::to_string(timestamp) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    std::error_code error;
    const auto created = std::filesystem::create_directory(path_, error);
    if (!created || error) {
      throw std::runtime_error{"could not create temporary project directory"};
    }
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] std::filesystem::path
  file(std::string_view name = "project.msproject") const {
    return path_ / std::filesystem::path{name};
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

[[nodiscard]] doc::Root restored_root(std::span<const doc::Node> nodes,
                                      doc::Version version) {
  auto restored = doc::Root::restore(nodes, version);
  PROJECT_CHECK(restored.has_value());
  return std::move(*restored);
}

[[nodiscard]] doc::Root unknown_project(doc::Version version = doc::Version{
                                            91U}) {
  const std::array nodes{
      doc::Node{
          doc::NodeId{2U},
          "vendor.future.effect",
          {
              {"future.boolean", true},
              {"future.float", 0.125},
              {"future.integer", std::int64_t{-9'223'372'036'854'775LL}},
              {"future.string", std::string{"opaque \xE2\x98\x83 value"}},
          },
      },
      doc::Node{
          doc::NodeId{4'000'000'001U},
          "plugin.unregistered.node",
          {
              {"plugin.payload", std::string{"keep exactly"}},
              {"plugin.revision", std::int64_t{42}},
          },
      },
  };
  return restored_root(nodes, version);
}

[[nodiscard]] bool same_document(const doc::Root &left,
                                 const doc::Root &right) {
  return left.version() == right.version() &&
         doc::diff(left, right).changes.empty();
}

[[nodiscard]] std::vector<std::byte>
read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  PROJECT_CHECK(input);
  const auto end = input.tellg();
  PROJECT_CHECK(end >= std::streampos{0});
  const auto size = static_cast<std::size_t>(end);
  std::vector<std::byte> bytes(size);
  input.seekg(0, std::ios::beg);
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    PROJECT_CHECK(input);
  }
  return bytes;
}

void write_file(const std::filesystem::path &path,
                std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  PROJECT_CHECK(output);
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.flush();
  PROJECT_CHECK(output);
}

void put_u16(std::span<std::byte> bytes, std::size_t offset,
             std::uint16_t value) noexcept {
  for (std::uint32_t shift = 0U; shift < 16U; shift += 8U) {
    bytes[offset + static_cast<std::size_t>(shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void put_u32(std::span<std::byte> bytes, std::size_t offset,
             std::uint32_t value) noexcept {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    bytes[offset + static_cast<std::size_t>(shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void put_u64(std::span<std::byte> bytes, std::size_t offset,
             std::uint64_t value) noexcept {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    bytes[offset + static_cast<std::size_t>(shift / 8U)] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(
                 bytes[offset + static_cast<std::size_t>(shift / 8U)]))
             << shift;
  }
  return value;
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256U>
make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256U> table{};
  for (std::size_t index = 0U; index < table.size(); ++index) {
    auto value = static_cast<std::uint32_t>(index);
    for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
      value = (value >> 1U) ^ ((value & 1U) != 0U ? 0x82f63b78U : 0U);
    }
    table[index] = value;
  }
  return table;
}

constexpr auto crc32c_table = make_crc32c_table();

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (const auto byte : bytes) {
    const auto lookup = static_cast<std::uint8_t>(
        (crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU);
    crc = crc32c_table[lookup] ^ (crc >> 8U);
  }
  return ~crc;
}

void refresh_header_crc(std::vector<std::byte> &bytes) {
  PROJECT_CHECK(bytes.size() >= project_header_size);
  put_u32(bytes, project_header_crc_offset,
          crc32c(std::span<const std::byte>{bytes}.first(
              project_header_crc_offset)));
}

[[nodiscard]] std::vector<std::byte> wrap_wire_payload(
    std::span<const std::byte> payload,
    std::uint16_t version = persist::current_project_format_version) {
  std::vector<std::byte> bytes(project_header_size + payload.size());
  std::copy(project_magic.begin(), project_magic.end(), bytes.begin());
  put_u16(bytes, project_version_offset, version);
  put_u16(bytes, 10U, static_cast<std::uint16_t>(project_header_size));
  put_u32(bytes, 12U, 0U);
  put_u64(bytes, project_payload_size_offset, payload.size());
  put_u32(bytes, 24U, crc32c(payload));
  refresh_header_crc(bytes);
  std::copy(payload.begin(), payload.end(),
            bytes.begin() + project_header_size);
  return bytes;
}

[[nodiscard]] std::vector<std::byte> encode_wire(const wire::Message &message) {
  auto encoded = wire::encode(message);
  PROJECT_CHECK(encoded.has_value());
  return std::move(encoded).value();
}

void unknown_nodes_and_properties_round_trip_exactly() {
  TempDirectory temporary;
  const auto path = temporary.file();
  const auto project = unknown_project();

  const auto saved = persist::save_project_file(path, project);
  PROJECT_CHECK(saved.has_value());
  PROJECT_CHECK(saved.value().version == project.version());
  PROJECT_CHECK(saved.value().durable);
  PROJECT_CHECK(saved.value().bytes_written ==
                std::filesystem::file_size(path));

  auto loaded = persist::load_project_file(path);
  PROJECT_CHECK(loaded.has_value());
  PROJECT_CHECK(same_document(loaded.value(), project));
  PROJECT_CHECK(*loaded.value().get(doc::NodeId{2U}) ==
                *project.get(doc::NodeId{2U}));
  PROJECT_CHECK(*loaded.value().get(doc::NodeId{4'000'000'001U}) ==
                *project.get(doc::NodeId{4'000'000'001U}));
}

void empty_project_preserves_a_high_historical_revision() {
  TempDirectory temporary;
  const auto version =
      doc::Version{std::numeric_limits<std::uint64_t>::max() - 17U};
  const auto project = restored_root({}, version);

  const auto saved = persist::save_project_file(temporary.file(), project);
  PROJECT_CHECK(saved.has_value());
  PROJECT_CHECK(saved.value().version == version);

  auto loaded = persist::load_project_file(temporary.file());
  PROJECT_CHECK(loaded.has_value());
  PROJECT_CHECK(loaded.value().empty());
  PROJECT_CHECK(loaded.value().version() == version);
}

void overwrite_succeeds_and_preencode_failure_preserves_the_previous_file() {
  TempDirectory temporary;
  const auto path = temporary.file();
  const auto first = unknown_project(doc::Version{100U});
  PROJECT_CHECK(persist::save_project_file(path, first));

  const std::array replacement_nodes{
      doc::Node{doc::NodeId{77U}, "replacement", {{"value", std::int64_t{8}}}},
  };
  const auto replacement = restored_root(replacement_nodes, doc::Version{701U});
  PROJECT_CHECK(persist::save_project_file(path, replacement));
  auto replaced = persist::load_project_file(path);
  PROJECT_CHECK(replaced.has_value());
  PROJECT_CHECK(same_document(replaced.value(), replacement));

  const auto before_failure = read_file(path);
  auto invalid = replacement.set(doc::Node{
      doc::NodeId{78U},
      std::string(1U, static_cast<char>(0xff)),
      {},
  });
  const auto rejected = persist::save_project_file(path, invalid);
  PROJECT_CHECK(!rejected.has_value());
  PROJECT_CHECK(rejected.error().code ==
                persist::ProjectFileErrorCode::EncodeFailed);
  PROJECT_CHECK(rejected.error().encode_error ==
                wire::EncodeError::InvalidNode);
  PROJECT_CHECK(read_file(path) == before_failure);

  auto still_replacement = persist::load_project_file(path);
  PROJECT_CHECK(still_replacement.has_value());
  PROJECT_CHECK(same_document(still_replacement.value(), replacement));

  std::size_t entries{};
  for (const auto &unused :
       std::filesystem::directory_iterator(temporary.path())) {
    static_cast<void>(unused);
    ++entries;
  }
  PROJECT_CHECK(entries == 1U);
}

void container_magic_version_and_checksums_are_enforced() {
  TempDirectory temporary;
  const auto path = temporary.file();
  PROJECT_CHECK(persist::save_project_file(path, unknown_project()));
  const auto valid = read_file(path);
  PROJECT_CHECK(valid.size() > project_header_size);

  auto wrong_magic = valid;
  wrong_magic[0] ^= std::byte{0x20U};
  refresh_header_crc(wrong_magic);
  write_file(path, wrong_magic);
  auto loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::InvalidFormat);

  auto future_version = valid;
  put_u16(
      future_version, project_version_offset,
      static_cast<std::uint16_t>(persist::current_project_format_version + 1U));
  refresh_header_crc(future_version);
  write_file(path, future_version);
  loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::UnsupportedVersion);

  auto damaged_header = valid;
  damaged_header[project_header_crc_offset] ^= std::byte{0x01U};
  write_file(path, damaged_header);
  loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::CorruptFile);

  auto damaged_payload = valid;
  damaged_payload[project_header_size +
                  (damaged_payload.size() - project_header_size) / 2U] ^=
      std::byte{0x40U};
  write_file(path, damaged_payload);
  loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::CorruptFile);
}

void every_truncation_and_trailing_bytes_are_rejected() {
  TempDirectory temporary;
  const auto path = temporary.file();
  PROJECT_CHECK(persist::save_project_file(path, unknown_project()));
  const auto complete = read_file(path);

  for (std::size_t size = 0U; size < complete.size(); ++size) {
    write_file(path, std::span<const std::byte>{complete}.first(size));
    const auto loaded = persist::load_project_file(path);
    PROJECT_CHECK(!loaded.has_value());
  }

  auto trailing = complete;
  trailing.push_back(std::byte{0x00U});
  write_file(path, trailing);
  const auto loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::CorruptFile);
}

void project_load_and_save_apply_wire_resource_limits() {
  TempDirectory temporary;
  const auto path = temporary.file();
  const auto project = unknown_project();
  PROJECT_CHECK(persist::save_project_file(path, project));

  auto limits = wire::DecodeLimits{};
  limits.max_nodes = 1U;
  auto loaded = persist::load_project_file(path, limits);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::SizeLimitExceeded);
  PROJECT_CHECK(loaded.error().decode_error ==
                wire::DecodeError::SizeLimitExceeded);

  const auto bytes = read_file(path);
  PROJECT_CHECK(bytes.size() >= project_header_size);
  const auto payload_size = get_u64(bytes, project_payload_size_offset);
  PROJECT_CHECK(payload_size > 0U);
  limits = wire::DecodeLimits{};
  limits.max_message_bytes = static_cast<std::size_t>(payload_size - 1U);
  loaded = persist::load_project_file(path, limits);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::SizeLimitExceeded);

  limits = wire::DecodeLimits{};
  limits.max_nodes = 1U;
  const auto save_path = temporary.file("limited.msproject");
  const auto saved = persist::save_project_file(save_path, project, limits);
  PROJECT_CHECK(!saved.has_value());
  PROJECT_CHECK(saved.error().code ==
                persist::ProjectFileErrorCode::SizeLimitExceeded);
  PROJECT_CHECK(saved.error().encode_error ==
                wire::EncodeError::SizeLimitExceeded);
  PROJECT_CHECK(!std::filesystem::exists(save_path));
}

void non_project_wire_payloads_and_noncanonical_cursors_are_rejected() {
  TempDirectory temporary;
  const auto path = temporary.file();

  const cmd::StreamCursor cursor{cmd::FrameNumber{0U}, doc::Version{0U},
                                 cmd::CommandId{1U}};
  const auto cursor_payload = encode_wire(wire::Message{cursor});
  write_file(path, wrap_wire_payload(cursor_payload));
  auto loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::UnexpectedPayload);

  const auto project = unknown_project();
  const cmd::StreamCheckpoint noncanonical{
      cmd::StreamCursor{cmd::FrameNumber{12U}, project.version(),
                        cmd::CommandId{99U}},
      project,
  };
  const auto checkpoint_payload = encode_wire(wire::Message{noncanonical});
  write_file(path, wrap_wire_payload(checkpoint_payload));
  loaded = persist::load_project_file(path);
  PROJECT_CHECK(!loaded.has_value());
  PROJECT_CHECK(loaded.error().code ==
                persist::ProjectFileErrorCode::InvalidProjectCursor);
}

using TestFunction = void (*)();

struct TestCase final {
  const char *name;
  TestFunction function;
};

} // namespace

int main() {
  const std::array tests{
      TestCase{"unknown nodes and properties round trip exactly",
               &unknown_nodes_and_properties_round_trip_exactly},
      TestCase{"empty project preserves high historical revision",
               &empty_project_preserves_a_high_historical_revision},
      TestCase{
          "overwrite and pre-encode preservation",
          &overwrite_succeeds_and_preencode_failure_preserves_the_previous_file},
      TestCase{"container magic version and checksums",
               &container_magic_version_and_checksums_are_enforced},
      TestCase{"truncation and trailing bytes",
               &every_truncation_and_trailing_bytes_are_rejected},
      TestCase{"wire resource limits",
               &project_load_and_save_apply_wire_resource_limits},
      TestCase{
          "wire payload kind and project cursor",
          &non_project_wire_payloads_and_noncanonical_cursors_are_rejected},
  };

  std::size_t failures{};
  for (const auto &test : tests) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }

  if (failures != 0U) {
    std::cerr << failures << '/' << tests.size() << " tests failed\n";
    return 1;
  }
  std::cout << tests.size() << '/' << tests.size() << " tests passed\n";
  return 0;
}
