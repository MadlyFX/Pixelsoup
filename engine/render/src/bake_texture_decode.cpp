#include <mediaserver/render/model_loader.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define BCDEC_IMPLEMENTATION
#include "../../../third_party/bcdec/bcdec.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>

namespace mediaserver::render {
// Offline baker only. Runtime keeps compressed mip residency.
bool decode_bake_texture(ModelTexture &texture) {
  if (!texture.valid())
    return false;
  if (texture.encoding == ModelTextureEncoding::Rgba8)
    return true;
  if (texture.encoding != ModelTextureEncoding::Bc7)
    return false;
  std::vector<core::u8> pixels(static_cast<std::size_t>(texture.width) *
                               texture.height * 4U);
  const auto columns = (texture.width + 3U) / 4U;
  for (core::u32 y = 0; y < texture.height; y += 4) {
    for (core::u32 x = 0; x < texture.width; x += 4) {
      std::array<core::u8, 64> block{};
      bcdec_bc7(texture.rgba.data() +
                    (static_cast<std::size_t>(y / 4) * columns + x / 4) * 16,
                block.data(), 16);
      for (core::u32 by = 0; by < std::min(4U, texture.height - y); ++by)
        for (core::u32 bx = 0; bx < std::min(4U, texture.width - x); ++bx)
          std::copy_n(
              block.data() + (by * 4 + bx) * 4, 4,
              pixels.data() +
                  (static_cast<std::size_t>(y + by) * texture.width + x + bx) *
                      4);
    }
  }
  texture.rgba = std::move(pixels);
  texture.lower_mips.clear();
  texture.encoding = ModelTextureEncoding::Rgba8;
  return true;
}
} // namespace mediaserver::render
