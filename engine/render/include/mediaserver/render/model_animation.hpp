#pragma once

#include <mediaserver/render/model_loader.hpp>
#include <span>

namespace mediaserver::render {
// Fixed construction-time scratch and GPU palette bounds. Overflow refuses the
// frame; neither the camera nor the available frame time selects fewer joints.
inline constexpr core::u32 max_model_animation_nodes = 1024U;
inline constexpr core::u32 max_model_pose_bindings = 4096U;
inline constexpr core::u32 max_frame_pose_bindings = 65536U;

struct ModelPoseScratch final {
  std::array<scene::Mat4, max_model_animation_nodes> world{};
  std::array<scene::Vec3, max_model_animation_nodes> translation{};
  std::array<scene::Vec4, max_model_animation_nodes> rotation{};
  std::array<scene::Vec3, max_model_animation_nodes> scale{};
};

// Validation is worker-only, shared by glTF and the package reader/writer.
[[nodiscard]] bool validate_model_animation(const ModelAsset &asset) noexcept;
[[nodiscard]] double model_animation_time(core::u64 frame,
                                          scene::MediaFrameRate rate,
                                          const scene::ModelAnimation &settings,
                                          float duration) noexcept;
[[nodiscard]] bool evaluate_model_pose(const ModelAsset &asset,
                                       const scene::ModelAnimation &settings,
                                       core::u64 frame,
                                       scene::MediaFrameRate rate,
                                       ModelPoseScratch &scratch,
                                       std::span<scene::Mat4> palette) noexcept;
[[nodiscard]] ModelBounds
model_pose_bounds(const ModelAsset &asset, std::span<const scene::Mat4> palette,
                  scene::Vec3 wind) noexcept;
[[nodiscard]] scene::Vec3 model_wind(const scene::ModelAnimation &settings,
                                     core::u64 frame) noexcept;
} // namespace mediaserver::render
