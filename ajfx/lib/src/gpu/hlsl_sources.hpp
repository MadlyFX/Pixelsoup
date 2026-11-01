#pragma once

#include <string_view>

// The files in shaders/, embedded at configure time (see CMakeLists.txt) so
// kernel generation needs no files at runtime.
namespace ajfx::detail {

extern const std::string_view hlsl_random;
extern const std::string_view hlsl_particles;
extern const std::string_view hlsl_runtime;
extern const std::string_view hlsl_sprite;
extern const std::string_view hlsl_sort;

} // namespace ajfx::detail
