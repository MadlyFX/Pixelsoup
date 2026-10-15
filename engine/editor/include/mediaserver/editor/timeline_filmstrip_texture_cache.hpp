#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <list>
#include <span>

// Machine-local GPU residency for Timeline filmstrip atlases. The cache key is
// always the full derivative tuple rooted at the persistent Library Asset id;
// clip nodes merely borrow the matching atlas for the frame in which they draw.
namespace mediaserver::editor {

inline constexpr core::u32 timeline_filmstrip_tile_width = 160U;
inline constexpr core::u32 timeline_filmstrip_tile_height = 90U;
inline constexpr core::u32 timeline_filmstrip_tile_count = 8U;
inline constexpr core::u32 timeline_filmstrip_texture_width =
    timeline_filmstrip_tile_width * timeline_filmstrip_tile_count;
inline constexpr core::u32 timeline_filmstrip_texture_height =
    timeline_filmstrip_tile_height;
inline constexpr std::size_t default_timeline_filmstrip_texture_capacity = 32U;
inline constexpr std::size_t maximum_timeline_filmstrip_texture_capacity = 64U;

class TimelineFilmstripTextureCache final {
public:
  explicit TimelineFilmstripTextureCache(
      rhi::Device &device,
      std::size_t capacity =
          default_timeline_filmstrip_texture_capacity) noexcept;
  ~TimelineFilmstripTextureCache();

  TimelineFilmstripTextureCache(const TimelineFilmstripTextureCache &) = delete;
  TimelineFilmstripTextureCache &
  operator=(const TimelineFilmstripTextureCache &) = delete;
  TimelineFilmstripTextureCache(TimelineFilmstripTextureCache &&) = delete;
  TimelineFilmstripTextureCache &
  operator=(TimelineFilmstripTextureCache &&) = delete;

  [[nodiscard]] const rhi::Texture *
  find(const assets::DerivativeKey &key) noexcept;

  // Accepts only an exact Filmstrip8x160x90Srgb key and the profile's one-row
  // 1280x90 RGBA8 atlas. A full-key resident hit records no upload.
  [[nodiscard]] const rhi::Texture *
  upload(rhi::CommandList &commands, const assets::DerivativeKey &key,
         const render::DecodedFrame &frame) noexcept;

  // Lookup changes immediately; textures remain alive through their final
  // GPU use. Total resident and retiring storage is bounded to twice capacity.
  void retain(std::span<const assets::DerivativeKey> keys) noexcept;
  void clear() noexcept;
  void reset(core::u64 project_generation) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] core::u64 project_generation() const noexcept {
    return project_generation_;
  }

private:
  struct Entry final {
    assets::DerivativeKey key;
    rhi::Texture texture;
  };

  using EntryList = std::list<Entry>;
  void collect() noexcept;
  void retire(EntryList::iterator entry) noexcept;

  rhi::Device *device_{};
  std::size_t capacity_{};
  core::u64 project_generation_{};
  EntryList entries_;
  EntryList retiring_;
};

} // namespace mediaserver::editor
