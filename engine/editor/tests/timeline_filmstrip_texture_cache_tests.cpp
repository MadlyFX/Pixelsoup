#include <mediaserver/editor/timeline_filmstrip_texture_cache.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define FILMSTRIP_CHECK(...)                                                  \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
  rhi::DeviceConfig config{};
  config.debug = false;
  auto created = rhi::Device::create(config);
  FILMSTRIP_CHECK(created.has_value());
  return std::move(created).value();
}

[[nodiscard]] assets::DerivativeKey key(core::u64 asset_id,
                                        core::u64 revision,
                                        assets::DerivativeProfileId profile_id =
                                            assets::DerivativeProfileId::
                                                Filmstrip8x160x90Srgb) {
  const assets::DerivativeProfile *const profile =
      assets::derivative_profile(profile_id);
  FILMSTRIP_CHECK(profile != nullptr);
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 5'000U + revision;
  fingerprint.sha256.bytes[0U] =
      static_cast<core::u8>(revision & 0xffU);
  return assets::make_derivative_key(doc::NodeId{asset_id}, fingerprint,
                                     *profile);
}

[[nodiscard]] render::DecodedFrame atlas() {
  render::DecodedFrame frame{};
  frame.width = editor::timeline_filmstrip_texture_width;
  frame.height = editor::timeline_filmstrip_texture_height;
  frame.format = rhi::TextureFormat::Rgba8Unorm;
  frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height *
                      4U);
  for (core::u32 y = 0U; y < frame.height; ++y) {
    for (core::u32 x = 0U; x < frame.width; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * frame.width + x) * 4U;
      frame.pixels[offset] = static_cast<core::u8>(
          x / editor::timeline_filmstrip_tile_width);
      frame.pixels[offset + 1U] = static_cast<core::u8>(y);
      frame.pixels[offset + 2U] = 71U;
      frame.pixels[offset + 3U] = 255U;
    }
  }
  return frame;
}

void exact_atlas_uploads_one_sampled_texture() {
  rhi::Device device = open_device();
  editor::TimelineFilmstripTextureCache cache{device, 2U};
  const auto derivative_key = key(1U, 1U);
  const auto frame = atlas();
  auto acquired = device.acquire_commands();
  FILMSTRIP_CHECK(acquired);
  rhi::CommandList commands = std::move(acquired).value();
  const rhi::Texture *const texture =
      cache.upload(commands, derivative_key, frame);
  FILMSTRIP_CHECK(texture != nullptr);
  FILMSTRIP_CHECK(texture->desc().width ==
                  editor::timeline_filmstrip_texture_width);
  FILMSTRIP_CHECK(texture->desc().height ==
                  editor::timeline_filmstrip_texture_height);
  FILMSTRIP_CHECK(texture->desc().sampled);
  FILMSTRIP_CHECK(!texture->desc().color_target);

  const core::u32 byte_count = editor::timeline_filmstrip_texture_width *
                               editor::timeline_filmstrip_texture_height * 4U;
  auto readback = device.create_readback(byte_count);
  FILMSTRIP_CHECK(readback);
  FILMSTRIP_CHECK(commands.download(*texture, readback.value()));
  auto submitted = device.submit(std::move(commands));
  FILMSTRIP_CHECK(submitted);
  FILMSTRIP_CHECK(std::move(submitted).value().wait());
  const std::span<const core::u8> pixels = readback.value().map();
  FILMSTRIP_CHECK(pixels.size() == byte_count);
  const std::size_t last_tile =
      (static_cast<std::size_t>(45U) *
           editor::timeline_filmstrip_texture_width +
       7U * editor::timeline_filmstrip_tile_width + 80U) *
      4U;
  FILMSTRIP_CHECK(pixels[last_tile] == 7U);
  FILMSTRIP_CHECK(pixels[last_tile + 1U] == 45U);
  FILMSTRIP_CHECK(pixels[last_tile + 3U] == 255U);
  readback.value().unmap();
  FILMSTRIP_CHECK(cache.find(derivative_key) == texture);
}

void full_keys_drive_lru_retain_and_generation_reset() {
  rhi::Device device = open_device();
  editor::TimelineFilmstripTextureCache cache{device, 2U};
  const auto frame = atlas();
  const auto first = key(4U, 1U);
  const auto second = key(4U, 2U);
  const auto third = key(4U, 3U);
  const auto upload = [&](const assets::DerivativeKey &derivative_key) {
    auto acquired = device.acquire_commands();
    FILMSTRIP_CHECK(acquired);
    rhi::CommandList commands = std::move(acquired).value();
    FILMSTRIP_CHECK(cache.upload(commands, derivative_key, frame) != nullptr);
    auto submitted = device.submit(std::move(commands));
    FILMSTRIP_CHECK(submitted);
    FILMSTRIP_CHECK(std::move(submitted).value().wait());
  };
  upload(first);
  upload(second);
  FILMSTRIP_CHECK(cache.find(first) != nullptr);
  upload(third);
  FILMSTRIP_CHECK(cache.find(first) != nullptr);
  FILMSTRIP_CHECK(cache.find(second) == nullptr);
  FILMSTRIP_CHECK(cache.find(third) != nullptr);
  const std::array retained{third};
  cache.retain(retained);
  FILMSTRIP_CHECK(cache.size() == 1U);
  cache.reset(92U);
  FILMSTRIP_CHECK(cache.size() == 0U);
  FILMSTRIP_CHECK(cache.project_generation() == 92U);
}

void invalid_profiles_dimensions_and_capacity_create_nothing() {
  rhi::Device device = open_device();
  editor::TimelineFilmstripTextureCache clamped{device, 1'000U};
  FILMSTRIP_CHECK(clamped.capacity() ==
                  editor::maximum_timeline_filmstrip_texture_capacity);
  editor::TimelineFilmstripTextureCache zero{device, 0U};
  auto acquired = device.acquire_commands();
  FILMSTRIP_CHECK(acquired);
  rhi::CommandList commands = std::move(acquired).value();
  const auto valid_key = key(8U, 1U);
  const auto valid_frame = atlas();
  FILMSTRIP_CHECK(zero.upload(commands, valid_key, valid_frame) == nullptr);

  editor::TimelineFilmstripTextureCache accepting{device, 2U};
  FILMSTRIP_CHECK(accepting.upload(
                      commands,
                      key(8U, 1U,
                          assets::DerivativeProfileId::Thumbnail320x180Srgb),
                      valid_frame) == nullptr);
  auto wrong_size = valid_frame;
  --wrong_size.width;
  FILMSTRIP_CHECK(accepting.upload(commands, valid_key, wrong_size) == nullptr);
  auto short_payload = valid_frame;
  short_payload.pixels.pop_back();
  FILMSTRIP_CHECK(accepting.upload(commands, valid_key, short_payload) ==
                  nullptr);
  FILMSTRIP_CHECK(accepting.size() == 0U);
}

struct Case final {
  const char *name;
  void (*function)();
};

void native_pending_filmstrips_survive_tab_changes_and_eviction() {
  if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
  auto created = rhi::Device::create({.debug=true, .frames_in_flight=2,
                                    .backend=rhi::BackendKind::D3d12});
  FILMSTRIP_CHECK(created);
  auto device = std::move(created).value();
  editor::TimelineFilmstripTextureCache cache{device, 1};
  auto acquired = device.acquire_commands();
  FILMSTRIP_CHECK(acquired);
  auto commands = std::move(acquired).value();
  const auto first = key(301, 3), second = key(302, 7), third = key(303, 11);
  const auto frame = atlas();
  const auto* texture = cache.upload(commands, first, frame);
  FILMSTRIP_CHECK(texture && !texture->safe_to_release());
  auto readback = device.create_readback(editor::timeline_filmstrip_texture_width *
                                       editor::timeline_filmstrip_texture_height * 4);
  FILMSTRIP_CHECK(readback && commands.download(*texture, readback.value()));
  // Capacity eviction and switching to Cues both remove still-recorded
  // filmstrips. Their GPU storage must outlive those logical cache changes.
  FILMSTRIP_CHECK(cache.upload(commands, second, frame));
  FILMSTRIP_CHECK(!cache.find(first) && cache.size() == 1);
  cache.retain({});
  FILMSTRIP_CHECK(cache.size() == 0);
  FILMSTRIP_CHECK(!cache.upload(commands, third, frame));
  auto submitted = device.submit(std::move(commands));
  FILMSTRIP_CHECK(submitted && submitted.value().wait());
  const auto pixels = readback.value().map();
  FILMSTRIP_CHECK(pixels.size() == frame.pixels.size());
  const auto offset = (45U * editor::timeline_filmstrip_texture_width +
                       7U * editor::timeline_filmstrip_tile_width + 80U) * 4U;
  FILMSTRIP_CHECK(pixels[offset] == 7 && pixels[offset+1] == 45 &&
                  pixels[offset+2] == 71 && pixels[offset+3] == 255);
  readback.value().unmap();
  cache.reset(49);
  auto retry = device.acquire_commands();
  FILMSTRIP_CHECK(retry && cache.upload(retry.value(), third, frame));
  auto retried = device.submit(std::move(retry).value());
  FILMSTRIP_CHECK(retried && retried.value().wait());
}

constexpr std::array<Case, 4U> cases{{
    {"native pending filmstrips survive tab changes and eviction",
     &native_pending_filmstrips_survive_tab_changes_and_eviction},
    {"exact atlas uploads one sampled texture",
     &exact_atlas_uploads_one_sampled_texture},
    {"full keys drive lru retain and generation reset",
     &full_keys_drive_lru_retain_and_generation_reset},
    {"invalid profiles dimensions and capacity create nothing",
     &invalid_profiles_dimensions_and_capacity_create_nothing},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
