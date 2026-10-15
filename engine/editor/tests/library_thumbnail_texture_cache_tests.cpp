#include <mediaserver/editor/library_thumbnail_texture_cache.hpp>

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

#define CACHE_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
  rhi::DeviceConfig config{};
  config.debug = false;
  auto created = rhi::Device::create(config);
  CACHE_CHECK(created.has_value());
  return std::move(created).value();
}

[[nodiscard]] assets::DerivativeKey thumbnail_key(core::u64 asset_id,
                                                  core::u64 revision) {
  const assets::DerivativeProfile *const profile = assets::derivative_profile(
      assets::DerivativeProfileId::Thumbnail320x180Srgb);
  CACHE_CHECK(profile != nullptr);
  assets::ContentFingerprint fingerprint{};
  fingerprint.byte_length = 10'000U + revision;
  fingerprint.sha256.bytes[0U] = static_cast<core::u8>(revision & 0xffU);
  fingerprint.sha256.bytes[1U] =
      static_cast<core::u8>((revision >> 8U) & 0xffU);
  return assets::make_derivative_key(doc::NodeId{asset_id}, fingerprint,
                                     *profile);
}

[[nodiscard]] render::DecodedFrame solid_frame(core::u8 red, core::u8 green,
                                               core::u8 blue,
                                               core::u8 alpha = 255U) {
  render::DecodedFrame frame{};
  frame.width = editor::library_thumbnail_texture_width;
  frame.height = editor::library_thumbnail_texture_height;
  frame.format = rhi::TextureFormat::Rgba8Unorm;
  const std::size_t pixel_count =
      static_cast<std::size_t>(frame.width) * frame.height;
  frame.pixels.resize(pixel_count * 4U);
  for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
    const std::size_t offset = pixel * 4U;
    frame.pixels[offset] = red;
    frame.pixels[offset + 1U] = green;
    frame.pixels[offset + 2U] = blue;
    frame.pixels[offset + 3U] = alpha;
  }
  return frame;
}

[[nodiscard]] const rhi::Texture *
upload_and_wait(editor::LibraryThumbnailTextureCache &cache,
                rhi::Device &device, const assets::DerivativeKey &key,
                const render::DecodedFrame &frame) {
  auto acquired = device.acquire_commands();
  CACHE_CHECK(acquired.has_value());
  rhi::CommandList commands = std::move(acquired).value();
  const rhi::Texture *const texture = cache.upload(commands, key, frame);
  CACHE_CHECK(texture != nullptr);
  auto submitted = device.submit(std::move(commands));
  CACHE_CHECK(submitted.has_value());
  auto work = std::move(submitted).value();
  CACHE_CHECK(work.wait());
  return texture;
}

void an_exact_key_uploads_one_sampled_texture() {
  rhi::Device device = open_device();
  editor::LibraryThumbnailTextureCache cache{device, 2U};
  const assets::DerivativeKey key = thumbnail_key(40U, 1U);
  const render::DecodedFrame frame = solid_frame(31U, 97U, 203U, 221U);

  auto acquired = device.acquire_commands();
  CACHE_CHECK(acquired.has_value());
  rhi::CommandList commands = std::move(acquired).value();
  const rhi::Texture *const texture = cache.upload(commands, key, frame);
  CACHE_CHECK(texture != nullptr);
  CACHE_CHECK(texture->desc().width == editor::library_thumbnail_texture_width);
  CACHE_CHECK(texture->desc().height ==
              editor::library_thumbnail_texture_height);
  CACHE_CHECK(texture->desc().format == rhi::TextureFormat::Rgba8Unorm);
  CACHE_CHECK(texture->desc().sampled);
  CACHE_CHECK(!texture->desc().color_target);
  CACHE_CHECK(!texture->desc().depth_target);
  CACHE_CHECK(!texture->desc().streaming_upload);

  const core::u32 byte_count = editor::library_thumbnail_texture_width *
                               editor::library_thumbnail_texture_height * 4U;
  auto readback = device.create_readback(byte_count);
  CACHE_CHECK(readback.has_value());
  CACHE_CHECK(commands.download(*texture, readback.value()));
  auto submitted = device.submit(std::move(commands));
  CACHE_CHECK(submitted.has_value());
  auto work = std::move(submitted).value();
  CACHE_CHECK(work.wait());

  const std::span<const core::u8> pixels = readback.value().map();
  CACHE_CHECK(pixels.size() == byte_count);
  CACHE_CHECK(pixels[0U] == 31U);
  CACHE_CHECK(pixels[1U] == 97U);
  CACHE_CHECK(pixels[2U] == 203U);
  CACHE_CHECK(pixels[3U] == 221U);
  CACHE_CHECK(pixels[pixels.size() - 4U] == 31U);
  CACHE_CHECK(pixels[pixels.size() - 1U] == 221U);
  readback.value().unmap();

  CACHE_CHECK(cache.size() == 1U);
  CACHE_CHECK(cache.find(key) == texture);
  cache.clear();
  CACHE_CHECK(cache.size() == 0U);
  CACHE_CHECK(cache.find(key) == nullptr);
}

void full_derivative_keys_drive_lru_and_exact_retain() {
  rhi::Device device = open_device();
  editor::LibraryThumbnailTextureCache cache{device, 2U};
  const assets::DerivativeKey first = thumbnail_key(55U, 1U);
  const assets::DerivativeKey second = thumbnail_key(55U, 2U);
  const assets::DerivativeKey third = thumbnail_key(55U, 3U);
  const render::DecodedFrame frame = solid_frame(12U, 34U, 56U);

  static_cast<void>(upload_and_wait(cache, device, first, frame));
  static_cast<void>(upload_and_wait(cache, device, second, frame));
  CACHE_CHECK(cache.size() == 2U);
  CACHE_CHECK(cache.find(first) != nullptr); // first is most-recent again
  static_cast<void>(upload_and_wait(cache, device, third, frame));

  CACHE_CHECK(cache.size() == 2U);
  CACHE_CHECK(cache.find(first) != nullptr);
  CACHE_CHECK(cache.find(second) == nullptr);
  CACHE_CHECK(cache.find(third) != nullptr);

  const std::array<assets::DerivativeKey, 1U> retained{{third}};
  cache.retain(retained);
  CACHE_CHECK(cache.size() == 1U);
  CACHE_CHECK(cache.find(first) == nullptr);
  CACHE_CHECK(cache.find(third) != nullptr);
  cache.reset(91U);
  CACHE_CHECK(cache.size() == 0U);
  CACHE_CHECK(cache.project_generation() == 91U);
}

void invalid_profiles_frames_and_capacities_create_nothing() {
  rhi::Device device = open_device();
  editor::LibraryThumbnailTextureCache clamped{device, 1'000U};
  CACHE_CHECK(clamped.capacity() ==
              editor::maximum_library_thumbnail_texture_capacity);
  clamped.clear();
  CACHE_CHECK(clamped.size() == 0U);

  editor::LibraryThumbnailTextureCache cache{device, 0U};
  CACHE_CHECK(cache.capacity() == 0U);
  auto acquired = device.acquire_commands();
  CACHE_CHECK(acquired.has_value());
  rhi::CommandList commands = std::move(acquired).value();
  const assets::DerivativeKey key = thumbnail_key(80U, 1U);
  const render::DecodedFrame valid = solid_frame(1U, 2U, 3U);
  CACHE_CHECK(cache.upload(commands, key, valid) == nullptr);

  editor::LibraryThumbnailTextureCache accepting{device, 2U};
  assets::DerivativeKey corrupt = key;
  corrupt.digest.bytes[0U] ^= 0xffU;
  CACHE_CHECK(accepting.upload(commands, corrupt, valid) == nullptr);

  const assets::DerivativeProfile *const filmstrip_profile =
      assets::derivative_profile(
          assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
  CACHE_CHECK(filmstrip_profile != nullptr);
  assets::ContentFingerprint filmstrip_fingerprint{};
  filmstrip_fingerprint.byte_length = 42U;
  filmstrip_fingerprint.sha256.bytes[0U] = 42U;
  const assets::DerivativeKey filmstrip_key = assets::make_derivative_key(
      doc::NodeId{80U}, filmstrip_fingerprint, *filmstrip_profile);
  CACHE_CHECK(accepting.upload(commands, filmstrip_key, valid) == nullptr);

  render::DecodedFrame wrong_size = valid;
  wrong_size.width -= 1U;
  CACHE_CHECK(accepting.upload(commands, key, wrong_size) == nullptr);
  render::DecodedFrame short_payload = valid;
  short_payload.pixels.pop_back();
  CACHE_CHECK(accepting.upload(commands, key, short_payload) == nullptr);
  CACHE_CHECK(accepting.size() == 0U);
}

struct Case final {
  const char *name;
  void (*function)();
};

void native_pending_draws_survive_retain_reset_and_budget_pressure() {
  if (!rhi::Device::available(rhi::BackendKind::D3d12)) return;
  auto created = rhi::Device::create({.debug=true, .frames_in_flight=2,
                                    .backend=rhi::BackendKind::D3d12});
  CACHE_CHECK(created);
  auto device = std::move(created).value();
  editor::LibraryThumbnailTextureCache cache{device, 1};
  auto acquired = device.acquire_commands();
  CACHE_CHECK(acquired);
  auto commands = std::move(acquired).value();
  const auto first = thumbnail_key(201, 3), second = thumbnail_key(202, 7),
             third = thumbnail_key(203, 11);
  const auto frame = solid_frame(19, 73, 181, 229);
  const auto* texture = cache.upload(commands, first, frame);
  CACHE_CHECK(texture && !texture->safe_to_release());
  auto readback = device.create_readback(editor::library_thumbnail_texture_width *
                                       editor::library_thumbnail_texture_height * 4);
  CACHE_CHECK(readback && commands.download(*texture, readback.value()));
  // Hiding Library drops its keys while the prior UI recording still owns
  // the image. The copy must retain its original pixels through that removal.
  cache.retain({});
  CACHE_CHECK(cache.size() == 0 && !cache.find(first));
  CACHE_CHECK(cache.upload(commands, second, frame));
  cache.reset(48);
  CACHE_CHECK(cache.project_generation() == 48 && cache.size() == 0);
  CACHE_CHECK(!cache.upload(commands, third, frame)); // both slots still owned
  auto submitted = device.submit(std::move(commands));
  CACHE_CHECK(submitted && submitted.value().wait());
  const auto pixels = readback.value().map();
  CACHE_CHECK(pixels.size() == frame.pixels.size());
  CACHE_CHECK(pixels[0] == 19 && pixels[1] == 73 && pixels[2] == 181 && pixels[3] == 229);
  readback.value().unmap();
  // Completion releases the budget without a blocking wait in cache calls.
  auto retry = device.acquire_commands();
  CACHE_CHECK(retry);
  CACHE_CHECK(cache.upload(retry.value(), third, frame));
  auto retried = device.submit(std::move(retry).value());
  CACHE_CHECK(retried && retried.value().wait());
}

constexpr std::array<Case, 4U> cases{{
    {"native pending draws survive retain reset and budget pressure",
     &native_pending_draws_survive_retain_reset_and_budget_pressure},
    {"an exact key uploads one sampled texture",
     &an_exact_key_uploads_one_sampled_texture},
    {"full derivative keys drive LRU and exact retain",
     &full_derivative_keys_drive_lru_and_exact_retain},
    {"invalid profiles frames and capacities create nothing",
     &invalid_profiles_frames_and_capacities_create_nothing},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
