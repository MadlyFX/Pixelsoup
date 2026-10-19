#pragma once

#include <mediaserver/core/types.hpp>

namespace mediaserver::core {

// A single output window can span a Mosaic desktop. Validate dimensions
// independently of aspect ratio; actual allocation still depends on the
// selected device and its available memory.
inline constexpr u32 max_output_dimension = 16'384U;

} // namespace mediaserver::core
