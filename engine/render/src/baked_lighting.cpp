#include <mediaserver/scene/lighting.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/ies_profile.hpp>
#include <mediaserver/render/image_decode.hpp>
#include <mediaserver/render/model_loader.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace mediaserver::render {
bool decode_bake_texture(ModelTexture &texture);
namespace {
using scene::Vec3;
using scene::Vec4;
constexpr float pi = 3.14159265358979323846F;
constexpr float ray_bias = 0.0005F;
constexpr core::u64 max_asset_bytes = 8U * 1024U * 1024U;
Vec3 product(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 positive(Vec3 a) {
  return {std::max(a.x, 0.0F), std::max(a.y, 0.0F), std::max(a.z, 0.0F)};
}
float axis(Vec3 v, unsigned i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; }
Vec3 minimum(Vec3 a, Vec3 b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}
Vec3 maximum(Vec3 a, Vec3 b) {
  return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}
bool finite(Vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
std::array<float, 9> basis(Vec3 n) {
  return {0.282095F,
          0.488603F * n.y,
          0.488603F * n.z,
          0.488603F * n.x,
          1.092548F * n.x * n.y,
          1.092548F * n.y * n.z,
          0.315392F * (3 * n.z * n.z - 1),
          1.092548F * n.x * n.z,
          0.546274F * (n.x * n.x - n.y * n.y)};
}
float radical(core::u32 value, core::u32 base) {
  float result = 0, scale = 1;
  while (value) {
    scale /= static_cast<float>(base);
    result += static_cast<float>(value % base) * scale;
    value /= base;
  }
  return result;
}
Vec3 sphere(float u, float v) {
  const float y = 1 - 2 * u, r = std::sqrt(std::max(0.0F, 1 - y * y)),
              phi = 2 * pi * v;
  return {r * std::cos(phi), y, r * std::sin(phi)};
}
Vec3 cosine(Vec3 n, float u, float v) {
  const Vec3 t = scene::normalize(
      scene::cross(std::abs(n.y) < 0.9F ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, n),
      {1, 0, 0});
  const Vec3 b = scene::cross(n, t);
  const float r = std::sqrt(u), phi = 2 * pi * v;
  return scene::normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) +
                              n * std::sqrt(1 - u),
                          n);
}
Vec3 oct_direction(float x, float y) {
  Vec3 n{x, y, 1 - std::abs(x) - std::abs(y)};
  if (n.z < 0) {
    n.x = (1 - std::abs(y)) * (x >= 0 ? 1.0F : -1.0F);
    n.y = (1 - std::abs(x)) * (y >= 0 ? 1.0F : -1.0F);
  }
  return scene::normalize(n, {0, 0, 1});
}
Vec3 oct_texel(Vec3 d) {
  const float divisor = std::abs(d.x) + std::abs(d.y) + std::abs(d.z);
  d = d * (1 / std::max(divisor, 1e-8F));
  if (d.z < 0) {
    const float x = d.x;
    d.x = (1 - std::abs(d.y)) * (x >= 0 ? 1.0F : -1.0F);
    d.y = (1 - std::abs(x)) * (d.y >= 0 ? 1.0F : -1.0F);
  }
  return {(d.x * 0.5F + 0.5F) * 8 - 0.5F, (d.y * 0.5F + 0.5F) * 8 - 0.5F, 0};
}
Vec3 visibility_moments(const BakedProbe &probe, Vec3 direction) {
  const auto texel = oct_texel(direction);
  const int x = static_cast<int>(std::floor(texel.x));
  const int y = static_cast<int>(std::floor(texel.y));
  const float fx = texel.x - std::floor(texel.x),
              fy = texel.y - std::floor(texel.y);
  const auto sample = [&](int px, int py) {
    // The octahedral edges fold onto their opposite half, rather than clamping
    // unrelated directions together at a pole.
    if (px < 0 || px > 7) {
      px = std::clamp(px, 0, 7);
      py = 7 - py;
    }
    if (py < 0 || py > 7) {
      py = std::clamp(py, 0, 7);
      px = 7 - px;
    }
    const unsigned cell = static_cast<unsigned>(py * 8 + px);
    const auto &packed = probe.values[9 + cell / 2];
    return cell % 2 == 0 ? Vec3{packed.x, packed.y, 0}
                         : Vec3{packed.z, packed.w, 0};
  };
  return (sample(x, y) * (1 - fx) + sample(x + 1, y) * fx) * (1 - fy) +
         (sample(x, y + 1) * (1 - fx) + sample(x + 1, y + 1) * fx) * fy;
}
Vec3 probe_position(const scene::BakedLighting &g, unsigned x, unsigned y,
                    unsigned z) {
  return {g.minimum.x + (g.maximum.x - g.minimum.x) * static_cast<float>(x) /
                            static_cast<float>(g.count_x - 1),
          g.minimum.y + (g.maximum.y - g.minimum.y) * static_cast<float>(y) /
                            static_cast<float>(g.count_y - 1),
          g.minimum.z + (g.maximum.z - g.minimum.z) * static_cast<float>(z) /
                            static_cast<float>(g.count_z - 1)};
}
core::u64 hash_bytes(std::span<const core::u8> bytes) {
  core::u64 h = 14695981039346656037ULL;
  for (auto b : bytes) {
    h ^= b;
    h *= 1099511628211ULL;
  }
  return h;
}
std::string hex(core::u64 h) {
  std::ostringstream s;
  s << std::hex << std::setfill('0') << std::setw(16) << h;
  return s.str();
}
void put(std::vector<core::u8> &b, core::u32 v) {
  for (unsigned i = 0; i < 4; ++i)
    b.push_back(static_cast<core::u8>(v >> (i * 8)));
}
void put(std::vector<core::u8> &b, float v) {
  put(b, std::bit_cast<core::u32>(v));
}
std::vector<core::u8> read(const std::string &source, core::u64 limit) {
  std::ifstream f(
      std::filesystem::path{std::u8string(source.begin(), source.end())},
      std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  const auto n = f.tellg();
  if (n <= 0 || static_cast<core::u64>(n) > limit)
    return {};
  std::vector<core::u8> bytes(static_cast<std::size_t>(n));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size())))
    return {};
  return bytes;
}
float half(core::u16 v) {
  const auto sign = (v & 0x8000U) ? -1.0F : 1.0F;
  const int e = (v >> 10U) & 31U;
  const float m = static_cast<float>(v & 1023U);
  return sign *
         (e == 0 ? std::ldexp(m, -24) : std::ldexp(1 + m / 1024, e - 15));
}
struct Triangle {
  Vec3 a, b, c, n, albedo, emission;
  Vec3 uv_a{}, uv_b{}, uv_c{};
  int color_texture{-1}, emission_texture{-1}, metal_texture{-1};
  float metallic{};
  ModelSampler color_sampler{}, emission_sampler{}, metal_sampler{};
};
struct BvhNode {
  Vec3 lo, hi;
  unsigned first{}, count{}, left{}, right{};
};
struct Hit {
  float distance{1e30F};
  unsigned triangle{};
  bool found{};
  float u{}, v{};
};
struct World {
  std::vector<std::shared_ptr<const IesProfile>> ies;
  std::vector<Triangle> triangles;
  std::vector<ModelTexture> textures;
  std::vector<unsigned> order;
  std::vector<BvhNode> nodes;
  DecodedFrame environment;
  std::vector<Vec3> physical_environment;
  std::vector<double> environment_cdf;
  double environment_weight{};
  const scene::Scene *source{};
  float far_distance{1000};
  double environment_solid_angle(unsigned y) const {
    return (2.0 * static_cast<double>(pi) / environment.width) *
           (std::cos(static_cast<double>(pi) * y / environment.height) -
            std::cos(static_cast<double>(pi) * (y + 1) / environment.height));
  }
  void prepare_environment_sampling() {
    if (!environment.valid())
      return;
    const auto width = environment.width, height = environment.height;
    std::vector<double> luminance(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < luminance.size(); ++i) {
      core::u16 rgb[3]{};
      std::memcpy(rgb, environment.pixels.data() + i * 8, sizeof(rgb));
      luminance[i] =
          std::max(0.0, 0.2126 * half(rgb[0]) + 0.7152 * half(rgb[1]) +
                            0.0722 * half(rgb[2]));
    }
    environment_cdf.reserve(luminance.size());
    for (unsigned y = 0; y < height; ++y)
      for (unsigned x = 0; x < width; ++x) {
        // Include the bilinear footprint so rays also cover the bright texel's
        // neighbouring cells. Solid angle prevents pole texels dominating.
        double energy = 0;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const auto px = static_cast<unsigned>(std::clamp(
                static_cast<int>(x) + dx, 0, static_cast<int>(width) - 1));
            const auto py = static_cast<unsigned>(std::clamp(
                static_cast<int>(y) + dy, 0, static_cast<int>(height) - 1));
            energy += luminance[static_cast<std::size_t>(py) * width + px] *
                      (dx == 0 ? 0.75 : 0.125) * (dy == 0 ? 0.75 : 0.125);
          }
        environment_weight += energy * environment_solid_angle(y);
        environment_cdf.push_back(environment_weight);
      }
  }
  Vec3 sample_direction(unsigned sample, unsigned count,
                        float &inverse_pdf) const {
    const bool importance = environment_weight > 0;
    const unsigned i = importance ? sample / 2 : sample;
    const unsigned n =
        importance ? ((sample & 1U) ? count / 2 : (count + 1) / 2) : count;
    Vec3 d = sphere((static_cast<float>(i) + 0.5F) / static_cast<float>(n),
                    radical(i + 1, 2));
    if (importance && (sample & 1U)) {
      const double target =
          (static_cast<double>(i) + 0.5) / n * environment_weight;
      const auto found = std::lower_bound(environment_cdf.begin(),
                                          environment_cdf.end(), target);
      const auto cell = static_cast<unsigned>(std::min<std::size_t>(
          found - environment_cdf.begin(), environment_cdf.size() - 1));
      const unsigned x = cell % environment.width, y = cell / environment.width;
      const float u = (static_cast<float>(x) + radical(i + 1, 3)) /
                      static_cast<float>(environment.width);
      const float high = std::cos(pi * static_cast<float>(y) /
                                  static_cast<float>(environment.height));
      const float low = std::cos(pi * static_cast<float>(y + 1) /
                                 static_cast<float>(environment.height));
      const float elevation = high + (low - high) * radical(i + 1, 5);
      const float radius = std::sqrt(std::max(0.0F, 1 - elevation * elevation));
      const float angle =
          (u - 0.5F - source->environment.map_rotation) * (2 * pi);
      d = {radius * std::cos(angle), elevation, radius * std::sin(angle)};
    }
    double pdf = 1.0 / (4 * pi);
    if (importance) {
      float u = std::atan2(d.z, d.x) / (2 * pi) + 0.5F +
                source->environment.map_rotation;
      u -= std::floor(u);
      const float v = std::acos(std::clamp(d.y, -1.0F, 1.0F)) / pi;
      const unsigned x = std::min(
          static_cast<unsigned>(u * static_cast<float>(environment.width)),
          environment.width - 1);
      const unsigned y = std::min(
          static_cast<unsigned>(v * static_cast<float>(environment.height)),
          environment.height - 1);
      const auto cell = static_cast<std::size_t>(y) * environment.width + x;
      const double mass =
          environment_cdf[cell] - (cell ? environment_cdf[cell - 1] : 0);
      const double uniform_fraction =
          static_cast<double>((count + 1) / 2) / count;
      pdf = uniform_fraction * pdf +
            (1 - uniform_fraction) * mass /
                (environment_weight * environment_solid_angle(y));
    }
    inverse_pdf = static_cast<float>(1 / pdf);
    return d;
  }
  unsigned build(unsigned first, unsigned count) {
    BvhNode node{};
    node.lo = {1e30F, 1e30F, 1e30F};
    node.hi = -node.lo;
    node.first = first;
    node.count = count;
    for (unsigned i = first; i < first + count; ++i) {
      const auto &t = triangles[order[i]];
      node.lo = minimum(node.lo, minimum(t.a, minimum(t.b, t.c)));
      node.hi = maximum(node.hi, maximum(t.a, maximum(t.b, t.c)));
    }
    const unsigned at = static_cast<unsigned>(nodes.size());
    nodes.push_back(node);
    if (count <= 8)
      return at;
    const Vec3 extent = node.hi - node.lo;
    const unsigned split = extent.x >= extent.y && extent.x >= extent.z ? 0U
                           : extent.y >= extent.z                       ? 1U
                                                                        : 2U;
    std::stable_sort(order.begin() + first, order.begin() + first + count,
                     [&](unsigned a, unsigned b) {
                       const auto &ta = triangles[a];
                       const auto &tb = triangles[b];
                       return axis(ta.a + ta.b + ta.c, split) <
                              axis(tb.a + tb.b + tb.c, split);
                     });
    nodes[at].count = 0;
    nodes[at].left = build(first, count / 2);
    nodes[at].right = build(first + count / 2, count - count / 2);
    return at;
  }
  bool box(const BvhNode &b, Vec3 o, Vec3 d, float far) const {
    float near = 0;
    for (unsigned i = 0; i < 3; ++i) {
      const float dir = axis(d, i), origin = axis(o, i), lo = axis(b.lo, i),
                  hi = axis(b.hi, i);
      if (std::abs(dir) < 1e-12F) {
        if (origin < lo || origin > hi)
          return false;
        continue;
      }
      const float a = (lo - origin) / dir, c = (hi - origin) / dir;
      near = std::max(near, std::min(a, c));
      far = std::min(far, std::max(a, c));
      if (near > far)
        return false;
    }
    return true;
  }
  Hit intersect(Vec3 o, Vec3 d, float far) const {
    Hit hit{};
    hit.distance = far;
    if (nodes.empty())
      return hit;
    std::array<unsigned, 64> stack{};
    unsigned size = 1;
    stack[0] = 0;
    while (size) {
      const auto &node = nodes[stack[--size]];
      if (!box(node, o, d, hit.distance))
        continue;
      if (!node.count) {
        stack[size++] = node.right;
        stack[size++] = node.left;
        continue;
      }
      for (unsigned i = node.first; i < node.first + node.count; ++i) {
        const unsigned index = order[i];
        const auto &t = triangles[index];
        const Vec3 e1 = t.b - t.a, e2 = t.c - t.a, p = scene::cross(d, e2);
        const float det = scene::dot(e1, p);
        if (std::abs(det) < 1e-9F)
          continue;
        const float inv = 1 / det;
        const Vec3 s = o - t.a;
        const float u = scene::dot(s, p) * inv;
        if (u < 0 || u > 1)
          continue;
        const Vec3 q = scene::cross(s, e1);
        const float v = scene::dot(d, q) * inv;
        if (v < 0 || u + v > 1)
          continue;
        const float distance = scene::dot(e2, q) * inv;
        if (distance > ray_bias && distance < hit.distance)
          hit = {distance, index, true, u, v};
      }
    }
    return hit;
  }
  Vec3 sky(Vec3 d) const {
    const auto &e = source->environment;
    if (!physical_environment.empty()) {
      const float u = std::atan2(d.z,d.x)/(2*pi)+0.5F;
      const float v = std::acos(std::clamp(d.y,-1.0F,1.0F))/pi;
      const float x = u*256-0.5F, y = v*128-0.5F;
      const int ix=static_cast<int>(std::floor(x)), iy=static_cast<int>(std::floor(y));
      const float fx=x-std::floor(x), fy=y-std::floor(y);
      const auto sample = [&](int px,int py) { return physical_environment[static_cast<std::size_t>(std::clamp(py,0,127)*256+std::clamp(px,0,255))]; };
      return (sample(ix,iy)*(1-fx)+sample(ix+1,iy)*fx)*(1-fy)
          +(sample(ix,iy+1)*(1-fx)+sample(ix+1,iy+1)*fx)*fy;
    }
    if (!environment.valid())
      return (e.ground_color * (0.5F - 0.5F * d.y) +
              e.sky_color * (0.5F + 0.5F * d.y)) *
             e.ambient;
    // Match ibl_equirect_uv and the runtime's linear clamp sampler. Opposite
    // rotation signs move the key light to the other side after baking.
    float u = std::atan2(d.z, d.x) / (2 * pi) + 0.5F + e.map_rotation;
    u -= std::floor(u);
    const float v = std::acos(std::clamp(d.y, -1.0F, 1.0F)) / pi;
    const float x = u * static_cast<float>(environment.width) - 0.5F;
    const float y = v * static_cast<float>(environment.height) - 0.5F;
    const int ix = static_cast<int>(std::floor(x)),
              iy = static_cast<int>(std::floor(y));
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const auto pixel = [&](int px, int py) {
      px = std::clamp(px, 0, static_cast<int>(environment.width) - 1);
      py = std::clamp(py, 0, static_cast<int>(environment.height) - 1);
      const auto offset = (static_cast<std::size_t>(py) * environment.width +
                           static_cast<unsigned>(px)) *
                          8;
      const auto channel = [&](unsigned c) {
        core::u16 value{};
        std::memcpy(&value, environment.pixels.data() + offset + c * 2, 2);
        return half(value);
      };
      return Vec3{channel(0), channel(1), channel(2)};
    };
    return ((pixel(ix, iy) * (1 - fx) + pixel(ix + 1, iy) * fx) * (1 - fy) +
            (pixel(ix, iy + 1) * (1 - fx) + pixel(ix + 1, iy + 1) * fx) * fy) *
           e.map_intensity;
  }
  Vec3 direct(Vec3 p, Vec3 n) const {
    Vec3 sum{};
    for (const auto &l : source->directional_lights) {
      if (!l.bake_indirect)
        continue;
      const Vec3 d = scene::normalize(-l.direction, {0, 1, 0});
      const float cosine_term = std::max(0.0F, scene::dot(n, d));
      if (cosine_term > 0 && !intersect(p, d, far_distance).found)
        sum = sum + l.color * (l.intensity * cosine_term);
    }
    for (const auto &l : source->point_lights) {
      if (!l.bake_indirect)
        continue;
      if (scene::area_light(l.kind)) {
        const float centre_distance = scene::length(l.position - p);
        if (centre_distance >= l.range) continue;
        const float fade = std::max(0.0F, 1 - centre_distance / l.range);
        for (core::u32 sample_index = 0; sample_index < scene::area_light_sample_count; ++sample_index) {
          const auto sample = scene::area_light_sample(l, sample_index);
          const Vec3 delta = sample.position - p;
          const float distance = scene::length(delta);
          if (distance <= ray_bias) continue;
          const Vec3 d = delta * (1 / distance);
          const float facing = scene::dot(sample.normal, -d);
          const float emission = l.kind == scene::LocalLightKind::Rectangle && l.two_sided ? std::abs(facing) : std::max(facing, 0.0F);
          const float weight = std::max(0.0F, scene::dot(n, d)) * emission * sample.area / (distance * distance);
          if (weight > 0 && !intersect(p, d, distance - ray_bias).found)
            sum = sum + l.color * (l.intensity * weight * fade * fade);
        }
        continue;
      }
      const Vec3 delta = l.position - p;
      const float distance = std::sqrt(scene::dot(delta, delta));
      if (distance < ray_bias || distance >= l.range)
        continue;
      const Vec3 d = delta * (1 / distance);
      float weight = std::max(0.0F, scene::dot(n, d));
      if (l.kind == scene::LocalLightKind::Spot) {
        weight *= scene::spot_cone_attenuation(
            l, scene::dot(-d, scene::normalize(l.direction, {0, -1, 0})));
      }
      const auto index = static_cast<std::size_t>(&l - source->point_lights.data());
      if (ies[index]) weight *= ies_weight(*ies[index], l, -d);
      const float falloff = std::max(0.0F, 1 - distance / l.range);
      if (weight > 0 && !intersect(p, d, distance - ray_bias).found)
        sum = sum + l.color * (l.intensity * weight * falloff * falloff /
                               std::max(distance * distance, 0.0001F));
    }
    return sum;
  }
  Vec3 texture(int index, Vec3 uv, bool srgb, const ModelSampler& sampler) const {
    if (index < 0)
      return {1, 1, 1};
    const auto &t = textures[static_cast<unsigned>(index)];
    const auto coordinate = [](float value, ModelAddressMode mode) {
      if (mode == ModelAddressMode::Clamp) return std::clamp(value, 0.0F, 1.0F);
      const float period = mode == ModelAddressMode::Mirror ? 2.0F : 1.0F;
      return value - std::floor(value / period) * period;
    };
    const float x = coordinate(uv.x, sampler.u) * static_cast<float>(t.width) - 0.5F;
    const float y = coordinate(uv.y, sampler.v) * static_cast<float>(t.height) - 0.5F;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const float fx = x - std::floor(x), fy = y - std::floor(y);
    const auto address = [](int index_value, int extent, ModelAddressMode mode) {
      if (mode == ModelAddressMode::Clamp) return std::clamp(index_value, 0, extent - 1);
      const int period = mode == ModelAddressMode::Mirror ? extent * 2 : extent;
      const int wrapped = (index_value % period + period) % period;
      return wrapped < extent ? wrapped : period - wrapped - 1;
    };
    const auto pixel = [&](int px, int py) {
      px = address(px, static_cast<int>(t.width), sampler.u);
      py = address(py, static_cast<int>(t.height), sampler.v);
      const auto offset =
          (static_cast<std::size_t>(py) * t.width + static_cast<unsigned>(px)) *
          4;
      const auto channel = [&](unsigned c) {
        const float v = static_cast<float>(t.rgba[offset + c]) / 255;
        return srgb ? (v <= 0.04045F ? v / 12.92F
                                     : std::pow((v + 0.055F) / 1.055F, 2.4F))
                    : v;
      };
      return Vec3{channel(0), channel(1), channel(2)};
    };
    // The offline tracer samples the base image without a pixel footprint;
    // use its authored magnification filter, with no view-dependent LOD bias.
    if (sampler.mag == ModelFilter::Nearest)
      return pixel(static_cast<int>(std::floor(x + 0.5F)), static_cast<int>(std::floor(y + 0.5F)));
    return (pixel(ix, iy) * (1 - fx) + pixel(ix + 1, iy) * fx) * (1 - fy) +
           (pixel(ix, iy + 1) * (1 - fx) + pixel(ix + 1, iy + 1) * fx) * fy;
  }
  Vec3 trace(Vec3 o, Vec3 d, unsigned sample, unsigned bounces) const {
    Vec3 throughput{1, 1, 1}, result{};
    for (unsigned depth = 0; depth <= bounces; ++depth) {
      const Hit h = intersect(o, d, far_distance);
      if (!h.found) {
        result = result + product(throughput, sky(d));
        break;
      }
      const auto &t = triangles[h.triangle];
      Vec3 n = t.n;
      // Closed surfaces stop transport on either side; the normal is
      // oriented to the incident ray for diffuse transport in a room.
      if (scene::dot(n, d) > 0)
        n = -n;
      const Vec3 p = o + d * h.distance + n * ray_bias;
      const Vec3 uv = t.uv_a * (1 - h.u - h.v) + t.uv_b * h.u + t.uv_c * h.v;
      const Vec3 albedo =
          minimum(product(t.albedo, texture(t.color_texture, uv, true, t.color_sampler)) *
                      (1 - t.metallic * texture(t.metal_texture, uv, false, t.metal_sampler).z),
                  {1, 1, 1});
      const Vec3 emission =
          product(t.emission, texture(t.emission_texture, uv, true, t.emission_sampler));
      result =
          result + product(throughput,
                           emission + product(albedo, direct(p, n)) * (1 / pi));
      if (depth == bounces)
        break;
      throughput = product(throughput, albedo);
      const unsigned seed = sample + depth * 65537U + 17U;
      d = cosine(n, radical(seed, 2), radical(seed, 3));
      o = p;
    }
    return positive(result);
  }
};

bool append_mesh(World &w, const scene::MeshData &mesh,
                 const scene::Mat4 &world, const scene::Material &m,
                 unsigned first, unsigned count, std::string &error,
                 int color_texture = -1, int emission_texture = -1,
                 int metal_texture = -1,
                 std::array<ModelSampler, 14U> samplers = {}) {
  if (m.alpha_mode != scene::MaterialAlphaMode::Opaque || m.transmission > 0) {
    error = "Bake geometry must be opaque diffuse surfaces; mark transmitting "
            "or alpha surfaces Dynamic for bake.";
    return false;
  }
  const Vec3 albedo = positive(m.base_color);
  for (unsigned i = first; i + 2 < first + count; i += 3) {
    Triangle t{};
    t.a =
        scene::transform_point(world, mesh.vertices[mesh.indices[i]].position);
    t.b = scene::transform_point(world,
                                 mesh.vertices[mesh.indices[i + 1]].position);
    t.c = scene::transform_point(world,
                                 mesh.vertices[mesh.indices[i + 2]].position);
    if (!finite(t.a) || !finite(t.b) || !finite(t.c)) {
      error = "Non-finite bake geometry";
      return false;
    }
    const Vec3 n = scene::cross(t.b - t.a, t.c - t.a);
    if (scene::dot(n, n) < 1e-16F)
      continue;
    t.n = scene::normalize(n, {0, 1, 0});
    t.albedo = albedo;
    t.emission = positive(m.emissive) * std::max(0.0F, m.emissive_strength);
    const auto va = mesh.vertices[mesh.indices[i]],
               vb = mesh.vertices[mesh.indices[i + 1]],
               vc = mesh.vertices[mesh.indices[i + 2]];
    t.uv_a = {va.u, va.v, 0};
    t.uv_b = {vb.u, vb.v, 0};
    t.uv_c = {vc.u, vc.v, 0};
    t.color_texture = color_texture;
    t.emission_texture = emission_texture;
    t.metal_texture = metal_texture;
    t.color_sampler = samplers[0];
    t.metal_sampler = samplers[1];
    t.emission_sampler = samplers[4];
    t.metallic = std::clamp(m.metallic, 0.0F, 1.0F);
    w.triangles.push_back(t);
  }
  return true;
}
} // namespace

std::vector<core::u8> encode_probe_asset(const ProbeAsset &a) {
  if (a.probes.size() != static_cast<std::size_t>(a.grid.count_x) *
                             a.grid.count_y * a.grid.count_z ||
      a.probes.size() > scene::max_baked_probes)
    return {};
  std::vector<core::u8> b;
  b.reserve(80 + a.probes.size() * sizeof(BakedProbe));
  put(b, core::u32{0x4947534d});
  const bool reflections = std::any_of(a.grid.reflections.begin(), a.grid.reflections.end(), [](const auto& p) { return p.enabled; });
  put(b, core::u32{a.input_digest ? 3U : reflections ? 2U : 1U});
  for (float f : {a.grid.minimum.x, a.grid.minimum.y, a.grid.minimum.z,
                  a.grid.maximum.x, a.grid.maximum.y, a.grid.maximum.z})
    put(b, f);
  for (unsigned v : {a.grid.count_x, a.grid.count_y, a.grid.count_z,
                     a.grid.samples, a.grid.bounces})
    put(b, v);
  for (const auto &p : a.probes)
    for (const auto &v : p.values)
      for (float f : {v.x, v.y, v.z, v.w})
        put(b, f);
  if (reflections || a.input_digest) {
    for (std::size_t i = 0; i < a.grid.reflections.size(); ++i) {
      const auto& p = a.grid.reflections[i];
      put(b, static_cast<core::u32>(p.enabled));
      if (!p.enabled) continue;
      if (p.source == doc::NodeId{} || scene::clamped(p) != p || a.reflections[i].size() != reflection_capture_texels) return {};
      put(b, static_cast<core::u32>(p.source.value())); put(b, static_cast<core::u32>(p.source.value() >> 32));
      put(b, static_cast<core::u32>(p.sphere));
      for (float v : {p.position.x, p.position.y, p.position.z, p.extent.x, p.extent.y, p.extent.z, p.blend, p.intensity}) put(b, v);
      for (const auto& v : a.reflections[i]) for (float f : {v.x, v.y, v.z, v.w}) put(b, f);
    }
  }
  if (a.input_digest) { put(b, static_cast<core::u32>(a.input_digest)); put(b, static_cast<core::u32>(a.input_digest >> 32)); }
  const auto h = hash_bytes(b);
  put(b, static_cast<core::u32>(h));
  put(b, static_cast<core::u32>(h >> 32));
  return b;
}
BakeResult decode_probe_asset(std::span<const core::u8> b) {
  const auto fail = [] {
    return BakeResult{{}, {}, "Invalid, unsupported or corrupt probe asset"};
  };
  if (b.size() < 60 || b.size() > max_asset_bytes)
    return fail();
  std::size_t at = 0;
  const auto u = [&]() {
    core::u32 v = 0;
    for (unsigned i = 0; i < 4; ++i)
      v |= static_cast<core::u32>(b[at++]) << (i * 8);
    return v;
  };
  const auto f = [&]() { return std::bit_cast<float>(u()); };
  if (u() != 0x4947534d) return fail();
  const auto version = u();
  if (version != 1 && version != 2 && version != 3) return fail();
  auto a = std::make_shared<ProbeAsset>();
  a->grid.minimum = {f(), f(), f()};
  a->grid.maximum = {f(), f(), f()};
  a->grid.count_x = u();
  a->grid.count_y = u();
  a->grid.count_z = u();
  a->grid.samples = u();
  a->grid.bounces = u();
  if (scene::clamped(a->grid) != a->grid)
    return fail();
  const auto count = static_cast<std::size_t>(a->grid.count_x) *
                     a->grid.count_y * a->grid.count_z;
  if ((version == 1 && b.size() != 52 + count * sizeof(BakedProbe) + 8) || b.size() < 52 + count * sizeof(BakedProbe) + 8)
    return fail();
  a->probes.resize(count);
  for (auto &p : a->probes)
    for (auto &v : p.values) {
      v = {f(), f(), f(), f()};
      if (!finite({v.x, v.y, v.z}) || !std::isfinite(v.w))
        return fail();
    }
  for (const auto &p : a->probes)
    for (unsigned i = 9; i < 41; ++i) {
      const auto v = p.values[i];
      if (v.x < 0 || v.y < 0 || v.z < 0 || v.w < 0)
        return fail();
    }
  if (version >= 2) {
    for (std::size_t i = 0; i < a->grid.reflections.size(); ++i) {
      if (at + 4 > b.size() - 8) return fail();
      const auto enabled = u(); if (enabled > 1) return fail();
      if (!enabled) continue;
      if (at + 44 + reflection_capture_texels * sizeof(Vec4) > b.size() - 8) return fail();
      auto& p = a->grid.reflections[i]; p.enabled = true;
      const core::u64 node_lo = u(), node_hi = u(); p.source = doc::NodeId{node_lo | (node_hi << 32)};
      const auto shape = u(); if (shape > 1) return fail(); p.sphere = shape != 0;
      p.position = {f(), f(), f()}; p.extent = {f(), f(), f()}; p.blend = f(); p.intensity = f();
      if (p.source == doc::NodeId{} || scene::clamped(p) != p) return fail();
      for (std::size_t j = 0; j < i; ++j) if (a->grid.reflections[j].enabled && a->grid.reflections[j].source == p.source) return fail();
      a->reflections[i].resize(reflection_capture_texels);
      for (auto& v : a->reflections[i]) {
        v = {f(), f(), f(), f()};
        if (!finite({v.x,v.y,v.z}) || v.x < 0 || v.y < 0 || v.z < 0 || v.w != 1) return fail();
      }
    }
  }
  if (version == 3) {
    if (at + 16 != b.size()) return fail();
    const core::u64 input_lo=u(), input_hi=u(); a->input_digest=input_lo | (input_hi<<32);
    if (!a->input_digest) return fail();
  }
  if (at != b.size() - 8) return fail();
  const core::u64 lo = u(), hi = u(), h = lo | (hi << 32);
  if (hash_bytes(b.first(b.size() - 8)) != h)
    return fail();
  a->content = hex(h);
  return {std::move(a), {}, {}};
}
BakeResult load_probe_asset(const std::string &source,
                            const std::string &expected) {
  try {
    auto result = decode_probe_asset(read(source, max_asset_bytes));
    if (result.ready() && !expected.empty() &&
        result.asset->content != expected)
      return {{}, {}, "Probe content identity mismatch"};
    result.source = source;
    return result;
  } catch (const std::exception &e) {
    return {{}, {}, e.what()};
  }
}
BakeResult write_probe_asset(const ProbeAsset &asset,
                             const std::string &destination) {
  try {
    const auto bytes = encode_probe_asset(asset);
    auto checked = decode_probe_asset(bytes);
    if (!checked.ready())
      return checked;
    const auto path = std::filesystem::absolute(
        std::filesystem::path{
            std::u8string(destination.begin(), destination.end())} +=
        std::filesystem::path{"." + checked.asset->content + ".msgi"});
    auto source = path.u8string();
    checked.source.assign(source.begin(), source.end());
    if (std::filesystem::exists(path)) {
      if (read(checked.source, max_asset_bytes) != bytes)
        return {{}, {}, "An immutable probe asset was replaced"};
      return checked;
    }
    // This worker is the sole writer. A temporary file is never an asset.
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".writing";
    {
      std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      f.flush();
      if (!f)
        return {{}, {}, "Could not write probe asset"};
    }
    std::filesystem::rename(temporary, path);
    return checked;
  } catch (const std::exception &e) {
    return {{}, {}, e.what()};
  }
}

core::u64 bake_input_digest(const scene::Scene &source) {
  scene::Scene input;
  // The transport manifest is process-local readiness metadata. Immutable
  // model paths already identify linked content; merely opening a project or
  // forming a publication manifest must not invalidate its lighting.
  input.asset_refresh_generation = source.asset_refresh_generation;
  input.renderer_settings.photometric_scale = source.renderer_settings.photometric_scale;
  input.environment.sky_color = source.environment.sky_color;
  input.environment.ground_color = source.environment.ground_color;
  input.environment.ambient = source.environment.ambient;
  input.environment.map_source = source.environment.map_source;
  input.environment.map_rotation = source.environment.map_rotation;
  input.environment.map_intensity = source.environment.map_intensity;
  input.environment.physical_sky = source.environment.physical_sky;
  input.environment.baked.reflections = source.environment.baked.reflections;
  for (const auto &instance : source.instances)
    if (instance.bake_static)
      input.instances.push_back(instance);
  for (const auto &model : source.models)
    if (model.bake_static)
      input.models.push_back(model);
  for (const auto &light : source.directional_lights)
    if (light.bake_indirect)
      input.directional_lights.push_back(light);
  for (const auto &light : source.point_lights)
    if (light.bake_indirect)
      input.point_lights.push_back(light);
  return scene::digest(input);
}

bool baked_lighting_stale(const scene::Scene& source) {
  const auto& b=source.environment.baked;
  if (!b.enabled && std::none_of(b.reflections.begin(),b.reflections.end(),[](const auto& p){return p.enabled;})) return false;
  return b.input_revision.empty() || b.input_revision != std::to_string(bake_input_digest(source));
}

BakeResult bake_lighting(const scene::Scene &scene, BakeProgress *progress) {
  try {
    if (progress)
      progress->stage.store(BakeStage::Reading);
    World world{};
    // This worker owns the copy. Convert once before tracing, using the same
    // values as GPU packing; neither a bounce nor a pixel evaluates temperature.
    scene::Scene lighting = scene;
    const auto convert = [&](auto& lights) {
      for (auto& light : lights) {
        if (!light.bake_indirect) continue;
        if (!scene::supported_light_unit(light)) return false;
        const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
        light.color = emission.color;
        light.intensity = emission.intensity;
      }
      return true;
    };
    if (!convert(lighting.directional_lights) || !convert(lighting.point_lights))
      return {{}, {}, "Unsupported light unit: use lux for directional lights and lumens or candela for points and spots."};
    world.ies.resize(scene.point_lights.size());
    std::array<std::string_view, max_ies_profiles> ies_sources{};
    std::array<std::shared_ptr<const IesProfile>, max_ies_profiles> ies_profiles{};
    std::size_t ies_count = 0;
    for (std::size_t i = 0; i < scene.point_lights.size(); ++i) {
      const auto& light = scene.point_lights[i];
      if (!light.bake_indirect || light.ies_source.empty()) continue;
      if (scene::area_light(light.kind)) return {{}, {}, "IES profiles require a point or spot light"};
      std::size_t slot = 0;
      while (slot < ies_count && ies_sources[slot] != light.ies_source) ++slot;
      if (slot == max_ies_profiles) return {{}, {}, "Bake exceeds sixteen distinct IES profiles"};
      if (slot == ies_count) {
        const auto loaded = load_ies_profile(light.ies_source);
        if (!loaded.profile) return {{}, {}, ies_error_label(loaded.error)};
        ies_sources[slot] = light.ies_source;
        ies_profiles[slot] = loaded.profile;
        ++ies_count;
      }
      world.ies[i] = ies_profiles[slot];
      const auto emission = ies_light_emission(light, scene.renderer_settings.photometric_scale, world.ies[i].get());
      lighting.point_lights[i].color = emission.color;
      lighting.point_lights[i].intensity = emission.intensity;
    }
    world.source = &lighting;
    if (scene.environment.physical_sky.enabled) {
      // Worker-owned table: path tracing does constant-time lookup per bounce,
      // rather than integrating the atmosphere again for every probe ray.
      world.physical_environment.resize(256U*128U);
      for (unsigned y=0; y<128; ++y) {
        if (progress && progress->cancel.load()) return {{}, {}, "Cancelled"};
        for (unsigned x=0; x<256; ++x) {
          const float phi=((static_cast<float>(x)+0.5F)/256-0.5F)*2*pi;
          const float theta=(static_cast<float>(y)+0.5F)/128*pi;
          const Vec3 direction{std::cos(phi)*std::sin(theta),std::cos(theta),std::sin(phi)*std::sin(theta)};
          world.physical_environment[y*256+x]=scene::physical_sky_radiance(scene.environment.physical_sky,direction)*scene.renderer_settings.photometric_scale;
        }
      }
    } else if (!scene.environment.map_source.empty()) {
      world.environment = decode_radiance_bytes(
          read(scene.environment.map_source, 512U * 1024U * 1024U));
      if (!world.environment.valid())
        return {{}, {}, "Could not read the bake environment (.hdr or .exr)"};
      world.prepare_environment_sampling();
    }
    std::string error;
    const auto material_texture = [&](const std::string &source) {
      if (source.empty())
        return -1;
      auto decoded = decode_image_bytes(read(source, 512U * 1024U * 1024U));
      if (!decoded.valid() ||
          decoded.pixels.size() !=
              static_cast<std::size_t>(decoded.width) * decoded.height * 4)
        throw std::runtime_error("Could not decode bake texture: " + source);
      const int index = static_cast<int>(world.textures.size());
      world.textures.push_back(ModelTexture{decoded.width, decoded.height,
                                            std::move(decoded.pixels)});
      return index;
    };
    for (const auto &instance : scene.instances)
      if (instance.bake_static) {
        if (progress && progress->cancel.load())
          return {{}, {}, "Bake cancelled"};
        scene::MeshData mesh;
        if (!scene::build_mesh(instance.mesh, mesh) ||
            !append_mesh(
                world, mesh, instance.world, instance.material, 0,
                static_cast<unsigned>(mesh.indices.size()), error,
                material_texture(instance.material.base_color_texture),
                material_texture(instance.material.emissive_texture),
                material_texture(instance.material.metallic_roughness_texture)))
          return {{}, {}, error};
      }
    for (const auto &instance : scene.models)
      if (instance.bake_static) {
        if (progress && progress->cancel.load())
          return {{}, {}, "Bake cancelled"};
        ModelAsset model;
          if (!load_model(instance.source, model))
            return {{}, {}, "Could not load bake model: " + instance.source};
          if (!model.animation_nodes.empty() || instance.animation.wind != scene::Vec3{})
            return {{}, {}, "Animated or wind-driven models must use Bake Geometry: Dynamic: " + instance.source};
        const unsigned texture_base =
            static_cast<unsigned>(world.textures.size());
        for (auto &texture : model.textures)
          world.textures.push_back(std::move(texture));
        const auto texture_index = [&](std::optional<core::u32> index) {
          if (!index)
            return -1;
          const unsigned at = texture_base + *index;
          if (at >= world.textures.size() ||
              !decode_bake_texture(world.textures[at]))
            throw std::runtime_error("Could not decode bake material texture");
          return static_cast<int>(at);
        };
        const unsigned parts = model.lods.empty()
                                   ? static_cast<unsigned>(model.parts.size())
                                   : model.lods[0].part_count;
        for (unsigned i = 0; i < parts; ++i) {
          const auto &part = model.parts[i];
          const auto &material =
              part.has_material ? part.material : instance.material;
          if (!append_mesh(
                  world, model.mesh, instance.world, material, part.first_index,
                  part.index_count, error,
                  part.base_color_texture
                      ? texture_index(part.base_color_texture)
                      : material_texture(material.base_color_texture),
                  part.emissive_texture
                      ? texture_index(part.emissive_texture)
                      : material_texture(material.emissive_texture),
                  part.metallic_roughness_texture
                      ? texture_index(part.metallic_roughness_texture)
                      : material_texture(material.metallic_roughness_texture), part.samplers))
            return {{}, {}, error};
        }
      }
    world.order.resize(world.triangles.size());
    std::iota(world.order.begin(), world.order.end(), 0U);
    if (!world.order.empty())
      static_cast<void>(
          world.build(0, static_cast<unsigned>(world.order.size())));
    auto asset = std::make_shared<ProbeAsset>();
    asset->input_digest = bake_input_digest(scene);
    asset->grid = scene::clamped(scene.environment.baked);
    asset->grid.input_revision.clear();
    asset->grid.enabled = false;
    asset->grid.source.clear();
    asset->grid.content.clear();
    asset->grid.strength = 1;
    const auto &g = asset->grid;
    const unsigned count = g.count_x * g.count_y * g.count_z;
    asset->probes.resize(count);
    const Vec3 extent = g.maximum - g.minimum;
    world.far_distance =
        std::max(100.0F, std::sqrt(scene::dot(extent, extent)) * 8);
    if (!world.nodes.empty()) {
      const Vec3 size = world.nodes[0].hi - world.nodes[0].lo;
      world.far_distance =
          std::max(world.far_distance, std::sqrt(scene::dot(size, size)) * 8);
    }
    if (progress) {
      progress->total.store(count);
      progress->stage.store(BakeStage::Tracing);
    }
    for (unsigned z = 0; z < g.count_z; ++z)
      for (unsigned y = 0; y < g.count_y; ++y)
        for (unsigned x = 0; x < g.count_x; ++x) {
          if (progress && progress->cancel.load())
            return {{}, {}, "Bake cancelled"};
          const unsigned index = (z * g.count_y + y) * g.count_x + x;
          const Vec3 p = probe_position(g, x, y, z);
          auto &probe = asset->probes[index];
          // Double accumulation avoids sample-order noise; every run visits
          // the same samples in the same order, including visibility rays.
          std::array<std::array<double, 3>, 9> sums{};
          for (unsigned s = 0; s < g.samples; ++s) {
            if (progress && (s % 64 == 0) && progress->cancel.load())
              return {{}, {}, "Bake cancelled"};
            float inverse_pdf{};
            const Vec3 d = world.sample_direction(s, g.samples, inverse_pdf);
            const auto light = world.trace(p, d, s + 1, g.bounces);
            const auto sh = basis(d);
            for (unsigned k = 0; k < 9; ++k) {
              sums[k][0] += static_cast<double>(light.x * sh[k]) * inverse_pdf;
              sums[k][1] += static_cast<double>(light.y * sh[k]) * inverse_pdf;
              sums[k][2] += static_cast<double>(light.z * sh[k]) * inverse_pdf;
            }
          }
          const double scale = 1.0 / g.samples;
          for (unsigned k = 0; k < 9; ++k)
            probe.values[k] = {static_cast<float>(sums[k][0] * scale),
                               static_cast<float>(sums[k][1] * scale),
                               static_cast<float>(sums[k][2] * scale), 0};
          for (unsigned cell = 0; cell < 64; ++cell) {
            double sum = 0, square = 0;
            for (unsigned s = 0; s < 16; ++s) {
              const float u = (static_cast<float>(cell % 8) +
                               (static_cast<float>(s % 4) + 0.5F) / 4) /
                                  8 * 2 -
                              1;
              const float v = (static_cast<float>(cell / 8) +
                               (static_cast<float>(s / 4) + 0.5F) / 4) /
                                  8 * 2 -
                              1;
              const float distance =
                  world.intersect(p, oct_direction(u, v), world.far_distance)
                      .distance;
              sum += distance;
              square += static_cast<double>(distance) * distance;
            }
            auto &packed = probe.values[9 + cell / 2];
            const float mean = static_cast<float>(sum / 16),
                        moment = static_cast<float>(square / 16);
            if (cell % 2 == 0) {
              packed.x = mean;
              packed.y = moment;
            } else {
              packed.z = mean;
              packed.w = moment;
            }
          }
          if (progress)
            progress->completed.store(index + 1);
        }
    // Canonical node order is independent of slot edits and of any camera.
    std::sort(asset->grid.reflections.begin(), asset->grid.reflections.end(), [](const auto& a, const auto& b) {
      if (a.enabled != b.enabled) return a.enabled > b.enabled;
      return a.source < b.source;
    });
    for (std::size_t slot = 0; slot < asset->grid.reflections.size(); ++slot) {
      const auto& probe = asset->grid.reflections[slot];
      if (!probe.enabled) continue;
      if (probe.source == doc::NodeId{} || (slot > 0 && asset->grid.reflections[slot-1].source == probe.source))
        return {{}, {}, "Reflection probes need distinct document node IDs"};
      auto& capture = asset->reflections[slot]; capture.resize(reflection_capture_texels);
      if (progress) progress->total.fetch_add(reflection_capture_height);
      for (unsigned y = 0; y < reflection_capture_height; ++y) {
        if (progress && progress->cancel.load()) return {{}, {}, "Bake cancelled"};
        for (unsigned x = 0; x < reflection_capture_width; ++x) {
          Vec3 sum{};
          for (unsigned sample = 0; sample < 4; ++sample) {
            const float longitude = (static_cast<float>(x) + (static_cast<float>(sample % 2) + 0.5F) * 0.5F) / reflection_capture_width - 0.5F;
            const float latitude = (static_cast<float>(y) + (static_cast<float>(sample / 2) + 0.5F) * 0.5F) / (2 * reflection_capture_height);
            const float sine = scene::deterministic::sin_turns(latitude);
            const Vec3 direction{scene::deterministic::cos_turns(longitude) * sine, scene::deterministic::cos_turns(latitude), scene::deterministic::sin_turns(longitude) * sine};
            sum = sum + world.trace(probe.position, direction, (y * reflection_capture_width + x) * 4 + sample + 1, g.bounces);
          }
          capture[y * reflection_capture_width + x] = {sum.x * 0.25F, sum.y * 0.25F, sum.z * 0.25F, 1};
        }
        if (progress) progress->completed.fetch_add(1);
      }
    }
    const auto bytes = encode_probe_asset(*asset);
    auto checked = decode_probe_asset(bytes);
    return checked;
  } catch (const std::exception &e) {
    return {{}, {}, e.what()};
  }
}

Vec3 sample_probe_irradiance(const ProbeAsset &a, Vec3 position,
                             Vec3 normal) noexcept {
  const auto &g = a.grid;
  if (a.probes.empty() || !finite(position) || !finite(normal))
    return {};
  normal = scene::normalize(normal, {0, 1, 0});
  Vec3 cell{(position.x - g.minimum.x) / (g.maximum.x - g.minimum.x) *
                static_cast<float>(g.count_x - 1),
            (position.y - g.minimum.y) / (g.maximum.y - g.minimum.y) *
                static_cast<float>(g.count_y - 1),
            (position.z - g.minimum.z) / (g.maximum.z - g.minimum.z) *
                static_cast<float>(g.count_z - 1)};
  cell = {std::clamp(cell.x, 0.0F, static_cast<float>(g.count_x - 1)),
          std::clamp(cell.y, 0.0F, static_cast<float>(g.count_y - 1)),
          std::clamp(cell.z, 0.0F, static_cast<float>(g.count_z - 1))};
  const unsigned bx = std::min(static_cast<unsigned>(cell.x), g.count_x - 2),
                 by = std::min(static_cast<unsigned>(cell.y), g.count_y - 2),
                 bz = std::min(static_cast<unsigned>(cell.z), g.count_z - 2);
  const Vec3 f{cell.x - static_cast<float>(bx), cell.y - static_cast<float>(by),
               cell.z - static_cast<float>(bz)};
  const auto sh = basis(normal);
  Vec3 result{};
  float total = 0;
  for (unsigned z = 0; z < 2; ++z)
    for (unsigned y = 0; y < 2; ++y)
      for (unsigned x = 0; x < 2; ++x) {
        const auto &probe =
            a.probes[((bz + z) * g.count_y + by + y) * g.count_x + bx + x];
        const Vec3 delta = position + normal * 0.01F -
                           probe_position(g, bx + x, by + y, bz + z);
        const float distance = std::sqrt(scene::dot(delta, delta));
        const auto moments = visibility_moments(probe, delta);
        const float mean = moments.x, moment = moments.y;
        const float variance = std::max(moment - mean * mean, 0.00001F),
                    excess = std::max(distance - mean - 0.01F, 0.0F);
        const float probability = variance / (variance + excess * excess);
        const float visibility = std::max(0.0F, (probability - 0.1F) / 0.9F);
        const float weight = (x ? f.x : 1 - f.x) * (y ? f.y : 1 - f.y) *
                             (z ? f.z : 1 - f.z) * visibility;
        Vec3 irradiance{};
        for (unsigned k = 0; k < 9; ++k) {
          const auto c = probe.values[k];
          irradiance =
              irradiance + Vec3{c.x, c.y, c.z} * (sh[k] * (k == 0  ? pi
                                                           : k < 4 ? 2 * pi / 3
                                                                   : pi / 4));
        }
        result = result + positive(irradiance) * weight;
        total += weight;
      }
  return total > 0.00001F ? result * (1 / total) : Vec3{};
}

std::string_view bake_stage_label(BakeStage s) noexcept {
  switch (s) {
  case BakeStage::Queued:
    return "Bake queued";
  case BakeStage::Reading:
    return "Reading bake geometry and environment";
  case BakeStage::Tracing:
    return "Tracing lighting probes";
  case BakeStage::Writing:
    return "Writing immutable lighting asset";
  case BakeStage::Ready:
    return "Bake complete";
  case BakeStage::Failed:
    return "Bake failed";
  case BakeStage::Cancelled:
    return "Bake cancelled";
  }
  return "Bake unavailable";
}
} // namespace mediaserver::render
