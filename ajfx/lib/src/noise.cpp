#include <ajfx/noise.hpp>

#include "noise.hpp"

namespace ajfx {

Vec3 curl_noise(Vec3 q, std::uint32_t seed) {
    return detail::curl_noise(q, seed);
}

} // namespace ajfx
