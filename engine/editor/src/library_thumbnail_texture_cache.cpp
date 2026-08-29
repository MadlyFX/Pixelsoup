#include <mediaserver/editor/library_thumbnail_texture_cache.hpp>

#include <algorithm>
#include <iterator>
#include <new>
#include <utility>

namespace mediaserver::editor {

LibraryThumbnailTextureCache::LibraryThumbnailTextureCache(
    rhi::Device &device, std::size_t capacity) noexcept
    : device_(&device),
      capacity_(
          std::min(capacity, maximum_library_thumbnail_texture_capacity)) {}

const rhi::Texture *
LibraryThumbnailTextureCache::find(const assets::DerivativeKey &key) noexcept {
  const auto found =
      std::find_if(entries_.begin(), entries_.end(),
                   [&](const Entry &entry) { return entry.key == key; });
  if (found == entries_.end()) {
    return nullptr;
  }
  entries_.splice(entries_.end(), entries_, found);
  return &entries_.back().texture;
}

const rhi::Texture *LibraryThumbnailTextureCache::upload(
    rhi::CommandList &commands, const assets::DerivativeKey &key,
    const render::DecodedFrame &frame) noexcept {
  if (const rhi::Texture *const resident = find(key); resident != nullptr) {
    return resident;
  }
  constexpr std::size_t expected_bytes =
      static_cast<std::size_t>(library_thumbnail_texture_width) *
      library_thumbnail_texture_height * 4U;
  if (capacity_ == 0U || !assets::valid_derivative_key(key) ||
      key.profile != assets::DerivativeProfileId::Thumbnail320x180Srgb ||
      !frame.valid() || frame.width != library_thumbnail_texture_width ||
      frame.height != library_thumbnail_texture_height ||
      frame.format != rhi::TextureFormat::Rgba8Unorm ||
      frame.pixels.size() != expected_bytes) {
    return nullptr;
  }

  rhi::TextureDesc desc{};
  desc.width = library_thumbnail_texture_width;
  desc.height = library_thumbnail_texture_height;
  desc.format = rhi::TextureFormat::Rgba8Unorm;
  desc.sampled = true;
  auto created = device_->create_texture(desc);
  if (!created) {
    return nullptr;
  }

  try {
    entries_.push_back(Entry{key, std::move(created).value()});
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
  EntryList::iterator inserted = std::prev(entries_.end());
  if (!commands.upload(inserted->texture, frame.pixels)) {
    entries_.erase(inserted);
    return nullptr;
  }
  while (entries_.size() > capacity_) {
    entries_.pop_front();
  }
  return &inserted->texture;
}

void LibraryThumbnailTextureCache::retain(
    std::span<const assets::DerivativeKey> keys) noexcept {
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    const bool retained =
        std::find(keys.begin(), keys.end(), entry->key) != keys.end();
    entry = retained ? std::next(entry) : entries_.erase(entry);
  }
}

void LibraryThumbnailTextureCache::clear() noexcept { entries_.clear(); }

void LibraryThumbnailTextureCache::reset(
    core::u64 project_generation) noexcept {
  clear();
  project_generation_ = project_generation;
}

} // namespace mediaserver::editor
