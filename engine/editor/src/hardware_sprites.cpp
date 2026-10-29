#include <mediaserver/editor/hardware_sprites.hpp>

#include <mediaserver/render/image_decode.hpp>

#include <array>
#include <cstddef>
#include <new>

namespace mediaserver::editor {

namespace detail {
// Built from engine/editor/assets/hardware_sprites.png by EmbedFile.cmake.
extern const unsigned char hardware_sprites_png[];
extern const std::size_t hardware_sprites_png_size;
} // namespace detail

namespace {
#include "hardware_sprites_atlas.inc"
} // namespace

const HardwareSpriteFrame &
hardware_sprite_frame(HardwareSprite sprite) noexcept {
  const auto index = static_cast<std::size_t>(sprite);
  return hardware_sprite_frames[index < hardware_sprite_frames.size() ? index
                                                                      : 0U];
}

core::u32 hardware_sprite_sheet_width() noexcept {
  return hardware_sprite_atlas_width;
}

core::u32 hardware_sprite_sheet_height() noexcept {
  return hardware_sprite_atlas_height;
}

std::span<const core::u8> hardware_sprite_png() noexcept {
  return std::span<const core::u8>{
      reinterpret_cast<const core::u8 *>(detail::hardware_sprites_png),
      detail::hardware_sprites_png_size};
}

std::optional<rhi::Texture>
create_hardware_sprite_texture(rhi::Device &device,
                               rhi::CommandList &commands) {
  try {
    const render::DecodedFrame decoded =
        render::decode_image_bytes(hardware_sprite_png());
    if (!decoded.valid() || decoded.format != rhi::TextureFormat::Rgba8Unorm ||
        decoded.width != hardware_sprite_atlas_width ||
        decoded.height != hardware_sprite_atlas_height ||
        decoded.pixels.size() !=
            static_cast<std::size_t>(decoded.width) * decoded.height * 4U) {
      return std::nullopt;
    }
    rhi::TextureDesc desc{};
    desc.width = decoded.width;
    desc.height = decoded.height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto created = device.create_texture(desc);
    if (!created) {
      return std::nullopt;
    }
    rhi::Texture texture = std::move(created).value();
    if (!commands.upload(texture, decoded.pixels)) {
      return std::nullopt;
    }
    return texture;
  } catch (const std::bad_alloc &) {
    return std::nullopt;
  }
}

} // namespace mediaserver::editor
