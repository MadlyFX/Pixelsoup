#include <mediaserver/render/model_animation.hpp>

#include <algorithm>
#include <cmath>

namespace mediaserver::render {
namespace {
bool finite(scene::Vec4 v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::isfinite(v.w);
}
bool finite(scene::Vec3 v) noexcept { return finite({v.x, v.y, v.z, 0}); }
scene::Vec4 add(scene::Vec4 a, scene::Vec4 b) noexcept {
  return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
scene::Vec4 scale(scene::Vec4 a, float b) noexcept {
  return {a.x * b, a.y * b, a.z * b, a.w * b};
}
scene::Vec4 unit(scene::Vec4 q) noexcept {
  const float square = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  return square > 1e-20F ? scale(q, 1.0F / std::sqrt(square))
                         : scene::Vec4{0, 0, 0, 1};
}
scene::Vec4 slerp(scene::Vec4 a, scene::Vec4 b, float t) noexcept {
  a = unit(a);
  b = unit(b);
  float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (cosine < 0) {
    b = scale(b, -1);
    cosine = -cosine;
  }
  cosine = std::clamp(cosine, 0.0F, 1.0F);
  if (cosine > 0.9995F)
    return unit(add(scale(a, 1 - t), scale(b, t)));
  const float sine = std::sqrt(1 - cosine * cosine);
  const auto angle =
      cosine == 0 ? 0.25F : scene::deterministic::atan_turns(sine / cosine);
  return unit(
      add(scale(a, scene::deterministic::sin_turns((1 - t) * angle) / sine),
          scale(b, scene::deterministic::sin_turns(t * angle) / sine)));
}
scene::Mat4 trs(scene::Vec3 p, scene::Vec4 q, scene::Vec3 s) noexcept {
  q = unit(q);
  const float x = q.x, y = q.y, z = q.z, w = q.w;
  auto m = scene::identity();
  m.m[0] = (1 - 2 * (y * y + z * z)) * s.x;
  m.m[1] = 2 * (x * y + z * w) * s.x;
  m.m[2] = 2 * (x * z - y * w) * s.x;
  m.m[4] = 2 * (x * y - z * w) * s.y;
  m.m[5] = (1 - 2 * (x * x + z * z)) * s.y;
  m.m[6] = 2 * (y * z + x * w) * s.y;
  m.m[8] = 2 * (x * z + y * w) * s.z;
  m.m[9] = 2 * (y * z - x * w) * s.z;
  m.m[10] = (1 - 2 * (x * x + y * y)) * s.z;
  m.m[12] = p.x;
  m.m[13] = p.y;
  m.m[14] = p.z;
  return m;
}
scene::Vec4 sample(const ModelAnimationChannel &c, double time) noexcept {
  const bool cubic = c.interpolation == ModelInterpolation::CubicSpline;
  const auto value = [&](std::size_t i) {
    return c.values[cubic ? 3 * i + 1 : i];
  };
  if (time <= c.times.front())
    return value(0);
  if (time >= c.times.back())
    return value(c.times.size() - 1);
  const auto right = static_cast<std::size_t>(
      std::upper_bound(c.times.begin(), c.times.end(), time) - c.times.begin());
  const auto left = right - 1;
  if (c.interpolation == ModelInterpolation::Step)
    return value(left);
  const float delta = c.times[right] - c.times[left];
  const float t = static_cast<float>((time - c.times[left]) / delta);
  if (cubic) {
    const float t2 = t * t, t3 = t2 * t;
    auto v = add(scale(value(left), 2 * t3 - 3 * t2 + 1),
                 scale(c.values[left * 3 + 2], delta * (t3 - 2 * t2 + t)));
    v = add(v, scale(value(right), -2 * t3 + 3 * t2));
    return add(v, scale(c.values[right * 3], delta * (t3 - t2)));
  }
  return c.path == ModelAnimationPath::Rotation
             ? slerp(value(left), value(right), t)
             : add(scale(value(left), 1 - t), scale(value(right), t));
}
// Binary modular multiplication consumes every frame bit. Absolute show time
// is never converted to float/double, including above 2^53 and at UINT64_MAX.
double frame_mod(core::u64 frame, double step, double period) noexcept {
  double result = 0;
  step = std::fmod(step, period);
  for (unsigned bit = 0; bit < 64; ++bit) {
    if ((frame & 1U) != 0) {
      result += step;
      if (result >= period)
        result -= period;
    }
    frame >>= 1U;
    step += step;
    if (step >= period)
      step -= period;
  }
  return result;
}
} // namespace

bool validate_model_animation(const ModelAsset &a) noexcept {
  if (a.animation_nodes.size() > max_model_animation_nodes ||
      a.pose_bindings.size() > max_model_pose_bindings ||
      a.animations.size() > 64)
    return false;
  if (a.animation_nodes.empty())
    return a.pose_bindings.empty() && a.animations.empty();
  if (a.pose_bindings.empty() ||
      a.vertex_metadata.size() != a.mesh.vertices.size())
    return false;
  for (std::size_t i = 0; i < a.animation_nodes.size(); ++i) {
    const auto &n = a.animation_nodes[i];
    if ((n.parent != 0xFFFFFFFFU && n.parent >= i) || !finite(n.translation) ||
        !finite(n.rotation) || !finite(n.scale))
      return false;
    for (auto v : n.matrix.m)
      if (!std::isfinite(v))
        return false;
  }
  for (const auto &b : a.pose_bindings) {
    if (b.node >= a.animation_nodes.size())
      return false;
    for (auto v : b.inverse_bind.m)
      if (!std::isfinite(v))
        return false;
  }
  for (const auto &v : a.vertex_metadata) {
    if (!finite(v.weights) || v.weights.x < 0 || v.weights.y < 0 ||
        v.weights.z < 0 || v.weights.w < 0 ||
        std::abs(v.weights.x + v.weights.y + v.weights.z + v.weights.w - 1) >
            1e-4F)
      return false;
    for (auto j : v.joints)
      if (j >= a.pose_bindings.size())
        return false;
  }
  std::size_t total_keys = 0;
  for (const auto &clip : a.animations) {
    if (!std::isfinite(clip.duration) || clip.duration < 0 ||
        clip.duration > 86400 ||
        clip.channels.size() > max_model_animation_nodes * 3U)
      return false;
    std::array<bool, max_model_animation_nodes * 3U> seen{};
    for (const auto &c : clip.channels) {
      if (c.node >= a.animation_nodes.size() ||
          a.animation_nodes[c.node].has_matrix ||
          static_cast<core::u32>(c.path) > 2 ||
          static_cast<core::u32>(c.interpolation) > 2 || c.times.empty())
        return false;
      const auto target = c.node * 3U + static_cast<core::u32>(c.path);
      if (seen[target])
        return false;
      seen[target] = true;
      const bool cubic = c.interpolation == ModelInterpolation::CubicSpline;
      if (c.values.size() != c.times.size() * (cubic ? 3U : 1U) ||
          (cubic && c.times.size() < 2))
        return false;
      total_keys += c.times.size();
      if (total_keys > 1000000U)
        return false;
      float previous = -1;
      for (float t : c.times) {
        if (!std::isfinite(t) || t <= previous || t < 0 || t > clip.duration)
          return false;
        previous = t;
      }
      for (auto v : c.values)
        if (!finite(v))
          return false;
    }
  }
  return true;
}

double model_animation_time(core::u64 frame, scene::MediaFrameRate rate,
                            const scene::ModelAnimation &settings,
                            float duration) noexcept {
  if (frame <= settings.start_frame || duration <= 0 || rate.numerator == 0 ||
      rate.denominator == 0)
    return 0;
  frame -= settings.start_frame;
  if (settings.loop)
    return frame_mod(frame, rate.denominator,
                     static_cast<double>(duration) * rate.numerator) /
           rate.numerator;
  const double last =
      static_cast<double>(duration) * rate.numerator / rate.denominator;
  if (frame >= static_cast<core::u64>(std::ceil(last)))
    return duration;
  return static_cast<double>(frame) * rate.denominator / rate.numerator;
}

bool evaluate_model_pose(const ModelAsset &asset,
                         const scene::ModelAnimation &settings, core::u64 frame,
                         scene::MediaFrameRate rate, ModelPoseScratch &s,
                         std::span<scene::Mat4> palette) noexcept {
  if (asset.animation_nodes.size() > s.world.size() ||
      palette.size() < asset.pose_bindings.size())
    return false;
  const ModelAnimationClip *clip = nullptr;
  if (settings.enabled && !asset.animations.empty()) {
    if (settings.clip >= asset.animations.size())
      return false;
    clip = &asset.animations[settings.clip];
  }
  for (std::size_t i = 0; i < asset.animation_nodes.size(); ++i) {
    s.translation[i] = asset.animation_nodes[i].translation;
    s.rotation[i] = asset.animation_nodes[i].rotation;
    s.scale[i] = asset.animation_nodes[i].scale;
  }
  if (clip) {
    const double time =
        model_animation_time(frame, rate, settings, clip->duration);
    for (const auto &c : clip->channels) {
      const auto v = sample(c, time);
      switch (c.path) {
      case ModelAnimationPath::Translation:
        s.translation[c.node] = {v.x, v.y, v.z};
        break;
      case ModelAnimationPath::Rotation:
        s.rotation[c.node] = unit(v);
        break;
      case ModelAnimationPath::Scale:
        s.scale[c.node] = {v.x, v.y, v.z};
        break;
      }
    }
  }
  for (std::size_t i = 0; i < asset.animation_nodes.size(); ++i) {
    const auto &n = asset.animation_nodes[i];
    const auto local = n.has_matrix
                           ? n.matrix
                           : trs(s.translation[i], s.rotation[i], s.scale[i]);
    s.world[i] = n.parent == 0xFFFFFFFFU
                     ? local
                     : scene::multiply(s.world[n.parent], local);
  }
  for (std::size_t i = 0; i < asset.pose_bindings.size(); ++i) {
    const auto &b = asset.pose_bindings[i];
    palette[i] = scene::multiply(s.world[b.node], b.inverse_bind);
    for (float v : palette[i].m)
      if (!std::isfinite(v))
        return false;
  }
  return true;
}

scene::Vec3 model_wind(const scene::ModelAnimation &s,
                       core::u64 frame) noexcept {
  if (s.wind_period_frames == 0 || frame < s.start_frame)
    return {};
  const auto phase =
      static_cast<float>((frame - s.start_frame) % s.wind_period_frames) /
      static_cast<float>(s.wind_period_frames);
  return s.wind * scene::deterministic::sin_turns(phase);
}

ModelBounds model_pose_bounds(const ModelAsset &asset,
                              std::span<const scene::Mat4> palette,
                              scene::Vec3 wind) noexcept {
  ModelBounds b = asset.bounds;
  bool first = true;
  for (const auto &m : palette)
    for (unsigned corner = 0; corner < 8; ++corner) {
      const auto p = scene::transform_point(
          m, {(corner & 1U) ? asset.bounds.maximum.x : asset.bounds.minimum.x,
              (corner & 2U) ? asset.bounds.maximum.y : asset.bounds.minimum.y,
              (corner & 4U) ? asset.bounds.maximum.z : asset.bounds.minimum.z});
      if (first) {
        b = {p, p};
        first = false;
      }
      b.minimum = {std::min(b.minimum.x, p.x), std::min(b.minimum.y, p.y),
                   std::min(b.minimum.z, p.z)};
      b.maximum = {std::max(b.maximum.x, p.x), std::max(b.maximum.y, p.y),
                   std::max(b.maximum.z, p.z)};
    }
  const scene::Vec3 extent{std::abs(wind.x), std::abs(wind.y),
                           std::abs(wind.z)};
  b.minimum = b.minimum - extent;
  b.maximum = b.maximum + extent;
  return b;
}
} // namespace mediaserver::render
