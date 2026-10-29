#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <optional>
#include <span>

// The Hardware Map's isometric pixel-art sprite sheet.
//
// engine/editor/assets/hardware_sprites.png is built into the editor. Each
// sprite has a ground anchor - the point under its footprint, which sits on
// the device's map cell - and the screen columns its input and output ports
// are drawn on, so a cable meets the face of the box it plugs into.
// scripts/generate-hardware-sprites.py makes the sheet and its frame table.
namespace mediaserver::editor {

enum class HardwareSprite : core::u8 {
  RenderServer,
  OperatorMachine,
  Processor,
  ProcessorNovastar,
  LedWall,
  ProcessorBrompton,
};

// A vertical run of sprite pixels: x, and where it starts and ends.
struct HardwareSpriteColumn final {
  core::i32 x{};
  core::i32 top{};
  core::i32 bottom{};
};

struct HardwareSpriteFrame final {
  core::u32 x{};
  core::u32 y{};
  core::u32 width{};
  core::u32 height{};
  core::i32 anchor_x{};
  core::i32 anchor_y{};
  HardwareSpriteColumn inputs;
  HardwareSpriteColumn outputs;
};

[[nodiscard]] const HardwareSpriteFrame &
hardware_sprite_frame(HardwareSprite sprite) noexcept;
[[nodiscard]] core::u32 hardware_sprite_sheet_width() noexcept;
[[nodiscard]] core::u32 hardware_sprite_sheet_height() noexcept;

// The PNG as built in.
[[nodiscard]] std::span<const core::u8> hardware_sprite_png() noexcept;

// Decodes the sheet and records its upload on `commands`. Nothing when the
// image cannot be decoded or the texture cannot be made.
[[nodiscard]] std::optional<rhi::Texture>
create_hardware_sprite_texture(rhi::Device &device,
                               rhi::CommandList &commands);

} // namespace mediaserver::editor
