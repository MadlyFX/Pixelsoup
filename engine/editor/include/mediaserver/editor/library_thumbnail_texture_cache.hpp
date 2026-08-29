#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <list>
#include <span>

// Machine-local GPU residency for Library thumbnails. Persistent asset
// identity remains the authored Asset NodeId; this cache is indexed only by
// the full derivative tuple and never by a source filename or cache path.
namespace mediaserver::editor {

inline constexpr core::u32 library_thumbnail_texture_width = 320U;
inline constexpr core::u32 library_thumbnail_texture_height = 180U;
inline constexpr std::size_t default_library_thumbnail_texture_capacity = 64U;
inline constexpr std::size_t maximum_library_thumbnail_texture_capacity = 64U;

class LibraryThumbnailTextureCache final {
public:
  explicit LibraryThumbnailTextureCache(
      rhi::Device &device,
      std::size_t capacity =
          default_library_thumbnail_texture_capacity) noexcept;

  LibraryThumbnailTextureCache(const LibraryThumbnailTextureCache &) = delete;
  LibraryThumbnailTextureCache &
  operator=(const LibraryThumbnailTextureCache &) = delete;
  LibraryThumbnailTextureCache(LibraryThumbnailTextureCache &&) = delete;
  LibraryThumbnailTextureCache &
  operator=(LibraryThumbnailTextureCache &&) = delete;

  // Returns and touches an exact resident key. The pointer is borrowed until
  // the next mutating cache call; retain(), clear(), or a capacity eviction
  // may invalidate it.
  [[nodiscard]] const rhi::Texture *
  find(const assets::DerivativeKey &key) noexcept;

  // Creates one immutable sampled texture and records its upload on the
  // caller's command list. Only a valid Thumbnail320x180Srgb key and an exact
  // 320x180 RGBA8 frame are accepted. A resident exact-key hit records no
  // upload and returns the existing texture.
  [[nodiscard]] const rhi::Texture *
  upload(rhi::CommandList &commands, const assets::DerivativeKey &key,
         const render::DecodedFrame &frame) noexcept;

  // Keeps exactly the listed resident keys. It never creates a key or loads
  // content; callers should apply this after prior GPU work is submitted and
  // before uploading the next frame's requested thumbnails.
  void retain(std::span<const assets::DerivativeKey> keys) noexcept;
  void clear() noexcept;
  // Project replacement is an explicit residency fence even if a new
  // project happens to reuse an Asset NodeId and byte-identical content.
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

  rhi::Device *device_{};
  std::size_t capacity_{};
  core::u64 project_generation_{};
  EntryList entries_;
};

} // namespace mediaserver::editor
